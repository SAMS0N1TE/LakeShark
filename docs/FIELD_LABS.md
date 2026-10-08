# LoRa Labs and Notes

The launcher groups apps under RADIO, FIELD, SYSTEM and USER. RADIO includes
LORA LABS and EXPERIMENTS. FIELD includes REC, MAP, GPS, NOTES and COMPASS.
Use PREV/NEXT or F5/F6 for more apps. Page size follows the available space.

## LoRa Labs

DIRECT off leaves Mesh in control. DIRECT on waits for the LoRa radio, saves
its configuration and receive state, and applies the lab settings. Leaving
Labs restores the prior state. RADIO/R chooses the fitted SX1262 or LR2021.
The chip's tuning range does not establish the antenna's useful coverage.

Modes:

- PACKETS: channel RSSI and a hexadecimal preview of LoRa packets. Frequency,
  SF, bandwidth, coding rate, preamble, sync word, CRC and IQ polarity must
  match the transmitter.
- SPECTRUM: channel energy across the selected band, or 1 MHz either side of
  the centre frequency. It is an RSSI sweep, not decoded traffic.
- BEARING: RSSI in 36 magnetic-heading bins. This is an experimental plot,
  not a validated transmitter bearing.
- GFSK: receive-only FSK with bitrate, deviation, bandwidth and sync settings.
- POCSAG: receive-only paging. OPTIONS selects baud and NORMAL or INVERTED
  polarity. The BAUD picker offers 1200 and 2400. For a 512 baud channel, use
  PAGER RECON and its 512N or 512I probe. Keep message text out
  of shared notes and examples.

SETUP/S and OPTIONS/O expose settings for the current mode. BAND/B chooses a
frequency or spectrum span. SEND/T sends one typed LoRa packet with DIRECT on;
SPECTRUM, GFSK and POCSAG do not transmit. MARK/J saves an observation to NOTES.
REC/E controls lab recording. HOLD/H freezes the plot; CLEAR/X clears it.

## Notes

NOTES replaces the old Journal launcher app. NEW/N creates a Markdown note on
the SD card. FIND/F filters the list; OPEN or Enter opens a note. EDIT/E changes
it. DONE saves the editor. INSERT adds a checklist, GPS, heading, radio, sensors
or a map picture. A map picture is saved beside the note, including in a newly
created note.

Notes live under `/sdcard/notes`. The editor holds up to 16 KiB of text; the list
indexes up to 200 notes. Keep an SD card fitted for saved notes and pictures.
MAP opens the saved place; GO TO opens COMPASS guidance when the note has a
place. A saved attachment describes the reading at the time it was inserted.
It does not update as the receiver moves.

REC still offers radio metadata and sensor CSV recording. Its older Journal
view is a recorder page, not the NOTES editor. The recorder's files under
`/sdcard/journal` include `entries.bin`, `notes.md` and `samples.csv`.
Radio selection does not tune or start an idle receiver. MIX-RF inputs require
that receiver's observation mode to be running. Unavailable readings are not
measured zero. Without valid time, recordings use an uptime stamp.

## Runtime budget

DIRECT pauses Mesh use of the LoRa radio. With DIRECT off, Mesh can receive
in the background. P25 and ADSB on the LR2021 yield when a lab or experiment
needs that chip. P25 shows LR2021 IN USE ELSEWHERE while it waits.

CAL/K opens the compass calibration guide. Follow screen up, USB edge down,
right edge down, top edge down, left edge down, screen down, then figure eight.
The hold bar advances while the board is steady. Movement or missing readings
restarts the current hold. RESTART clears the run; CANCEL keeps the prior
calibration. Rotation pauses during the guide. Follow the save result on screen.
Recalibrate when the keyboard or nearby magnetic hardware changes.

EXPAND/V in BEARING opens the detailed compass. BACK, V or Escape returns to
Labs. VIEW/B selects signals or sensors. MARK/J keeps the observation. The
heading is magnetic; COMPASS is the app for true-north and GO TO guidance.
COMPASS keeps the heading moving smoothly as the board is raised.
The received-packet dots show the board heading near reception. They are not
transmitter locations. A mesh peer can be relayed rather than nearby.

## Experiments

Open RADIO > EXPERIMENTS. Select a trial and press Enter. START/S starts or
stops it. RADIO/R chooses its receiver; OPTIONS/O configures the trial. One
experiment runs at a time. NOTE/N saves its readout to NOTES on SD.

- **RS41 SONDES** receives Vaisala weather balloon signals in 400-406 MHz
  on the LR2021.
  OPTIONS > RECEIVER selects FREQ MHz or PRESET MHz. TRACKS sets KEEP HOURS
  and SHOW ON MAP. SONDES lists heard sondes; open one for its track and
  RECOVER guidance in COMPASS when it has a position.
- **LR433** receives supported tire-pressure, weather and meter sensors at
  315-928 MHz on the LR2021. OPTIONS offers BANDS, DWELL, UNITS and OOK GAIN.
  SENSORS opens heard devices; open one for reading history and a name.
  A detected signal does not guarantee a supported sensor decode.
- **PAGER RECON** looks for POCSAG paging channels on the LR2021 from 150 MHz.
  OPTIONS > CHANNELS selects a plan or one frequency. FREQUENCY sets that
  channel; DWELL sets time on a live channel; PROBE sets baud and polarity.
  AUTO tries probes. N means normal polarity and I means inverted, for example
  1200N or 1200I. Set ONE FREQ and the matching probe for a known channel.

PAGER RECON > OPTIONS > RECEIVE offers **NATIVE STREAM**, **PACKET** and
**SIGN STREAM**. NATIVE STREAM is the 2.9.1 default and uses the selected probe's
baud. PACKET uses sync captures. SIGN STREAM searches streamed FSK signs for
POCSAG rates. The receiver continues after the first sync and uses full receive
gain. Frequency, channel plan, dwell, probe and receive method are kept after
reboot and update. MESSAGE TEXT is off by default. Do not copy message text
or personal data into shared examples.

POCSAG is also available in FM and LORA LABS. The LR2021's receiver clock
settles when receive starts. `lora lrclk` shows the saved clock choice;
`lora lrclk tcxo` selects the default TCXO and `lora lrclk xtal` selects the
older crystal setting. `lora lrstat` reports the active clock, regulator,
chip errors and calibration status. Run these in TERMINAL or a console.

## Mesh INSPECT and LINK SURVEY

In MESH, open a node's details and choose INSPECT. TRACE shows the route;
STATUS and TELEM request the node's status and telemetry. LOGIN uses the
password entered in OPTIONS; a blank password is a guest login. A node must
support and permit the request. OPTIONS also sets timeouts and intervals.
LakeShark answers telemetry requests according to its peer permissions.

Open LINK > SURVEY, or press V in LINK. If SCAN WI-FI is on, first turn both
Wi-Fi STA and AP off. In TERMINAL, `wifi leave` stops STA and `wifi off` stops
the AP. A connected STA also counts as on. START/S begins a walk;
STOP/S ends it. SORT/R changes ordering; DETAIL/D or Enter opens an observation.
OPTIONS sets SCAN WI-FI, LISTEN BLE, SCAN INTERVAL, ONLY WITH GPS FIX and KEEP
SESSIONS. Wi-Fi, BLE and ONLY WITH GPS FIX are on by default, with a 10-second
interval and 10 kept sessions. Turn ONLY WITH GPS FIX off to record without
a position. Stop the session before changing options.

Each walk saves a CSV under `/sdcard/lakeshark/survey`. Entries include the
heard Wi-Fi or BLE device, first and last sighting, best signal and position
when a fresh GPS fix exists. Best-signal position is where the receiver was,
not the device's location. Random BLE addresses can change. Keep device names,
addresses and locations out of public examples.

## MixRF bring-up with the keyboard attached

1. Install LakeShark through P4U. If the keyboard prevents USB installation,
   disconnect it for flashing, then reconnect it.
2. Open RADIO > MIX-RF and choose PROBE. Stop the monitors before retrying a probe.
3. Choose MONITOR for CC1101 energy, 2.4 SCAN for nRF24 activity, or NFC for
   NFC-A presence. Use STOP ALL before disconnecting the keyboard.
4. Use MARK to keep a selected observation in NOTES. Use REC for continuous
   sensor and radio metadata recording.

See [Keyboard radio monitor](MIX_RF.md) for the limits of each mode. A successful
probe identifies compatible registers; it does not prove useful RF reception.

### Subsequent firmware updates

2.9.1 uses two app slots and supports signed updates over WiFi with rollback.
From 2.8 or earlier, install once through USB or the web flasher. A full install
rewrites internal storage and keeps settings and saved Wi-Fi. Later updates
use SYSTEM > UPDATE. See [Updates and calls](TDP4_UPDATES_AND_CALLS.md).
