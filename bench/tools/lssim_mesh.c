

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ls_mesh.h"

/* ---------------------------------------------------------------- stats -- */

static ls_mesh_stats_t s_stats = {
    .running = true,
    .tx_enabled = false,
    .rx_packets = 412,
    .tx_packets = 37,
    .last_rssi = -92.0f,
    .last_snr = 8.5f,
    .self_id = "62E29FE68B1A3DC9",
};

void ls_mesh_get_stats(ls_mesh_stats_t *o) { if (o) *o = s_stats; }
bool ls_mesh_running(void) { return s_stats.running; }
bool ls_mesh_tx_enabled(void) { return s_stats.tx_enabled; }
void ls_mesh_set_tx(bool on) { s_stats.tx_enabled = on; }
esp_err_t ls_mesh_start(void) { s_stats.running = true; return 0; }
void ls_mesh_stop(void) { s_stats.running = false; }
esp_err_t ls_mesh_advertise(void) { s_stats.tx_packets++; return 0; }
void ls_mesh_boot(void) { }
void ls_mesh_diagnostics(void) { }

static char s_name[LS_MESH_PEER_NAME] = "SHARK";
const char *ls_mesh_name(void) { return s_name; }
esp_err_t ls_mesh_set_name(const char *n)
{
    if (n) snprintf(s_name, sizeof(s_name), "%s", n);
    return 0;
}

/* ---------------------------------------------------------------- peers -- */

static const ls_mesh_peer_t PEERS[] = {
    { .id = "A41F09D7C2B8E350", .name = "NORTHFIELD",
      .type = LS_MESH_ROLE_REPEATER, .has_loc = true,
      .lat_e6 = 43458200, .lon_e6 = -71651100,
      .first_heard = 41200, .last_heard = 44380, .adverts = 61,
      .rssi = -74.0f, .snr = 11.2f },
    /* A real public key on one of them, because the node page prints all 32
       bytes and a peer of zeros renders sixty-four noughts, which is exactly
       the wrong thing to check a wrapped hex dump against. */
    { .id = "17BC5E90A3D46F82", .name = "KB1QWE",
      .pub_key = { 0x17, 0xbc, 0x5e, 0x90, 0xa3, 0xd4, 0x6f, 0x82,
                   0x11, 0x4c, 0x09, 0xe7, 0x5a, 0x30, 0xd8, 0x6b,
                   0xf2, 0x28, 0x91, 0x40, 0xcd, 0x77, 0x3e, 0xa5,
                   0x06, 0xbb, 0x1f, 0x94, 0x62, 0xe0, 0x8d, 0x53 },
      .type = LS_MESH_ROLE_CHAT, .has_loc = true,
      .lat_e6 = 43441900, .lon_e6 = -71638400,
      .first_heard = 42010, .last_heard = 44361, .adverts = 24,
      .rssi = -88.0f, .snr = 7.4f },
    { .id = "0C3D82A5619FE7B4", .name = "TILTON ROOM",
      .type = LS_MESH_ROLE_ROOM, .has_loc = false,
      .first_heard = 40118, .last_heard = 44290, .adverts = 88,
      .rssi = -81.0f, .snr = 9.1f },
    { .id = "9E7A410B6C25D3F8", .name = "",
      .type = LS_MESH_ROLE_CHAT, .has_loc = false,
      .first_heard = 44201, .last_heard = 44244, .adverts = 2,
      .rssi = -113.0f, .snr = -6.5f },
    { .id = "3B60F1D8492AC7E5", .name = "WEIR TEMP",
      .type = LS_MESH_ROLE_SENSOR, .has_loc = true,
      .lat_e6 = 43429700, .lon_e6 = -71659900,
      .first_heard = 39004, .last_heard = 44118, .adverts = 142,
      .rssi = -97.0f, .snr = 2.8f },
    { .id = "D5824C7E30F91BA6", .name = "MOBILE-7",
      .type = LS_MESH_ROLE_CHAT, .has_loc = true,
      .lat_e6 = 43472400, .lon_e6 = -71601200,
      .first_heard = 43880, .last_heard = 44002, .adverts = 9,
      .rssi = -104.0f, .snr = -1.2f },
};
#define N_PEERS ((int)(sizeof(PEERS) / sizeof(PEERS[0])))

bool lssim_is_empty(void);

static void copy_peer(int rank, ls_mesh_peer_t *out)
{
    *out = PEERS[rank];
    out->first_heard += ls_mesh_now() - 44382u;
    out->last_heard += ls_mesh_now() - 44382u;
}

int ls_mesh_peers(ls_mesh_peer_t *out, int max)
{
    if (lssim_is_empty()) return 0;
    int n = (max < N_PEERS) ? max : N_PEERS;
    for (int i = 0; i < n; i++) copy_peer(i, &out[i]);
    return n;
}

uint32_t ls_mesh_now(void) { return 1757000000u; }

bool ls_mesh_peer_at(int rank, ls_mesh_peer_t *out)
{
    if (rank < 0 || rank >= N_PEERS || !out) return false;
    copy_peer(rank, out);
    return true;
}

esp_err_t ls_mesh_add_contact(const char *hex, const char *name)
{
    (void)hex; (void)name;
    return 0;
}

/* --------------------------------------------------------------- events -- */

/* KB1QWE gets a run of its own, walking from -71 down to -88.

   The node detail page draws a signal history from this log, and a fixture
   with two frames per node renders "no trend yet" on every node - which is
   the one thing that layout cannot be checked against. A node whose signal
   is collapsing is what the chart exists to show, so one node in the fixture
   is doing it. */
static const ls_mesh_event_t EVENTS[] = {
    { 44380, LS_MESH_EV_ADVERT, "A41F09D7C2B8E350", -74.0f, 11.2f, 96 },
    { 44372, LS_MESH_EV_RX,     "17BC5E90A3D46F82", -88.0f,  7.4f, 61 },
    { 44366, LS_MESH_EV_TX,     "",                   0.0f,  0.0f, 48 },
    { 44361, LS_MESH_EV_ADVERT, "17BC5E90A3D46F82", -86.0f,  7.9f, 96 },
    { 44350, LS_MESH_EV_RX,     "17BC5E90A3D46F82", -83.0f,  8.4f, 44 },
    { 44341, LS_MESH_EV_RX,     "17BC5E90A3D46F82", -79.0f,  9.6f, 51 },
    { 44333, LS_MESH_EV_ADVERT, "17BC5E90A3D46F82", -75.0f, 10.4f, 96 },
    { 44320, LS_MESH_EV_RX,     "17BC5E90A3D46F82", -71.0f, 11.8f, 38 },
    { 44344, LS_MESH_EV_ERR,    "",                -119.0f, -9.0f,  0 },
    { 44290, LS_MESH_EV_RX,     "0C3D82A5619FE7B4", -81.0f,  9.1f, 74 },
    { 44244, LS_MESH_EV_ADVERT, "9E7A410B6C25D3F8",-113.0f, -6.5f, 96 },
    { 44201, LS_MESH_EV_RX,     "9E7A410B6C25D3F8",-111.0f, -5.9f, 33 },
    { 44118, LS_MESH_EV_RX,     "3B60F1D8492AC7E5", -97.0f,  2.8f, 22 },
    { 44002, LS_MESH_EV_ADVERT, "D5824C7E30F91BA6",-104.0f, -1.2f, 96 },
};
#define N_EVENTS ((int)(sizeof(EVENTS) / sizeof(EVENTS[0])))

int ls_mesh_events(ls_mesh_event_t *out, int max)
{
    int n = (max < N_EVENTS) ? max : N_EVENTS;
    for (int i = 0; i < n; i++) out[i] = EVENTS[i];
    return n;
}

uint32_t ls_mesh_event_seq(void) { return 1174; }

/* ------------------------------------------------------------- messages -- */

static const ls_mesh_msg_t MSGS[] = {
    { 44120, false, false, "", LS_MSG_IN,      0, "NORTHFIELD: repeater is up on the hill again" },
    { 44166, true,  false, "", LS_MSG_HEARD,   3, "SHARK: copy, hearing you at -74" },
    { 44203, false, false, "", LS_MSG_IN,      0, "KB1QWE: anyone near the dam this afternoon" },
    { 44255, true,  false, "", LS_MSG_SENT,    0, "SHARK: heading that way around four" },
    { 44301, false, false, "", LS_MSG_IN,      0, "TILTON ROOM: net at 1900 local, usual channel" },
    { 44340, true,  true,  "17BC5E90A3D46F82", LS_MSG_ACKED, 0, "SHARK: bringing the handheld" },
    { 44366, true,  false, "", LS_MSG_SENDING, 0, "SHARK: testing the new firmware now" },
};
#define N_MSGS ((int)(sizeof(MSGS) / sizeof(MSGS[0])))

int ls_mesh_messages(ls_mesh_msg_t *out, int max)
{
    int n = (max < N_MSGS) ? max : N_MSGS;
    for (int i = 0; i < n; i++) out[i] = MSGS[i];
    return n;
}

uint32_t ls_mesh_msg_seq(void) { return 208; }

esp_err_t ls_mesh_send_text(const char *t) { (void)t; return 0; }
esp_err_t ls_mesh_send_dm(int peer_index, const char *t)
{
    (void)peer_index; (void)t;
    return 0;
}

/* The GPS screen reports how many nodes were met on a walk. The
   simulator has a peer fixture and no sighting log, so the honest answer is
   none - which is also the layout worth rendering, because a fresh board
   shows exactly that. */
int ls_mesh_sightings(void) { return 0; }
bool ls_mesh_sight_clear(void) { return false; }

bool ls_mesh_peer_known(const char *id)
{
    if (!id || !*id) return false;
    if (lssim_is_empty()) return false;
    for (int i = 0; i < N_PEERS; i++)
        if (!strcmp(PEERS[i].id, id)) return true;
    return false;
}

esp_err_t ls_mesh_send_dm_id(const char *id, const char *t)
{
    (void)t;
    return ls_mesh_peer_known(id) ? 0 : -1;
}

/* ---------------------------------------------------------------- radio -- */

static ls_mesh_radio_t s_radio = {
    .freq_hz = 910525000u,
    .sf = 11,
    .bw_hz = 250000u,
    .power_dbm = 20,
    .cr = 5,
    .sync_word = 0x12,
    .crc_on = 1,
    .advert_secs = 300,
    .advert_boot = 1,
    .role = LS_MESH_ROLE_CHAT,
    .repeat = 0,
    .share_loc = 1,
    .lat_e6 = 43444500,
    .lon_e6 = -71647300,
};

void ls_mesh_get_radio(ls_mesh_radio_t *o) { if (o) *o = s_radio; }
esp_err_t ls_mesh_set_radio(const ls_mesh_radio_t *c) { if (c) s_radio = *c; return 0; }

static int s_band = LS_MESH_BAND_US915;
int ls_mesh_band_current(void) { return s_band; }
esp_err_t ls_mesh_set_band(int b) { s_band = b; return 0; }
const char *ls_mesh_band_name(int band)
{
    switch (band) {
    case LS_MESH_BAND_US915: return "US915";
    case LS_MESH_BAND_EU868: return "EU868";
    default: return "?";
    }
}

/* -------------------------------------------------------------- channels -- */

/* Slot 0 fixed, one named channel with a key, and two empty - so the page
   draws a used row, a fixed row and an empty row all at once. */
static ls_mesh_chan_t s_chans[LS_MESH_MAX_CHANNELS] = {
    { .name = "public", .psk = "izOH6cXN6mrJ5e26oRXNcg==", .fixed = true },
    { .name = "LAKE", .psk = "OcJ1Y2VzZm9yTGFrZVNoYXJrMTIzNDU2Nzg5MA==", .fixed = false },
    { .name = "", .psk = "", .fixed = false },
    { .name = "", .psk = "", .fixed = false },
};
static int s_chan_active = 0;

int ls_mesh_channels(ls_mesh_chan_t *out, int max)
{
    int n = (max < LS_MESH_MAX_CHANNELS) ? max : LS_MESH_MAX_CHANNELS;
    for (int i = 0; i < n; i++) out[i] = s_chans[i];
    return n;
}
int ls_mesh_channel_active(void) { return s_chan_active; }
esp_err_t ls_mesh_set_channel_active(int i)
{
    if (i >= 0 && i < LS_MESH_MAX_CHANNELS) s_chan_active = i;
    return 0;
}
esp_err_t ls_mesh_set_channel(int idx, const char *name, const char *psk)
{
    if (idx <= 0 || idx >= LS_MESH_MAX_CHANNELS) return 0;
    if (name) snprintf(s_chans[idx].name, sizeof(s_chans[idx].name), "%s", name);
    if (psk)  snprintf(s_chans[idx].psk,  sizeof(s_chans[idx].psk),  "%s", psk);
    return 0;
}

static bool s_auto_listen = true, s_auto_tx = false;
bool ls_mesh_auto_listen(void) { return s_auto_listen; }
bool ls_mesh_auto_tx(void) { return s_auto_tx; }
esp_err_t ls_mesh_set_auto(bool l, bool t) { s_auto_listen = l; s_auto_tx = t; return 0; }

/* ---------------------------------------------------------------- scope -- */

int ls_mesh_scope(float *out, int max)
{
    const int n = (max < LS_MESH_SCOPE_N) ? max : LS_MESH_SCOPE_N;
    for (int i = 0; i < n; i++) {
        float v = -118.0f + (float)((i * 37) % 7);      /* floor, textured  */
        if (i > 18 && i < 30) v = -84.0f + (float)((i * 13) % 9);
        if (i > 52 && i < 61) v = -71.0f + (float)((i * 11) % 5);
        if (i > 70 && i < 76) v = -128.0f;              /* our own transmit */
        out[i] = v;
    }
    return n;
}

/* --------------------------------------------------------------- airtime -- */

uint32_t ls_lora_airtime_ms(int len)
{
    if (len < 0 || !s_radio.bw_hz) return 0;
    const uint32_t sf = s_radio.sf;

    uint64_t ts_us = ((uint64_t)(1u << sf) * 1000000ull) / s_radio.bw_hz;
    uint32_t sym_ms = (uint32_t)(ts_us / 1000);
    int de  = sym_ms >= 16 ? 1 : 0;
    int crc = s_radio.crc_on ? 1 : 0;

    int num = 8 * len - 4 * (int)sf + 28 + 16 * crc;
    int den = 4 * ((int)sf - 2 * de);
    int ceil_term = den > 0 ? (num + den - 1) / den : 0;
    if (ceil_term < 0) ceil_term = 0;
    const int payload_symb = 8 + ceil_term * (int)s_radio.cr;

    const uint64_t preamble_us = ts_us * 8 + (ts_us * 425) / 100;
    const uint64_t payload_us  = ts_us * (uint64_t)payload_symb;
    return (uint32_t)((preamble_us + payload_us + 999) / 1000);
}

/* The radio lease, and the LoRa sweep behind it.

   The simulator has no SX1262, so the honest answer to "can this source
   run" is no - which is exactly what the FALLS strip has to draw well: a
   chip for a radio that is not fitted, dim, saying so when tapped. Faking a
   working sweep here would mean the one layout nobody could check on the
   board is the one the simulator never renders. */
bool ls_mesh_radio_hold(bool on) { (void)on; return true; }
bool ls_mesh_radio_held(void) { return true; }

bool ls_lora_present(void) { return false; }
bool ls_lora_fsk_active(void) { return false; }
bool ls_lora_scanning(void) { return false; }
esp_err_t ls_lora_scan_begin(uint32_t a, uint32_t b)
{ (void)a; (void)b; return -1; }
int ls_lora_scan_sweep(float *d, int n) { (void)d; (void)n; return 0; }
/* The waterfall sweeps a pass at a time now. Nothing here either. */
int ls_lora_scan_pass(float *d, int n, bool *done)
{ (void)d; (void)n; if (done) *done = false; return 0; }
esp_err_t ls_lora_scan_end(void) { return 0; }
