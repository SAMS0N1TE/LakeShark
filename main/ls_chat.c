/* ls_chat: anonymous text chat through hub.terminalbay.com. See core/ls_hub.h. */
#include "ls_hub.h"
#include "ls_hub_int.h"
#include "ls_location.h"
#include "ls_nvs_safe.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "nvs.h"

#define POLL_BUF 12288

static ls_chat_msg_t *s_ring;           /* PSRAM, LS_CHAT_RING entries, oldest first */
static volatile int s_n;
static volatile uint32_t s_seq;
static uint32_t s_last;                 /* newest id held; 0 = nothing yet */
static bool s_has_last;
static char s_chan[20];
static char s_handle[24];
static volatile bool s_focus, s_joining, s_sending;
static EXT_RAM_BSS_ATTR char s_out[LS_CHAT_TEXT];      /* the message being sent: a hub job holds 110 bytes */
static EXT_RAM_BSS_ATTR struct { char ch[20]; uint32_t last; } s_peek[LS_CHAT_WATCH];
static uint8_t s_polls;

const char *ls_chat_channel(void) { return s_chan; }
const char *ls_chat_handle(void) { return s_handle; }
uint32_t ls_chat_seq(void) { return s_seq; }
int ls_chat_count(void) { return s_n; }
void ls_chat_focus(bool on) { s_focus = on; ls_hub_wake(); }

uint32_t chat_poll_wait_ms(void)
{
    return s_chan[0] ? (s_focus ? 3000 : 60000) : 0;     /* in the background: one light poll a minute */
}

/* LISTEN: keep the room polled (once a minute) after the chat screen closes. Kept in NVS, off at first. */
static int8_t s_bg = -1;
static esp_err_t bg_job(void *ctx)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("lshub", NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    if (ctx) { v = *(uint8_t *)ctx; nvs_set_u8(h, "chatbg", v); nvs_commit(h); }
    else if (nvs_get_u8(h, "chatbg", &v) != ESP_OK) v = 0;
    nvs_close(h);
    s_bg = (int8_t)v;
    return ESP_OK;
}
bool ls_chat_background(void) { if (s_bg < 0 && ls_nvs_run(bg_job, NULL, 0) != ESP_OK) s_bg = 0; return s_bg > 0; }
void ls_chat_set_background(bool on) { uint8_t v = on; if (ls_nvs_run(bg_job, &v, 0) != ESP_OK) s_bg = (int8_t)on; }

void ls_chat_watch(int slot, const char *chan)
{
    if (slot < 0 || slot >= LS_CHAT_WATCH) return;
    if (strcmp(s_peek[slot].ch, chan ? chan : "")) s_peek[slot].last = 0;
    strlcpy(s_peek[slot].ch, chan ? chan : "", sizeof(s_peek[slot].ch));
}

uint32_t ls_chat_last_id(int slot) { return slot >= 0 && slot < LS_CHAT_WATCH ? s_peek[slot].last : 0; }

bool ls_chat_here_names(char cc[4], char area[12])
{
    double lat = 0, lon = 0;
    char reg[8];
    cc[0] = area[0] = 0;
    if (ls_location_here(&lat, &lon) == LS_LOC_SRC_NONE) return false;
    ls_chat_area_name(lat, lon, area);
    if (ls_location_region(lat, lon, reg) && reg[0] && reg[1]) {
        cc[0] = (char)tolower((unsigned char)reg[0]);
        cc[1] = (char)tolower((unsigned char)reg[1]);
        cc[2] = 0;
    }
    return true;
}

bool ls_chat_get(int i, ls_chat_msg_t *out)
{
    if (!s_ring || i < 0) return false;
    for (int tries = 0; tries < 3; tries++) {
        const uint32_t q = s_seq;
        if (i >= s_n) return false;
        *out = s_ring[i];
        if (q == s_seq) return true;
    }
    return false;
}

/* Latitude (x100 degrees) where each zoom-6 Mercator row starts, rows 1..63: no libm, 126 bytes. The
   website's exact formula can differ by one cell within 0.01 degree of an edge. */
static const int16_t k_row[63] = {
    8454, 8398, 8336, 8268, 8192, 8109, 8018, 7917, 7806, 7684, 7550, 7402, 7240, 7061, 6866, 6651, 6417,
    6161, 5881, 5578, 5248, 4892, 4509, 4098, 3660, 3195, 2706, 2194, 1664, 1118, 562, 0, -562, -1118, -1664,
    -2194, -2706, -3195, -3660, -4098, -4509, -4892, -5248, -5578, -5881, -6161, -6417, -6651, -6866, -7061,
    -7240, -7402, -7550, -7684, -7806, -7917, -8018, -8109, -8192, -8268, -8336, -8398, -8454 };

void ls_chat_area_name(double lat, double lon, char out[12])
{
    const int la = (int)(lat * 100 + (lat < 0 ? -0.5 : 0.5));
    int lo = (int)(lon * 100 + (lon < 0 ? -0.5 : 0.5)) + 18000;
    lo = lo < 0 ? 0 : lo > 35999 ? 35999 : lo;
    int y = 0;
    while (y < 63 && la <= k_row[y]) y++;
    snprintf(out, 12, "z6-%d-%d", lo * 2 / 1125, y);
}

bool ls_chat_join(const char *chan)
{
    if (!chan || !*chan || strlen(chan) >= sizeof(s_chan)) return false;
    for (const char *p = chan; *p; p++) if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-')) return false;
    if (!s_ring) s_ring = heap_caps_calloc(LS_CHAT_RING, sizeof(*s_ring), MALLOC_CAP_SPIRAM);
    if (!s_ring) return false;
    s_chan[0] = 0;                      /* the task stops polling the old channel */
    s_n = 0;
    s_has_last = false;
    s_last = 0;
    s_seq++;
    s_handle[0] = 0;
    strlcpy(s_chan, chan, sizeof(s_chan));
    s_joining = true;
    hub_set(LS_HUB_BUSY, "chat", "joining");
    return hub_submit(HOP_CHAT_POLL, "", false);
}

bool ls_chat_join_here(bool area)
{
    double lat = 0, lon = 0;
    if (ls_location_here(&lat, &lon) == LS_LOC_SRC_NONE) { hub_set(LS_HUB_FAIL, "chat", "no position yet"); return false; }
    char c[12];
    if (area) ls_chat_area_name(lat, lon, c);
    else {
        char reg[8];
        if (!ls_location_region(lat, lon, reg) || !reg[0]) { hub_set(LS_HUB_FAIL, "chat", "no country for here"); return false; }
        c[0] = (char)tolower((unsigned char)reg[0]);
        c[1] = (char)tolower((unsigned char)reg[1]);
        c[2] = 0;
    }
    return ls_chat_join(c);
}

void ls_chat_leave(void)
{
    s_chan[0] = 0;
    s_n = 0;
    s_seq++;
}

bool ls_chat_send(const char *text)
{
    if (!s_chan[0] || !text || !*text || s_sending) return false;
    strlcpy(s_out, text, sizeof(s_out));
    s_sending = true;
    if (hub_submit(HOP_CHAT_SEND, "", false)) return true;
    s_sending = false;
    return false;
}

/* The newest id of each watched channel that is not the open one: n=1 is one short line. */
static void peek(char *buf)
{
    for (int i = 0; i < LS_CHAT_WATCH; i++) {
        char path[64];
        if (!s_peek[i].ch[0] || !strcmp(s_peek[i].ch, s_chan)) continue;
        snprintf(path, sizeof(path), "/v1/chat/%s?fmt=t&n=1", s_peek[i].ch);
        hub_req_t q = { .method = "GET", .path = path, .out = buf, .ocap = 512 };
        if (hub_call(&q, NULL) == 200 && !strncmp(buf, "last	", 5)) s_peek[i].last = (uint32_t)strtoul(buf + 5, NULL, 10);
    }
}

static void add(uint32_t id, uint32_t ts, const char *h, const char *x)
{
    if (id <= s_last && s_has_last) return;
    if (s_n == LS_CHAT_RING) {
        memmove(s_ring, s_ring + 1, (LS_CHAT_RING - 1) * sizeof(*s_ring));
        s_n--;
    }
    ls_chat_msg_t *m = &s_ring[s_n];
    m->id = id;
    m->ts = ts;
    strlcpy(m->handle, h, sizeof(m->handle));
    strlcpy(m->text, x, sizeof(m->text));
    s_n++;
    s_last = id;
    s_has_last = true;
    s_seq++;
}

static int poll(char *buf)
{
    char path[80], chan[20];
    strlcpy(chan, s_chan, sizeof(chan));
    if (!chan[0]) return 0;
    if (s_has_last) snprintf(path, sizeof(path), "/v1/chat/%s?fmt=t&since=%u", chan, (unsigned)s_last);
    else snprintf(path, sizeof(path), "/v1/chat/%s?fmt=t&n=30", chan);
    hub_req_t q = { .method = "GET", .path = path, .out = buf, .ocap = POLL_BUF };
    const int s = hub_call(&q, NULL);
    if (s != 200 || strcmp(chan, s_chan)) return s;
    char *l = strchr(buf, '\n');
    if (strncmp(buf, "last\t", 5) || !l) return s;
    const uint32_t last = (uint32_t)strtoul(buf + 5, NULL, 10);
    if (!s_has_last) { s_last = 0; }
    for (++l; *l;) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        char *f[4] = { l, NULL, NULL, NULL };
        int k = 1;
        for (char *p = l; *p && k < 4; p++)
            if (*p == '\t') { *p = 0; f[k++] = p + 1; }
        if (k == 4) add((uint32_t)strtoul(f[0], NULL, 10), (uint32_t)strtoul(f[1], NULL, 10), f[2], f[3]);
        if (!e) break;
        l = e + 1;
    }
    if (!s_has_last) { s_last = last; s_has_last = true; }
    return s;
}

void chat_run(hub_op_t op, const char *arg)
{
    char *buf = heap_caps_malloc(POLL_BUF, MALLOC_CAP_SPIRAM);
    if (!buf) {
        if (op == HOP_CHAT_SEND) { s_sending = false; hub_set(LS_HUB_FAIL, "chat", "out of memory"); }
        return;
    }
    if (op == HOP_CHAT_SEND && !arg[0]) arg = s_out;
    if (op == HOP_CHAT_SEND && !s_chan[0]) hub_set(LS_HUB_FAIL, "chat", "join a room first");
    else if (op == HOP_CHAT_SEND) {
        char path[48], r[160];
        snprintf(path, sizeof(path), "/v1/chat/%s?fmt=t", s_chan);
        hub_req_t q = { .method = "POST", .path = path, .ctype = "text/plain", .body = arg, .blen = (uint32_t)strlen(arg), .out = r, .ocap = sizeof(r) };
        const int s = hub_call(&q, NULL);
        if (s == 200) {
            char *t1 = strchr(r, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            if (t2) {
                strlcpy(s_handle, t2 + 1, sizeof(s_handle));
                s_handle[strcspn(s_handle, "\r\n")] = 0;
            }
            hub_set(LS_HUB_OK, "chat", "sent");
            poll(buf);
        } else {
            char e[48];
            if (s == -1) strcpy(e, "no Wi-Fi");
            else if (s < 0) strcpy(e, "hub unreachable");
            else if (!hub_json_str(r, "error", e, sizeof(e))) snprintf(e, sizeof(e), "not sent (%d)", s);
            hub_set(LS_HUB_FAIL, "chat", "%s", e);
        }
    }
    if (op == HOP_CHAT_SEND) s_sending = false;         /* s_out is free again */
    else {
        const int s = poll(buf);
        if (s_focus && !(s_polls++ & 3)) peek(buf);      /* unread counts on the other channels, every 12 s */
        if (op == HOP_CHAT_POLL && s_has_last && !s_handle[0]) {
            hub_req_t w = { .method = "GET", .path = "/v1/whoami", .out = buf, .ocap = 200 };
            if (hub_call(&w, NULL) == 200) hub_json_str(buf, "handle", s_handle, sizeof(s_handle));
        }
        if (s_joining) {
            s_joining = false;
            if (s == 200) hub_set(LS_HUB_OK, "chat", "joined #%s", s_chan);
            else hub_set(LS_HUB_FAIL, "chat", s == -1 ? "no Wi-Fi" : "hub unreachable");
        }
    }
    heap_caps_free(buf);
}

void chat_console(int argc, char **argv)
{
    const char *v = argc > 1 ? argv[1] : "";
    if (!strcmp(v, "join") && argc > 2) {
        const bool ok = !strcmp(argv[2], "here") ? ls_chat_join_here(false) : !strcmp(argv[2], "area") ? ls_chat_join_here(true) : ls_chat_join(argv[2]);
        if (!ok) { ls_hub_status_t st; ls_hub_status(&st); printf("chat: %s\n", st.state == LS_HUB_FAIL ? st.text : "cannot join"); return; }
        hub_wait(30000);
        printf("chat: in #%s as %s, %d messages\n", s_chan, s_handle[0] ? s_handle : "?", s_n);
    } else if (!strcmp(v, "send") && argc > 2) {
        char t[LS_CHAT_TEXT] = "";
        for (int i = 2; i < argc; i++) { if (i > 2) strlcat(t, " ", sizeof(t)); strlcat(t, argv[i], sizeof(t)); }
        if (!ls_chat_send(t)) { printf("chat: join a channel first\n"); return; }
        hub_wait(30000);
        ls_hub_status_t st;
        ls_hub_status(&st);
        printf("chat: %s\n", st.text);
    } else if (!strcmp(v, "read")) {
        for (int i = 0; i < s_n; i++) printf("%s: %s\n", s_ring[i].handle, s_ring[i].text);
    } else if (!strcmp(v, "who")) {
        printf("chat: #%s, you are %s\n", s_chan[0] ? s_chan : "-", s_handle[0] ? s_handle : "(join first)");
    } else if (!strcmp(v, "leave")) {
        ls_chat_leave();
        printf("chat: left\n");
    } else {
        printf("chat join global|here|area|<code> | send <text> | read | who | leave\n");
    }
}
