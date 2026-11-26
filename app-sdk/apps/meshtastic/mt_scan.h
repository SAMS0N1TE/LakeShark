/* SCAN: which Meshtastic presets are in use nearby. The radio steps through
   the modem presets, each on the slot its default channel name hashes to,
   and counts what it hears on each.

   Source-derived from Meshtastic's firmware (github.com/meshtastic/firmware,
   Copyright (C) Meshtastic LLC and contributors, GPL-3.0): the preset
   parameters of modemPresetToParams() (MeshRadio.h), the display names of
   DisplayFormatters.cpp, the region profiles and the slot rule of
   RadioInterface.cpp (numFreqSlots = round(band / slot width), slot =
   djb2(name) % numFreqSlots). GPL-3.0 as part of LakeShark.

   Pure logic, no board calls: the bench tests it. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mt_proto.h"

#define MT_SCAN_MAX    10       /* presets in a cycle */
#define MT_SCAN_NODES  24       /* distinct senders remembered a preset */
#define MT_SCAN_HASHES 4        /* distinct channel hashes remembered a preset */
#define MT_SCAN_LOG    24       /* packets kept for the live list */
#define MT_SCAN_SEEN   64       /* distinct senders over all presets */

/* A modem preset as upstream defines it (narrow-band bandwidth; wide regions
   multiply by 3.25). eu868: allowed in EU_868 (PRESETS_EU_868). */
typedef struct {
    const char *name;           /* the display name: the default channel's name */
    const char *tag;            /* 5 letters for narrow screens */
    uint32_t bw_hz;
    uint8_t sf, cr, eu868;
} mt_scan_preset_t;
extern const mt_scan_preset_t MT_SCAN_PRESETS[];
extern const int MT_SCAN_PRESET_COUNT;

/* Upstream's slot count for a region and an on-air bandwidth (spacing and
   padding are 0 in the profiles MT_REGIONS covers), 0 when none fits. */
int      mt_scan_slots(int region, uint32_t bw_hz);
uint32_t mt_scan_bw_hz(int region, int preset);
/* The default channel's frequency for a preset; slot is 1-based. 0 when the
   preset does not fit the band. */
uint64_t mt_scan_freq_hz(int region, int preset, int *slot, int *slots);
/* Time on air of one packet of len bytes (explicit header, CRC on). */
uint32_t mt_scan_airtime_ms(uint32_t bw_hz, uint8_t sf, uint8_t cr, uint8_t preamble, int len);

typedef struct {
    uint8_t preset;             /* index into MT_SCAN_PRESETS */
    uint8_t on;                 /* in the cycle */
    uint8_t sf, cr, preamble;
    uint8_t def_hash;           /* the default channel's hash (name ^ key AQ==) */
    uint16_t slot, slots;
    uint32_t bw_hz;
    uint64_t freq_hz;
    uint32_t dwell_ms;          /* this preset's own dwell (long presets get more) */
    uint32_t hang_ms;           /* stay this long after a packet */
    /* what was heard */
    uint32_t packets, readable, listen_ms, last_ms;
    int16_t rssi, snr;          /* tenths; INT16_MIN unknown */
    uint32_t nodes[MT_SCAN_NODES];
    uint8_t node_n, node_more;  /* node_more: senders past the list (capped 255) */
    uint8_t hash_n;
    uint8_t hash[MT_SCAN_HASHES];
    uint16_t hash_count[MT_SCAN_HASHES];
} mt_scan_entry_t;

/* One packet as the live list shows it. */
typedef struct {
    uint32_t at_ms, from, to, id;
    uint8_t entry, hop_limit, hop_start, hash;
    uint8_t is_default;         /* the hash is the preset's default channel */
    uint8_t readable;           /* decrypted with AQ== and parsed */
    uint8_t via_mqtt, len;
    int16_t rssi, snr;
    uint32_t portnum;
    char text[48];              /* TEXT: the words, printable only */
} mt_scan_pkt_t;

typedef struct {
    int region;
    int n;                      /* entries */
    mt_scan_entry_t e[MT_SCAN_MAX];
    int cur;                    /* tuned entry, -1 none */
    int pin;                    /* -1: cycling; else stays on that entry */
    bool running, rx_step;      /* rx_step: a packet came on this step */
    uint32_t base_dwell_ms;
    uint32_t step_at, last_rx_at, acc_at;
    uint32_t sweeps, total, readable;
    uint32_t seen[MT_SCAN_SEEN];
    int seen_n;
    mt_scan_pkt_t log[MT_SCAN_LOG];
    int log_n, log_head;
} mt_scan_t;

/* The presets the region's profile allows, each on its default slot. Stats empty. */
void mt_scan_init(mt_scan_t *s, int region, uint32_t base_dwell_ms);
void mt_scan_set_dwell(mt_scan_t *s, uint32_t base_dwell_ms);
void mt_scan_clear(mt_scan_t *s);               /* counts only; the plan stays */
/* Start or stop. Start returns the entry to tune to (-1: none on). */
int  mt_scan_start(mt_scan_t *s, uint32_t now);
void mt_scan_stop(mt_scan_t *s, uint32_t now);
/* Call often: -1 stay, else the entry to tune to now. */
int  mt_scan_tick(mt_scan_t *s, uint32_t now);
/* Pin an entry (listen there only), or -1 to cycle again. Returns the entry
   to tune to, -1 none. */
int  mt_scan_pin(mt_scan_t *s, int entry, uint32_t now);
/* Turn an entry in or out of the cycle; false when it would leave none. */
bool mt_scan_enable(mt_scan_t *s, int entry, bool on);
/* A frame heard on the current entry: header, the default key tried when the
   hash is the default channel's, counted and logged. False when it is no
   Meshtastic frame (shorter than the header). */
bool mt_scan_heard(mt_scan_t *s, const uint8_t *buf, int n, int16_t rssi, int16_t snr, uint32_t now);
/* The decode alone (entry e's default channel), for tests and the log. */
bool mt_scan_decode(const mt_scan_entry_t *e, const uint8_t *buf, int n, mt_scan_pkt_t *out);
/* Share of the traffic, 0..1: this preset's packets a minute of listening
   over all presets' (packets over all packets until a second is heard). */
float mt_scan_share(const mt_scan_t *s, int entry);
/* Packets a minute of listening there. */
float mt_scan_rate(const mt_scan_t *s, int entry);
/* The newest packet first: k = 0 .. log_n-1. */
const mt_scan_pkt_t *mt_scan_log_at(const mt_scan_t *s, int k);
/* The most common hash an entry heard, -1 none. */
int  mt_scan_top_hash(const mt_scan_entry_t *e);
