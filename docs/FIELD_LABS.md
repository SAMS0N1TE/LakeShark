# LoRa Labs and Journal

The launcher groups apps under Radio, Field, System and User. Radio contains P25,
FM, ADS-B, Waterfall, Mesh and LoRa Labs. Field contains the recorder, map, GPS and
Journal. A group pages after six apps; it does not shrink the existing controls.

## LoRa Labs

DIRECT off leaves Mesh in control. DIRECT on waits for Mesh to finish using the
SX1262, saves its configuration and receive state, and applies the lab settings.
Leaving the app requests restoration. A failed restoration keeps the mesh held
instead of resuming it on the wrong frequency. Console radio commands and
waterfall acquisition refuse while Labs owns the radio.

Modes:

- Packets: live channel RSSI and a bounded hexadecimal preview of received LoRa
  packets. SF, bandwidth, coding rate, preamble, sync word, CRC and IQ polarity
  must match the transmitter.
- Spectrum: RSSI across the selected centre frequency plus/minus 1 MHz. The
  existing SX1262 scan primitive runs on the field worker.
- Bearing: measured RSSI in 36 magnetic-heading bins. Hold the board level.
  This is an experimental polar plot, not a validated direction estimate.

SETUP exposes frequency, modulation, power and packet settings. SEND transmits
one typed packet, only with DIRECT enabled and outside spectrum mode. There is
no repeating transmitter. MARK saves the observation with GPS and motion data.
The chip's programmable range does not establish the fitted module's antenna
and matching-network coverage.

## Journal

NEW creates a titled note; EDIT changes its text while preserving its original
sensor attachment. A note can contain up to 767 characters. The recent list holds
48 entries. Older saved entries remain in the SD archive. MAP centres the map on
the selected entry when its attached GPS fix was valid.

Open a note, then choose SENSORS / ENTRY to inspect its saved radio, GPS and
nine-axis attachment. SENSORS / LIVE on the list shows the current readings.
Sub-GHz WATCH can bookmark a selected capture directly into Journal; its note
separates historical capture timestamps from the sensors sampled at bookmark time.

Choose an optional radio attachment: Mesh, LoRa Labs, RTL, Wi-Fi or Bluetooth.
GPS and all nine IMU axes are sampled independently of the radio. The
CC1101 input observes the receive-energy monitor after opening MIX-RF and
enabling MONITOR. nRF24 and NFC inputs use the MIX-RF snapshot providers after
their hardware probes pass and the corresponding observation mode is enabled.
Selecting an input does not tune it, start a Wi-Fi scan or start transmitting.

RECORD writes a one-second sample stream while other apps are open. Notes remain
usable without SD; unsaved notes are marked RAM. Editing and saving a RAM note
after an empty SD journal directory is available persists it. If a card with a
different existing archive is inserted during the session, that archive stays
read-only and notes remain in RAM; export or transcribe those notes before
restarting with the card fitted. Hot-swapping archives is not supported. The bounded RAM list refuses new notes when
it would evict an unsaved note.

Files under `/sdcard/journal`:

- `entries.bin`: versioned, checksummed recovery records. An incomplete trailing
  record is discarded before the next append; earlier valid records remain.
- `notes.md`: readable note history, including edited versions.
- `samples.csv`: GPS, time, accelerometer, gyro, magnetometer and radio samples.

Recovery and sample logs each stop accepting new data at 16 MiB. Copy archives
off the card before removing or rotating them. The app never silently deletes
an archive. GPS, IMU, radio and signal validity flags distinguish unavailable
measurements from measured zero. Unavailable SNR is `nan` in CSV and omitted
from Markdown; channel RSSI alone does not measure packet SNR. Heading is magnetic and uncompensated for tilt;
it is not true north. Without valid GNSS time, entries retain the uptime stamp.

## Runtime budget

Labs includes CRC, IQ inversion, public/private sync, plot hold/clear, and
compass calibration controls. DIRECT gives Labs the SX1262 and pauses Mesh
reception; the screen marks this in yellow. With DIRECT off, Mesh continues
in the background and its normal notification banner remains available.

CAL opens a guide with a board picture, animated arrows, and one instruction
at a time: screen up, USB edge down, right edge down, top edge down, left edge
down, then screen down. Each position needs twenty consecutive valid readings
at 10 Hz while the board is steady. A hold bar fills, the guide advances, and
the final fit/save happens automatically. Sensor interruptions or movement
reset only the current hold. RESTART clears the run; CANCEL keeps the prior
calibration. Screen auto-rotation pauses during the run so the guide stays put.
The initial screen-up hold establishes the board's Z polarity; edge directions
use the same mounted screen axes as normal auto-rotation.

The fit bounds the contribution from each gravity region so a long pause on
one side cannot exhaust the sample budget or swamp the other positions.
Flat-only, stationary, insufficient, and poor-fit data are
rejected while the previous calibration remains usable. Successful calibration
uses two versioned, checksummed SD slots so an interrupted newer save preserves
the prior generation. The screen distinguishes a saved calibration from one
held only in RAM. Recalibrate when the keyboard or nearby magnetic hardware
changes. Compass use still requires a level board; tilt compensation and full
soft-iron correction are not implemented. P4 heading direction was checked
against user-provided north/east/south references; this is not a precision calibration.
The fit follows the sphere model in
[NXP AN4246, section 6](https://www.nxp.com/docs/en/application-note/AN4246.pdf).

The compass dial rotates beneath a fixed yellow physical-board-top marker.
Red N indicates magnetic north. EXPAND COMPASS at the bottom of Bearing mode,
or V, opens the detailed view. BACK, V or Escape returns to Labs. VIEW/B
switches between detected signals and sensors; MARK/J attaches a Journal observation.
The expanded view displays actual field strength, turn rate, tilt, temperature
and GPS coordinates/altitude when valid. Opening uses a short ease-out zoom;
the heading follows the shortest path across north. Global app navigation stays available.

The signals view shows recent received Mesh/LoRa packets and up to three Mesh
peers with verified advertisements. A peer may be relayed and is not necessarily
nearby. Unknown device identities are not inferred. Packet observations use a
fixed eight-entry RAM ring and do not create SD files. Mesh polling reports
the latest observation when its receive counter advances; it is not a packet
sniffer and can coalesce multiple arrivals. On entering observation, old counts
establish a baseline rather than creating new detections. Cyan dots show board
heading sampled near packet reception, with radius based on packet RSSI. They
are not transmitter locations or a direction-finding result. Idle energy samples
do not add dots. The sensor view hides this overlay.

Top and bottom text uses the rounded-panel geometry, including the Labs and
Journal status rows. A transient IMU bus collision retains the last coherent
sample for less than 350 ms; longer failures become unavailable.

The UI copies fixed-size snapshots and draws through the same button, panel and
palette functions as Mesh. It performs no SD access or radio transactions.
The field worker polls received packets every 25 ms while direct packet/bearing
mode is active. Sensor and graph snapshots update at at most 10 Hz; a separate,
lower-priority journal worker writes SD. Both workers use PSRAM stacks on P4;
they must not call SPI flash or NVS APIs. Notes and command queues have explicit limits. GPS/IMU sampling
pauses when neither app is visible and recording/direct control are off.

Map aircraft ease between received fixes for 750 ms, without extrapolating past
the latest fix. Only overlays change; tile rasterisation is not invalidated by
the animation. Node ages use the mesh timestamp clock. Aircraft age follows the
position timestamp, not unrelated messages from the same aircraft.

## MixRF bring-up with the keyboard attached

1. Install the development image once using the known working USB arrangement
   with the attachment disconnected. Reconnect the keyboard for actual tests.
2. Join the device to the local Wi-Fi network in LINK. The existing HTTP server
   also works in its access-point mode. Poll `http://DEVICE_IP/debug/field` to
   observe ownership, counters, queue drops and GPS/IMU validity without serial.
   The endpoint is read-only and returns no note contents or coordinates.
3. Export journal files through the existing Wi-Fi file browser. Drive the app
   using the fitted keyboard/touchscreen. This provides configuration, counters
   and captured evidence without reopening a USB serial port.
4. Before enabling MixRF pins, compare the fitted keyboard revision against
   LilyGO's schematic and driver configuration. The commented GPIO placeholders
   in the LakeShark variant are not a verified pin map. Include expander-controlled
   selects, CE, RF-switch selection and power in that comparison.
5. Establish inactive chip selects and nRF CE low before any transfer. Use the
   shared SPI bus lock across select/transfer/deselect. Probe one device at a time:
   CC1101 part/version and restored register round-trip; nRF restored address
   round-trip (STATUS alone is not identification); ST25 identity and an owned
   tag. All-zero/all-one bus reads are failures.
6. Verify reception with known signals and retain the evidence in Journal. A
   successful register probe is not a successful receiver. Test concurrent LoRa,
   keyboard, touch, Wi-Fi and SD use, then rapid app switching and power cycles.
   Measure queue drops, packet loss, UI frame time and stack high-water marks.

The vendor's [board repository](https://github.com/Xinyuan-LilyGO/T-Display-P4)
and [keyboard examples](https://github.com/Xinyuan-LilyGO/T-Display-P4/tree/main/main/keyboard_examples)
are the hardware references. The keyboard's USB flashing problem is not yet
diagnosed; do not assume it is a boot strap or USB routing fault without evidence.

### Subsequent firmware updates

Wi-Fi diagnostics do not make the current factory-only flash layout OTA-capable.
ESP-IDF requires OTA application slots and an OTA data partition for the normal
update/rollback workflow. See [Espressif's OTA documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/system/ota.html).

A candidate 16 MiB layout preserves NVS and the existing SPIFFS offset:

| Partition | Offset | Size |
| --- | --- | --- |
| NVS | 0x9000 | 0x6000 |
| PHY | 0xF000 | 0x1000 |
| OTA 0 | 0x10000 | 0x480000 |
| OTA 1 | 0x490000 | 0x480000 |
| Existing storage | 0x910000 | 0x400000 |
| Existing coredump | 0xD10000 | 0x10000 |
| OTA data | 0xD20000 | 0x2000 |

This is a proposal, not an activated partition change. Before installing it:
back up flash, verify actual flash size and image fit, prepare the updater and
rollback-enabled bootloader, and prove interrupted-upload recovery. The measured
existing app is about 2.22 MB; each proposed slot is 4.5 MiB. A running app cannot
fix a ROM-mode electrical conflict through Wi-Fi. One initial working flash
arrangement is still needed to install the update path.
