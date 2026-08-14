# BC250 Switch → Home Assistant (WiFi STA + MQTT)

## Context

The firmware today is a self-contained ATX power controller: it drives `PS_ON#`, senses board power via TPMS1, and is driven by a physical button. WiFi exists only as a SoftAP during first-time setup, and the device has no presence on the network in normal operation.

The goal is to expose the BC250 in Home Assistant as an **on/off switch with power status**, so the machine can be controlled and monitored from HA rather than only from the physical button. That requires the board to join an existing WiFi network in normal mode — something it has never done — and to speak MQTT with HA auto-discovery.

Decisions already settled (do not revisit):
- **Extend this project in C++.** Not an ESPHome rebase. The state machine carries real, hard-won fixes (ADC oversampling against spike-induced shutdown stalls, the single-`millis()`-per-iteration discipline against unsigned underflow, the #6551 TX-power workaround). Re-expressing those as YAML lambdas risks silently reintroducing bugs already paid for.
- **MQTT with HA auto-discovery**, via `dawidchyrzynski/home-assistant-integration` (ArduinoHA). A broker is already running.
- **Keep the single-OTA partition layout.** No repartitioning. Flash headroom is ample: 1.42 MiB free of app0's 2.75 MiB.

Everything new must be **optional**. A device with no WiFi configured must behave byte-for-byte as it does today — the button is the primary control and has to work standalone.

---

## Architecture

One new module, `include/ha.h` + `src/ha.cpp`, owning both the WiFi station and MQTT lifecycles. They aren't independent (MQTT's `begin()` is deferred until WiFi associates), so splitting them would only create a bidirectional dependency.

`main.cpp` must never include `WiFi.h` or `ArduinoHA.h`. The entire interface is a state snapshot in, a command out:

```cpp
struct HaState {            // filled once per loop from values the state machine just acted on
  const char *stateName;    // "OFF" / "BOOTING" / "ON"
  bool powerOn;             // state != STATE_OFF (BOOTING reports ON)
  bool boardUp, blePresent, bleActive;
  uint32_t senseMv;
};

enum HaCommand { HA_CMD_NONE, HA_CMD_POWER_ON, HA_CMD_POWER_OFF,
                 HA_CMD_ENTER_SETUP, HA_CMD_RESTART };

void      haBegin();
void      haLoop(unsigned long now, const HaState &s, bool allowReconnect);
HaCommand haTakeCommand();   // returns and clears; last-wins single slot
bool      haWifiConnected();
bool      haMqttConnected();
int8_t    haRssi();
```

**The command indirection is the load-bearing design choice.** MQTT callbacks fire inside `mqtt.loop()` and have no access to the loop's `now`. Setting a pending-command flag and draining it at the top of `normalLoop()` means every action still runs through the existing `powerOn(reason, now)` / `powerOff(reason, now)` with the loop's single clock. The boot watchdog and BLE cooldown then arm identically whether the trigger was a button, BLE, or HA — no parallel code path, no bypass.

`haLoop()` is called **last** in `normalLoop()`, so HA sees the state this iteration produced and the switch never bounces back.

---

## Phase 1 — Config plumbing + instrumentation

**`include/config.h`, `src/config.cpp`**

Add to `Config`: `wifiSsid`, `wifiPass`, `mqttHost`, `mqttPort` (`uint16_t`), `mqttUser`, `mqttPass`, `deviceName`. NVS namespace stays `"bc250"`; all reads keep defaults so **existing devices upgrade in place with no wipe**. Keys ≤15 chars (`devName`, not `deviceName`).

Grouped setters — `setWifiCreds(ssid, pass)`, `setMqttSettings(host, port, user, pass)`, `setDeviceName(name)` — each one NVS transaction. Add a multi-key helper alongside the existing `putString()` (`config.cpp:17`), which opens/closes per key and would otherwise cost four transactions per form submit.

**Relax `isConfigured()` to password-only.** It currently gates just two things: the `configured` field in `/api/status`, and the `/api/finish` guard (`portal.cpp:210`). Nothing in `setup()` consults it. This is what makes "skip WiFi / skip MQTT / skip controller" possible. Add `wifiConfigured()`, `mqttConfigured()`, `mqttPort()`, `haDeviceName()`.

**Instrumentation** (do this now — it makes every later phase falsifiable): add `esp_reset_reason()` logging in `setup()`, and `heap=` plus `loopMax=` (a `micros()` span over `normalLoop()`, reset each heartbeat) to the existing heartbeat line at `main.cpp:321-329`.

*Verify:* boots on a provisioned device with `wakeAddr` intact; tap-on / 5s-hold-off / 8s-hold-to-portal all unchanged; `loopMax` is a few hundred µs.

---

## Phase 2 — Portal extension

**`app/index.html`** — flow becomes `pass → wifi → mqtt → dev → done`, everything after `pass` skippable. Replace the hardcoded two-screen `show()` (`index.html:91-94`) with a list-driven one over a `SCREENS` array. Each screen gets a `.row` footer with a `button.secondary` Back/Skip — both classes already exist (`index.html:25,36`) and `.secondary` is currently unused.

**Fix the XSS sink while you're in there.** `index.html:153-156` interpolates `d.name` raw into `innerHTML`. Today that needs a hostile BLE name; after this change the same renderer displays **scanned SSIDs**, which anyone in radio range can choose. Rebuild rows with `createElement` + `textContent`, and switch `refreshSel()` (`index.html:173-176`) from reading `querySelector("small").textContent` to `el.dataset.addr`.

**`src/portal.cpp`** — restructure `portalBegin()` to scan WiFi **before** the AP exists, and cache it:

1. pins, `SPIFFS.begin()`
2. `WiFi.mode(WIFI_STA)`, `WiFi.scanNetworks()`, copy into a static array (dedupe by SSID keeping best RSSI), `WiFi.scanDelete()`
3. `WiFi.mode(WIFI_AP)`, `softAP()`, **then** `setTxPower(AP_TX_POWER)` — this ordering (`portal.cpp:233-237`) is the #6551 workaround and is non-negotiable
4. DNS, BLE scan, routes, `server.begin()`

Costs ~3s at startup, before anyone can have connected. A live `scanNetworks()` in AP mode takes the AP off-channel for seconds and drops the phone's association — avoid it. Implement "Rescan" as re-enter-setup-and-reboot, not a live scan. Manual SSID entry stays available for hidden networks.

New endpoints (all **authed** — note `/api/status` is deliberately unauthenticated and already leaks `wakeAddr`; don't extend that):
- `GET /api/settings` → current values, with `wifiPassSet`/`mqttPassSet` booleans. **Never return stored passwords.**
- `GET /api/wifi/scan` → cached list
- `POST /api/wifi` `{ssid, password?}` — empty ssid clears/disables. **Key absent = keep stored password; key present-and-empty = clear it.** Comment that sentinel; it's the kind of thing that gets "simplified" into a bug.
- `POST /api/mqtt` `{host, port, user, password?}` — same sentinel rule; port 1..65535, default 1883
- `POST /api/ble/select` — allow empty `addr` to **un-bind**. Currently `portal.cpp:198` hard-rejects anything not 17 chars, so there's no way to clear a controller.

*Verify:* `pio run -t upload && pio run -t uploadfs` — **the filesystem image must be reflashed**, or you get a stale UI. All screens navigate forward/back; Skip works; finish succeeds with only a password set; passwords absent from every response (check devtools, not the UI); an SSID containing `<b>x</b>` renders literally.

---

## Phase 3 — WiFi STA only (the RF checkpoint)

Do **not** combine with MQTT. This phase exists to isolate the single highest risk.

**`src/ha.cpp`** WiFi section. `haBegin()`: if `!wifiConfigured()` → `WiFi.mode(WIFI_OFF)`, log, return (this is the graceful-degradation base case). Else `WIFI_STA`, `setSleep(true)`, `setTxPower(STA_TX_POWER)`, `setAutoReconnect(true)`, `persistent(false)`, `WiFi.begin()` (returns immediately).

`WiFi.setSleep(true)` is a **coexistence requirement**, not an optimisation — with modem sleep off, the WiFi MAC holds the radio continuously and starves the BLE scan.

Poll `WiFi.status()` from `haLoop()` using the passed `now`. Do not drive control flow from `WiFi.onEvent()` — that runs on the system event task and breaks the single-clock discipline. An event handler is fine purely to log disconnect reason codes.

**Radio coexistence.** Normal mode currently runs a 50%-duty continuous BLE scan (`main.cpp:150-151`). Parameterise `startBleScan(bool coexWithWifi)` and put both pairs in `board.h`:

```cpp
const uint16_t BLE_SCAN_INTERVAL_SOLO_MS = 160;  // ~50% duty, no WiFi
const uint16_t BLE_SCAN_WINDOW_SOLO_MS   = 80;
const uint16_t BLE_SCAN_INTERVAL_COEX_MS = 320;  // ~15% duty, WiFi associated
const uint16_t BLE_SCAN_WINDOW_COEX_MS   = 48;
const bool     BLE_SCAN_ONLY_WHEN_OFF    = false; // escape hatch; wake only acts in STATE_OFF
```

Called as `startBleScan(wifiConfigured())`, so **no-WiFi devices keep today's behaviour exactly**. 15% duty against a controller advertising every 30–100ms still gives many detections inside the 4s presence window — the cost is wake *latency*, not the feature.

Add `STA_TX_POWER` separately from `AP_TX_POWER` (they can never be active at once — different boots). Start at `WIFI_POWER_19_5dBm`; this board has a known RF/power flaw and `CONFIG_ESP_BROWNOUT_DET_LVL=7`, so be ready to drop it.

**Also fix the NimBLE leak here** (verified in `.pio/libdeps/.../NimBLEScan.cpp`): `m_maxResults` defaults to `0xFF`, the cap check at line 270 requires `>0 && <0xFF` so **no cap applies**, every new advertiser is `new`'d onto the vector (line 280), the erase at line 343 requires `m_maxResults == 0`, and `clearResults()` (line 366) only runs on `DISC_COMPLETE` — which never fires for `start(0, false)`. With BLE privacy addresses rotating every ~15 min, this grows unbounded for as long as the machine sits OFF. Add `scan->setMaxResults(0)` in `startBleScan()`; line 327 confirms a passive scan reaches `m_callbackSent >= 2` on first report, so the erase path does fire. Leave the portal's active scan alone — it accumulates deliberately and is short-lived.

*Verify, in order:*
1. Empty SSID → BLE at solo params, wake latency and `loopMax` match Phase 0 baseline. **Regression gate.**
2. Valid SSID at 19.5dBm → associates; watch 10 min for brownout resets and disconnect churn. If unstable, step to `WIFI_POWER_8_5dBm`.
3. **A/B the coexistence.** 15 min at COEX params: log RSSI, count `WL_CONNECTED` transitions, time controller-wake ×3. Repeat with SOLO params and WiFi up. Pick from the data. If COEX still churns, set `BLE_SCAN_ONLY_WHEN_OFF = true`.
4. Wrong password → backoff loop, `loopMax` stays small, **button works throughout**.
5. Leave OFF and idle several hours → `heap=` flat, confirming the NimBLE fix.

Record a **Phase 0 baseline** (RAM/flash line, 60s of heartbeat, three cold wake timings) before touching anything, or step 3 is unfalsifiable.

---

## Phase 4 — MQTT + entities

**Blocking is the hazard.** `HAMqtt::loop()` → `PubSubClient::connect()` → blocking DNS + TCP. Four mitigations, all needed:

1. **Pre-resolve the hostname once** on WiFi association via `WiFi.hostByName()`, cache the `IPAddress`, use the `HAMqtt::begin(IPAddress, ...)` overload. Removes DNS from the reconnect path.
2. `wifiClient.setConnectionTimeout(2000)` — bounds the TCP stall at ~2s instead of PubSubClient's 15s default.
3. **Gate reconnects yourself.** ArduinoHA only connects from inside `loop()`, so *not calling* `mqtt.loop()` suppresses the attempt. Call it every iteration while connected; otherwise only once per `MQTT_RETRY_MS` (20s).
4. `allowReconnect = (state != STATE_BOOTING)` — a 2s stall inside the 10s boot watchdog is a real spurious-timeout risk.

**Accepted residual:** with the broker down, ~2s in every 20s isn't sampling GPIO5, so a ~150ms tap can be missed while the broker is down. Not fixable without an async MQTT client, which ArduinoHA's `Client&` interface precludes. Levers if it annoys: 1s timeout, 60s retry.

**Static init order will bite you if ignored.** Every `HABaseDeviceType` registers with `HAMqtt::instance()` from its constructor. Declare `WiFiClient` → `HADevice` → `HAMqtt` → entities, in that order, in one translation unit. Pass `HA_MAX_DEVICE_TYPES = 12` explicitly — the default is **6** and excess entities are dropped silently.

Device identity from `esp_read_mac(mac, ESP_MAC_WIFI_STA)` (works before WiFi starts, unlike `WiFi.macAddress()`). Call `device.enableSharedAvailability()` and `device.enableLastWill()` **before** `mqtt.begin()` so HA shows `unavailable` on power loss.

| Entity | Type | Source |
|---|---|---|
| Power | `HASwitch("power")` | `state != STATE_OFF` |
| Board power | `HABinarySensor("board")`, class `power` | `boardSenseStable` |
| State | `HASensor("state")` | `"OFF"/"BOOTING"/"ON"` |
| Controller present | `HABinarySensor("controller")`, class `connectivity` | `blePresent`, only when `bleActive` |
| Sense voltage | `HASensorNumber("sense_mv")`, unit `mV` | `senseMv` — **no device_class** (HA's `voltage` expects volts) |
| WiFi signal | `HASensorNumber("rssi")`, class `signal_strength` | primary evidence for coexistence health |
| Free heap / Uptime | `HASensorNumber` | diagnostics |
| Restart / Setup mode | `HAButton` | → `HA_CMD_RESTART` / `HA_CMD_ENTER_SETUP` |

**Publish policy** — `senseMv` changes every loop, so throttle or you'll emit ~1000 msg/s. Binary/switch setters already no-op on unchanged values (verify with `mosquitto_sub`, don't trust it). `senseMv` only on ≥100mV delta or every 30s; diagnostics every 30s.

**Never call `setState()` optimistically from the command callback.** State always comes from the snapshot. A consequence worth documenting in the README: if HA turns it on and the board never asserts TPMS1, the 10s watchdog powers off and **the HA switch flips itself back off** — that's correct (the switch reflects reality, not intent), but it looks like a bug if unexplained.

**`platformio.ini`:**
```ini
	dawidchyrzynski/home-assistant-integration
	knolleary/PubSubClient@^2.8
```
PubSubClient is a transitive dep but declare it explicitly — pinning the MQTT client is worth one line, and PlatformIO has known transitive-resolution issues. If HA shows entities with missing attributes, add `-D MQTT_MAX_PACKET_SIZE=512` to `build_flags`.

*Verify:*
1. `mosquitto_sub -v -t 'homeassistant/#' -t 'aha/#'` — discovery appears once, state topics quiet at rest.
2. HA switch ON → `[ACT ] Home Assistant switch on -> powering on`, `OFF → BOOTING → ON`. OS shutdown on the board → HA switch follows off.
3. **Watchdog isolation:** point `mqttHost` at `192.0.2.1`, then button-boot. No spurious `boot timed out`; `loopMax` shows ~2s spikes every 20s while OFF but **none during BOOTING**.
4. **Broker down** with machine ON: button force-off still works.
5. **LWT:** pull power → all entities `unavailable` within keepalive.
6. Broker returns → reconnects and republishes without a reboot.

---

## Risks

- **Radio contention (highest).** Single C3 radio, 50%-duty BLE today. Failure modes are soft and easy to miss: slower wake, WiFi latency, periodic reassociation. Cannot be validated from code — Phase 3 step 3 is an explicit A/B measurement for exactly this reason. Software coexistence (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`) is compiled in, so it will *work*; the question is degradation.
- **Brownout.** `CONFIG_ESP_BROWNOUT_DET_LVL=7` + known board RF flaw + first-ever full-power STA transmit. `esp_reset_reason()` logging from Phase 1 makes this diagnosable. Hardware fallback: 470µF bulk cap on the 5V feed.
- **RAM.** WiFi STA wants ~40–50KB heap on top of NimBLE's ~35–40KB. 320KB total should absorb it. `AsyncWebServer`/SPIFFS are **not** resident in normal mode — preserve that by never starting a web server outside setup mode.
- **The single-clock discipline.** Every new timer must use the `now` passed into `haLoop()`, never a fresh `millis()`. Restate the rationale (`main.cpp:104-109`) in `ha.h` so the constraint travels with the new code.
- **Portal regression surface.** Phase 2 touches the only provisioning path. Not brickable — an 8s hold in setup mode escapes to normal (`portal.cpp:283-290`) — but run Phase 2's checklist in full.

## Open items to confirm at implementation time

- Exact current ArduinoHA registry version (unpinned above; check on install).
- Whether ArduinoHA 2.x exposes `entity_category` for the diagnostic sensors. If not, they land as normal entities and can be recategorised per-entity in HA's UI. Not worth blocking on.
