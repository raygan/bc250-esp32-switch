# Measurements & verification log

Recorded 2026-08-14 on a **bare ESP32-C3 devkit**: no button on GPIO5/6, no BC250,
no PSU, no bound controller. TPMS1 (GPIO3) is unconnected and floating, which
matters for reading the sense figures below.

## Build size

| Build | RAM | Flash |
|---|---|---|
| Phase 0 (pre-change) | 45,524 B (13.9%) | 1,265,820 B (46.0%) |
| Phase 1 (instrumentation) | 45,612 B (13.9%) | 1,267,132 B (46.0%) |
| Phase 3 (WiFi STA) | 46,700 B (14.3%) | 1,277,120 B (46.4%) |
| Phase 4 (MQTT + entities) | 49,028 B (15.0%) | 1,311,388 B (47.6%) |

Total cost of the feature: **+3.5 KB RAM, +45.6 KB flash**, against 1.42 MiB free.

## Runtime, idle in STATE_OFF

| | Phase 0 baseline (no WiFi) | Phase 4 (WiFi + MQTT up) |
|---|---|---|
| `loopMax` | 700–780 µs | 1,300–1,800 µs |
| `heap` | 233,364 B | ~162,200 B |

`loopMax` roughly doubles once the WiFi stack is running, and the heap cost of
WiFi STA + MQTT is **~71 KB** — above the 40–50 KB the plan estimated, but with
162 KB still free it is not a concern. Both figures were flat over multi-minute
captures, i.e. no leak.

Blocking-connect cost, measured with the broker rejecting the login: `loopMax`
spikes of **44–108 ms**, once per `MQTT_RETRY_MS`. Far under the 2 s bound set by
`MQTT_CONNECT_TIMEOUT_MS`, because an actively-refused connection fails fast; a
silently-dropped one is what would approach the bound.

## Verified on hardware

- **WiFi STA at 19.5 dBm** — associates, `txpwr=78`, RSSI −44 to −70. No brownout:
  reset reason stayed `USB` across every reboot. **No TX-power reduction needed.**
- **Association stability** — `connected #1` with zero further transitions across
  several 45 s captures.
- **DNS avoidance** — an IP-literal broker is detected without a lookup.
- **Reconnect gating** — attempts land exactly `MQTT_RETRY_MS` apart.
- **Auth failure diagnosis** — a wrong login reports
  `state=5 UNAUTHORIZED (broker refused login)` rather than a bare "disconnected".
- **MQTT discovery** — all 10 entities announce exactly once; payloads 206–259 B.
- **Publish rates at rest** — one message per entity, diagnostics every 30 s.
- **HA switch ON** → `powering on`, `OFF -> BOOTING`; boot watchdog fired at
  **+9.96 s** and returned to OFF, and the HA switch followed itself back off.
  This exercises the command indirection, confirms the watchdog arms from an
  HA-triggered power-on exactly as it does from the button, and demonstrates that
  the single-clock discipline holds (no unsigned underflow in the timeout).
- **HA switch OFF** — interrupts BOOTING cleanly.
- **Portal** — WiFi scan runs before the SoftAP exists (`12 found, 5 cached`);
  `softAP ret=1 txpwr=34`; a full pass through the screens persisted WiFi, MQTT
  and device name to NVS, and a later firmware flash picked them up unchanged.

## Discovery payload sizes vs. the MQTT buffer

Measured discovery payloads are **206–259 B**. PubSubClient's default buffer is
**256 B**, so the largest entities (`rssi` at 259 B, `controller` at 254 B) would
have been silently dropped at the default. `mqtt.setBufferSize(512)` in
`ha.cpp` is therefore required, not precautionary. Headroom is comfortable, but
adding entities or a longer device name pushes these up — re-check if entities
start disappearing from HA.

## Not yet verified — needs hardware not currently attached

- Button: tap-on, 5 s hold-off, 8 s hold-to-portal, and escape-from-setup.
- BLE: controller bind, wake-on-presence, the post-power-off cooldown.
- Real power switching and TPMS1-driven shutdown detection.
- **Phase 3 step 3, the coexistence A/B** — needs a bound controller. Cold wake
  timings (×3) must be captured at SOLO params *before* comparing against COEX,
  or the comparison is unfalsifiable.
- **Phase 3 step 5**, the NimBLE leak check — needs a bound controller so a scan
  is actually running. The `setMaxResults(0)` fix is verified by code inspection
  only (passive scan reaches `m_callbackSent >= 2`, so the erase path at
  `NimBLEScan.cpp:343` does fire).
- LWT / `unavailable` on power loss.

## Pre-existing quirks (not introduced by this work)

- `Preferences.cpp begin(): nvs_open failed: NOT_FOUND` on a fresh device, and
  `getString(): nvs_get_str len fail: wakeAddr NOT_FOUND` when no controller is
  bound. Harmless; the defaults in `loadConfig()` cover both.
- `esp32-hal-adc.c __analogChannelConfig(): Pin is not configured as analog
  channel` — `analogSetPinAttenuation(BOARD_SENSE)` in `normalBegin()` runs before
  the pin's first `analogReadMilliVolts()`. Readings work because ADC_11db is the
  core 3.x default, so the intended attenuation applies regardless.

## Floating-pin artefact worth knowing

With TPMS1 unconnected, the sense reading oscillates between ~56 mV and ~205 mV
once the radio is transmitting. That is ~10× below `SENSE_HIGH_MV` (2000 mV), so
it never threatens the hysteresis — but its amplitude exceeds
`HA_SENSE_DELTA_MV` (100 mV), which defeated delta-only publish throttling and
produced ~1 msg/s. That is what motivated `HA_SENSE_MIN_INTERVAL_MS`; a delta
threshold is not a rate limit. Re-measure once TPMS1 is actually wired: a driven
line should be quiet enough to fall back to the 30 s keepalive.
