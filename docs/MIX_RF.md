# Keyboard radio monitor

Open RADIO > MIX-RF with the keyboard expansion attached. PROBE/P checks its
CC1101, nRF24 and NFC radios. Stop observation modes before retrying PROBE.
Stop a wired Flipper connection first if it owns the same header pins.
A successful register probe does not prove reception or antenna coupling.

REC and SUB-GHZ share captures. RADIO/R chooses RTL-SDR, HackRF, CC1101 or the
LoRa chip where supported. Stop WATCH before changing the capture source.
Each source keeps its own frequency for the session. In REC, TOOLS/D opens
RTL capture controls; BACK/B returns to the recorder.

CC1101 WATCH receives raw OOK pulse timing. Its profile uses 650 kHz bandwidth,
a 40 us capture filter and a 30 ms end gap. A full pulse buffer is discarded
and counted as an overflow. Long continuous signals can exceed the buffer.
RX unavailable means capture could not start. PIN keeps a capture in the
archive; SAVE .SUB exports it. JOURNAL bookmarks it to NOTES with current
GPS and motion readings. Those readings describe the bookmark time.

OOK24 means a repeated 24-bit pulse pattern with about 1:3 timing and a 1:31
sync gap. It needs matching repeated frames. This identifies a timing family,
not a manufacturer or unique device. RAW and unsupported patterns remain
unclassified. CC1101 raw OOK and 2-FSK captures can be replayed where supported.
Replay transmits; MIX-RF MONITOR only receives.

Stop CC1101 MONITOR before CC1101 WATCH. CC1101 capture can continue while a
USB SDR is used in another app. Stop WATCH in REC or SUB-GHZ; MIX-RF STOP ALL
only stops observation modes owned by MIX-RF.

MONITOR/M measures CC1101 channel energy. BAND/B selects 315, 433.920,
868.350 or 915 MHz and the matching RF path. The bandwidth is 650 kHz.
RSSI is a nominal dBm conversion, not a calibrated power measurement.
Energy samples are not packets. This mode does not decode traffic or transmit.

2.4 SCAN/S samples the nRF24 received-power detector over 2400-2483 MHz.
Its activity map shows threshold hits near nominal -64 dBm. It does not
identify Wi-Fi, Bluetooth, addresses or packets. Turning it off powers down
the nRF24 receiver.

NFC WATCH/F detects an external reader field. A passive tag alone produces no
field. Brief fields can be missed. This mode does not read UIDs or card data.

NFC/N is a separate NFC-A presence scan at 13.56 MHz. It sends a short REQA
probe about twice a second and checks ATQA replies. The RF field turns off
between probes. CARD DETECTED needs matching valid replies; energy alone is
not a card detection. ATQA alone does not identify a MIFARE model. Presence
scan and passive field watch are mutually exclusive.

The NFC view keeps 32 probes of receive history. Bar length is raw level 0-15,
not calibrated dBm or a frequency sweep. Cyan shows activity, amber `!` a
rejected frame, white `R` a valid reply, and green `C` matching replies.
Newest rows are at the bottom. This history does not read card memory or
save files automatically.

VIEW/V cycles channel energy, 2.4 GHz activity and NFC history. MARK/J saves a
selected energy observation to NOTES. In the NFC view, the same control is
CARDS and opens the card workbench. Observation modes continue when leaving
MIX-RF. STOP ALL/X stops them before the keyboard is disconnected.

The NFC card workbench opens with C or CARDS. SCAN validates a card's UID and
SAK. READ uses factory `FFFFFFFFFFFF` keys by default. KEY A/B sets a key in
reader memory. Before identification it sets the default; afterward it sets
the selected sector's key. Sector overrides clear when a different UID is
selected. Use cards you own or have permission to read.

Classic Mini, 1K and 4K memory layouts are supported. Other NFC-A cards can be
identified, but unsupported memory protocols are reported. Unread or unknown
key bytes show `??`. Authentication success does not imply that every block
is readable. Key A remains unknown; Key B is shown only when the verified
access conditions permit it to be read as data.

Arrows select blocks. Enter opens their details; Tab focuses controls. Touch
selects a block, then VIEW shows its bytes. Portrait has paging controls.
A failed selection keeps the prior capture. Stop keeps verified blocks. SAVE
writes a text capture under `/sdcard/nfc`; MARK/J keeps a note with current
sensors. A partial capture stays partial. The workbench does not write cards,
change card keys, emulate cards or search for unknown keys.

For setup and saved observations, see [Field and lab guide](FIELD_LABS.md).
RADIOS > ANTENNA selects the board's internal antenna or MMCX1 and keeps the
choice after reboot. This is separate from the keyboard CC1101 BAND setting.
