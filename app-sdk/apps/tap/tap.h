/* TAP: a packet analyser over the board's radios. Shared between the
   dissectors (tap_core.c), the display filters (tap_filter.c), the
   statistics (tap_stats.c), the capture files (tap_pcap.c) and the screens
   (main.c). Receive only. */
#ifndef TAP_H
#define TAP_H
#include "lakeshark_app.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { R_WIFI, R_BLE, R_LORA, R_DEC, R_COUNT };

/* What a packet is, for filters and colours. */
#define T_WIFI       0x00001u
#define T_MGMT       0x00002u
#define T_CTRL       0x00004u
#define T_DATA       0x00008u
#define T_BEACON     0x00010u
#define T_PROBE      0x00020u
#define T_EAPOL      0x00040u
#define T_DEAUTH     0x00080u
#define T_BLE        0x00100u
#define T_IBEACON    0x00200u
#define T_EDDYSTONE  0x00400u
#define T_LORA       0x00800u
#define T_MESHCORE   0x01000u
#define T_MESHTASTIC 0x02000u
#define T_DECODE     0x04000u
#define T_AUTH       0x08000u
#define T_SCANRSP    0x10000u
#define T_RID        0x20000u
#define T_PROTECTED  0x40000u
#define T_BAD        0x80000u   /* too short for its header, or an element runs past the end */
#define T_RETRY      0x100000u
#define T_CUT        0x200000u  /* cut to the snap length by the radio */
#define T_LLC        0x400000u
#define T_IP         0x800000u
#define T_ARP        0x1000000u
#define T_IP6        0x2000000u

/* Leaves of the protocol hierarchy. */
enum { L_BEACON, L_PROBE, L_MGMT, L_CTRL, L_DATA, L_EAPOL, L_ADV, L_IBEACON, L_EDDYSTONE, L_SCANRSP,
       L_MESHCORE, L_MESHTASTIC, L_LORA, L_DECODE, L_COUNT };
extern const char *const tap_leaf_name[L_COUNT];
extern const uint8_t tap_leaf_radio[L_COUNT];
extern const char *const tap_radio_name[R_COUNT];

/* The colouring rules, first match wins, as Wireshark's are. */
enum { CR_BAD, CR_DEAUTH, CR_EAPOL, CR_AUTH, CR_BEACON, CR_PROBE, CR_MGMT, CR_CTRL, CR_NULL, CR_DATA,
       CR_BLE_BEACON, CR_BLE, CR_SCANRSP, CR_MESHCORE, CR_MESHTASTIC, CR_LORA, CR_DECODE, CR_COUNT };
typedef struct { const char *name, *filter; uint8_t fg, bg; } tap_rule_t;
extern const tap_rule_t tap_rule[CR_COUNT];

typedef struct {
    uint32_t no, off;
    uint64_t ts_us;
    uint32_t freq_khz;          /* LoRa */
    uint16_t len, orig, rate;
    int16_t snr10;              /* LoRa, tenths of a dB */
    int8_t rssi;
    uint8_t radio, ch, kind;
    uint8_t addr[6], addr_type;
    uint8_t sf, sync, bw;       /* LoRa; bw in 125 kHz steps */
} tap_pkt_t;

/* Which addresses a frame carries (tap_dis_t.have). */
#define H_SA  1u
#define H_DA  2u
#define H_TA  4u
#define H_RA  8u
#define H_BSS 16u
#define H_ADV 32u               /* Bluetooth advertiser, in sa, as it is written (most significant first) */

typedef struct { uint8_t depth; uint16_t off, len; char text[76]; } tap_line_t;
typedef struct {
    uint32_t tags;
    uint8_t leaf, colour, rule;
    uint8_t fc_type, fc_sub, eapol_msg, ds_ch, have, companies;
    uint8_t sa[6], da[6], ta[6], ra[6], bss[6];
    uint16_t company[2], interval, capab, reason;
    char proto[12], src[20], dst[20], bssid[20], name[33], sec[10], info[100];
    tap_line_t *lines;          /* optional detail tree */
    int nlines, cap;
} tap_dis_t;
void tap_dissect(const tap_pkt_t *p, const uint8_t *d, tap_dis_t *o);
/* Bytes a record takes in a capture file beyond the frame on air: frame.len
   is the length Wireshark shows for it. */
int tap_file_overhead(const tap_pkt_t *p);

/* ---- display filters --------------------------------------------------- */
typedef struct { uint8_t op, cmp, slen, field; uint32_t mask; int64_t num; char str[40]; } tap_fnode_t;
#define TAP_FNODES 32
typedef struct { tap_fnode_t n[TAP_FNODES]; int count; char err[64]; } tap_filter_t;
typedef struct { uint64_t t0; bool marked; } tap_fctx_t;
/* An empty text matches everything. False: f->err says what is wrong. */
bool tap_filter_compile(const char *text, tap_filter_t *f);
bool tap_filter_match(const tap_filter_t *f, const tap_pkt_t *p, const uint8_t *d, const tap_dis_t *dis, const tap_fctx_t *c);
/* The plain search FIND uses when its text is not a filter: case-blind, in
   the bytes, the info column, the names and the addresses. */
bool tap_text_match(const char *lowered, const tap_pkt_t *p, const uint8_t *d, const tap_dis_t *dis);

/* ---- statistics --------------------------------------------------------- */
#define TAP_EPS 160
#define TAP_CVS 128
#define TAP_IO 240              /* buckets of the I/O graph */
#define TAP_BSS 48
#define TAP_HS 24
typedef struct {
    char addr[20], label[20];
    uint32_t tx, rx, bytes, first_ms, last_ms;      /* ms since the first packet */
    int8_t rssi, rssi_min, rssi_max;                /* of the frames it sent */
    uint8_t radio;
} tap_ep_t;
typedef struct { uint8_t a, b; int8_t rssi; uint32_t pk, bytes, ab, ba, first_ms, last_ms; } tap_cv_t;
/* Expert information, most severe first within the table. */
enum { X_BAD, X_FLOOD, X_TWIN, X_BCN_SSID, X_BCN_SEC, X_DEAUTH, X_BCN_CH, X_RETRY, X_CUT, X_HS_OK, X_EAPOL, X_COUNT };
enum { SEV_ERROR, SEV_WARN, SEV_NOTE, SEV_CHAT };
typedef struct { uint8_t sev; const char *what, *filter; } tap_expert_kind_t;
extern const tap_expert_kind_t tap_expert_kind[X_COUNT];
typedef struct { uint32_t n, last_no; char note[56]; } tap_expert_t;
typedef struct { uint8_t bss[6], ch, used; char ssid[33], sec[10]; } tap_bss_t;
typedef struct { uint8_t ap[6], sta[6], mask, used; } tap_hs_t;
typedef struct {
    uint32_t pk[L_COUNT], by[L_COUNT];
    uint32_t wifi_ch[15];       /* 1..14; [0] everything else (5 GHz) */
    uint32_t ble_ch[4];         /* 37, 38, 39, unknown */
    uint32_t rssi[R_COUNT][9];  /* -100 and below, then 10 dB steps, up to -30 and above */
    tap_ep_t ep[TAP_EPS];
    tap_cv_t cv[TAP_CVS];
    int eps, cvs;
    uint32_t ep_over, cv_over, total, bytes;
    uint64_t t0;                /* the first packet counted */
    uint32_t last_ms;
    /* packets per bucket and source; a full graph halves its resolution */
    uint16_t io[R_COUNT][TAP_IO];
    uint32_t io_step_ms;
    int io_n;
    tap_expert_t x[X_COUNT];
    tap_bss_t bss[TAP_BSS];
    tap_hs_t hs[TAP_HS];
    uint32_t wifi_frames, flood_sec, flood_n, flood_peak, hs_partial;
} tap_stats_t;
void tap_stats_add(tap_stats_t *s, const tap_pkt_t *p, const tap_dis_t *d);

/* ---- capture files --------------------------------------------------------- */
/* Section header and three interfaces: 0 = 802.11 + radiotap (127),
   1 = Bluetooth LE LL + PHDR (256), 2 = LoRaTap (270). */
size_t tap_pcapng_header(uint8_t *out, size_t cap);
/* One enhanced packet block; 0 when the record is not a frame (a decode) or
   does not fit. epoch_us is added to the packet's stamp. */
size_t tap_pcapng_packet(const tap_pkt_t *p, const uint8_t *d, uint8_t *out, size_t cap, uint64_t epoch_us);
#define TAP_PCAPNG_MAX 2600

/* Reading: pcapng and classic pcap, little-endian, link types 127 (802.11
   radiotap), 105 (802.11), 256 (Bluetooth LE LL with PHDR), 270 (LoRaTap). */
typedef struct {
    uint8_t fmt, nif, nsec;     /* fmt: 0 not begun, 1 pcapng, 2 pcap */
    uint16_t link[16];
    uint32_t tsdiv[16];         /* stamp units in a second, per interface */
    uint32_t frames, other;     /* frames handed out; frames of kinds TAP does not read */
    char err[56];
} tap_reader_t;
/* The next block of b[0..n). 1: a frame, in *p and *data (which points into
   b), *used bytes consumed. 0: *used bytes consumed, no frame. -1: the block
   is not whole yet; *used is its size when known, else 0. -2: not a file TAP
   reads; r->err says why. */
int tap_read_block(tap_reader_t *r, const uint8_t *b, size_t n, tap_pkt_t *p, const uint8_t **data, size_t *used);
#endif
