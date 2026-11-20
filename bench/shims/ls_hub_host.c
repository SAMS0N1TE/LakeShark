/* lssim: core/ls_hub.h over fixed sample data, so SEND REPORT, LOCKER and CHAT can be looked at.
   LSSIM_HUB picks a state: offline, unlinked, pair (a live code), busy (transfers part way), sent
   (the next report answers with an id). Default: online, linked, a few files, a busy #global. */
#include "ls_hub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static ls_hub_status_t s_st;
static int s_reads;

static bool is(const char *v) { const char *e = getenv("LSSIM_HUB"); return e && !strcmp(e, v); }

static void set(ls_hub_state_t st, const char *what, const char *text)
{
    s_st.state = st;
    snprintf(s_st.what, sizeof(s_st.what), "%s", what);
    snprintf(s_st.text, sizeof(s_st.text), "%s", text);
    s_st.seq++;
}

void ls_hub_status(ls_hub_status_t *out)
{
    if (s_st.state == LS_HUB_BUSY && !strcmp(s_st.what, "report") && is("sent") && ++s_reads > 2)
        set(LS_HUB_OK, "report", "sent 0mwn1fbwy-25485a");
    *out = s_st;
}
bool ls_hub_online(void) { return !is("offline"); }
bool ls_hub_device_id(char out[17]) { snprintf(out, 17, "3f9a0c12b7e4d581"); return true; }
void ls_hub_console_register(void) {}

/* ---- report ---- */
bool ls_report_send(const char *note, bool with_gps)
{
    (void)note; (void)with_gps;
    if (!ls_hub_online()) { set(LS_HUB_FAIL, "report", "no Wi-Fi"); return true; }
    set(LS_HUB_BUSY, "report", "working");
    s_st.done = 11 * 1024;
    s_st.total = 18 * 1024 + 310;
    return true;
}
size_t ls_report_preview(char *out, size_t cap, bool with_gps)
{
    if (cap) out[0] = 0;
    return with_gps ? 18 * 1024 + 350 : 18 * 1024 + 310;
}
bool ls_report_offer_pending(void) { return getenv("LSSIM_HUB_CRASH") != NULL; }
void ls_report_offer_done(void) {}

/* ---- locker ---- */
static const ls_locker_file_t FILES[] = {
    { "lake_map_z14.pmtiles", 19504128, 1260, true },
    { "rec_2026-11-18_1402.wav", 3565158, 7300, false },
    { "pinball-1.2.lap", 22528, 4100, true },
    { "wigle_1118.csv", 225280, 90000, false },
    { "gps_track_1117.gpx", 90112, 180000, false },
    { "p25_calls_week.zip", 12687769, 260000, false },
    { "freqs_lee_county.csv", 4096, 420000, true },
};
#define NFILES ((int)(sizeof(FILES) / sizeof(FILES[0])))
static bool linked(void) { return !is("unlinked") && !is("offline"); }
bool ls_locker_pair(void) { set(LS_HUB_OK, "pair", "code 482913"); return true; }
bool ls_locker_code(char out[8], uint32_t *left_s)
{
    if (!is("pair")) { out[0] = 0; if (left_s) *left_s = 0; return false; }
    snprintf(out, 8, "482913");
    if (left_s) *left_s = 462;
    return true;
}
bool ls_locker_list(void) { return true; }
int  ls_locker_count(void) { return linked() ? NFILES : 0; }
bool ls_locker_file(int i, ls_locker_file_t *out)
{
    if (i < 0 || i >= ls_locker_count()) return false;
    *out = FILES[i];
    return true;
}
bool ls_locker_upload(const char *sd_path)
{
    (void)sd_path;
    set(LS_HUB_BUSY, "upload", "working");
    s_st.done = 1258291;
    s_st.total = 3565158;
    return true;
}
bool ls_locker_pull(void) { set(LS_HUB_OK, "pull", "2 new in /inbox"); return true; }
bool ls_locker_unpair(void) { set(LS_HUB_OK, "unpair", "website and desktop sessions ended"); return true; }
bool ls_locker_delete(const char *name) { (void)name; set(LS_HUB_OK, "delete", "deleted"); return true; }
bool ls_locker_get(const char *name) { (void)name; set(LS_HUB_OK, "get", "in /inbox"); return true; }
int  ls_locker_ready(void) { return is("offline") ? -1 : linked() ? 1 : 0; }

/* ---- chat ---- */
static const struct { const char *h, *x; int ago; } MSG[] = {
    { "amber-lynx-0c3f", "anyone else hearing the county fire dispatch on P25 tonight? control channel is busy", 2400 },
    { "steady-crane-91aa", "yes, phase 1 here, clear audio from 40 km out", 2280 },
    { "quiet-heron-77c1", "same on mine, the T-Display picked it up with the stock antenna", 2100 },
    { "bold-marten-5e02", "first time on here. this is neat", 1500 },
    { "amber-lynx-0c3f", "welcome! try #area too, it is just people near you", 1420 },
    { "nimble-gecko-3b7d", "is there a way to share a frequency list? I made one for the lake marinas", 900 },
    { "steady-crane-91aa", "put it in LOCKER and pull it on the website, then post the name here", 840 },
    { "quiet-heron-77c1", "or STORE once packs are up. ok heading out, 73", 300 },
    { "lucid-otter-a41e", "ADS-B is wild at the airport fence, 60 planes on the map", 95 },
    { "bold-marten-5e02", "nice. what gain?", 40 },
};
#define NMSG ((int)(sizeof(MSG) / sizeof(MSG[0])))
static char s_chan[20] = "global";
static int s_peeks;
bool ls_chat_join(const char *chan) { snprintf(s_chan, sizeof(s_chan), "%s", chan); set(LS_HUB_OK, "chat", "joined"); return true; }
bool ls_chat_join_here(bool area) { return ls_chat_join(area ? "z6-17-26" : "us"); }
void ls_chat_leave(void) { s_chan[0] = 0; }
bool ls_chat_send(const char *text) { (void)text; set(LS_HUB_FAIL, "chat", "slow down"); return true; }
void ls_chat_focus(bool on) { (void)on; }
const char *ls_chat_channel(void) { return is("offline") ? "" : s_chan; }
const char *ls_chat_handle(void) { return "quiet-heron-77c1"; }
uint32_t ls_chat_seq(void) { return 1; }
int  ls_chat_count(void) { return is("offline") ? 0 : NMSG; }
bool ls_chat_get(int i, ls_chat_msg_t *out)
{
    if (i < 0 || i >= ls_chat_count()) return false;
    out->id = 4180 + (uint32_t)i;
    out->ts = (uint32_t)time(NULL) - (uint32_t)MSG[i].ago;
    snprintf(out->handle, sizeof(out->handle), "%s", MSG[i].h);
    snprintf(out->text, sizeof(out->text), "%s", MSG[i].x);
    return true;
}
void ls_chat_area_name(double lat, double lon, char out[12]) { (void)lat; (void)lon; snprintf(out, 12, "z6-17-26"); }
void ls_chat_watch(int slot, const char *chan) { (void)slot; (void)chan; }
uint32_t ls_chat_last_id(int slot) { return slot == 1 ? 731 + (++s_peeks > 6 ? 4 : 0) : slot == 2 ? 88 : 0; }
bool ls_chat_here_names(char cc[4], char area[12]) { snprintf(cc, 4, "us"); snprintf(area, 12, "z6-17-26"); return true; }
static bool s_bg;
bool ls_chat_background(void) { return s_bg; }
void ls_chat_set_background(bool on) { s_bg = on; }
