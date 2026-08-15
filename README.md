# BC250 ESP32 Power Switch

An ESP32-C3 power controller for an AMD **BC250** board running as a desktop. The
BC250 is fed from a PCI-E connector and has no ATX power button, so this firmware
drives the SFX PSU's `PS_ON#` line and senses board power, giving you a real power
button — plus optional "turn on when I pick up my controller" via Bluetooth.

## Features

- **Push-button power**: tap to turn on; hold 5 s while running to force off.
- **Follows the board**: if the OS shuts the board down, the PSU is cut automatically.
- **Boot watchdog**: if the board doesn't come up within 10 s, the PSU is released.
- **BLE controller wake** (optional): when a bound controller (e.g. an 8BitDo) powers
  on, the machine powers on with it.
- **Home Assistant** (optional): joins your WiFi and appears in HA over MQTT with
  auto-discovery as a switch plus power/state/signal sensors.
- **WiFi setup portal**: configure the bound controller from a phone — no reflashing.

## Wiring

The ESP32-C3 is permanently powered from the ATX connector's **5 V standby**, so it
runs whether the machine is on or off. Share a common ground between the ESP, the PSU,
and the board.

| ESP32-C3 | Connects to | Notes |
|----------|-------------|-------|
| GPIO5 | Momentary switch, terminal A | Read with internal pull-up |
| GPIO6 | Momentary switch, terminal B | Driven LOW as the switch's ground |
| GPIO4 | ATX `PS_ON#` (green wire) | **Open-drain**, active LOW: LOW = PSU on, released = off |
| GPIO3 | BC250 `TPMS1` (pin 9) | ~3.3 V when the board is up, 0 when off |
| 5VSB / GND | PSU standby + common ground | Permanent power for the ESP |

`PS_ON#` idles at ~5 V (pulled up inside the PSU). GPIO4 is driven open-drain so the
3.3 V part never fights the 5 V rail — it only ever sinks to ground to switch the PSU on.

`TPMS1` is a higher-impedance signal that hovers near the logic threshold, so it's read
as an analog voltage with hysteresis rather than a digital pin.

### Connector pinouts

**ATX 24-pin main connector** — tap three pins:

```
               +3.3V ─┤  1 │ 13 ├─ +3.3V
               +3.3V ─┤  2 │ 14 ├─ −12V
                 GND ─┤  3 │ 15 ├─ GND
                 +5V ─┤  4 │ 16 ├─ PS_ON#   ◄── GPIO4  (green, open-drain, active LOW)
                 GND ─┤  5 │ 17 ├─ GND      ◄── ESP GND (any GND pin works)
                 +5V ─┤  6 │ 18 ├─ GND
                 GND ─┤  7 │ 19 ├─ GND
              PWR_OK ─┤  8 │ 20 ├─ (RSVD)
ESP 5V/VIN ◄── +5VSB ─┤  9 │ 21 ├─ +5V
                +12V ─┤ 10 │ 22 ├─ +5V
                +12V ─┤ 11 │ 23 ├─ +5V
               +3.3V ─┤ 12 │ 24 ├─ GND
```

**TPMS1 header** — single pin for board-power sense:

```
   PCICLK ─┤  1   2 ├─ GND
    FRAME ─┤  3   4 ├─ SMB_CLK_MAIN
  PCIRST# ─┤  5   6 ├─ SMB_DATA_MAIN
     LAD3 ─┤  7   8 ├─ LAD2
       3V ─┤  9  10 ├─ LAD1      ◄── pin 9 (3V) = board-on sense ──► GPIO3
     LAD0 ─┤ 11  12 ├─ GND
          ─┤     14 ├─ S_PWRDWN#
     3VSB ─┤ 15  16 ├─ SERIRQ#
      GND ─┤ 17  18 ├─ GND
```

Pin 9 is the only TPMS1 pin used: it reads ~3.3 V when the board is powered and 0 V when
off. No ground wire is needed from this header — the ESP already shares ground with the
board through the ATX connector.

## Button controls

| Action | Result |
|--------|--------|
| Tap while **off** | Power on |
| Hold ≥ 5 s while **on** | Force power off |
| Hold ≥ 8 s while **off** | Enter WiFi setup portal |

The button is the primary control and always works, even with no controller configured.

## Bluetooth controller wake

When a controller is bound (via the portal), the machine **follows the controller**:
turn the controller on and the machine powers up. After a power-off there's a short
guard window so the controller's reconnect burst can't immediately switch it back on —
turn the controller off within that window to keep the machine down.

## Setup portal

Hold the button ≥ 8 s while off (or on first use) to start the portal:

1. Connect to the open WiFi network **`BC250 Switch Setup`** and open `http://192.168.4.1`.
2. Create a password.
3. **WiFi** — pick your network from the list captured at startup, or type one in.
4. **MQTT** — your broker's host, port and credentials.
5. **Controller** — pick it from the live BLE scan, or enter its MAC.
6. **Finish** — name the device, review, and reboot into normal operation.

Only the password is required. **Every later step can be skipped**, and each is
independent: no WiFi means the device runs exactly as it always has, on the button
alone. Skipping just MQTT joins the network without appearing in Home Assistant.

To clear something later, blank the field and save — an empty SSID disables WiFi, an
empty broker host disables MQTT, and an empty MAC un-binds the controller. Password
fields left blank keep whatever is already stored; clear one and save to remove it.

The WiFi list is scanned once at startup, before the setup AP exists — a live rescan
would take the AP off-channel and drop your phone's connection. To refresh it, re-enter
setup mode.

## Home Assistant

With WiFi and a broker configured, the device publishes MQTT discovery and shows up
automatically. Entities:

| Entity | Type | Meaning |
|---|---|---|
| Power | switch | On whenever the PSU is asserted (booting counts as on) |
| Board power | binary sensor | `TPMS1` — whether the board itself is actually up |
| State | sensor | `OFF` / `BOOTING` / `ON` |
| Controller present | binary sensor | Bound controller is advertising (only if one is bound) |
| Sense voltage | sensor | Raw `TPMS1` reading in mV |
| WiFi signal, Free heap, Uptime | sensors | Diagnostics, published every 30 s |
| Restart, Setup mode | buttons | Reboot, or reboot into the setup portal |

**The switch reports reality, not intent.** Commands from HA run through exactly the
same power path as the physical button, including the boot watchdog. So if you turn the
switch on and the board never asserts `TPMS1`, the 10 s watchdog cuts the PSU and **the
switch flips itself back off**. That is correct behaviour — the machine genuinely did
not come up — but it looks like a bug if you aren't expecting it.

If the broker is unreachable, the device keeps working normally on the button; it just
retries the broker every 20 s in the background.

## Build & flash

PlatformIO (pioarduino). Two steps — firmware and the portal's web UI (a single
`app/index.html` packed into SPIFFS):

```bash
pio run -t upload     # firmware
pio run -t uploadfs   # web UI filesystem
```

## Notes

- **WiFi TX power**: these ESP32-C3 *mini* boards have an RF/power quirk
  ([arduino-esp32 #6551](https://github.com/espressif/arduino-esp32/issues/6551)) where
  the radio is unusable at full power. Both the SoftAP (`AP_TX_POWER`) and station
  mode (`STA_TX_POWER`) therefore run at `WIFI_POWER_8_5dBm`
  (in [include/board.h](include/board.h)). At full power the symptom is *not* a
  brownout — there are no resets at all — it is a silent failure to work: the portal
  is invisible, and station mode never completes association, logging
  `disconnected, reason=2` (`AUTH_EXPIRE`) forever. Severity varies between physical
  boards, so one unit working at 19.5 dBm says nothing about the next. See
  [docs/baseline.md](docs/baseline.md) for the measurements.
- **Radio coexistence**: WiFi and BLE share one radio. Once an SSID is configured the
  BLE scan drops from ~50 % to ~15 % duty (`BLE_SCAN_*_COEX_MS` in
  [include/board.h](include/board.h)) so it doesn't starve the WiFi MAC — controller
  wake still works, it just takes a little longer to notice. With no SSID configured the
  original ~50 % duty is used unchanged.
- Serial debug runs over USB-CDC at **115200** baud.
- Pin assignments and all timing constants live in [include/board.h](include/board.h).
- **Every "off" is a hard rail cut.** The controller drives `PS_ON#` directly and has
  no channel into the running OS, so the HA switch and the 5 s button hold yank power
  rather than requesting a shutdown. A shutdown started *inside* the OS is handled
  cleanly — the ESP sees TPMS1 drop and follows the board down. Plans for a graceful
  path are in [docs/future-work.md](docs/future-work.md).
