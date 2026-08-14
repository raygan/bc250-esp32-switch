#include "ha.h"

#include <ArduinoHA.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "board.h"
#include "config.h"

// ---------------------------------------------------------------------------
// STATIC INITIALISATION ORDER IS LOAD-BEARING.
//
// Every HABaseDeviceType registers itself with HAMqtt::instance() from its own
// constructor, so HAMqtt must already exist by the time any entity below is
// constructed, and HADevice must exist before HAMqtt. Within a single
// translation unit C++ guarantees construction in declaration order, which is
// why all of this lives in one file, in this order:
//
//     WiFiClient  ->  HADevice  ->  HAMqtt  ->  entities
//
// Splitting them across translation units would make the order undefined and
// the entities would silently fail to register.
// ---------------------------------------------------------------------------

// Registration cap. The entity list below is 10; 12 leaves room for one or two
// more without silent loss. HAMqtt::addDeviceType() just `return`s when the cap
// is hit — no error, no log — so an overflow shows up only as entities missing
// in Home Assistant. (The library default is 24 on ESP32, so this explicit value
// is documentation of intent, not a rescue from the AVR-only default of 6.)
static const uint8_t HA_MAX_DEVICE_TYPES = 12;

// HA discovery payloads carry the full device block (name, model, manufacturer,
// version, identifiers) on every entity, which overruns PubSubClient's 256-byte
// default buffer and silently truncates the publish. ArduinoHA never sizes the
// buffer itself, so we do it here.
static const uint16_t MQTT_BUFFER_SIZE = 512;

static WiFiClient wifiClient;
static HADevice   device;
static HAMqtt     mqtt(wifiClient, device, HA_MAX_DEVICE_TYPES);

static HASwitch       entPower("power");
static HABinarySensor entBoard("board");
static HASensor       entState("state");
static HABinarySensor entController("controller");
static HASensorNumber entSenseMv("sense_mv");
static HASensorNumber entRssi("rssi");
static HASensorNumber entHeap("heap");
static HASensorNumber entUptime("uptime");
static HAButton       btnRestart("restart");
static HAButton       btnSetup("setup");

// --- Module state ---

static bool          g_wifiEnabled  = false;  // an SSID was configured
static bool          g_mqttEnabled  = false;  // a broker host was configured too
static bool          g_mqttStarted  = false;  // mqtt.begin() has been called
static bool          g_wasConnected = false;
static bool          g_wasMqttUp    = false;
static unsigned long g_lastAttempt  = 0;
static unsigned long g_lastMqttTry  = 0;
static uint32_t      g_connectCount = 0;      // WL_CONNECTED transitions
static IPAddress     g_brokerIp;
static bool          g_brokerResolved = false;

// Publish throttling.
static unsigned long g_lastDiagPublish  = 0;
static unsigned long g_lastSensePublish = 0;
static uint32_t      g_lastSenseMv      = 0;
static const char   *g_lastStateName    = nullptr;

// Buffers for strings handed to ArduinoHA, which stores the pointer rather than
// copying. They must outlive every publish, so they are static.
static char g_deviceName[33];
static char g_swVersion[24];

// Pending command from Home Assistant. Single slot, last-wins.
static volatile HaCommand g_pending = HA_CMD_NONE;

// Why the broker connection is failing. Without this the log only ever says
// "disconnected", which cannot distinguish a broker that isn't listening from
// one that rejected our credentials — the two have completely different fixes,
// and a rejected login fails just as fast as a refused socket.
static const char *mqttStateName(HAMqtt::ConnectionState s) {
  switch (s) {
    case HAMqtt::StateConnecting:        return "connecting";
    case HAMqtt::StateConnectionTimeout: return "timeout (broker unreachable)";
    case HAMqtt::StateConnectionLost:    return "connection lost";
    case HAMqtt::StateConnectionFailed:  return "TCP connect failed";
    case HAMqtt::StateDisconnected:      return "disconnected";
    case HAMqtt::StateConnected:         return "connected";
    case HAMqtt::StateBadProtocol:       return "bad protocol version";
    case HAMqtt::StateBadClientId:       return "bad client id";
    case HAMqtt::StateUnavailable:       return "broker unavailable";
    case HAMqtt::StateBadCredentials:    return "BAD USERNAME/PASSWORD";
    case HAMqtt::StateUnauthorized:      return "UNAUTHORIZED (broker refused login)";
    default:                             return "unknown";
  }
}

// --- Command callbacks ---
//
// These run inside mqtt.loop(), which is inside haLoop(), which has no business
// mutating the power state directly: it has no access to the loop's `now` and
// would bypass powerOn()/powerOff(). They only park a command; normalLoop()
// drains it on the next iteration.

static void onPowerCommand(bool state, HASwitch *sender) {
  (void)sender;
  Serial.printf("[HA  ] switch command: %s\n", state ? "ON" : "OFF");
  g_pending = state ? HA_CMD_POWER_ON : HA_CMD_POWER_OFF;
  // Deliberately NOT calling sender->setState(state) here. The entity must
  // report reality, never intent: if HA turns the machine on and the board
  // never asserts TPMS1, the boot watchdog powers it back off and the switch
  // follows it off. Echoing the command optimistically would show ON for a
  // machine that is not on.
}

static void onButtonCommand(HAButton *sender) {
  if (sender == &btnRestart) {
    Serial.println("[HA  ] restart button");
    g_pending = HA_CMD_RESTART;
  } else if (sender == &btnSetup) {
    Serial.println("[HA  ] setup-mode button");
    g_pending = HA_CMD_ENTER_SETUP;
  }
}

// --- WiFi station lifecycle ---
//
// State is polled from haLoop() using the loop's `now`. Control flow is
// deliberately NOT driven from WiFi.onEvent(): that callback runs on the system
// event task, off the loop's clock, and acting on it would break the
// single-clock discipline described in ha.h. The event handler below only logs.

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WIFI] disconnected, reason=%d\n",
                    info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WIFI] got ip %s\n", WiFi.localIP().toString().c_str());
      break;
    default:
      break;
  }
}

static void wifiConnect() {
  WiFi.begin(config.wifiSsid.c_str(),
             config.wifiPass.length() ? config.wifiPass.c_str() : nullptr);
}

// Resolve the broker hostname ONCE, when WiFi comes up. PubSubClient::connect()
// would otherwise do a blocking DNS lookup on every reconnect attempt, on top of
// the blocking TCP connect. An IP literal skips DNS entirely.
//
// allowBlocking must be false while the machine is BOOTING: hostByName() blocks
// for as long as the lookup takes, and a stall inside the 10s boot watchdog is
// the same spurious-timeout hazard as a blocking TCP connect. Parsing a literal
// address is pure string work and is always safe.
static void resolveBroker(bool allowBlocking) {
  if (g_brokerResolved) return;
  if (g_brokerIp.fromString(config.mqttHost)) {
    g_brokerResolved = true;
    Serial.printf("[MQTT] broker %s is a literal address\n",
                  config.mqttHost.c_str());
    return;
  }
  if (!allowBlocking) return;
  if (WiFi.hostByName(config.mqttHost.c_str(), g_brokerIp) == 1) {
    g_brokerResolved = true;
    Serial.printf("[MQTT] resolved %s -> %s\n", config.mqttHost.c_str(),
                  g_brokerIp.toString().c_str());
  } else {
    Serial.printf("[MQTT] could not resolve '%s'; will retry\n",
                  config.mqttHost.c_str());
  }
}

static void startMqtt() {
  snprintf(g_deviceName, sizeof(g_deviceName), "%s", haDeviceName());
  snprintf(g_swVersion, sizeof(g_swVersion), "%s", __DATE__);

  // esp_read_mac() works before the WiFi stack is started, unlike
  // WiFi.macAddress(), so device identity is stable from the very first boot.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  device.setUniqueId(mac, sizeof(mac));

  device.setName(g_deviceName);
  device.setManufacturer("AMD");
  device.setModel("BC250");
  device.setSoftwareVersion(g_swVersion);

  // Both must be set BEFORE mqtt.begin(): the availability topic is part of the
  // discovery payload, and the last will has to be registered in the CONNECT
  // packet itself. Without them HA keeps showing the last known state after the
  // device loses power instead of marking it unavailable.
  device.enableSharedAvailability();
  device.enableLastWill();

  entPower.setName("Power");
  entPower.onCommand(onPowerCommand);

  entBoard.setName("Board power");
  entBoard.setDeviceClass("power");

  entState.setName("State");

  entController.setName("Controller present");
  entController.setDeviceClass("connectivity");

  // No device_class: Home Assistant's "voltage" class expects volts, and this is
  // millivolts. Declaring it would make HA render 2900 V.
  entSenseMv.setName("Sense voltage");
  entSenseMv.setUnitOfMeasurement("mV");

  entRssi.setName("WiFi signal");
  entRssi.setDeviceClass("signal_strength");
  entRssi.setUnitOfMeasurement("dBm");

  entHeap.setName("Free heap");
  entHeap.setUnitOfMeasurement("B");

  entUptime.setName("Uptime");
  entUptime.setUnitOfMeasurement("s");

  btnRestart.setName("Restart");
  btnRestart.setIcon("mdi:restart");
  btnRestart.onCommand(onButtonCommand);

  btnSetup.setName("Setup mode");
  btnSetup.setIcon("mdi:wifi-cog");
  btnSetup.onCommand(onButtonCommand);

  mqtt.setBufferSize(MQTT_BUFFER_SIZE);

  mqtt.begin(g_brokerIp, mqttPort(),
             config.mqttUser.length() ? config.mqttUser.c_str() : nullptr,
             config.mqttPass.length() ? config.mqttPass.c_str() : nullptr);
  g_mqttStarted = true;
  Serial.printf("[MQTT] begin %s:%u as '%s'\n", g_brokerIp.toString().c_str(),
                mqttPort(), g_deviceName);
}

// Push the snapshot into the entities. The switch/binary/number setters already
// no-op when the value is unchanged, so they are safe to call every iteration.
// HASensor::setValue (the state string) does NOT — it publishes unconditionally
// — so it is deduped here against the last published pointer.
static void publishState(unsigned long now, const HaState &s) {
  entPower.setState(s.powerOn);
  entBoard.setState(s.boardUp);
  // Only meaningful when a controller is bound; otherwise it would report a
  // permanent "disconnected" for a feature that isn't in use.
  if (s.bleActive) entController.setState(s.blePresent);

  if (s.stateName != g_lastStateName) {
    g_lastStateName = s.stateName;
    entState.setValue(s.stateName);
  }

  // senseMv moves with ADC noise on nearly every loop, so publishing on change
  // alone would emit hundreds of messages a second. Publish on a meaningful
  // step, or as a slow keepalive.
  uint32_t delta = s.senseMv > g_lastSenseMv ? s.senseMv - g_lastSenseMv
                                             : g_lastSenseMv - s.senseMv;
  unsigned long sinceSense = now - g_lastSensePublish;
  // The delta test is gated behind a minimum interval, because on its own it is
  // not a rate limit — see HA_SENSE_MIN_INTERVAL_MS. The keepalive is separate
  // so a genuinely static reading still refreshes every HA_SENSE_PUBLISH_MS.
  if ((delta >= HA_SENSE_DELTA_MV && sinceSense >= HA_SENSE_MIN_INTERVAL_MS) ||
      sinceSense >= HA_SENSE_PUBLISH_MS) {
    g_lastSenseMv = s.senseMv;
    g_lastSensePublish = now;
    entSenseMv.setValue((int32_t)s.senseMv);
  }

  if ((now - g_lastDiagPublish) >= HA_DIAG_PUBLISH_MS) {
    g_lastDiagPublish = now;
    entRssi.setValue((int32_t)WiFi.RSSI());
    entHeap.setValue((int32_t)ESP.getFreeHeap());
    entUptime.setValue((int32_t)(now / 1000));
  }
}

// --- Public interface ---

void haBegin() {
  if (!wifiConfigured()) {
    // Graceful-degradation base case: no SSID means the radio stays off and this
    // module does nothing at all. The device is byte-for-byte its old self.
    WiFi.mode(WIFI_OFF);
    Serial.println("[WIFI] no SSID configured; station mode disabled");
    return;
  }

  g_wifiEnabled = true;
  g_mqttEnabled = mqttConfigured();

  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);

  // Modem sleep is a COEXISTENCE REQUIREMENT, not a power optimisation. With it
  // off, the WiFi MAC holds the shared radio continuously and starves the BLE
  // scan, so controller wake stops working.
  WiFi.setSleep(true);

  WiFi.setTxPower(STA_TX_POWER);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);  // credentials live in our NVS namespace, not the SDK's

  // Bound a single blocking TCP connect. PubSubClient's own default is 15s,
  // which would stall the loop far past the point of missing a button press.
  wifiClient.setConnectionTimeout(MQTT_CONNECT_TIMEOUT_MS);

  Serial.printf("[WIFI] connecting to '%s' (txpwr=%d)\n",
                config.wifiSsid.c_str(), (int)WiFi.getTxPower());
  wifiConnect();
  g_lastAttempt = millis();  // pre-loop; every later timestamp uses haLoop()'s now

  if (!g_mqttEnabled) {
    Serial.println("[MQTT] no broker configured; Home Assistant disabled");
  }
}

void haLoop(unsigned long now, const HaState &s, bool allowReconnect) {
  if (!g_wifiEnabled) return;

  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected != g_wasConnected) {
    g_wasConnected = connected;
    if (connected) {
      g_connectCount++;
      Serial.printf("[WIFI] connected #%lu rssi=%d ip=%s\n",
                    (unsigned long)g_connectCount, WiFi.RSSI(),
                    WiFi.localIP().toString().c_str());
      if (g_mqttEnabled) resolveBroker(allowReconnect);
    } else {
      Serial.println("[WIFI] link lost");
    }
  }

  // Backstop re-issue of WiFi.begin(). setAutoReconnect() covers most drops, but
  // not every failure mode leaves the SDK retrying. Non-blocking: begin()
  // returns immediately, so this is safe even while BOOTING.
  if (!connected && (now - g_lastAttempt) >= WIFI_RETRY_MS) {
    g_lastAttempt = now;
    Serial.println("[WIFI] retrying association");
    wifiConnect();
  }

  if (!g_mqttEnabled || !connected) return;

  // A hostname that didn't resolve at association time gets retried on the same
  // cadence as the connection itself.
  if (!g_brokerResolved) {
    if (!allowReconnect || (now - g_lastMqttTry) < MQTT_RETRY_MS) return;
    g_lastMqttTry = now;
    resolveBroker(true);
    return;
  }

  if (!g_mqttStarted) startMqtt();

  // Reconnect gating. ArduinoHA only ever attempts a connection from inside
  // loop(), so NOT calling loop() is what suppresses the attempt — there is no
  // other lever. While connected, loop() is cheap and must run every iteration
  // to service keepalives and inbound commands. While disconnected, each call
  // can block for up to MQTT_CONNECT_TIMEOUT_MS, so it is rationed.
  bool up = mqtt.isConnected();
  if (up) {
    mqtt.loop();
  } else if (allowReconnect && (now - g_lastMqttTry) >= MQTT_RETRY_MS) {
    g_lastMqttTry = now;
    mqtt.loop();
    up = mqtt.isConnected();
    if (!up) {
      // ArduinoHA sets StateConnecting before attempting, and does NOT update
      // the state when the attempt fails — it only syncs from PubSubClient at
      // the TOP of the next loop(). Reading getState() here would therefore
      // always report a useless "connecting". This second call does that sync:
      // it cannot trigger another connection because ArduinoHA's own
      // ReconnectInterval (10s) is shorter than the attempt we just made, so
      // connectToServer() returns immediately and nothing blocks.
      mqtt.loop();
      Serial.printf("[MQTT] connect to %s:%u failed (state=%d %s)\n",
                    g_brokerIp.toString().c_str(), mqttPort(),
                    (int)mqtt.getState(), mqttStateName(mqtt.getState()));
    }
  }

  if (up != g_wasMqttUp) {
    g_wasMqttUp = up;
    Serial.printf("[MQTT] %s (state=%d %s)\n", up ? "connected" : "disconnected",
                  (int)mqtt.getState(), mqttStateName(mqtt.getState()));
    if (up) {
      // Force a full republish after a reconnect so HA isn't left holding
      // whatever was true before the link dropped.
      g_lastStateName    = nullptr;
      g_lastDiagPublish  = 0;
      g_lastSensePublish = 0;
    }
  }

  if (up) publishState(now, s);
}

HaCommand haTakeCommand() {
  HaCommand c = g_pending;
  g_pending = HA_CMD_NONE;
  return c;
}

bool haWifiConnected() { return g_wifiEnabled && WiFi.status() == WL_CONNECTED; }

bool haMqttConnected() { return g_mqttStarted && mqtt.isConnected(); }

int8_t haRssi() { return haWifiConnected() ? (int8_t)WiFi.RSSI() : 0; }
