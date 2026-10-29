# SDR front ends and RF LAB: how to check them on a board

Branch `feature/ovh-bias-tee-20261028`. Nothing here was run on hardware when it
was written; every step below is a check still to do.

## Bias-T (no bias-T device needed)

RTL-SDR Blog V3/V4 (and copies) switch the bias-T from RTL2832U GPIO0. The driver
writes block 2 registers 0x3001 GPO, 0x3003 GPOE, 0x3004 GPD and reads them back.

```
bias status            # both radios; RTL line shows the three GPIO registers
bias rtl on            # prints the warning; banner on the screen
bias status            # expect: ON  [GPIO0: GPO 0x?1 GPOE 0x?1 GPD 0x?0 -> out enabled, level high]
bias rtl off           # expect: off, GPO bit0 = 0
```

Meter check: DC volts between the SMA centre pin and its shell, nothing connected.
RTL-SDR Blog V3: about 4.5 V when on, 0 V when off. V4: about 4.5-5 V.
HackRF (`bias hackrf on`): about 3.0-3.3 V, but only while it is receiving: the
HackRF has no read-back request, and its firmware may drop port power when RX
stops, so the driver re-sends it at every RX start. Start FM on the HackRF, then
measure. A generic RTL dongle without a bias-T shows the register change and 0 V.

Auto-off: with it on, unplug and replug the dongle (or `rtl reset`): `bias status`
must say off and the meter 0 V. Reboot: off. `bias rtl pin`, replug: on, with the
"BIAS-T ON ... pinned" banner; `bias rtl unpin` to clear.

## Front-end settings

```
rffe                    # both radios, saved values, the RTL tuner gain table
rffe rtl ppm 12         # saved for this dongle (key from its USB strings); retunes
rffe rtl dagc off       # RTL2832 digital AGC: follow (default) | on | off
rffe rtl offset on      # E4000/FC00xx only; an R820T says "not this tuner"
rffe hackrf amp on      # warning banner; amp off | auto (by gain, as before)
rffe hackrf lna 24      # fixed LNA; vga 20 fixes the VGA; auto frees them
hackrf                  # existing status: shows the lna/vga/amp now in force
```

On screen: OPTIONS > FRONT END in FM (every mode), P25, ADS-B and FALLS when the
radio is an RTL-SDR or HackRF; RADIOS > OPTIONS has both. Direct sampling shows
"not in this build" until the HF branch answers LS_RADIO_FE_DIRECT_SAMPLING.

## RF LAB

RF LAB is an SD app (`app-sdk/apps/rflab`; install it from the store or with
`app put`). It opens the radio picked under SOURCE (ANY: the first free SDR,
else the LR2021) itself, tuned to TUNE; it no longer follows another app's
blocks, and there is no `rflab` console command.

PPM calibration: tune a known carrier (NOAA weather, a GSM/LTE downlink via CELL,
a signal generator), set OPTIONS > EXPECTED CARRIER if it is off the tuning,
read PPM "+N in FRONT END", then add N in that dongle's FRONT END > PPM (or
`rffe rtl ppm <now + N>`) and measure again: the error should fall under 1 ppm.

Spectrum, SNR, readings and the SNR history update about four times a second
from real readings; HOLD freezes them and gives the radio back.
`bench/tests/test_rflab.c` checks the measurement against synthetic I/Q.
