/* TAP: a packet analyser over the board's radios. Shared between the
   dissectors (tap_core.c) and the screens (main.c). Receive only. */
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

/* Leaves of the protocol hierarchy. */
enum { L_BEACON, L_PROBE, L_MGMT, L_CTRL, L_DATA, L_EAPOL, L_ADV, L_IBEACON, L_EDDYSTONE, L_SCANRSP,
       L_MESHCORE, L_MESHTASTIC, L_LORA, L_DECODE, L_COUNT };
extern const char *const tap_leaf_name[L_COUNT];
extern const uint8_t tap_leaf_radio[L_COUNT];
extern const char *const tap_radio_name[R_COUNT];

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

typedef struct { uint8_t depth; uint16_t off, len; char text[76]; } tap_line_t;
typedef struct {
    uint32_t tags;
    uint8_t leaf, colour;
    char proto[12], src[20], dst[20], bssid[20], name[33], info[100];
    tap_line_t *lines;          /* optional detail tree */
    int nlines, cap;
} tap_dis_t;
void tap_dissect(const tap_pkt_t *p, const uint8_t *d, tap_dis_t *o);

/* ---- display filters --------------------------------------------------- */
typedef struct { uint8_t op, cmp, slen; int32_t num; uint32_t mask; char str[40]; } tap_fnode_t;
typedef struct { tap_fnode_t n[24]; int count; char err[48]; } tap_filter_t;
/* An empty text matches everything. False: f->err says what is wrong. */
bool tap_filter_compile(const char *text, tap_filter_t *f);
bool tap_filter_match(const tap_filter_t *f, const tap_pkt_t *p, const uint8_t *d, const tap_dis_t *dis);

/* ---- statistics --------------------------------------------------------- */
#define TAP_EPS 64
#define TAP_CVS 64
typedef struct { char addr[20], label[20]; uint32_t tx, rx, bytes; int8_t rssi; uint8_t radio; } tap_ep_t;
typedef struct { uint8_t a, b; uint32_t pk, bytes; } tap_cv_t;
typedef struct {
    uint32_t pk[L_COUNT], by[L_COUNT];
    uint32_t wifi_ch[15];       /* 1..14; [0] everything else (5 GHz) */
    uint32_t ble_ch[4];         /* 37, 38, 39, unknown */
    uint32_t rssi[R_COUNT][9];  /* -100 and below, then 10 dB steps, up to -30 and above */
    tap_ep_t ep[TAP_EPS];
    tap_cv_t cv[TAP_CVS];
    int eps, cvs;
    uint32_t ep_over, cv_over, total, bytes;
} tap_stats_t;
void tap_stats_add(tap_stats_t *s, const tap_pkt_t *p, const tap_dis_t *d);

/* ---- pcapng -------------------------------------------------------------- */
/* Section header and three interfaces: 0 = 802.11 + radiotap (127),
   1 = Bluetooth LE LL + PHDR (256), 2 = LoRaTap (270). */
size_t tap_pcapng_header(uint8_t *out, size_t cap);
/* One enhanced packet block; 0 when the record is not a frame (a decode) or
   does not fit. epoch_us is added to the packet's uptime stamp. */
size_t tap_pcapng_packet(const tap_pkt_t *p, const uint8_t *d, uint8_t *out, size_t cap, uint64_t epoch_us);
#define TAP_PCAPNG_MAX 2600
#endif
