#pragma once

#include <Arduino.h>

// Persistent configuration, stored in NVS via the Preferences library.
//
//   wakeAddr   - bound controller BLE MAC, lower-case colon form ("aa:bb:..").
//   passHash   - SHA-256 hex of the portal password ("" => not set yet).
//   forceSetup - request that the next boot enters the WiFi setup portal.
//   wifiSsid   - station-mode SSID ("" => WiFi disabled, AP-only at setup).
//   wifiPass   - station-mode passphrase ("" => open network).
//   mqttHost   - broker hostname or IP ("" => MQTT disabled).
//   mqttPort   - broker port (default 1883).
//   mqttUser   - broker username ("" => anonymous).
//   mqttPass   - broker password.
//   deviceName - friendly name shown in Home Assistant.
//
// Every field is optional. A device with no WiFi and no MQTT configured must
// behave exactly as it did before those fields existed: the button is the
// primary control and has to work standalone.
struct Config {
  String   wakeAddr;
  String   passHash;
  bool     forceSetup;
  String   wifiSsid;
  String   wifiPass;
  String   mqttHost;
  uint16_t mqttPort;
  String   mqttUser;
  String   mqttPass;
  String   deviceName;
};

extern Config config;

// Default friendly name when the user hasn't set one.
extern const char *const DEFAULT_DEVICE_NAME;

void loadConfig();

void setWakeAddr(const String &addr);
void setPassHash(const String &hash);
void setForceSetup(bool force);

// Grouped setters. Each writes all of its keys in a single NVS transaction —
// the per-key putString() below opens and closes the namespace every call, so
// a four-field form submit would otherwise cost four transactions.
void setWifiCreds(const String &ssid, const String &pass);
void setMqttSettings(const String &host, uint16_t port, const String &user,
                     const String &pass);
void setDeviceName(const String &name);

// Provisioned enough to leave the setup portal: a password has been set.
// Deliberately password-only — WiFi, MQTT and the bound controller are each
// independently skippable, and nothing in setup() consults this.
bool isConfigured();

bool wifiConfigured();  // an SSID is set
bool mqttConfigured();  // wifiConfigured() and a broker host is set

uint16_t    mqttPort();        // config.mqttPort, or 1883 if unset/invalid
const char *haDeviceName();    // config.deviceName, or DEFAULT_DEVICE_NAME
