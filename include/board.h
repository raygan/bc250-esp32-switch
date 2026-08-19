#pragma once

//*******  Pin definitions  ***************
//
// BC250 PSU controller wiring:
//
//   ESP32-C3                      External
//   --------                      --------
//   GPIO5  (BUTTON_SENSE)  <-----> momentary switch terminal A
//   GPIO6  (BUTTON_GND)    <-----> momentary switch terminal B
//   GPIO4  (PS_ON_PIN)     <-----> ATX PS_ON# (green wire, active LOW)
//   GPIO3  (BOARD_SENSE)   <-----> BC250 TPMS1 pin 9 (3.3V = board on)
//
// The switch bridges GPIO5 and GPIO6. GPIO6 is driven LOW to act as a local
// ground, and GPIO5 is read with an internal pull-up: pressed reads LOW.
const int BUTTON_SENSE = 5;
const int BUTTON_GND   = 6;

// ATX PS_ON# is active LOW and idles at ~5V (pulled up inside the PSU).
// Driven as OPEN-DRAIN so we never push 3.3V against the PSU's 5V pull-up:
//   LOW  -> sink to GND -> PSU on
//   HIGH -> high-impedance -> PSU pull-up wins -> PSU off
const int PS_ON_PIN = 4;

// BC250 TPMS1 (pin 9): reads ~3.3V while the board is powered/booted, 0 when
// off. In practice it's a higher-impedance source that settles near ~2.9V and
// hovers close to the ESP's digital logic threshold, so digitalRead() flickers.
// We read it as an ADC voltage with hysteresis instead (see thresholds below).
const int BOARD_SENSE = 3;

// Hysteresis thresholds for the analog board-sense reading. The gap between
// them keeps a noisy signal sitting near the threshold from chattering:
//   reading rises above HIGH -> treat as "board up"
//   reading falls below LOW  -> treat as "board down"
//   in between               -> hold previous state
const int SENSE_HIGH_MV = 2000;
const int SENSE_LOW_MV  = 800;

// TPMS1 is high-impedance and the ESP32 single-shot ADC is noisy, so an isolated
// analogReadMilliVolts() can spike hundreds of mV above the true level. Once the
// board powers off the line floats near 0V but still throws the occasional spike
// past SENSE_HIGH_MV. A single such spike flips the hysteresis HIGH for one loop,
// which restarts the BOARD_OFF_DEBOUNCE_MS countdown -> shutdown detection stalls
// for an unbounded, random time. Averaging this many samples per reading is a
// low-pass that keeps a lone spike from ever crossing a threshold.
const int SENSE_OVERSAMPLE = 16;

//*******  Logic levels  ***************

const int PS_ON_ASSERT  = LOW;   // PSU on
const int PS_ON_RELEASE = HIGH;  // PSU off (open-drain -> high-Z)

//*******  Timing (milliseconds)  ***************

// Switch debounce window.
const unsigned long DEBOUNCE_MS = 30;

// Hold the button this long while the board is ON to force it off.
const unsigned long LONG_PRESS_MS = 5000;

// Hold the button this long while OFF to enter WiFi setup mode (reconfigure the
// bound controller / password). Longer than LONG_PRESS_MS and only armed for
// presses that begin while OFF, so it never collides with force-off.
const unsigned long SETUP_HOLD_MS = 8000;

// TPMS1 must stay LOW continuously for this long before we treat the board as
// having shut itself down. Filters out brief dips/transients during boot/reset.
const unsigned long BOARD_OFF_DEBOUNCE_MS = 1500;

// How long to wait for TPMS1 to go HIGH after asserting PS_ON#. If the board
// hasn't signalled UP by then we assume the boot failed, release the PSU and
// return to idle (OFF).
const unsigned long BOOT_TIMEOUT_MS = 10000;

// Periodic heartbeat log interval.
const unsigned long HEARTBEAT_MS = 1000;

//*******  WiFi setup portal  ***************

// SoftAP name shown when the device is in setup mode (open network).
const char *const AP_SSID = "BC250 Switch Setup";

// WiFi TX power for the SoftAP. These ESP32-C3 mini boards have an RF/power
// design flaw (arduino-esp32 #6551): at full power the AP emits no usable
// beacons, so the portal is invisible. A low value fixes it. WIFI_POWER_8_5dBm
// is confirmed working on this board.
#define AP_TX_POWER WIFI_POWER_8_5dBm

//*******  WiFi station (Home Assistant)  ***************

// TX power for station mode. Kept separate from AP_TX_POWER because the two are
// never active at once (the SoftAP only exists in setup mode, station mode only
// in normal mode), so they can be tuned independently.
//
// This was WIFI_POWER_19_5dBm, on the strength of one devkit that associated
// fine at full power. The wired board does not: at 19.5 dBm it never completes
// association at all, emitting `disconnected, reason=2` (AUTH_EXPIRE) forever.
// The same RF design flaw behind AP_TX_POWER (arduino-esp32 #6551) distorts the
// transmit signal badly enough that the AP never hears a clean auth response, so
// *lowering* the power is what fixes connectivity. At 8.5 dBm the same board
// associates on the first attempt at rssi -71.
//
// Note the symptom is NOT the brownout this comment used to predict: there are
// no resets at all, `esp_reset_reason()` stays clean, and the loop keeps running
// throughout. Do not go looking for BROWNOUT/PWR_GLITCH as the tell — a silent
// failure to associate at full power is the tell. Severity varies between
// physical units, so a board that works at 19.5 dBm proves nothing about the
// next one.
#define STA_TX_POWER WIFI_POWER_8_5dBm

// How often to retry association while disconnected. WiFi.setAutoReconnect()
// handles most cases; this is the backstop that re-issues WiFi.begin().
const unsigned long WIFI_RETRY_MS = 20000;

// How often to retry the MQTT broker while disconnected. ArduinoHA only attempts
// a connection from inside HAMqtt::loop(), so simply not calling loop() gates the
// attempt — see MQTT_CONNECT_TIMEOUT_MS.
const unsigned long MQTT_RETRY_MS = 20000;

// Bound on a single blocking TCP connect to the broker. PubSubClient defaults to
// 15s, which would stall the loop long enough to miss button presses and to trip
// the BOOT_TIMEOUT_MS watchdog. At 2s the worst case is ~2s of missed GPIO5
// sampling once per MQTT_RETRY_MS, and only while the broker is unreachable.
const uint16_t MQTT_CONNECT_TIMEOUT_MS = 2000;

// Diagnostic sensors (heap, uptime, RSSI) publish at most this often.
const unsigned long HA_DIAG_PUBLISH_MS = 30000;

// The sense voltage changes on nearly every loop, so publishing it unthrottled
// would emit ~1000 msg/s. Publish only on a meaningful change, or as a keepalive.
const uint32_t       HA_SENSE_DELTA_MV   = 100;
const unsigned long  HA_SENSE_PUBLISH_MS = 30000;

// Floor on the interval between sense publishes. A delta threshold alone is NOT
// a rate limit: a signal that oscillates with an amplitude wider than
// HA_SENSE_DELTA_MV crosses it on every flip and publishes at loop rate. That is
// not hypothetical — an unconnected TPMS1 pin swings ~56mV to ~205mV and pushed
// this to ~1 msg/s. This bounds the worst case regardless of the signal.
const unsigned long  HA_SENSE_MIN_INTERVAL_MS = 2000;

//*******  BLE wake  ***************

// BLE scan duty cycle. The C3 has a single radio shared between WiFi and BLE, so
// the scan that is affordable when BLE is the only user is not affordable once a
// WiFi station is associated: a 50%-duty scan starves the WiFi MAC and shows up
// as association churn and latency.
//
// SOLO is today's behaviour, used verbatim when no WiFi is configured, so a
// device with no network keeps exactly the wake latency it has always had.
// COEX drops to ~15% duty once WiFi is in play. A controller advertising every
// 30-100ms still lands many detections inside BLE_PRESENCE_TIMEOUT_MS at 15%,
// so the cost is wake *latency*, not the feature itself.
const uint16_t BLE_SCAN_INTERVAL_SOLO_MS = 160;  // ~50% duty, no WiFi
const uint16_t BLE_SCAN_WINDOW_SOLO_MS   = 80;
const uint16_t BLE_SCAN_INTERVAL_COEX_MS = 320;  // ~15% duty, WiFi associated
const uint16_t BLE_SCAN_WINDOW_COEX_MS   = 48;

// Escape hatch. The BLE wake only ever acts in STATE_OFF, so the scan is dead
// weight while the machine is ON. If coexistence still churns at COEX duty, set
// this true to stop the scan entirely outside STATE_OFF and hand the radio to
// WiFi whenever the machine is up.
const bool     BLE_SCAN_ONLY_WHEN_OFF    = false;

// The bound controller's BLE MAC is configured via the setup portal and stored
// in NVS (see config.h: config.wakeAddr). When the machine is OFF and that
// controller is advertising, we power on ("machine follows controller").

// The controller counts as "present" while it has been seen within this window.
// While OFF, presence => the machine powers on ("machine follows controller").
const unsigned long BLE_PRESENCE_TIMEOUT_MS = 4000;

// Guard window after any power-off during which BLE presence is ignored. This is
// your chance to also switch the controller off (it then goes absent and the
// machine stays down). If you leave the controller on, once this elapses the
// machine follows it back on. It also rides out the brief reconnect-advertising
// burst the controller emits when it loses its host at shutdown.
const unsigned long BLE_WAKE_COOLDOWN_MS = 15000;
