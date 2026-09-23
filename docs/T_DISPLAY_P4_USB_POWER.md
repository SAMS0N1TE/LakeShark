# T-Display-P4: USB host power, and recovering a dongle without it

Read this before treating an RTL-SDR problem on the T-Display-P4 as a
firmware fault. The comment on the USB PHY enable in
`components/lakeshark/board/ls_board_hw.c` points here.

## Where the USB-A port gets its power

These facts come from the LilyGO AMOLED V1.0 schematic (202601061148),
sheet 8, as that comment records them. The schematic itself is not
in this repository. Nobody has re-measured these on a board since they were
written, so treat them as a schematic reading.

- **USB-A VBUS comes from VCC_5V through U19, a TPS2051C power-distribution
  switch.** It does not come from the internal battery.
- **The internal battery feeds INPUT_5V, and no boost connects the battery
  to VCC_5V.** On battery alone, then, the schematic gives the USB-A port no
  VBUS. An RTL-SDR that never enumerates while the board is on battery is
  unpowered. That is the hardware, not the firmware.
- **Nothing the ESP32-P4 drives controls U19.** The vendor pin map names no
  GPIO for the VBUS switch, so `LS_HAS_VBUS_CTRL` is 0 on this board and
  `CONFIG_LS_VBUS_EN_GPIO=-1` in `boards/t_display_p4.defaults`.
- **USB PHY power is XL9535 IO10** (vendor `gpio::xl9535::kUsbPhyPowerEn =
  Xl95x5::Pin::kIo10`). `ls_board_hw_early_init()` drives it once at boot,
  keeping the vendor's PHY-enable sequence. It does not generate VBUS, and
  toggling it later has not been established as a way to recover a receiver.

So the firmware cannot cut the dongle's power. Anything that needs a real
power loss to clear needs a replug.

## The failure this was written for

Seen on 2026-09-11 with firmware 1.0.3-g951f9933, after about 18 minutes of
use:

- `rtl` reported `rtl-sdr.usb ok present=1 stream=0 ... attach=7 detach=6`.
  The dongle had dropped off USB and come back six times.
- After the seventh attach, every `rtlsdr_demod_write_reg` and
  `rtlsdr_demod_read_reg` failed. FM retried its radio open about every
  150 ms, logging `E fm: radio open failed: io` each time. P25 showed
  `RX stopped`. Leaving and re-entering apps did not help.
- An app-only flash, which reboots the board, revived the dongle without a
  replug. During that boot the host logged `E HUB: Root port reset failed`
  about ten times between 2.7 s and 5.3 s, then found the R820T at high
  speed.

Two things follow. First, VBUS never dropped, because it cannot drop here, so
what revived the dongle was a USB bus reset and a fresh enumeration. Second,
the health watchdog never noticed: it judged stream progress, and nothing was
streaming because nothing could be configured. It reported `ok` throughout.

## What the firmware does now

**It notices.** The radio endpoint service counts control operations
(configure, gain, start, retune) that fail with `io` in a row. Stop is not
counted. `rtl` shows the count as `ctlio=`. After
`RADIO_HEALTH_CONTROL_IO_FAULTS` (3) of them, the health watchdog stops
waiting for a stall that cannot come. It skips in-place recovery, which
re-claims the interface on the host side and cannot reach a device that
ignores its control pipe. It goes straight to the board's power-cycle hook.

**It resets the root port.** On a board without a VBUS switch the hook is a
root-port reset, not a VBUS cycle:

1. The dongle is torn down in the detach-safe order, because it is still on the
   bus. The endpoint is unregistered, the transfer pool is drained with the
   device handle still valid, and the device is closed.
2. The root port is powered off with `usb_host_lib_set_root_port_power(false)`.
   VBUS stays up. The host stack sees a disconnect and frees the device.
3. Once the device is released, the port is powered back on. The dongle sees
   a bus reset and enumerates from address 0 through the usual probe.

`rtl reset` on the console does the same thing on demand, and so does
`SDR power` from a Flipper head. The worker waits up to 15 s for the dongle
to come back, because the boot above spent 2.6 s on failed resets first.

**It keeps a budget.** Automatic resets count against the same RTC budget as
the VBUS path on other boards: two, refilled only after ten seconds of
samples actually moving. `ok` alone does not refill it, because `ok` is what
the wedged dongle reported. The budget survives a reboot. Resets asked for
from the console or the head are not counted.

**It does not reboot unless told to.** If the port does not come back on, or
nothing re-enumerates, the worker restarts the board only when the USB
auto-reboot setting is on. That setting is off by default. With it
off, the board stays up and logs that a replug or a reboot is needed.

**FM stops flooding.** FM counts opens that fail with `io`. After
`RADIO_OPEN_IO_ATTEMPTS` (5) in a row it logs once and stops trying until a
receiver attaches or detaches. The reset above does both, so a dongle that
comes back is picked up without re-entering FM. The limit is held at or
above the watchdog's threshold at compile time. If FM stopped sooner, the
watchdog would never see enough failures to act.

## What it cannot do

- **Cut power.** A dongle whose fault only a power loss clears still needs a
  replug.
- **Find an absent dongle.** A root-port reset needs an enumerated device to
  act on. When the dongle has been absent for 30 s, this board only logs it.
  On a port holding some other device, a reset would hit that device instead.
- **Close every race in IDF 5.4.3.** Powering the root port off with nothing
  enumerated makes `hub.c` abort, and the reset refuses to do it. A real
  unplug in the few milliseconds between that check and the power-off can
  still reach an assert in the host stack. The worker runs on core 1 below
  the USB lib task to narrow that window, but it cannot close it from outside
  the stack.

## What has been proven on hardware

On 2026-09-21, firmware 2.2.1 on the bench board, `rtl reset` with the
dongle idle released the device, cycled the root port and re-enumerated
the R820T at high speed 1.1 s later (`attach=2 detach=1`). FM on 162.550
then streamed about 510 kB/s. The first attempt at this port showed why a
board run matters: the endpoint was never unregistered, the dongle came
back and failed to register with `exists`, and the worker reported
success anyway. The bench could not see that, because `rtlsdr_dev.c` is
not compiled there.

The automatic path, where three control operations failing with `io`
escalate without anyone typing `rtl reset`, has been seen only halfway.
In that broken first attempt the detection worked on the board: `ctlio=5`,
then `settling -> powercycle (endpoint does not answer control requests)`.
The reset it asked for then ended `powercycle -> failed (endpoint power
cycle rejected)`, and why it was rejected in that state was not
established. With the fix in, the automatic path has run only on the
bench, which pins it in `test_usb_port_cycle`, `test_radio_control_fault` and
`test_radio_open_retry`. The next time the fault appears on its own,
`rtl` should show a climbing `ctlio=` followed by a `root-port cycle
done` line. Whether a bus reset alone revives every wedged dongle, and
not just the idle one tested here, is still open.

## Where it lives

| What | File |
|---|---|
| Port cycle sequence, pure and bench-tested | `components/lakeshark/radio/usb_port_cycle.c` |
| IDF binding | `components/lakeshark/radio/usb_host.c` |
| Detach-safe teardown then cycle | `rtl_adapter_port_reset()` in `components/lakeshark/radio/rtlsdr_dev.c` |
| Control io count, endpoint generation | `components/lakeshark/radio/radio_endpoint.c` |
| Escalation without a stall | `components/lakeshark/radio/radio_health.c` |
| Worker, budget, `rtl reset` | `main/headless_main.c` |
| FM's bounded open | `components/lakeshark/radio/radio_open_retry.c`, `components/lakeshark/apps/fm/app_fm.c` |
