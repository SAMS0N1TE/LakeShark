# WIFI SURVEY

A site survey: which access points are around, how crowded each channel is
and how each signal moves, and RECORD, the walk recorder that writes every
access point and Bluetooth device heard, with where you were, as WiGLE CSV.
Passive: it reads scan results, beacons and advertisements and never joins
or transmits on its own.

| Source | Gives |
|---|---|
| `lap_wifi_scan` / `lap_wifi_aps` | SSID, channel, RSSI, open or secured (up to 16 per scan, every 6 s) |
| `lap_wifi_capture_*` (API 2.7), hopping 1-13 | BSSID and security suite (WPA2, WPA3, WPA2/3, OWE, WPA2-EAP, WEP, open) from beacons; frames and airtime per channel |

Either source is enough. A scan row without a BSSID is joined to the beacons
of the same SSID on the same channel when capture hears them.

## Tabs

- ACCESS POINTS: strongest first; SSID, security, channel, RSSI, BSSID, how
  it was heard, age and a signal bar. Tap a row to select it, tap again for
  its history.
- CHANNELS: 2.4 GHz as an overlap plot filling the panel: one hump per AP
  over its channel and two either side, as tall as it is strong. Every hump
  is drawn faint; the strongest carry their names on their own peaks (placed
  strongest first, never over one another, as many as fit), and the AP picked
  on ACCESS POINTS is white with its name lit. ZOOM (the third key here, or
  Z, + and -) goes ALL 1-13, x2, x3, x4, centred on the view or on the
  channel tapped; zoomed, a sideways drag on the plot pans (`,` and `.` on a
  keyboard) and the channel numbers follow, with a strip under them showing
  where the view sits. Tap a channel on the plot, or its row in the table,
  to list its APs under the plot; tap one of those to pick it. Below: a table
  of APs, overlap and strongest signal per channel, and the 5 GHz channels
  when the scanner reports them. The T-Display's C6 scans 2.4 GHz only; the
  Nano's C5 is dual-band. On a short (landscape) screen this tab gives the
  header's rows to the plot.
- TRAFFIC (capture): frames, airtime (from length and rate; 6 Mb/s when the
  radio does not say), time listened and busy percent per channel.
- HISTORY: the selected AP's RSSI, one column per 2 s, strongest reading in
  each, dots where it was not heard. UP/DOWN pick another AP.
- RECORD: the survey recorder (below). A cassette that turns while it
  records, the GPS and UTC lamps, how many access points and Bluetooth
  devices it has, new ones a minute, rows written, the file, and every device
  in its table; UP/DOWN or a tap picks one, a second tap (or ENTER) opens its
  detail: first seen, best signal and where, and the centre of its sightings.
  The third key is REC/STOP; MORE here is RECORD's options.

Handheld: swipe or `[<]` `[>]` for tabs (a sideways drag on the zoomed
channel plot pans it instead), `[UP]` `[DOWN]`, `[MORE]` for options.
A desk window shows a wide table and clickable tabs. Keys: 1-5 tabs, W save
CSV, S scan now, H channel plan, SPACE pause, R record/stop, Z / + / - zoom
and `,` `.` pan the channel plot.

MORE: scan now, channel plan (hop 1-13, hop 1/6/11, stay), allow link drop,
save CSV, pause, clear, restart capture, record options, close.

## RECORD

The Wi-Fi and Bluetooth survey recorder. It was LINK's SURVEY tab until
firmware 2.23 and moved here unchanged in what it writes, so anything that
read those files reads these:

```
/sdcard/lakeshark/survey/YYYYMMDD_HHMMSS.csv          (_NNN when the name is taken)
WigleWifi-1.6,appRelease=wifi_survey-1.3.0,model=ESP32-P4,...
MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type

/sdcard/lakeshark/survey/YYYYMMDD_HHMMSS_summary.csv
MAC,Type,AddressType,SSID,Samples,EstimatedLatitude,EstimatedLongitude,Weight
```

The rules (`survey_rec.h`, tested in `bench/tests/test_wifi_survey_rec.c`):

- One entry per address: a Wi-Fi BSSID, or a Bluetooth address and its type.
  The strongest sighting is kept with the position it had.
- A position counts only from a GPS fix at most 10 s old. With ONLY GPS FIX
  on (the default) nothing is taken in without one.
- A row is written for the first sighting with a UTC time, again when the
  signal is 6 dB above the last row's, and again 50 m on from the last
  positioned row. AccuracyMeters is HDOP x 5 m, an estimate.
- 4096 devices are held; a full table lets the least recently heard go,
  writing its summary row first. The summary's centre weights every
  fresh-fix sighting by its power. A device counts once a second.
- STOP writes the summary of every device left and then deletes the oldest
  sessions past KEEP SESSIONS (recorder-named files only).

MORE on RECORD, changed while nothing records (SORT any time): WI-FI ROWS,
BLE ROWS, ONLY GPS FIX, SCAN EVERY (3-120 s, the scanner's period for the
whole app), KEEP SESSIONS (1-50), SORT (strongest, newest, name). They are
kept between visits.

What is different from LINK, because an app reaches the radios through the
SDK:

- Wi-Fi rows come from beacons and probe responses heard by frame capture,
  which carry the BSSID and the security suite. A scan result has neither,
  so an access point heard only by scan is not written (the T-Display needs
  the C6 forwarder firmware for capture; the tab says so). Scans add
  sightings of access points whose beacons were heard. The board's Wi-Fi link
  can stay up: LINK needed Wi-Fi off.
- Bluetooth comes from the shared passive scanner (`lap_ble_scan_start`);
  when another owner holds it, RECORD waits and says so.
- Times come from GPS, else from the board's clock once it is set (network or
  RTC). LINK waited for GPS time.
- Rows reach the card every 5 s through `lap_sd` jobs.
- It keeps recording behind HOME as a background service (asked once at
  install). Without it, closing the app ends the session and the last
  seconds may not reach the card.
- AuthMode comes from the beacon's suite (`[WPA2-PSK-CCMP][ESS]`,
  `[WPA3-SAE-CCMP][ESS]`, `[OWE-CCMP][ESS]` ...); 5 GHz channels carry their
  frequency.

A position is where the board heard the strongest signal, not where the
device is. Random Bluetooth addresses rotate. Keep names, addresses and
places out of public examples.

## Files

SAVE CSV writes two files to `/sdcard/wifi_survey`:

```
survey-YYYYMMDD-HHMMSS-aps.csv
ssid,bssid,channel,band,rssi,rssi_min,rssi_max,security,heard_by,first_s,last_s

survey-YYYYMMDD-HHMMSS-channels.csv
channel,mhz,aps,strongest_dbm,frames,airtime_ms,listened_ms,busy_percent
```

`security` is the suite from beacons, or `secured`/`open` from a scan alone.

## Without capture

On the T-Display the C6 needs the forwarder firmware before it can capture:

```
c6 flash /sdcard/lakeshark/c6/ls_c6_net.bin
```

Until then the survey runs on scans (SCAN ONLY in the title) and TRAFFIC says
why it is empty. While the board is joined to a network, hopping would drop
it: the survey listens on the link's channel and MORE > ALLOW LINK DROP asks
first.

## Limits

48 access points on the tabs (the least recently heard makes room), 64
history samples each, the newest scan's 16 results. RECORD holds 4096
devices of its own.

## Simulator

```
LAPSIM_CAPTURE=capture.pcapng lsdk sim wifi_survey
```

The script word `wifi=N` makes a finished scan of N made-up access points
(TEST- names) and `wifiwalk=D` moves their RSSI by D dB, one scan each.
For RECORD, `LAPSIM_HW=wifi,ble,gps,sd` and `gpsfix=LAT,LON,ALT` give it a
receiver and a fix; `hide` and `show` close and reopen the screen.
