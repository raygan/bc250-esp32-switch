# Future work

## Graceful shutdown instead of a power yank

Today every "off" is a hard rail cut. `powerOff()` calls `psuOff()` immediately,
so the HA switch, the 5 s button hold and the boot watchdog all yank power out
from under a running OS. That is fine for a machine that is already wedged and
wrong for one that isn't.

**Half of this already works.** `src/main.cpp:417` follows the board down when it
shuts itself off:

```cpp
case STATE_ON:
  // Board dropped TPMS1 on its own (OS shutdown / crash) -> follow it down.
  if (boardChanged && !boardSenseStable) {
    powerOff("TPMS1 LOW while ON, board shut down", now);
  }
```

So a `poweroff` issued inside the OS is already handled cleanly: TPMS1 drops, the
ESP notices and releases `PS_ON#`. What is missing is not shutdown *handling* but
a channel to *ask* the OS to shut down. The ESP owns the rail and has no path
into the running system.

### Preferred approach: tap the physical power button (option 2)

The BC250 has **no power-button header**, but it does have a built-in power
button on the board. Soldering a two-wire connector across that button's pads
and driving it from a spare GPIO (open-drain, momentary pull to ground) gives
real ACPI semantics for free — the OS decides what a short press means via
logind's `HandlePowerKey`, and the ESP needs to know nothing about the OS, the
network, or MQTT. It also keeps working when WiFi is down, which the MQTT-based
alternatives do not.

This is the chosen direction, deferred only because it needs soldering to the
board itself. Note the ESP32-C3 has spare GPIOs available: the project currently
uses 3, 4, 5 and 6, and 0/1/7/10 are free without touching strapping pins.

### Alternative considered: an agent on the BC250 (option 1)

Enable SSH and drive `systemctl poweroff` from an HA `shell_command`, or run a
small systemd service on the BC250 subscribing to an MQTT topic. Needs no
hardware work and no firmware change, since the existing TPMS1 detection closes
the loop. Rejected as the primary plan because it makes shutdown depend on the
network and on software running inside the very OS you are trying to stop —
precisely the thing that is broken when you most want the button to work.

Worth keeping as a fallback path even after the hardware route exists.

### Required either way: a grace window in the firmware (option 3)

This is the complement to either approach above, not a solution on its own. A
`STATE_STOPPING` that waits up to `SHUTDOWN_GRACE_MS` for TPMS1 to drop by
itself, and only yanks the rail if it does not, turns "off" into *ask nicely,
then insist*. That fallback is what makes the graceful path safe to rely on: a
wedged OS that cannot respond still ends up powered down.

Scope is small — one new state, one timeout constant, and routing the existing
off-triggers through it instead of straight to `psuOff()`.

**Design constraint:** keep the existing MQTT switch as the honest hardware
control and layer graceful policy above it. Do not quietly redefine what the
switch means. The "switch reports reality, not intent" property is what makes
the state shown in HA trustworthy, and it should survive this change.

## Minor: association backstop fights the SDK's auto-reconnect

`src/ha.cpp:349` re-issues `WiFi.begin()` every `WIFI_RETRY_MS` while
disconnected, unconditionally — including while `WiFi.setAutoReconnect(true)` has
an attempt already in flight. The SDK rejects the overlapping call:

```
E (20439) wifi:sta is connecting, return error
[ 20114][E][STA.cpp:417] connect(): STA connect failed! 0x3007: ESP_ERR_WIFI_CONN
```

Harmless — it never fires while the link is healthy, and the backstop is worth
keeping for the cases `setAutoReconnect()` misses. But it wastes a call and
clutters the log every 20 s during an outage, which is exactly when that log is
being read. Guard it on the station not already being mid-attempt.
