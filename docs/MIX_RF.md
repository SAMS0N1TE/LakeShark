# Keyboard radio monitor

Radio → MIX-RF probes the powered keyboard expansion. PROBE can retry after
reconnecting. It preserves the keyboard expander's unrelated outputs. Keypad
and expander register transactions share a mutex. A wired Flipper connection
must be stopped before the probe because it can own the same header pins.

CC1101 MONITOR is a receive-only channel-energy monitor. BAND selects the
matching 315, 434 or 868/915 MHz RF path. RSSI uses the receiver's nominal
conversion and is not a calibrated power measurement. The reset modem's
receive bandwidth applies. Continuous asynchronous reception disables packet
completion and FIFO handling for this energy monitor. This is neither a
spectrum analyzer nor a packet decoder. No transmit strobe is implemented.

The monitors continue when leaving the app. STOP ALL stops all three.
Journal's CC1101 input can then sample the monitor alongside GPS and nine-axis
motion. MARK creates a note explicitly describing the energy observation.
Packet count remains zero: RSSI samples are not packets.

nRF24 identification checks address-width and channel registers and restores
the address-width register after a readback test, with CE held low. It is a
compatible-register check, not a unique silicon identity. NFC checks the
ST25R3916 family identity register without starting an RF field. These two
checks do not establish packet reception or card reading.

2.4 SCAN samples the nRF24 received-power detector over 2400–2483 MHz. Each
channel gets 200 microseconds in RX (settling plus detector time), followed
by the next channel on the worker's next 10 ms cycle. An 84-byte smoothed
activity map shows threshold hits near the nominal -64 dBm level; it does
not identify Wi-Fi, Bluetooth, addresses or packets. Automatic acknowledgments
and packet pipes are disabled. STOP powers the nRF24 receiver down.

NFC WATCH uses only the ST25R3916 peer-field detector. Its oscillator,
receiver and transmitter remain disabled. It shows whether an external NFC
reader field is present and counts sampled rising edges; brief fields between
10 ms samples can be missed. A passive tag alone does not produce a field.
This is not a tag reader and does not read UIDs or card data.

NFC SCAN (N) is a separate, basic NFC-A card-presence detector. It sends a
REQA at 106 kbit/s roughly twice a second, validates the two-byte ATQA response
and displays CARD DETECTED. The RF field turns off after each bounded probe.
It does not select a UID, authenticate, read sectors, or write anything to a
card. ATQA alone cannot positively identify a MIFARE model. F switches to
passive external-field watch; the two NFC modes are mutually exclusive.
STOP ALL stops both. Detection continues when leaving the screen. Card observations
can be bookmarked to Journal. Positive card detection needs hardware testing;
successful command transmission alone does not prove antenna coupling.

Landscape uses compact bordered controls and side-by-side MIX-RF status and
visuals. Arrow keys move focus and Enter activates it. Labs supports movement
between both control rows. Journal and SUB-GHZ use Left/Right for controls;
Up/Down returns to entry/pattern browsing. Disabled controls are skipped.

VIEW cycles channel energy, 2.4 GHz activity and NFC field views. JOURNAL MARK
saves the selected observation with the current GPS/motion snapshot. Journal
also offers these three sources as live attachments. Energy observations
never increment a packet count. Measurements use bounded RAM and create no
automatic SD files; only explicit marks enqueue notes through Journal.

Wiring source: LILYGO's pinned
[keyboard configuration](https://github.com/Xinyuan-LilyGO/lilygo_device_driver/blob/a47cf73e2c9d442c0f00b618749150f4482dcd0a/src/device/t_display_p4/t_display_p4_keyboard_expansion_config.h)
and the initialization/RF-switch logic in its sibling driver. CC1101 register
access and nominal RSSI conversion follow the
[TI datasheet](https://www.ti.com/lit/ds/symlink/cc1101.pdf).
NFC identity is documented in the
[ST25R3916 datasheet](https://www.st.com/resource/en/datasheet/st25r3916.pdf).

The shared bus uses 1 MHz register transfers and an internal-RAM transaction
descriptor. The SDK can still require DMA bounce buffers for short transfers.
Probe-only and failed devices release their SPI allocations immediately;
probing sequentially avoids reserving buffers for all three chips together.
CC1101 and nRF24 share one mode-0 SPI handle with separate manual selects;
NFC uses mode 1. Together with SX1262 this stays within three device slots.
The worker and GPS stacks are in PSRAM; neither writes SPI flash. Moving the
GPS stack avoids exhausting the contiguous internal memory required by SPI.
Receiver snapshots are fixed-size and the UI does no radio I/O. CC1101 polls
at about 10 Hz; nRF24 and NFC advance at about 100 Hz under normal load.

The probe follows the radio-enable low/output/high transition used by LILYGO's
keyboard examples and the CC1101 CS/reset sequence used by their pinned native
driver. Ready waits are bounded to 2 ms and the shared SPI bus remains locked
through reset. `enable on` reports a successful expander write, not a measured
radio supply voltage; an unavailable chip is not proof of a missing battery.

On the attached USB-powered keyboard without batteries, the independent
LILYGO native-driver harness identified CC1101 version 0x14 and nRF24. The
integrated firmware subsequently identified CC1101 0x14, nRF24 address width
3 and NFC identity 0x2A with GPS running. See `bench/mixrf_probe` for the
isolated diagnostic. NFC's positive external-field response still requires
a nearby powered reader to validate; an idle-field read alone does not prove it.

The integrated receive test collected 1,301 CC1101 samples, 152 nRF24 sweeps
and 12,640 NFC field samples while GPS parsed sentences and RTL received P25
on 154.7850 MHz. RTL reported 16 USB transfers in flight and zero dropped
bytes; one received P25 call had 19 successful BCH checks and no BCH failures.
All three observation types saved to Journal with IMU attachments. Two GPS
stop/start cycles retained the same free DMA memory; STOP ALL released the
NFC device allocation and a subsequent probe identified all three chips again.
These are bench observations on one board, not a long-duration soak test or
calibration of RF sensitivity. Indoor GPS had no position fix during the test.

NFC card scan includes a bounded 32-probe receive history at 13.56 MHz (two
probes per second). Rows scroll toward the bottom; bar length is the ST25's
raw 4-bit AM RSSI, not calibrated dBm or a frequency sweep. Cyan indicates
activity, amber `!` rejected frames, white `R` a valid ATQA, and green `C`
two consecutive matching ATQA responses. Energy alone is not card detection.
The scan does not read UID/sectors, write cards, or automatically save files.

NFC card workbench: press C in MIX-RF, or CARDS in its NFC view. SCAN selects
and validates the UID (BCC) and SAK (CRC). READ uses factory FFFFFFFFFFFF keys
by default. KEY A/B sets the selected sector's key in RAM after identification;
before identification it sets the default. Sector overrides clear on a different
UID. Authentication requires a valid cryptographic card response. Each memory
block requires encrypted parity and CRC validation. Unknown or unreadable key
bytes are displayed/exported as ??, never assumed zero. Access-bit redundancy
checks apply only to a verified sector trailer.

The suite supports Classic Mini/1K/4K reads, with 4K's larger final eight sectors.
Other NFC-A cards can be identified; unsupported memory protocols are explicitly
reported. This is a read/authentication workbench; it does not write cards,
change their keys, emulate them, or recover unknown keys through attacks.
Colours reflect actual operations: unread, active, verified read, authentication
failure, integrity/transport error, or card NAK. Authentication success does not
imply permission to read every block. A new failed selection retains the prior
capture. Stop retains verified blocks. SD saves are manual bounded text captures
under /sdcard/nfc, and Journal marks include NFC metadata plus current sensors.

The landscape map fits all 256 blocks and previews the selected block's actual
hex/ASCII bytes. Arrows select blocks; Enter switches to the detailed view;
Tab focuses controls. Touching a block selects it. Each configured key gets at
most two authentication attempts per sector to tolerate intermittent reception;
there is no unbounded key search. A partial capture stays visibly partial.

Bench validation on the attached Classic 4K read all 256 blocks. Deliberately
setting incorrect reader keys for sector 0 left its four blocks unknown; restoring
the reader keys restored full reads. These changes were reader RAM only, with no
card writes. Mini/1K geometry has host coverage; their full reads have not yet
been validated on this board. Trailer Key A remains unknown; Key B is shown only
when the card's verified access conditions make it readable as data.

Crypto1 forward operations are adapted from Flipper Zero firmware revision
7f0b6e1c14431708cfde75ae1ba13df59e868041; see nfc_classic.c and LICENSE.
Memory layout and access-bit definitions follow NXP MF1S70YYX_V1 rev3.2.
The fixed-frequency receive history is separate from the suite's card map;
energy in that history is not evidence of authenticated or readable memory.
