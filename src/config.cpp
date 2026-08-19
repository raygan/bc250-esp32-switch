#include "config.h"
#include <Preferences.h>

const char *const DEFAULT_DEVICE_NAME = "BC250";

// Broker port used when nothing is stored (or a stored value is nonsense).
static const uint16_t DEFAULT_MQTT_PORT = 1883;

Config config{"", "", false, "", "", "", DEFAULT_MQTT_PORT, "", "", ""};

static Preferences prefs;
static const char *NS = "bc250";

// NVS keys are capped at 15 characters, so the stored names don't always match
// the struct field names ("devName" vs deviceName). Keep them here, once.
static const char *K_WAKE_ADDR  = "wakeAddr";
static const char *K_PASS_HASH  = "passHash";
static const char *K_FORCE_SETUP = "forceSetup";
static const char *K_WIFI_SSID  = "wifiSsid";
static const char *K_WIFI_PASS  = "wifiPass";
static const char *K_MQTT_HOST  = "mqttHost";
static const char *K_MQTT_PORT  = "mqttPort";
static const char *K_MQTT_USER  = "mqttUser";
static const char *K_MQTT_PASS  = "mqttPass";
static const char *K_DEV_NAME   = "devName";

void loadConfig() {
  prefs.begin(NS, true);  // read-only
  // Every read carries a default, so a device provisioned before these keys
  // existed upgrades in place — no NVS wipe, no reprovisioning.
  config.wakeAddr   = prefs.getString(K_WAKE_ADDR, "");
  config.passHash   = prefs.getString(K_PASS_HASH, "");
  config.forceSetup = prefs.getBool(K_FORCE_SETUP, false);
  config.wifiSsid   = prefs.getString(K_WIFI_SSID, "");
  config.wifiPass   = prefs.getString(K_WIFI_PASS, "");
  config.mqttHost   = prefs.getString(K_MQTT_HOST, "");
  config.mqttPort   = prefs.getUShort(K_MQTT_PORT, DEFAULT_MQTT_PORT);
  config.mqttUser   = prefs.getString(K_MQTT_USER, "");
  config.mqttPass   = prefs.getString(K_MQTT_PASS, "");
  config.deviceName = prefs.getString(K_DEV_NAME, "");
  prefs.end();
}

static void putString(const char *key, const String &val) {
  prefs.begin(NS, false);
  prefs.putString(key, val);
  prefs.end();
}

void setWakeAddr(const String &addr) {
  config.wakeAddr = addr;
  putString(K_WAKE_ADDR, addr);
}

void setPassHash(const String &hash) {
  config.passHash = hash;
  putString(K_PASS_HASH, hash);
}

void setForceSetup(bool force) {
  config.forceSetup = force;
  prefs.begin(NS, false);
  prefs.putBool(K_FORCE_SETUP, force);
  prefs.end();
}

void setWifiCreds(const String &ssid, const String &pass) {
  config.wifiSsid = ssid;
  config.wifiPass = pass;
  prefs.begin(NS, false);
  prefs.putString(K_WIFI_SSID, ssid);
  prefs.putString(K_WIFI_PASS, pass);
  prefs.end();
}

void setMqttSettings(const String &host, uint16_t port, const String &user,
                     const String &pass) {
  config.mqttHost = host;
  config.mqttPort = port ? port : DEFAULT_MQTT_PORT;
  config.mqttUser = user;
  config.mqttPass = pass;
  prefs.begin(NS, false);
  prefs.putString(K_MQTT_HOST, config.mqttHost);
  prefs.putUShort(K_MQTT_PORT, config.mqttPort);
  prefs.putString(K_MQTT_USER, config.mqttUser);
  prefs.putString(K_MQTT_PASS, config.mqttPass);
  prefs.end();
}

void setDeviceName(const String &name) {
  config.deviceName = name;
  putString(K_DEV_NAME, name);
}

bool isConfigured() {
  return config.passHash.length() > 0;
}

bool wifiConfigured() {
  return config.wifiSsid.length() > 0;
}

bool mqttConfigured() {
  return wifiConfigured() && config.mqttHost.length() > 0;
}

uint16_t mqttPort() {
  return config.mqttPort ? config.mqttPort : DEFAULT_MQTT_PORT;
}

const char *haDeviceName() {
  return config.deviceName.length() ? config.deviceName.c_str()
                                    : DEFAULT_DEVICE_NAME;
}
