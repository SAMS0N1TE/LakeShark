/* See ls_app_docs.h.
 *
 * These describe the firmware as it is, not as it should be. Several apps
 * here keep nothing and use no position where they obviously could - FM does
 * not record a decoded page, ADS-B does not record a contact although it
 * already navigates by position. Those entries say so plainly. A contract that
 * flattered the build would be worth nothing to the operator holding it, and
 * the honest version is also the to-do list. */

#include "ls_app_docs.h"

const ls_app_doc_t ls_doc_home = {
    .purpose = "The directory. Every app on the board, the four main ones "
               "along the top and the rest as tiles.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_p25 = {
    .purpose = "Listen to a P25 trunked system: follow its control channel, "
               "decode voice, and watch which talkgroups are active. Profiles "
               "for a system are loaded from the card.",
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = "N keeps one site hit - the frequency, NAC, talkgroup and "
                   "unit, the signal it was heard at, and the sync and voice "
                   "counts for the session",
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = "where this receiver was when you kept the note, which is not "
                "where the transmitter is - a trunked site can be tens of "
                "kilometres away",
};

const ls_app_doc_t ls_doc_fm = {
    .purpose = "Listen to one analogue channel - narrow or wide FM, AM, or a "
               "pager and ACARS decoder - or sweep a band to find what is "
               "transmitting on it.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_adsb = {
    .purpose = "Aircraft heard directly off 1090 MHz, as a list and as a "
               "mini map centred on your saved home.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_NAVIGATES,
    .gps_note = "SET HOME remembers GPS, decimal coordinates or the map center. "
                "The offline mini map uses saved home, or a fresh GPS fix "
                "when home is unset. FULL MAP opens the SD map controls.",
};

const ls_app_doc_t ls_doc_falls = {
    .purpose = "A waterfall over whichever receiver is selected: what the "
               "band looked like over the last minute, not just now.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_cell = {
    .purpose = "Survey the cellular bands: find LTE carriers, decode what "
               "they broadcast about themselves, and compare that against a "
               "baseline learned from the same area.",
    .records = LS_APP_RECORDS_AUTOMATIC,
    /* Named exactly, because it is the one app that keeps a lot and none of
       it reaches the JOURNAL screen or notes.md. */
    .record_note = "detections to its own /sdcard/cell/lte.jsonl, which the "
                   "JOURNAL screen does not show",
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = "a capture carries the fix it was taken at, and a transmitted "
                "report carries one only when the fix is under five seconds "
                "old and HDOP is five or better",
};

const ls_app_doc_t ls_doc_mesh = {
    .purpose = "A MeshCore node: hear other nodes, relay for them, and send "
               "short messages over LoRa without any infrastructure.",
    .records = LS_APP_RECORDS_AUTOMATIC,
    .record_note = "node sightings with the position you heard them from, "
                   "exported as GPX waypoints beside the track - not journal "
                   "entries",
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = "a sighting stores where you were when you heard it, and "
                "bearing and range to each peer come from that",
};

const ls_app_doc_t ls_doc_labs = {
    .purpose = "The measurement bench: LoRa and FSK packet capture, a "
               "spectrum trace, and a bearing plot that bins signal strength "
               "against magnetic heading.",
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = "MARK keeps one observation - the frequency, signal and "
                   "packet counts at that moment, with the position and "
                   "motion the board had",
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = "attached to a kept observation; the plot itself is magnetic "
                "heading, which is the compass and not the receiver",
};

const ls_app_doc_t ls_doc_journal = {
    .purpose = "The field notebook. Notes you write, and the observations "
               "other apps kept, each with the sensor snapshot taken at the "
               "time. Exports to notes.md on the card.",
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = "a title and up to 767 characters of text, plus the time, "
                   "position, motion and radio readings as they were when it "
                   "was kept",
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = "every entry carries the fix the board had, or records that "
                "it had none - a position more than a moment stale is dropped "
                "rather than attached",
};

const ls_app_doc_t ls_doc_rec = {
    .purpose = "Capture what a radio is receiving to the card: an OOK "
               "recorder that writes Flipper-compatible sub-GHz files, and a "
               "one-per-second sensor log. REPLAY loads a capture from FILES with "
               "frequency, pulse preview and nominal power; PLAY ONCE sends it.",
    .records     = LS_APP_RECORDS_AUTOMATIC,
    .record_note = "a samples.csv row every second while recording, with "
                   "position, motion and signal; the captured waveform itself "
                   "is a separate file and carries no position",
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = "written into each samples.csv row; the capture file itself "
                "is not stamped, which is a gap",
};

const ls_app_doc_t ls_doc_files = {
    .purpose = "What is on the card. Open a .sub capture to see its pulses, "
               "send it to a Flipper or open REPLAY in RECORD; read text files; delete "
               "what is no longer wanted.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_subghz = {
    .purpose = "Watch the licence-free sub-GHz bands without transmitting: "
               "recognise a pattern that repeats, count how often it is "
               "heard, and decode it where the encoding is known.",
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = "one selected pattern - its edges, span, how many times it "
                   "was seen, when it was first and last heard, and the "
                   "decoded payload if there is one",
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = "a kept pattern carries the fix the board had when you kept "
                "it, not where the pattern was first heard",
};

const ls_app_doc_t ls_doc_mixrf = {
    .purpose = "The short-range radios on the keyboard board: NFC at "
               "13.56 MHz, a 2.4 GHz survey, and the sub-GHz transceiver, "
               "each read-only.",
    .records     = LS_APP_RECORDS_MANUAL,
    .record_note = "one observation per radio - a card seen, a field "
                   "detected, a 2.4 GHz sweep with its busiest channel, or "
                   "the energy on a sub-GHz channel",
    .gps      = LS_APP_GPS_STAMPS,
    .gps_note = "attached to a kept observation, so a survey can be placed on "
                "the map afterwards",
};

const ls_app_doc_t ls_doc_diag = {
    .purpose = "What the hardware is doing: memory, tasks, temperature, the "
               "SD card, and the health of every radio including the GPS "
               "receiver's own wire counters.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_settings = {
    .purpose = "Display and interface: theme, brightness, orientation, "
               "fonts, and the home position used when there is no fix.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_map = {
    .purpose = "An offline vector map from a .pmtiles archive on the card, "
               "with your position on it and anything the journal recorded a "
               "location for.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps      = LS_APP_GPS_NAVIGATES,
    .gps_note = "the map follows a fresh fix; one more than ten seconds old "
                "is hidden rather than drawn in the wrong place",
};

const ls_app_doc_t ls_doc_gps = {
    .purpose = "The position receiver: fix quality, which satellites are up "
               "and which are being used, and a track recorder that writes "
               "GPX.",
    .records     = LS_APP_RECORDS_AUTOMATIC,
    .record_note = "a track point once you have moved ten metres or thirty "
                   "seconds have passed, whichever comes first, to "
                   "/sdcard/lakeshark/track.log - both thresholds adjustable",
    .gps      = LS_APP_GPS_NAVIGATES,
    .gps_note = "this is the position receiver; SILENT, SEARCHING and FIX are "
                "three different states and the screen never conflates them",
};

const ls_app_doc_t ls_doc_radios = {
    .purpose = "Every radio on the board and whether it is powered: turn one "
               "off to save current or to stop it contending for the bus.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_t ls_doc_link = {
    .purpose = "Wi-Fi and Bluetooth: join a network, serve the status page, "
               "and pair the control head.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

const ls_app_doc_row_t ls_app_docs_all[] = {
    { "home",    &ls_doc_home    },
    { "p25",     &ls_doc_p25     },
    { "fm",      &ls_doc_fm      },
    { "adsb",    &ls_doc_adsb    },
    { "falls",   &ls_doc_falls   },
    { "cell",    &ls_doc_cell    },
    { "mesh",    &ls_doc_mesh    },
    { "labs",    &ls_doc_labs    },
    { "journal", &ls_doc_journal },
    { "rec",     &ls_doc_rec     },
    { "subghz",  &ls_doc_subghz  },
    { "files",   &ls_doc_files   },
    { "mixrf",   &ls_doc_mixrf   },
    { "diag",    &ls_doc_diag    },
    { "set",     &ls_doc_settings},
    { "map",     &ls_doc_map     },
    { "gps",     &ls_doc_gps     },
    { "radios",  &ls_doc_radios  },
    { "link",    &ls_doc_link    },
};

const int ls_app_docs_count =
    (int)(sizeof(ls_app_docs_all) / sizeof(ls_app_docs_all[0]));
