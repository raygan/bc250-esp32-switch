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
  reset reason stayed `USB` across every reboot. ~~No TX-power reduction
  needed.~~ **That conclusion was wrong — see "TX power" below.** It generalised
  from a single physical board; the wired board cannot associate at all at
  19.5 dBm.
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

## TX power — the 19.5 dBm default was wrong

On the wired board, station mode at `WIFI_POWER_19_5dBm` **never associates**. It
emits `[WIFI] disconnected, reason=2` (`AUTH_EXPIRE`) continuously — measured
unbroken for 200 s, with the 20 s backstop re-issuing `WiFi.begin()` to no
effect. Dropping to `WIFI_POWER_8_5dBm` fixes it outright:

```
[WIFI] connecting to '<ssid>' (txpwr=34)
[WIFI] got ip <addr>
[WIFI] connected #1 rssi=-71
```

First attempt, no retries. This is the same RF design flaw that already forced
`AP_TX_POWER` down (arduino-esp32 #6551): at full power the transmit signal is
distorted enough that the AP never hears a clean auth response, so *lowering*
power is what restores the link.

Two things worth remembering:

- **The predicted symptom was wrong.** `board.h` warned to step this down if
  `esp_reset_reason()` started reporting BROWNOUT or PWR_GLITCH. There were no
  resets at all — the reset reason stayed clean and the loop ran throughout. A
  silent failure to associate is the tell, not a reset.
- **Severity varies between physical units.** The devkit associates happily at
  19.5 dBm. That proved nothing about the next board, and treating one sample as
  a settled result cost most of an evening's debugging.

RSSI on the wired board runs −68 to −74 at 8.5 dBm. Workable but not generous;
if the link proves marginal, try intermediate powers (11/13 dBm) rather than
assuming higher is better.

## Verified on the wired board

- **End-to-end HA control** — appears in HA on connect, powers the BC250 on from
  the HA switch, and reports board power back. The full path works.
- **8 s hold-to-portal**, twice, including re-entering settings on a device that
  was already configured.
- **Settings survive reflashing** — firmware upload erases `0x0–0x4fff`,
  `0x8000–0x8fff`, `0xe000–0xffff` and `0x10000+`, leaving NVS at
  `0x9000–0xdfff` untouched. WiFi credentials persisted across every flash.
- **LWT / availability** — an unplugged device's `avty_t` goes to `offline` on
  the broker. Observed on the retained topics of the devkit after it was
  disconnected.

## Not yet verified — needs hardware not currently attached

- Button: tap-on, 5 s hold-off, and escape-from-setup.
- BLE: controller bind, wake-on-presence, the post-power-off cooldown.
- TPMS1-driven shutdown detection (`STATE_ON` following the board down).
- **Phase 3 step 3, the coexistence A/B** — needs a bound controller. Cold wake
  timings (×3) must be captured at SOLO params *before* comparing against COEX,
  or the comparison is unfalsifiable.
- **Phase 3 step 5**, the NimBLE leak check — needs a bound controller so a scan
  is actually running. The `setMaxResults(0)` fix is verified by code inspection
  only (passive scan reaches `m_callbackSent >= 2`, so the erase path at
  `NimBLEScan.cpp:343` does fire). Partial evidence: 293 consecutive heartbeats
  with a scan running showed a single distinct `heap` value — but the bound
  controller was not advertising, so the result-accumulation path where a leak
  would live was never exercised.

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

**Resolved.** On the wired board (below) TPMS1 reads **1–5 mV** with the BC250
off, a ~4 mV band against a 100 mV threshold. The artefact was purely the
floating pin. `HA_SENSE_MIN_INTERVAL_MS` is now belt-and-braces rather than
load-bearing — keep it, since it costs nothing and bounds the worst case if the
line ever gets noisy under load, but expect `sense_mv` to publish on the 30 s
keepalive in normal operation.

## Wired board — 44:b1:76:1a:48:64

A second ESP32-C3, soldered to the button and connected to a BC250, with a
controller already bound (`f4:6a:d7:16:b9:86`). Flashed 2026-08-14. Its NVS
carried only `forceSetup` and `wakeAddr` from the previous firmware — no
`passHash` — so WiFi/MQTT are unconfigured on it and it must be taken through
the portal before it appears in HA.

Idle in `STATE_OFF`, WiFi disabled, BLE scanning at SOLO params, 24 consecutive
heartbeats:

| | value |
|---|---|
| `sense` | 1–5 mV, `low` |
| `heap` | 179,460 B — **identical on every sample** |
| `loopMax` | 1,143–1,386 µs |

Two things this establishes:

- **The optional-feature guarantee holds on real hardware.** With no SSID
  configured the firmware logs `station mode disabled` and never brings up WiFi;
  the board runs button-and-BLE only, exactly as the pre-change build did.
- **A byte-identical heap across 24 samples with a scan running** is the first
  real evidence for the `setMaxResults(0)` fix (Phase 3 step 5), which until now
  was verified only by code inspection. It is not yet the multi-hour check the
  plan asks for, and note the scan is finding nothing — the bound controller was
  not advertising. The leak, if any, is in the result-accumulation path, so the
  check only becomes meaningful with the controller powered on and discoverable.

`loopMax` here is not comparable to the 700–780 µs Phase 0 figure: that baseline
had no BLE scan running. Treat 1,150 µs as the new no-WiFi reference for a board
that is actively scanning.
