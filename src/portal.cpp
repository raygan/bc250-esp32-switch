#include "portal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include "mbedtls/sha256.h"

#include "board.h"
#include "config.h"

static const size_t MIN_PASSWORD_LEN = 6;

static DNSServer       dnsServer;
static AsyncWebServer  server(80);

// Single-session bearer token, handed out on password set / login and required
// by the protected endpoints. Regenerated each set/login; "" means no session.
static String          g_token;

// Deferred restart (so the HTTP response can flush before we reboot).
static unsigned long   g_restartAt = 0;

// --- BLE device discovery (active scan, accumulated for the picker) ---
struct BleDev {
  char addr[18];
  char name[33];
  int  rssi;
  bool used;
};
static const int       MAX_DEVS = 48;
static BleDev          devs[MAX_DEVS];
static portMUX_TYPE    devsMux = portMUX_INITIALIZER_UNLOCKED;

// --- WiFi network list (scanned once, before the AP exists) ---
// A live scanNetworks() in AP mode takes the SoftAP off-channel for seconds and
// drops the phone's association, so the list is captured at startup and served
// from this cache. "Rescan" in the UI is re-enter-setup-and-reboot, not a live
// scan. Manual SSID entry stays available for hidden networks.
struct WifiNet {
  char ssid[33];
  int  rssi;
  bool open;
};
static const int MAX_NETS = 24;
static WifiNet   nets[MAX_NETS];
static int       netCount = 0;

class SetupScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *d) override {
    std::string addrStr = d->getAddress().toString();
    const char *addr = addrStr.c_str();
    std::string name = d->getName();
    int rssi = d->getRSSI();

    portENTER_CRITICAL(&devsMux);
    int idx = -1, free = -1;
    for (int i = 0; i < MAX_DEVS; i++) {
      if (devs[i].used) {
        if (strcmp(devs[i].addr, addr) == 0) { idx = i; break; }
      } else if (free < 0) {
        free = i;
      }
    }
    if (idx < 0 && free >= 0) {
      idx = free;
      devs[idx].used = true;
      strncpy(devs[idx].addr, addr, sizeof(devs[idx].addr) - 1);
      devs[idx].addr[sizeof(devs[idx].addr) - 1] = 0;
      devs[idx].name[0] = 0;
    }
    if (idx >= 0) {
      devs[idx].rssi = rssi;
      if (!name.empty()) {
        strncpy(devs[idx].name, name.c_str(), sizeof(devs[idx].name) - 1);
        devs[idx].name[sizeof(devs[idx].name) - 1] = 0;
      }
    }
    portEXIT_CRITICAL(&devsMux);
  }
};
static SetupScanCallbacks scanCallbacks;

// --- Helpers ---

static String sha256hex(const String &s) {
  uint8_t out[32];
  mbedtls_sha256((const unsigned char *)s.c_str(), s.length(), out, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[i]);
  hex[64] = 0;
  return String(hex);
}

static String makeToken() {
  char buf[33];
  for (int i = 0; i < 4; i++) sprintf(buf + i * 8, "%08x", (unsigned)esp_random());
  buf[32] = 0;
  return String(buf);
}

static bool authed(AsyncWebServerRequest *req) {
  if (g_token.isEmpty()) return false;
  if (!req->hasHeader("X-Auth-Token")) return false;
  return req->header("X-Auth-Token") == g_token;
}

static void sendJsonError(AsyncWebServerRequest *req, int code, const char *msg) {
  JsonDocument doc;
  doc["error"] = msg;
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

static void serveIndex(AsyncWebServerRequest *req) {
  if (SPIFFS.exists("/index.html")) {
    req->send(SPIFFS, "/index.html", "text/html");
  } else {
    req->send(200, "text/html",
              "<h1>BC250 Switch</h1><p>Filesystem image missing. Flash it with "
              "<code>pio run -t uploadfs</code>.</p>");
  }
}

// Scan for access points and cache the result. Must run BEFORE the SoftAP is
// started — see the WifiNet comment above. Costs ~3s at startup, which is
// before anyone can have connected to the AP.
static void scanWifiNetworks() {
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  netCount = 0;
  for (int i = 0; i < n && netCount < MAX_NETS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;  // hidden; use manual entry for these
    int  rssi = WiFi.RSSI(i);
    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);

    // Dedupe by SSID, keeping the strongest signal (mesh/repeater networks
    // advertise the same SSID from several BSSIDs).
    int found = -1;
    for (int j = 0; j < netCount; j++) {
      if (ssid == nets[j].ssid) { found = j; break; }
    }
    if (found >= 0) {
      if (rssi > nets[found].rssi) nets[found].rssi = rssi;
      continue;
    }
    strncpy(nets[netCount].ssid, ssid.c_str(), sizeof(nets[netCount].ssid) - 1);
    nets[netCount].ssid[sizeof(nets[netCount].ssid) - 1] = 0;
    nets[netCount].rssi = rssi;
    nets[netCount].open = open;
    netCount++;
  }
  WiFi.scanDelete();
  Serial.printf("[PORTAL] wifi scan: %d found, %d cached\n", n, netCount);
}

// --- Endpoint handlers ---

static void handleStatus(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["passwordSet"] = config.passHash.length() > 0;
  doc["wakeAddr"]    = config.wakeAddr;
  doc["configured"]  = isConfigured();
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// Current settings for the UI to prefill. Authed, unlike /api/status: this
// exposes the network the device is joined to and the broker it talks to.
// Passwords are NEVER returned — only whether one is stored.
static void handleGetSettings(AsyncWebServerRequest *req) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonDocument doc;
  doc["wifiSsid"]    = config.wifiSsid;
  doc["wifiPassSet"] = config.wifiPass.length() > 0;
  doc["mqttHost"]    = config.mqttHost;
  doc["mqttPort"]    = mqttPort();
  doc["mqttUser"]    = config.mqttUser;
  doc["mqttPassSet"] = config.mqttPass.length() > 0;
  doc["deviceName"]  = config.deviceName;
  doc["wakeAddr"]    = config.wakeAddr;
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleWifiScan(AsyncWebServerRequest *req) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < netCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = nets[i].ssid;
    o["rssi"] = nets[i].rssi;
    o["open"] = nets[i].open;
  }
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// Password sentinel, shared by /api/wifi and /api/mqtt:
//
//   key ABSENT            -> keep whatever is already stored
//   key PRESENT and ""    -> clear the stored password
//   key PRESENT, non-""   -> replace
//
// This is what lets the UI show "password saved" and submit the form without
// ever round-tripping the secret. Do NOT "simplify" this to `o["password"] |
// config.wifiPass` — that collapses the clear case into the keep case, and a
// user who deletes their password would silently keep the old one.
static bool keyPresent(JsonObject o, const char *key) {
  return !o[key].isNull();
}

static void handleWifi(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonObject o = json.as<JsonObject>();
  String ssid = o["ssid"] | "";
  ssid.trim();

  if (ssid.length() > 32) { sendJsonError(req, 400, "ssid too long"); return; }

  // An empty SSID disables station mode entirely; drop the password with it so
  // no orphaned secret is left in NVS.
  if (ssid.isEmpty()) {
    setWifiCreds("", "");
    Serial.println("[PORTAL] wifi cleared (station mode disabled)");
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }

  String pass = keyPresent(o, "password") ? (o["password"] | "") : config.wifiPass;
  if (pass.length() > 63) { sendJsonError(req, 400, "password too long"); return; }

  setWifiCreds(ssid, pass);
  Serial.printf("[PORTAL] wifi set: ssid='%s' pass=%s\n", ssid.c_str(),
                pass.length() ? "(set)" : "(open)");
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleMqtt(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonObject o = json.as<JsonObject>();
  String host = o["host"] | "";
  host.trim();

  // An empty host disables MQTT; clear the credentials with it.
  if (host.isEmpty()) {
    setMqttSettings("", mqttPort(), "", "");
    Serial.println("[PORTAL] mqtt cleared (disabled)");
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (host.length() > 64) { sendJsonError(req, 400, "host too long"); return; }

  uint32_t port = o["port"] | (uint32_t)mqttPort();
  if (port < 1 || port > 65535) { sendJsonError(req, 400, "invalid port"); return; }

  String user = o["user"] | "";
  String pass = keyPresent(o, "password") ? (o["password"] | "") : config.mqttPass;

  setMqttSettings(host, (uint16_t)port, user, pass);
  Serial.printf("[PORTAL] mqtt set: %s:%u user=%s pass=%s\n", host.c_str(),
                (unsigned)port, user.length() ? user.c_str() : "(none)",
                pass.length() ? "(set)" : "(none)");
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleDevice(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonObject o = json.as<JsonObject>();
  String name = o["name"] | "";
  name.trim();
  if (name.length() > 32) { sendJsonError(req, 400, "name too long"); return; }
  setDeviceName(name);
  Serial.printf("[PORTAL] device name: '%s'\n",
                name.length() ? name.c_str() : haDeviceName());
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handlePassword(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonObject o = json.as<JsonObject>();
  String pw = o["password"] | "";
  if (pw.length() < MIN_PASSWORD_LEN) {
    sendJsonError(req, 400, "password too short");
    return;
  }
  // Setting the first password is open; changing an existing one needs a session.
  if (config.passHash.length() > 0 && !authed(req)) {
    sendJsonError(req, 401, "unauthorized");
    return;
  }
  setPassHash(sha256hex(pw));
  g_token = makeToken();
  Serial.println("[PORTAL] password set");

  JsonDocument doc;
  doc["token"] = g_token;
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleLogin(AsyncWebServerRequest *req, JsonVariant &json) {
  if (config.passHash.isEmpty()) {
    sendJsonError(req, 400, "no password set");
    return;
  }
  JsonObject o = json.as<JsonObject>();
  String pw = o["password"] | "";
  if (sha256hex(pw) != config.passHash) {
    sendJsonError(req, 401, "wrong password");
    return;
  }
  g_token = makeToken();
  JsonDocument doc;
  doc["token"] = g_token;
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleBleDevices(AsyncWebServerRequest *req) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }

  // Snapshot under the lock, then build JSON without holding it.
  BleDev snap[MAX_DEVS];
  portENTER_CRITICAL(&devsMux);
  memcpy(snap, devs, sizeof(devs));
  portEXIT_CRITICAL(&devsMux);

  JsonDocument doc;
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (int i = 0; i < MAX_DEVS; i++) {
    if (!snap[i].used) continue;
    JsonObject o = arr.add<JsonObject>();
    o["addr"] = snap[i].addr;
    o["name"] = snap[i].name;
    o["rssi"] = snap[i].rssi;
  }
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleBleSelect(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonObject o = json.as<JsonObject>();
  String addr = o["addr"] | "";
  addr.trim();
  // An empty address un-binds the controller (BLE wake off). Without this there
  // is no way to clear a binding once made — the only escape was an NVS wipe.
  if (addr.isEmpty()) {
    setWakeAddr("");
    Serial.println("[PORTAL] controller un-bound (BLE wake disabled)");
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (addr.length() != 17) {  // "aa:bb:cc:dd:ee:ff"
    sendJsonError(req, 400, "invalid address");
    return;
  }
  addr.toLowerCase();
  setWakeAddr(addr);
  Serial.printf("[PORTAL] bound controller %s\n", addr.c_str());
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleFinish(AsyncWebServerRequest *req) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  if (!isConfigured()) { sendJsonError(req, 400, "not fully configured"); return; }
  setForceSetup(false);
  req->send(200, "application/json", "{\"ok\":true}");
  Serial.println("[PORTAL] setup complete, restarting into normal mode");
  g_restartAt = millis() + 800;
}

// --- Lifecycle ---

void portalBegin() {
  Serial.println("=== BC250 PSU controller : SETUP MODE ===");

  // Keep the PSU off and the button readable while configuring.
  pinMode(PS_ON_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);
  pinMode(BUTTON_GND, OUTPUT);
  digitalWrite(BUTTON_GND, LOW);
  pinMode(BUTTON_SENSE, INPUT_PULLUP);

  if (!SPIFFS.begin(true)) {
    Serial.println("[PORTAL] WARNING: SPIFFS mount failed");
  }

  // Scan while we're still in STA mode and nothing is associated. Doing this
  // after softAP() would knock any connected phone off the AP for seconds.
  scanWifiNetworks();

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID);  // open network
  // These C3 mini boards have an RF/power design flaw (arduino-esp32 #6551):
  // at full TX power the SoftAP emits no usable beacons. Lowering TX power makes
  // it work. Must be set AFTER softAP().
  WiFi.setTxPower(AP_TX_POWER);
  Serial.printf("[PORTAL] softAP ret=%d ssid='%s' ip=%s txpwr=%d\n", ok, AP_SSID,
                WiFi.softAPIP().toString().c_str(), WiFi.getTxPower());

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  // BLE active scan so the picker shows device names + live RSSI. Keep the duty
  // cycle LOW (window << interval): WiFi and BLE share the C3's single radio, so
  // a high-duty scan starves the SoftAP and its beacons never go out.
  NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(500);  // ms
  scan->setWindow(45);     // ms (~9% duty, leaves the radio free for WiFi)
  scan->start(0, false);

  // /api/status is deliberately unauthenticated (the UI needs it before login)
  // and already leaks wakeAddr. Everything below is authed — don't extend the
  // unauthenticated surface with network names or broker addresses.
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/ble/devices", HTTP_GET, handleBleDevices);
  server.on("/api/finish", HTTP_POST, handleFinish);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/password", handlePassword));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/login", handleLogin));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi", handleWifi));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/mqtt", handleMqtt));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/device", handleDevice));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/ble/select", handleBleSelect));

  server.on("/", HTTP_GET, serveIndex);
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->url().startsWith("/api/")) {
      sendJsonError(req, 404, "not found");
    } else {
      serveIndex(req);  // captive-portal catch-all
    }
  });

  server.begin();
  Serial.println("[PORTAL] web server up. Connect to the AP and open http://192.168.4.1");
}

void portalLoop() {
  dnsServer.processNextRequest();

  // Button held in setup mode = escape back to normal mode (only useful once
  // configured; an unconfigured device just re-enters setup).
  static unsigned long pressStart = 0;
  static bool wasDown = false;
  bool down = (digitalRead(BUTTON_SENSE) == LOW);
  if (down && !wasDown) {
    pressStart = millis();
  } else if (down && (millis() - pressStart) >= SETUP_HOLD_MS) {
    Serial.println("[PORTAL] button held, leaving setup mode");
    setForceSetup(false);
    delay(50);
    ESP.restart();
  }
  wasDown = down;

  if (g_restartAt && (int32_t)(millis() - g_restartAt) >= 0) {
    ESP.restart();
  }
}
