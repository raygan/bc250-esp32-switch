#pragma once

#include <Arduino.h>

// Home Assistant integration: WiFi station + MQTT with HA auto-discovery.
//
// Everything here is OPTIONAL. With no SSID configured, haBegin() turns the
// radio off and every other entry point is a no-op, so the device behaves
// exactly as it did before this module existed. The button is the primary
// control and must work standalone.
//
// WiFi and MQTT live in one module on purpose: MQTT's begin() is deferred until
// WiFi associates, so splitting them would only create a bidirectional
// dependency between two files.
//
// main.cpp must never include <WiFi.h> or <ArduinoHA.h>. The entire interface is
// a state snapshot in, a command out.
//
// THE SINGLE-CLOCK DISCIPLINE (restated from main.cpp so the constraint travels
// with this code): every timer in this module must use the `now` passed into
// haLoop(), never a fresh millis(). main.cpp caches one millis() per iteration
// because the BOOTING timeout compares against it; a fresh millis() taken deeper
// in the same iteration can land a millisecond later, and since the comparison
// is unsigned, a timestamp greater than `now` makes (now - stamp) underflow to a
// huge value and fire the timeout immediately. One clock per iteration keeps the
// arithmetic monotonic.

// Snapshot of the state machine, filled once per loop from the values the state
// machine just acted on.
struct HaState {
  const char *stateName;  // "OFF" / "BOOTING" / "ON"
  bool        powerOn;    // state != STATE_OFF (BOOTING reports ON)
  bool        boardUp;    // debounced TPMS1
  bool        blePresent;
  bool        bleActive;  // a controller is bound and the scan is running
  uint32_t    senseMv;
};

// Commands arriving from Home Assistant. MQTT callbacks fire inside mqtt.loop()
// and have no access to the loop's `now`, so they only set a pending command;
// normalLoop() drains it at the top of the next iteration and runs it through
// the same powerOn()/powerOff() paths the button uses. That keeps the boot
// watchdog and the BLE cooldown arming identically no matter what triggered the
// action — no parallel code path, and nothing that can bypass them.
enum HaCommand {
  HA_CMD_NONE,
  HA_CMD_POWER_ON,
  HA_CMD_POWER_OFF,
  HA_CMD_ENTER_SETUP,
  HA_CMD_RESTART,
};

// Start the radio. Safe to call when nothing is configured.
void haBegin();

// Service WiFi and MQTT. Call LAST in normalLoop(), so HA observes the state
// this iteration produced and the switch never bounces back.
//   now           - the loop's cached millis()
//   s             - state snapshot for this iteration
//   allowReconnect- false suppresses blocking connection attempts. Pass false
//                   during STATE_BOOTING: a ~2s TCP stall inside the 10s boot
//                   watchdog is a real spurious-timeout risk.
void haLoop(unsigned long now, const HaState &s, bool allowReconnect);

// Returns the pending command and clears it. Single slot, last-wins.
HaCommand haTakeCommand();

bool   haWifiConnected();
bool   haMqttConnected();
int8_t haRssi();
