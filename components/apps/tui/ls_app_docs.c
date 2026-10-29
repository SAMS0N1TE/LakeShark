/* See ls_app_docs.h.
 *
 * These describe the firmware as it is, not as it should be. Several apps
 * here keep nothing and use no position where they obviously could - ADS-B
 * does not record a contact although it
 * already navigates by position. Those entries say so plainly. A contract that
 * flattered the build would be worth nothing to the operator holding it, and
 * the honest version is also the to-do list. */

#include "ls_app_docs.h"
#include "ls_textpack.h"

const ls_app_doc_t ls_doc_home = {
    .purpose = LS_TXT(doc_home_purpose, "The directory. Every app on the board, the four main ones "
               "along the top and the rest as tiles."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_p25 = {
    .purpose = LS_TXT(doc_p25_purpose, "Listen to a P25 trunked system: follow its control channel, "
               "decode voice, and watch which talkgroups are active. Profiles "
               "for a system are loaded from the card."),
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_p25_record_note, "N keeps one site hit - the frequency, NAC, talkgroup and "
                   "unit, the signal it was heard at, and the sync and voice "
                   "counts for the session"),
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_p25_gps_note, "where this receiver was when you kept the note, which is not "
                "where the transmitter is - a trunked site can be tens of "
                "kilometres away"),
};

const ls_app_doc_t ls_doc_fm = {
    .purpose = LS_TXT(doc_fm_purpose, "Listen to one analogue channel - narrow or wide FM, AM, or a "
               "pager and ACARS decoder - or sweep a band to find what is "
               "transmitting on it."),
    .records = LS_APP_RECORDS_AUTOMATIC,
    .record_note = LS_TXT(doc_fm_record_note, "CALLS recording, enabled by default and configurable, saves "
                   "squelched audio as WAV plus metadata in "
                   "/sdcard/lakeshark/calls. Optional SAME logging appends voted "
                   "alert headers to /sdcard/lakeshark/alerts/same.log. Pager and "
                   "ACARS payloads are not archived by these options"),
    .gps = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_fm_gps_note, "CALLS metadata carries the receiver fix only when it is no "
                "more than five seconds old. The optional SAME header log "
                "contains no receiver position"),
};

const ls_app_doc_t ls_doc_adsb = {
    .purpose = LS_TXT(doc_adsb_purpose, "Aircraft heard directly off 1090 MHz, as a list and as a "
               "mini map that follows them: all in view, the selected one, "
               "or the one nearest you."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_adsb_gps_note, "SET HOME remembers GPS, decimal coordinates or the map center. "
                "The offline mini map rests on saved home, or a fresh GPS fix "
                "when home is unset, and FOLLOW OFF keeps it there. FULL MAP "
                "opens the SD map controls."),
};

const ls_app_doc_t ls_doc_falls = {
    .purpose = LS_TXT(doc_falls_purpose, "A waterfall over whichever receiver is selected: what the "
               "band looked like over the last minute, not just now."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_cell = {
    .purpose = LS_TXT(doc_cell_purpose, "Compare cellular downlink power with a learned site baseline. "
               "LTE search lists cells with PCI, power and MIB; HIGH RATE separately checks MIB. "
               "Network identity and SIB are not decoded; RF changes do not "
               "identify a simulator."),
    .records = LS_APP_RECORDS_AUTOMATIC,
    /* Named exactly, because it is the one app that keeps a lot and none of
       it reaches the JOURNAL screen or notes.md. */
    .record_note = LS_TXT(doc_cell_record_note, "detections to its own /sdcard/cell/lte.jsonl, which the "
                   "JOURNAL screen does not show"),
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_cell_gps_note, "a capture carries the fix it was taken at, and a transmitted "
                "report carries one only when the fix is under five seconds "
                "old and HDOP is five or better"),
};

const ls_app_doc_t ls_doc_mesh = {
    .purpose = LS_TXT(doc_mesh_purpose, "A MeshCore node: hear other nodes, relay for them, and send "
               "short messages over LoRa without any infrastructure."),
    .records = LS_APP_RECORDS_AUTOMATIC,
    .record_note = LS_TXT(doc_mesh_record_note, "node sightings with the position you heard them from, "
                   "exported as GPX waypoints beside the track - not journal "
                   "entries"),
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_mesh_gps_note, "a sighting stores where you were when you heard it, and "
                "bearing and range to each peer come from that"),
};

const ls_app_doc_t ls_doc_labs = {
    .purpose = LS_TXT(doc_labs_purpose, "The measurement bench: LoRa and FSK packet capture, a "
               "spectrum trace, and a bearing plot that bins signal strength "
               "against magnetic heading."),
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_labs_record_note, "MARK keeps one observation - the frequency, signal and "
                   "packet counts at that moment, with the position and "
                   "motion the board had"),
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_labs_gps_note, "attached to a kept observation; the plot itself is magnetic "
                "heading, which is the compass and not the receiver"),
};

const ls_app_doc_t ls_doc_experiments = {
    .purpose = LS_TXT(doc_experiments_purpose, "Radio experiments on the LoRa chip that may become apps of "
               "their own: each says how far along it is and what it listens "
               "for, and shows what it measures while it runs."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_experiments_gps_note, "RADIOSONDE gives a balloon's range and bearing from saved "
                "home, or from a fresh GPS fix when home is unset."),
};

const ls_app_doc_t ls_doc_journal = {
    .purpose = LS_TXT(doc_journal_purpose, "The field notebook. Notes you write, and the observations "
               "other apps kept, each with the sensor snapshot taken at the "
               "time. Exports to notes.md on the card."),
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_journal_record_note, "a title and up to 767 characters of text, plus the time, "
                   "position, motion and radio readings as they were when it "
                   "was kept"),
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_journal_gps_note, "every entry carries the fix the board had, or records that "
                "it had none - a position more than a moment stale is dropped "
                "rather than attached"),
};

const ls_app_doc_t ls_doc_notes = {
    .purpose = LS_TXT(doc_notes_purpose, "A field notebook: write notes with a real cursor, and insert "
               "the time, a GPS fix, the heading, a map picture or what a "
               "radio is hearing right where you are typing."),
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_notes_record_note, "each note as a Markdown file in /sdcard/notes, with map "
                   "pictures saved beside it as PNG; trash moves them to "
                   "notes/trash and deletes nothing"),
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_notes_gps_note, "a GPS line is written only from a fix under three seconds "
                "old, and says there was no fix otherwise"),
};

const ls_app_doc_t ls_doc_compass = {
    .purpose = LS_TXT(doc_compass_purpose, "A tilt-compensated compass that reads true north from the "
               "World Magnetic Model, follows a saved place, finds a "
               "transmitter by signal strength on any radio, and levels."),
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_compass_record_note, "bearings and fixes you save go into NOTES as BEARING and "
                   "FIX lines with the place they were taken from"),
    .gps      = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_compass_gps_note, "the fix sets declination, places each bearing and gives the "
                "distance to a target; without one the compass reads "
                "magnetic north"),
};

const ls_app_doc_t ls_doc_rfsurvey = {
    .purpose = LS_TXT(doc_rfsurvey_purpose, "Walk or drive and build a map of how loud each channel of a "
               "band is where: a heat map of RF activity over MAP, from any "
               "receiver that gives a level."),
    .records     = LS_APP_RECORDS_AUTOMATIC,
    .record_note = LS_TXT(doc_rfsurvey_record_note, "every pass over the channels at a new place: each channel's "
                   "level, noise floor and whether it was busy, with the place "
                   "and time, in a .lsrf file; EXPORT adds .csv and .geojson"),
    .gps      = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_rfsurvey_gps_note, "rows are kept every few metres walked and binned into square "
                "cells on the ground; without a fix levels show live and rows "
                "wait (rfsim on the console stands in indoors)"),
};

const ls_app_doc_t ls_doc_fox = {
    .purpose = LS_TXT(doc_fox_purpose, "Find a transmitter with one receiver: turn with the board to "
               "get a bearing from signal strength and the compass, take "
               "bearings from several places, and see where they cross."),
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_fox_record_note, "each bearing you TAKE, with its place, spread and level, as "
                   "a line in a hunt .csv on the card"),
    .gps      = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_fox_gps_note, "places every bearing; the fix and its error ellipse are worked "
                "out from where the bearings were taken"),
};

const ls_app_doc_t ls_doc_rec = {
    .purpose = LS_TXT(doc_rec_purpose, "Capture what a radio is receiving to the card: an OOK "
               "recorder that writes Flipper-compatible sub-GHz files, and a "
               "one-per-second sensor log. REPLAY loads a capture from FILES with "
               "frequency, pulse preview and nominal power; PLAY ONCE sends it."),
    .records     = LS_APP_RECORDS_AUTOMATIC,
    .record_note = LS_TXT(doc_rec_record_note, "a samples.csv row every second while recording, with "
                   "position, motion and signal; the captured waveform itself "
                   "is a separate file and carries no position"),
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_rec_gps_note, "written into each samples.csv row; the capture file itself "
                "is not stamped, which is a gap"),
};

const ls_app_doc_t ls_doc_files = {
    .purpose = LS_TXT(doc_files_purpose, "What is on the card. Open a .sub capture to see its pulses, "
               "send it to a Flipper or open REPLAY in RECORD; read text files; delete "
               "what is no longer wanted."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_subghz = {
    .purpose = LS_TXT(doc_subghz_purpose, "Watch the licence-free sub-GHz bands: "
               "recognise a pattern that repeats, count how often it is "
               "heard, decode it where the encoding is known, and replay a "
               "capture within the transmit limits."),
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_subghz_record_note, "one selected pattern - its edges, span, how many times it "
                   "was seen, when it was first and last heard, and the "
                   "decoded payload if there is one"),
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_subghz_gps_note, "a kept pattern carries the fix the board had when you kept "
                "it, not where the pattern was first heard"),
};

const ls_app_doc_t ls_doc_mixrf = {
    .purpose = LS_TXT(doc_mixrf_purpose, "The short-range radios on the keyboard board: NFC at "
               "13.56 MHz, a 2.4 GHz survey, and the sub-GHz transceiver, "
               "for observation. NFC SCAN sends active card requests; "
               "FIELD passively watches external reader fields."),
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_mixrf_record_note, "one observation per radio - a card seen, a field "
                   "detected, a 2.4 GHz sweep with its busiest channel, or "
                   "the energy on a sub-GHz channel"),
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = LS_TXT(doc_mixrf_gps_note, "attached to a kept observation, so a survey can be placed on "
                "the map afterwards"),
};

const ls_app_doc_t ls_doc_diag = {
    .purpose = LS_TXT(doc_diag_purpose, "What the hardware is doing: memory, temperature, the "
               "SD card, the health of every radio including the GPS "
               "receiver's own wire counters, and the error log: each crash, "
               "watchdog or brownout, saved at the next boot with the last "
               "error lines before it."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_terminal = {
    .purpose = LS_TXT(doc_terminal_purpose, "The serial console's commands, typed on the board: crumb, "
               "heap, safemode and the rest, with their output on the screen "
               "when no laptop is plugged in."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_update = {
    .purpose = LS_TXT(doc_update_purpose, "Updates over WiFi: asks the LakeShark update server for a "
               "newer build, checks LakeShark signed it, writes it to the "
               "other half of the flash and restarts into it. A build that "
               "fails to start puts the last one back."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_settings = {
    .purpose = LS_TXT(doc_settings_purpose, "Volume, brightness, mute and screen lock, with menus for "
               "the display, sounds and alerts, and the keyboard and USB."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_tiles = {
    .purpose = LS_TXT(doc_tiles_purpose, "Install and manage CartoCore map regions on the SD card. Download over WiFi or receive a file from a laptop. OPEN displays the selected map."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_map = {
    .purpose = LS_TXT(doc_map_purpose, "An offline vector map from a .pmtiles archive on the card, "
               "with your position on it and anything the journal recorded a "
               "location for."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps      = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_map_gps_note, "the map follows a fresh fix; one more than ten seconds old "
                "is hidden rather than drawn in the wrong place"),
};

const ls_app_doc_t ls_doc_gps = {
    .purpose = LS_TXT(doc_gps_purpose, "The position receiver: fix quality, which satellites are up "
               "and which are being used, and a track recorder that writes "
               "GPX."),
    .records     = LS_APP_RECORDS_AUTOMATIC,
    .record_note = LS_TXT(doc_gps_record_note, "a track point once you have moved ten metres or thirty "
                   "seconds have passed, whichever comes first, to "
                   "/sdcard/lakeshark/track.log - both thresholds adjustable"),
    .gps      = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_gps_gps_note, "this is the position receiver; SILENT, SEARCHING and FIX are "
                "three different states and the screen never conflates them"),
};

const ls_app_doc_t ls_doc_radios = {
    .purpose = LS_TXT(doc_radios_purpose, "Every radio on the board and whether it is powered: turn one "
               "off to save current or to stop it contending for the bus."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_link = {
    .purpose = LS_TXT(doc_link_purpose, "Join Wi-Fi networks, connect a Flipper running LakeShark, "
               "or record a passive Wi-Fi/BLE Survey. Bluetooth OPTIONS sets "
               "the head pin, name filter and telemetry rate."),
    .records = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_link_record_note, "Survey saves observed Wi-Fi/BLE identities and signal "
                   "strength to SD CSV, with positions when a fresh fix is available."),
    .gps = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_link_gps_note, "Survey can require a fresh GPS fix before recording observations."),
};

const ls_app_doc_t ls_doc_music = {
    .purpose = LS_TXT(doc_music_purpose, "Play 16-bit PCM WAV tracks with a separate library, touch transport, volume-adjusted output history and an optional warm tone. Landscape uses a side-by-side layout."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_drones = {
    .purpose = LS_TXT(doc_drones_purpose, "Receive-only early warning for drone links: ELRS 900 and 2.4 GHz "
               "control, 900 MHz hoppers, 2.4 GHz wideband links, 5.8 GHz analog "
               "FPV video with a HackRF, and BLE Remote ID. Nothing is sent or "
               "decoded; each link is a strength and a trend, and only Remote ID "
               "is a confirmed identity. RSSI ranks strength, not distance."),
    .records = LS_APP_RECORDS_NOTHING,
    .gps = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_drones_gps_note, "Your GPS fix provides distance and true bearing to a reported "
                "drone position. Without both fixes, radar placement is "
                "illustrative RSSI ranking. Entries expire after 60 seconds."),
};

const ls_app_doc_t ls_doc_sweep = {
    .purpose = LS_TXT(doc_sweep_purpose, "Receive nearby BLE and Wi-Fi broadcasts. Signatures and names "
               "are unverified hints. LIST ranks observed contacts; HUNT "
               "follows RSSI and hands a selected address to FIND. Mute and "
               "your selected speaker volume are respected."),
    .records = LS_APP_RECORDS_MANUAL,
    .record_note = LS_TXT(doc_sweep_record_note, "Optional SD alert logging: uptime, MAC, category and RSSI; "
                   "no location or broadcast payload. Off by default."),
    .gps = LS_APP_GPS_NAVIGATES,
    .gps_note = LS_TXT(doc_sweep_gps_note, "GPS contributes movement counts only, never retained "
                "coordinates. Remote ID bearing/distance is derived from "
                "live fixes."),
};

const ls_app_doc_t ls_doc_tmach = {
    .purpose = "Keeps the last half minute of whatever receiver is running, so WHAT WAS "
               "THAT? saves the moment you just missed, and plays a saved clip back "
               "through the pager decoders. TIMELINE lists what every decoder reported.",
    .records = LS_APP_RECORDS_MANUAL,
    .record_note = "WHAT WAS THAT? writes an I/Q clip and a JSON sidecar (REC's fields "
                   "plus rate, format, decoder and GPS) to /sdcard/lakeshark/tmach; "
                   "CLIP EVERY EVENT, off by default, does it for every new event.",
    .gps = LS_APP_GPS_STAMPS,
    .gps_note = "a clip carries the fix the board had when it was saved",
};

const ls_app_doc_t ls_doc_meters = {
    .purpose = "Receive-only list of Itron ERT utility meters (SCM, SCM+, IDM) on "
               "902-928 MHz: ID, reading, use per hour and per day, use as braille "
               "bars, last heard. An SDR listens through REC at one centre or a "
               "scan; the LR2021 runs LR433's ERT session. Meters heard by "
               "SUB-GHZ WATCH or LR433 appear too. Readings stay in memory; "
               "only pins and names are a setting on the card (meters.json).",
    .records = LS_APP_RECORDS_NOTHING,
    .gps = LS_APP_GPS_UNUSED,
};

const ls_app_doc_row_t ls_app_docs_all[] = {
    { "tmach", &ls_doc_tmach },
    { "meters", &ls_doc_meters },
    { "drones", &ls_doc_drones },
    { "sweep", &ls_doc_sweep },
    { "music", &ls_doc_music },
    { "home",    &ls_doc_home    },
    { "p25",     &ls_doc_p25     },
    { "fm",      &ls_doc_fm      },
    { "adsb",    &ls_doc_adsb    },
    { "falls",   &ls_doc_falls   },
    { "cell",    &ls_doc_cell    },
    { "mesh",    &ls_doc_mesh    },
    { "labs",    &ls_doc_labs    },
    { "experiments", &ls_doc_experiments },
    { "journal", &ls_doc_journal },
    { "notes",   &ls_doc_notes   },
    { "compass", &ls_doc_compass },
    { "rfsurvey", &ls_doc_rfsurvey },
    { "fox",     &ls_doc_fox     },
    { "rec",     &ls_doc_rec     },
    { "subghz",  &ls_doc_subghz  },
    { "files",   &ls_doc_files   },
    { "mixrf",   &ls_doc_mixrf   },
    { "diag",    &ls_doc_diag    },
    { "terminal", &ls_doc_terminal },
    { "update",  &ls_doc_update   },
    { "set",     &ls_doc_settings},
    { "map",     &ls_doc_map     },
    { "tiles",   &ls_doc_tiles   },
    { "gps",     &ls_doc_gps     },
    { "radios",  &ls_doc_radios  },
    { "link",    &ls_doc_link    },
};

const int ls_app_docs_count =
    (int)(sizeof(ls_app_docs_all) / sizeof(ls_app_docs_all[0]));


