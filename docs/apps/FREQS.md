# FREQS

One list of the frequencies worth knowing, for every app. Find one by band,
group, name or how near it is; TUNE hands it to the app that can hear it.
SAVE keeps a frequency from here or from FM, SUB-GHZ, P25, LORA LABS or CELL
WATCH.

| | |
|---|---|
| HOME group | RADIO |
| Radio | None of its own. TUNE opens the app for the entry's mode |
| Card | `/sdcard/freqs/*.csv`; without a card, the built-in set |

## What it does

- Reads every bank (a CSV file) in `/sdcard/freqs` and lists them together,
  or one bank at a time, or the built-in set.
- Shows the entry under the cursor in the big digits with its name, mode,
  group, tone or notes, and how far away it is when it has a place and there
  is a GPS fix.
- Filters by BAND, GROUP, NEAR (within reach of the fix) and FIND (text).
- TUNE goes to the right app with the right mode on the right frequency.
- SAVE writes into `mine.csv`; so does the SAVE TO FREQS row in other apps.
- Edits and deletes in place, imports CHIRP, SDR++, gqrx and `.lscan` files,
  exports the filtered list as CSV or as a scanner list, and loads the NFM
  and P25 entries in view into FM's channel scanner.

The dial in the header shows where the selected entry sits on the spectrum,
1 MHz to 3 GHz. Its needle sweeps only while a bank is being read.

## How to use it

Open **HOME > RADIO > FREQS**.

| Button | Key | Does |
|---|---|---|
| TUNE (app) | T or Enter | Tune the selected entry in the app named on the key |
| SAVE NEW | S | Save a typed frequency, what FM or P25 is on, or a copy of this one into `mine.csv` |
| BANK (name) | K | Choose ALL BANKS, one bank or BUILT-IN; IMPORT A FILE |
| OPTIONS | O | Sort, edit, delete, scanner, export, reload, clear filters |
| BAND | B | ALL, HF (below 30 MHz), VHF (30-300), UHF (300-700), 700-960, L-BAND (960 MHz and up) |
| GROUP | G | One group, with how many entries each has |
| NEAR | N | Only entries that have a place within their radius of the GPS fix, nearest first |
| FIND | F | Text in the name, tags, notes, group, mode, or the start of the MHz. Press again (or Esc) to clear |

- Up / Down move the cursor; F5 / F6 page. Tap a row to select it, tap it
  again (or the big frequency) to tune it. The `^` and `v` strip pages.
- Left / Right or Tab walk the bottom keys; Enter presses the highlighted one.
- NEAR without a fix says **NO FIX** and hides nothing. A fix up to a minute
  old counts. Turning NEAR off puts the list back in its earlier order.
- A save from any app shows a banner: yellow when it was written, red with
  the reason when it was not (no card, for one). Tap it to open FREQS.

### Where TUNE goes

| Mode | App |
|---|---|
| NFM, WFM, APRS, POCSAG, ACARS, AIS | FM, in that mode, on the frequency, which FM then holds |
| AM, USB, LSB, CW | FM in AM; HF and SSB/CW on the HF receiver |
| P25 | P25 on the frequency |
| DMR | P25 on the frequency; DMR/6 there shows Tier II activity (no DMR voice) |
| OOK, FSK | SUB-GHZ: LISTEN HERE on READ, which tunes and starts WATCH as soon as the receiver is up (a few seconds after boot); on the LR2021 it sets READ to OOK or 2-FSK to match |
| LORA | LORA LABS, direct receive on the frequency |
| ADSB, GPS, LTE | ADS-B, GPS, CELL WATCH (each picks its own frequency) |
| NXDN, TETRA, DSTAR, YSF | FM NFM, so the channel can be heard; no decoder |
| APT | FM WFM, to hear the pass; no APT decoder |
| LRPT, VDL2, IRIDIUM | Nowhere: FREQS says what is missing |

The TUNE key names the app before it is pressed, and says NOT HERE when there
is none. A receiver that refuses a frequency says so on the bottom line.

### OPTIONS

| Row | Does |
|---|---|
| SORT | AS LISTED, FREQUENCY, NAME, NEAREST |
| RENAME, CHANGE MODE, CHANGE FREQUENCY | Edit the entry in its file. Only that field changes; the rest of the line, comments and other lines stay as they were |
| COPY TO MINE | Keep a copy in `mine.csv` (the way to change a built-in entry) |
| DELETE | Press twice within six seconds |
| LOAD INTO SCANNER | Press twice: replaces FM's scanner channel list with the NFM and P25 entries in view |
| EXPORT VIEW: CSV / SCANNER | Write what is in view to `/sdcard/freqs/export/<bank>.csv` or `.lscan` |
| RELOAD THE CARD | Read the files again after editing them on a computer |
| CLEAR FILTERS | Band, group, near and find back to everything |

Band, sort, NEAR and the bank are kept across restarts.

## The bank format

A bank is a CSV file in `/sdcard/freqs`, edited with anything that writes
text. The first line names the columns:

```
freq_mhz,mode,bw_khz,name,group,tags,lat,lon,radius_km,tone,notes
156.800,NFM,,MARINE CH16,MARINE,listen,,,,,distress and calling
118.850,AM,25,AEP TWR,AVIATION,CABA SABE,-34.5597,-58.4150,40,,Aeroparque
146.940,NFM,12.5,LU1AA REPETIDORA,AMATEUR,tx-ok,-34.60,-58.38,60,88.5,"shift -0.6, club"
```

| Column | |
|---|---|
| freq_mhz | Required. 0.009 to 6000 MHz |
| mode | NFM WFM AM USB LSB CW P25 DMR NXDN TETRA DSTAR YSF FSK OOK LORA ADSB AIS APRS POCSAG ACARS VDL2 APT LRPT GPS IRIDIUM (and LTE, which CELL WATCH saves). Other tools' spellings (FM, NBFM, C4FM, D-STAR, ADS-B) are understood. Blank: AM on airband and HF, WFM on broadcast FM, NFM elsewhere |
| bw_khz | Optional bandwidth |
| name | Up to 24 characters. Blank: the frequency |
| group | A short category: AVIATION, MARINE, AMATEUR, PMR/LICENSE-FREE, ISM/IOT, LORA/MESH, SATELLITE, WEATHER, BROADCAST FM, BROADCAST AM, PUBLIC SAFETY, RAIL/TRANSPORT, UTILITY, EMERGENCY, NAVIGATION, MINE. Each has its tag and colour on the list; others are listed as `---` |
| tags | Space separated: a city, an ICAO code, `tx-ok`, `listen`, `priority` |
| lat, lon | Where it is used. Both blank: everywhere |
| radius_km | How far it reaches; 50 when a place is given without one |
| tone | CTCSS (`88.5`) or DCS (`D023N`) |
| notes | Free text; the screen shows the first 55 characters |

- The columns can be in any order and extra columns are kept. Without a
  header line, the order above is assumed.
- Quoted fields (`"a, b"`, `""` for a quote), blank lines, `#` comments,
  CRLF line ends and a spreadsheet's byte order mark are all fine.
- Accented letters are shown without their accents (`Policía` as `Policia`).
- A line that cannot be read (no frequency, an unknown mode, half a place) is
  skipped and counted. The header says how many and the first line number:
  `566 of 570, 4 lines skipped (17)`.
- Up to 2000 entries are held at once. A bigger set loads to there and says
  so; it can be split into banks.
- Only bank files belong in `/sdcard/freqs`. Reference tables such as
  `argentina_bands.csv` are not banks and would show as skipped lines.

`mine.csv` is created the first time something is saved.

The full Argentina bank ships as `tools/freqs/argentina.csv` (566 entries);
copy it to `/sdcard/freqs/` on the card.

### Built in

With no card, or with BANK > BUILT-IN, FREQS has a small set every unit
carries: 121.5 and 243 MHz, air-to-air, ACARS, ADS-B, marine 16, 6 and 13,
AIS, CB 9 and 19, PMR446 1-8 (listen only in Argentina), ISS voice and APRS,
APRS Argentina (144.930), 315 and 433.92 MHz, LoRaWAN AU915, Meteor LRPT,
GPS L1 and Iridium. It cannot be edited; COPY TO MINE keeps a copy that can.

## Importing

**BANK > IMPORT A FILE** lists `.csv`, `.json`, `.lscan` and `.txt` files in
`/sdcard/freqs/import` and the card's root. The format is told by what is in
the file, and the result is a new bank named after the file
(`My Radio.json` becomes `myradio.csv`):

| Format | Read |
|---|---|
| FREQS CSV | As above |
| `.lscan` (the scanner's list) | Name, frequency, NFM/P25, place and radius; zone and priority become tags |
| CHIRP CSV | Name, Frequency, Mode, Tone/TSQL/DTCS tones; Duplex and Offset and Comment go to notes |
| SDR++ frequency manager JSON | Every bookmark: its name, frequency, bandwidth and mode; the list name becomes the group |
| gqrx `bookmarks.csv` | Frequency, name, modulation, bandwidth; the first tag becomes the group |

The banner line says what came in: `CHIRP: 48 into repeaters, 2 skipped (line 17)`.

## Saving from other apps

| App | Where | Saves |
|---|---|---|
| FM | OPTIONS > SAVE TO FREQS (NFM, AM, HF, WFM, POCSAG, ACARS) | The carrier and mode |
| SUB-GHZ | BAND: a detection's menu > SAVE TO FREQS; READ: OPTIONS > Save to FREQS | The frequency as OOK |
| P25 | OPTIONS > Save to FREQS | The channel as P25 |
| LORA LABS | OPTIONS > SAVE TO FREQS | The frequency as LORA, FSK or POCSAG |
| CELL WATCH | OPTIONS > SAVE TO FREQS | The carrier as LTE |

Each goes into `mine.csv` with group MINE, a name made of the app and the
frequency (rename it in FREQS), and the app as a tag. User apps and the
console can do the same through the `freq.save` action:

```
call freq.save 433.92 OOK GATE subghz
```

## Console

```
freq banks                          the bank files on the card
freq list [text]                    every entry, numbered, or those matching text
freq save <MHz> <mode> <name...>    add to mine.csv
freq import <path>                  a file in any format above into a new bank
freq export <bank|all|builtin> <path.csv|path.lscan>
freq tune <number|MHz>              tune a numbered entry, or the first on that frequency
```

`freq tune` asks the screen, which does it on its next frame.

## Limits

- Notes longer than 55 characters are shortened on screen; the file keeps
  them whole.
- NEAR uses the place and radius in the file; it does not know terrain.
- A bank larger than 2000 entries cannot be edited from the screen.
- LOAD INTO SCANNER replaces the scanner list; it does not merge.
