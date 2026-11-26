/* BLE EXPLORER: advertisements in, then a GATT client. Connect to up to
   three devices (as many as the board grants an app), walk each one's whole
   table (services, characteristics, every descriptor, every readable value),
   name and decode the Bluetooth SIG ones, log notifications, and save the
   table to the card as JSON. Writes happen only on an explicit key (W writes
   0x01, N/U set notifications) or a flow the user starts. Names come from
   the shared name library (lap_ids), NAME files the owner's own, and FLOWS
   runs short scripted sequences. The walk lives in bx_core.c, flows in
   bx_flow.c. */
#include "bx.h"
#include "bx_flow.h"
#include "lap_ids.h"
#include "lakeshark_ui.h"
#include "lap_scroll.h"
#include "lap_art.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what the scan heard -------------------------------------------------------------------- */
#define MAXDEV 48
typedef struct {
    uint8_t addr[6], type;
    char name[24];
    int8_t rssi;
    uint16_t company, svc, look;            /* look: the advert's appearance, 0 none */
    bool full;                              /* its name came complete (AD 0x09) */
    uint32_t last, count;
} bxdev_t;
static bxdev_t *devs;
static int ndev;

static int scan = -1;
static bool fitted;
static bx_conn_t *conns[BX_MAX_CONN];
static int cur;                         /* the connection shown */
static int board_limit;                 /* learnt when the board refuses another; 0 unknown */
static bx_note_t notes[BX_NOTES];
static int nnotes, note_head;
static uint32_t events;
static char message[96] = "Waiting for adverts";
static uint32_t message_at;

static bx_airsync_t airsync[BX_MAX_CONN];
static char pair_text[48];              /* pairing as it goes, or a code to type on the device */

/* flows: the list, the one running, the FLOWS page */
static bxf_lib_t *flib;
static bxf_run_t *frun;
static int frun_slot, forder[BXF_FLOWS], nfit, fsel, ftop, flist_y, flist_h, fsmax;
static bool fview_run, fshow = true;
static lap_scroll_t fdrag;
/* loading the pack and the owner's files: one card job at a time */
#define FL_USER_FILES 8
#define FL_USER_BYTES 16384
#define FL_PACK_BYTES 60000
static struct {
    int stage, job, i, n;
    char *pack, *user[FL_USER_FILES];
    int nuser;
    lap_sd_entry_t *ents;
    char path[112];
    uint32_t size;
} fload;

/* the one dialog open: a name, a confirm, a passkey */
enum { DLG_NONE, DLG_NAME, DLG_RUN, DLG_PAIR, DLG_PASSKEY, DLG_NUMCMP, DLG_MORE };
static int dlg = -1, dlg_kind, dlg_flow, dlg_slot;
static char dlg_kw[12], dlg_key[48];

/* screen */
enum { T_SCAN, T_GATT, T_NOTIFY, T_FLOWS, T_COUNT };
static int W, H, tab, dsel, dtop, gsel, gtop, ntop;
static bool desk;
static int dlist_y, dlist_h, glist_y, glist_h, tab_x[T_COUNT + 1];
/* the finger-sized layout: tab strip rows (tr), key bar rows (kr), the body
   between them. Each list follows the finger; a pick made with the keys or a
   tap brings itself into view (dshow, gshow), a drag leaves the pick alone. */
static int kr = 1, tr = 1, hd = 6, ty = 7, body0 = 2, body1, nlist_y, nlist_h, dsmax, gsmax, nsmax, seen_tab, seen_cur;
static bool dragging, dshow = true, gshow = true;
static lap_scroll_t ddrag, gdrag, ndrag;
typedef struct { uint8_t kind; uint16_t i; uint8_t tree; } row_t;     /* kind 0 service, 1 characteristic, 2 descriptor */
enum { TREE_LAST = 1, TREE_SVC_LAST = 2, TREE_CHR_LAST = 4 };            /* row_t.tree: where the branches end */
static row_t rows[BX_MAX_SVC + BX_MAX_CHR + BX_MAX_DSC];
static int nrows;

/* saving */
static struct { int stage, job; char *buf; size_t len, done; char path[80]; } save;

static void put(int x, int y, int wd, uint8_t attr, const char *fmt, ...)
{
    char t[200];
    va_list ap;
    if (wd <= 0 || y < 0 || y >= H || x >= W || x < 0) return;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (x + wd > W) wd = W - x;
    if ((size_t)wd < sizeof(t)) t[wd] = 0;
    lap_put_str(x, y, t, attr);
}
/* Text over up to `lines` rows, broken at spaces. Returns the rows used. */
static int put_wrap(int x, int y, int wd, int lines, uint8_t attr, const char *s)
{
    int used = 0;
    while (*s && used < lines && wd > 0) {
        int n = (int)strlen(s);
        if (n > wd) {
            n = wd;
            if (used + 1 < lines) { int b = n; while (b > wd / 2 && s[b] != ' ') b--; if (s[b] == ' ') n = b; }
        }
        put(x, y + used, n, attr, "%.*s", n, s);
        s += n;
        while (*s == ' ') s++;
        used++;
    }
    return used;
}
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    message_at = lap_millis();
}
static void addr_text(const uint8_t a[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}
static bx_conn_t *shown(void) { return conns[cur] && (conns[cur]->handle || conns[cur]->nsvc || conns[cur]->state) ? conns[cur] : NULL; }
static int live(void)
{
    int n = 0;
    for (int i = 0; i < BX_MAX_CONN; i++) n += conns[i] && conns[i]->handle;
    return n;
}

/* ---- adverts ------------------------------------------------------------------------------ */
static void heard(const lap_ble_advert_t *a, const uint8_t *p, int n)
{
    int i = 0;
    while (i < ndev && (memcmp(devs[i].addr, a->addr, 6) || devs[i].type != (a->addr_type & 1))) i++;
    if (i == ndev) {
        if (ndev < MAXDEV) ndev++;
        else {                                  /* full: the longest unheard goes */
            i = 0;
            for (int k = 1; k < ndev; k++) if (devs[k].last < devs[i].last) i = k;
        }
        memset(&devs[i], 0, sizeof(devs[i]));
        memcpy(devs[i].addr, a->addr, 6);
        devs[i].type = a->addr_type & 1;
        devs[i].company = 0xFFFF;
    }
    bxdev_t *d = &devs[i];
    d->rssi = (int8_t)a->rssi;
    d->last = lap_millis();
    d->count++;
    /* its own name: a complete one wins over a short one heard in the other
       half of the advert, so the two never take turns */
    char nm[sizeof(d->name)];
    const int kind = bx_ad_name(p, (size_t)n, nm, sizeof(nm));
    if (kind && (kind == 0x09 || !d->full) && strcmp(nm, d->name)) {
        snprintf(d->name, sizeof(d->name), "%s", nm);
        lap_ids_device_learn(d->addr, d->type, nm);     /* kept for when it advertises without one */
    }
    if (kind == 0x09) d->full = true;
    for (int j = 0; j < n;) {
        const int len = p[j];
        if (!len || j + 1 + len > n) break;
        const uint8_t type = p[j + 1], *v = p + j + 2;
        if (type == 0xFF && len >= 3) d->company = (uint16_t)(v[0] | v[1] << 8);
        if ((type == 0x02 || type == 0x03 || type == 0x16) && len >= 3) d->svc = (uint16_t)(v[0] | v[1] << 8);
        if (type == 0x19 && len >= 3) d->look = (uint16_t)(v[0] | v[1] << 8);
        j += len + 1;
    }
}
static int by_rssi(const void *a, const void *b) { return ((const bxdev_t *)b)->rssi - ((const bxdev_t *)a)->rssi; }

/* The shared library's maker of a public address (addr as the radio has it). */
static const char *maker(const uint8_t addr[6], uint8_t type)
{
    if (type & 1) return NULL;
    uint8_t be[6];
    for (int i = 0; i < 6; i++) be[i] = addr[5 - i];
    return lap_ids_maker(be);
}
/* A device's name: the owner's or one it gave before (lap_ids), its own,
   else what it is: service, maker of its data, maker of its address, look. */
static const char *dev_label(const bxdev_t *d, char *buf, size_t cap)
{
    const char *kept = lap_ids_device(d->addr, d->type);
    bx_uuid_t u = {{(uint8_t)d->svc, (uint8_t)(d->svc >> 8)}, 2};
    const char *sn = d->svc ? bx_service_name(&u) : NULL, *co = d->company != 0xFFFF ? lap_ids_get(LAP_IDS_COMPANY, d->company) : NULL;
    const char *mk = maker(d->addr, d->type), *lk = d->look ? lap_ids_get(LAP_IDS_APPEARANCE, d->look) : NULL;
    if (kept) snprintf(buf, cap, "%s", kept);
    else if (d->name[0]) snprintf(buf, cap, "%s", d->name);
    else if (sn) snprintf(buf, cap, "(%s)", sn);
    else if (co) snprintf(buf, cap, "(%s)", co);
    else if (mk) snprintf(buf, cap, "(%s)", mk);
    else if (lk) snprintf(buf, cap, "(%s)", lk);
    else if (d->company != 0xFFFF) snprintf(buf, cap, "(company %04x)", d->company);
    else snprintf(buf, cap, "(no name)");
    return buf;
}
static const char *conn_name(const bx_conn_t *c)
{
    const char *kept = lap_ids_device(c->addr, c->addr_type);
    return kept ? kept : c->name[0] ? c->name : "unnamed device";
}
/* The namer bx_core asks first. */
static const char *card_name(int kind, const uint8_t be[16]) { return lap_ids_attr(kind, be); }

/* ---- connections ------------------------------------------------------------------------ */
static int slot_of(int handle)
{
    for (int i = 0; i < BX_MAX_CONN; i++) if (conns[i] && conns[i]->handle == handle) return i;
    return -1;
}
static void connect_dev(const bxdev_t *d)
{
    if (!d) { say("No device picked"); return; }
    for (int i = 0; i < BX_MAX_CONN; i++)
        if (conns[i] && conns[i]->handle && !memcmp(conns[i]->addr, d->addr, 6)) { cur = i; tab = T_GATT; say("Already connected"); return; }
    int s = -1;
    for (int i = 0; i < BX_MAX_CONN && s < 0; i++) if (!conns[i] || !conns[i]->handle) s = i;
    if (s < 0) { say("All %d slots in use: X disconnects one", BX_MAX_CONN); return; }
    if (!conns[s]) conns[s] = calloc(1, sizeof(bx_conn_t));
    if (!conns[s]) { say("No memory for another connection"); return; }
    const int h = lap_ble_connect(d->addr, d->type);
    if (h < 0) {
        const int n = live();
        if (h == LAP_IO_EBUSY && n) {
            board_limit = n;
            say("Only %d app connection%s on this board: X frees one", n, n == 1 ? "" : "s");
        } else say("Connect refused: %s (%d)", bx_error_text(h), h);
        return;
    }
    bx_begin(conns[s], h, d->addr, d->type, d->name);
    memset(&airsync[s], 0, sizeof(airsync[s]));
    pair_text[0] = 0;
    cur = s;
    gsel = gtop = 0;
    tab = T_GATT;                       /* on a wide screen: keys now move in the table */
    say("Connecting to slot %d", s + 1);
}
static void disconnect_cur(void)
{
    bx_conn_t *c = conns[cur];
    if (!c || !c->handle) return;
    lap_ble_disconnect(c->handle);
    c->handle = 0;
    if (c->state == BX_READY || c->state == BX_CONNECTING) c->state = BX_GONE;
    say("Slot %d disconnected; its table stays until reused", cur + 1);
}
static void drive(bx_conn_t *c)
{
    const bx_op_t op = bx_next(c);
    if (op.kind == BX_OP_NONE) return;
    const int r = op.kind == BX_OP_DISCOVER ? lap_gatt_discover(c->handle, op.a, op.b)
                : op.kind == BX_OP_DESCRIPTORS ? lap_gatt_descriptors(c->handle, op.a, op.b)
                : lap_gatt_read(c->handle, op.a);
    if (r >= 0) bx_started(c, &op);
    else bx_refused(c, &op, r);
}
/* A write: a request when the characteristic takes one, else a command
   (write without response) where the board has it. */
static int gatt_write(const bx_conn_t *c, uint16_t a, const uint8_t *v, size_t n, bool no_rsp)
{
#if defined(LAP_API_29)
    if (no_rsp) return lap_gatt_write_cmd(c->handle, a, v, n);
#else
    (void)no_rsp;
#endif
    return lap_gatt_write(c->handle, a, v, n);
}

/* ---- pairing (API 2.9) ---------------------------------------------------------------------- */
#if defined(LAP_API_29)
static void security_event(int s, const lap_ble_event_t *e, const uint8_t *data)
{
    bx_conn_t *c = conns[s];
    lap_ble_security_t sec;
    memset(&sec, 0, sizeof(sec));
    memcpy(&sec, data, e->payload_size < sizeof(sec) ? e->payload_size : sizeof(sec));
    if (e->kind == LAP_BLE_SECURITY_REQUEST) { say("Slot %d asks to pair: P pairs", s + 1); return; }
    if (e->kind == LAP_BLE_PASSKEY) {
        if (dlg >= 0) lap_dialog_cancel(dlg);
        dlg = -1;
        dlg_slot = s;
        if (sec.action == LAP_BLE_PASSKEY_INPUT) {
            dlg = lap_text_input("Type the 6-digit code the device shows", "", 6, 0);
            dlg_kind = DLG_PASSKEY;
        } else if (sec.action == LAP_BLE_PASSKEY_NUMCMP) {
            char m[96];
            snprintf(m, sizeof(m), "Does the device show %06lu?", (unsigned long)sec.passkey);
            dlg = lap_confirm(m);
            dlg_kind = DLG_NUMCMP;
        } else {
            snprintf(pair_text, sizeof(pair_text), "type %06lu on the device", (unsigned long)sec.passkey);
            say("Type %06lu on the device", (unsigned long)sec.passkey);
        }
        return;
    }
    /* LAP_BLE_SECURITY: pairing ended */
    if (e->error || !sec.encrypted) { say("Pairing failed: %s", e->error ? bx_error_text(e->error) : "not encrypted"); pair_text[0] = 0; return; }
    snprintf(pair_text, sizeof(pair_text), "%s%s", sec.authenticated ? "paired, checked" : "paired", sec.bonded ? ", kept" : "");
    if (bx_reread(c)) say("Slot %d paired: reading the table again", s + 1);
    else say("Slot %d paired", s + 1);
}
#endif

static void note_in(int s, bx_conn_t *c, const lap_ble_event_t *e, const uint8_t *data)
{
    bx_note_t *n = &notes[note_head];
    note_head = (note_head + 1) % BX_NOTES;
    if (nnotes < BX_NOTES) nnotes++;
    memset(n, 0, sizeof(*n));
    n->at_ms = lap_millis();
    n->slot = (uint8_t)s;
    n->handle = e->attribute;
    n->vlen = (uint8_t)(e->payload_size < BX_VAL ? e->payload_size : BX_VAL);
    memcpy(n->val, data, n->vlen);
    bool wechat = false;
    for (int k = 0; k < c->nchr; k++)
        if (c->chr[k].value == e->attribute) {
            n->uuid16 = bx_uuid16(&c->chr[k].uuid);
            wechat = n->uuid16 == 0xFEC8 || (c->chr[k].svc < c->nsvc && bx_uuid16(&c->svc[c->chr[k].svc].uuid) == 0xFEE7);
        }
    bx_airsync(&airsync[s], e->attribute, wechat, data, e->payload_size, n->text, sizeof(n->text));
}
/* A Device Name read (walk, R or a flow): the device's own name, kept. */
static void value_in(bx_conn_t *c, const lap_ble_event_t *e, const uint8_t *data)
{
    if (e->error) return;
    for (int k = 0; k < c->nchr; k++) {
        if (c->chr[k].value != e->attribute || bx_uuid16(&c->chr[k].uuid) != 0x2A00) continue;
        char nm[24];
        size_t m = e->payload_size < sizeof(nm) - 1 ? e->payload_size : sizeof(nm) - 1;
        for (size_t i = 0; i < m; i++) {
            if (!data[i]) { m = i; break; }                     /* NUL padding ends it */
            nm[i] = data[i] >= 32 && data[i] < 127 ? (char)data[i] : '.';
        }
        while (m && nm[m - 1] == ' ') m--;
        nm[m] = 0;
        if (!m) return;
        lap_ids_device_learn(c->addr, c->addr_type, nm);
        if (!c->name[0]) snprintf(c->name, sizeof(c->name), "%s", nm);
    }
}
static void events_in(void)
{
    lap_ble_event_t e;
    static uint8_t data[BX_EVENT_MAX];          /* more than 20 bytes: the board exchanges a bigger MTU (API 2.9) */
    for (int i = 0; i < 32; i++) {
        const int got = lap_ble_event_next(&e, data, sizeof(data));
        if (got != 1) break;
        events++;
        const int s = slot_of(e.connection);
        if (s < 0) continue;
        bx_conn_t *c = conns[s];
        if (e.payload_size > sizeof(data)) e.payload_size = sizeof(data);
#if defined(LAP_API_29)
        if (e.kind == LAP_BLE_SECURITY || e.kind == LAP_BLE_PASSKEY || e.kind == LAP_BLE_SECURITY_REQUEST) { security_event(s, &e, data); continue; }
#endif
        const uint8_t was = c->state;
        if (e.kind == LAP_GATT_VALUE) value_in(c, &e, data);
        if (bx_event(c, &e, data, e.payload_size)) note_in(s, c, &e, data);
        if (c->state == BX_FAILED && was != BX_FAILED) {
            if (c->error == LAP_IO_EBUSY && live() > 1) {
                board_limit = live() - 1;
                say("Only %d app connection%s on this board: X frees one", board_limit, board_limit == 1 ? "" : "s");
            } else say("Slot %d: connect failed, %s", s + 1, bx_error_text(c->error));
        }
        if (c->state == BX_READY && was == BX_CONNECTING) say("Slot %d connected: walking the table", s + 1);
        if (c->state == BX_GONE && was != BX_GONE) say("Slot %d: link lost%s", s + 1, c->error ? " (timed out)" : "");
        if ((c->state == BX_FAILED || c->state == BX_GONE) && c->handle) {
            lap_ble_disconnect(c->handle);          /* hand the link back so another connect can start */
            c->handle = 0;
        }
    }
}

/* ---- manual operations on the picked row ----------------------------------------------- */
static void build_rows(const bx_conn_t *c)
{
    nrows = 0;
    if (!c) return;
    for (int s = 0; s < c->nsvc; s++) {
        rows[nrows++] = (row_t){0, (uint16_t)s, 0};
        for (int i = 0; i < c->nchr; i++) {
            if (c->chr[i].svc != s) continue;
            rows[nrows++] = (row_t){1, (uint16_t)i, 0};
            for (int k = 0; k < c->ndsc; k++) if (c->dsc[k].chr == i) rows[nrows++] = (row_t){2, (uint16_t)k, 0};
        }
    }
    if (gsel >= nrows) gsel = nrows ? nrows - 1 : 0;
    /* the tree's branches: is each row the last of its kind under its parent? */
    uint8_t svc_last = 0, chr_last = 0;
    for (int r = 0; r < nrows; r++) {
        bool last = true;
        for (int j = r + 1; j < nrows; j++) {
            if (rows[j].kind < rows[r].kind) break;
            if (rows[j].kind == rows[r].kind) { last = false; break; }
        }
        if (rows[r].kind == 0) svc_last = last ? TREE_SVC_LAST : 0;
        if (rows[r].kind == 1) chr_last = last ? TREE_CHR_LAST : 0;
        rows[r].tree = (uint8_t)((last ? TREE_LAST : 0) | svc_last | (rows[r].kind == 2 ? chr_last : 0));
    }
}
static int picked_chr(const bx_conn_t *c)
{
    if (!c || gsel >= nrows) return -1;
    if (rows[gsel].kind == 1) return rows[gsel].i;
    if (rows[gsel].kind == 2) return c->dsc[rows[gsel].i].chr;
    return -1;
}
static bool ready(bx_conn_t *c)
{
    if (!c || !c->handle || c->state != BX_READY) { say("Not connected"); return false; }
    if (c->busy) { say("Busy: one GATT request at a time"); return false; }
    return true;
}
static void manual(bx_conn_t *c, uint8_t kind, uint16_t a, bool on)
{
    int r;
    if (kind == BX_OP_READ) r = lap_gatt_read(c->handle, a);
    else if (kind == BX_OP_WRITE) {
        const uint8_t one = 1;
        bool no_rsp = false;
        for (int i = 0; i < c->nchr; i++) if (c->chr[i].value == a) no_rsp = !(c->chr[i].props & 0x08);
        r = gatt_write(c, a, &one, 1, no_rsp);
    }
    else r = lap_gatt_subscribe(c->handle, a, on);
    bx_op_t op = {.kind = kind, .a = a, .manual = true, .on = on};
    if (r >= 0) { bx_started(c, &op); say("Sent %s to handle %u", kind == BX_OP_READ ? "read" : kind == BX_OP_WRITE ? "write 01" : on ? "subscribe" : "unsubscribe", a); }
    else say("Refused: %s (%d)", bx_error_text(r), r);
}
static void act_read(void)
{
    bx_conn_t *c = shown();
    if (!ready(c) || gsel >= nrows) return;
    if (rows[gsel].kind == 2) manual(c, BX_OP_READ, c->dsc[rows[gsel].i].handle, false);
    else if (rows[gsel].kind == 1) {
        if (!(c->chr[rows[gsel].i].props & 0x02)) { say("Not readable"); return; }
        manual(c, BX_OP_READ, c->chr[rows[gsel].i].value, false);
    } else say("Pick a characteristic or descriptor");
}
static void act_write(void)
{
    bx_conn_t *c = shown();
    const int i = picked_chr(c);
    if (!ready(c)) return;
    if (i < 0 || !(c->chr[i].props & 0x0C)) { say("Pick a writable characteristic"); return; }
    manual(c, BX_OP_WRITE, c->chr[i].value, false);
}
static void act_notify(int on)        /* -1 toggles */
{
    bx_conn_t *c = shown();
    const int i = picked_chr(c);
    if (!ready(c)) return;
    if (i < 0 || !c->chr[i].cccd) { say("Pick a characteristic that notifies"); return; }
    manual(c, BX_OP_SUBSCRIBE, c->chr[i].cccd, on < 0 ? !c->chr[i].notifying : on != 0);
}
static void act_rewalk(void)
{
    bx_conn_t *c = shown();
    if (!ready(c)) return;
    bx_rewalk(c);
    gsel = gtop = 0;
    say("Walking the table again");
}

/* ---- NAME: the owner's own name for a row or a device --------------------------------------- */
static const bxdev_t *picked_dev(void);
static void save_begin(void);
static void ask_name(const char *kw, const char *key, const char *what, const char *now_name)
{
    if (dlg >= 0) return;
    char prompt[96];
    snprintf(prompt, sizeof(prompt), "Name for %s (saved to ble-names.txt)", what);
    snprintf(dlg_kw, sizeof(dlg_kw), "%s", kw);
    snprintf(dlg_key, sizeof(dlg_key), "%s", key);
    dlg = lap_text_input(prompt, now_name ? now_name : "", LAP_IDS_NAME - 1, 0);
    dlg_kind = DLG_NAME;
    if (dlg < 0) say("No text input here (%d)", dlg);
}
static void act_name(void)
{
    char key[48], what[64];
    if (tab == T_SCAN) {
        const bxdev_t *d = picked_dev();
        if (!d) { say("Pick a device to name"); return; }
        addr_text(d->addr, key);
        if ((d->type & 1) && (d->addr[5] & 0xC0) == 0x40) { say("This address changes every few minutes: a name would not stick"); return; }
        char label[32];
        snprintf(what, sizeof(what), "device %s", key);
        ask_name("device", key, what, lap_ids_device(d->addr, d->type) ? dev_label(d, label, sizeof(label)) : d->name);
        return;
    }
    bx_conn_t *c = shown();
    if (tab != T_GATT || !c || gsel >= nrows) { say("Pick a row in GATT or a device in SCAN to name"); return; }
    const row_t *rw = &rows[gsel];
    const bx_uuid_t *u = rw->kind == 0 ? &c->svc[rw->i].uuid : rw->kind == 1 ? &c->chr[rw->i].uuid : &c->dsc[rw->i].uuid;
    const char *kw = rw->kind == 0 ? "service" : rw->kind == 1 ? "char" : "desc";
    const char *now_name = rw->kind == 0 ? bx_service_name(u) : rw->kind == 1 ? bx_char_name(u) : bx_desc_name(u);
    bx_uuid_key(u, key, sizeof(key));
    snprintf(what, sizeof(what), "%s %.36s", kw, key);
    ask_name(kw, key, what, now_name);
}

/* ---- PAIR (API 2.9) -------------------------------------------------------------------------- */
static void act_pair(void)
{
    bx_conn_t *c = shown();
    if (!c || !c->handle || c->state != BX_READY) { say("Connect first, then pair"); return; }
#if defined(LAP_API_29)
    if (dlg >= 0) return;
    static const char *const how[] = {"Pair and keep the keys", "Pair for this link only", "Forget kept keys"};
    dlg = lap_choose(how, 3);
    dlg_kind = DLG_PAIR;
    dlg_slot = cur;
#else
    say("Pairing needs newer firmware");
#endif
}

/* ---- FLOWS ----------------------------------------------------------------------------------- */
static void flows_free(void)
{
    free(fload.pack);
    fload.pack = NULL;
    for (int i = 0; i < FL_USER_FILES; i++) { free(fload.user[i]); fload.user[i] = NULL; }
    free(fload.ents);
    fload.ents = NULL;
    fload.nuser = 0;
}
static void flows_reload(void)
{
    if (!flib) return;
    if (frun && frun->state == BXF_RUNNING) { say("Stop the flow first"); return; }
    if (frun) frun->state = BXF_IDLE;          /* its steps point into the text about to go */
    fview_run = false;
    flows_free();
    bxf_lib_init(flib);
    bxf_lib_add(flib, bx_flows_builtin, bx_flows_builtin_len, BXF_ORIGIN_BUILTIN, "built-in");
    fload.stage = 1;
    fload.job = -1;
}
/* One card job at a time: the pack's size, the pack, the folder, each file. */
static void flows_load_step(void)
{
    if (!fload.stage) return;
    if (fload.job >= 0) {
        const int r = lap_sd_poll(fload.job);
        if (r == LAP_SD_PENDING) return;
        fload.job = -1;
        switch (fload.stage) {
        case 1:                                 /* the pack's size */
            if (r > 0 && r <= FL_PACK_BYTES && (fload.pack = malloc((size_t)r + 1))) { fload.size = (uint32_t)r; fload.stage = 2; }
            else fload.stage = 3;
            return;
        case 2:
            if (r > 0) bxf_lib_add(flib, fload.pack, (size_t)r, BXF_ORIGIN_PACK, "ble-flows.bin");
            fload.stage = 3;
            return;
        case 3:
            fload.n = r > 0 ? r : 0;
            fload.i = 0;
            fload.stage = 4;
            return;
        case 4:
            if (r > 0 && fload.user[fload.nuser]) {
                bxf_lib_add(flib, fload.user[fload.nuser], (size_t)r, BXF_ORIGIN_USER, fload.ents[fload.i].name);
                fload.nuser++;
            } else { free(fload.user[fload.nuser]); fload.user[fload.nuser] = NULL; }
            fload.i++;
            return;
        default: fload.stage = 0; return;
        }
    }
    int j = -1;
    switch (fload.stage) {
    case 1: j = lap_sd_size(BXF_PACK_FILE); break;
    case 2: j = lap_sd_read(BXF_PACK_FILE, 0, fload.pack, fload.size); break;
    case 3:
        if (!fload.ents) fload.ents = calloc(32, sizeof(lap_sd_entry_t));
        if (!fload.ents) { fload.stage = 0; return; }
        j = lap_sd_list(BXF_USER_DIR, fload.ents, 32);
        break;
    case 4:
        for (; fload.i < fload.n && fload.nuser < FL_USER_FILES; fload.i++) {
            const lap_sd_entry_t *en = &fload.ents[fload.i];
            const size_t k = strlen(en->name);
            if (en->dir || k < 6 || strcmp(en->name + k - 5, ".flow") || !en->size || en->size > FL_USER_BYTES) continue;
            if (!fload.user[fload.nuser] && !(fload.user[fload.nuser] = malloc(en->size + 1))) continue;   /* kept when the card said busy */
            snprintf(fload.path, sizeof(fload.path), "%s/%s", BXF_USER_DIR, en->name);
            j = lap_sd_read(fload.path, 0, fload.user[fload.nuser], en->size);
            break;
        }
        if (j < 0 && (fload.i >= fload.n || fload.nuser >= FL_USER_FILES)) {
            fload.stage = 0;
            free(fload.ents);
            fload.ents = NULL;
            return;
        }
        break;
    default: fload.stage = 0; return;
    }
    if (j == LAP_SD_EBUSY) return;              /* the card is busy: next frame */
    if (j < 0) {                                 /* refused: on to the next source */
        if (fload.stage == 4) { free(fload.user[fload.nuser]); fload.user[fload.nuser] = NULL; fload.i++; }
        else fload.stage = fload.stage == 1 || fload.stage == 2 ? 3 : 0;
        return;
    }
    fload.job = j;
}
static void flow_go(int idx)
{
    bx_conn_t *c = shown();
    lap_time_t t;
    lap_time(&t);
    const bxf_time_t tm = {t.year, t.month, t.day, t.hour, t.minute, t.second, t.weekday};
    if (bxf_begin(frun, &flib->e[idx], c, &tm, lap_millis())) { say("%s", frun->why); return; }
    frun_slot = cur;
    fview_run = true;
    say("Running %s", flib->e[idx].name);
}
static void act_run(void)
{
    if (!flib || !frun) return;
    if (fview_run && frun->state != BXF_RUNNING) { fview_run = false; return; }      /* RUN in the log: back to the list */
    if (frun->state == BXF_RUNNING) { say("A flow is running: STOP first"); fview_run = true; return; }
    if (fsel >= flib->n) { say("No flow picked"); return; }
    const int idx = forder[fsel];
    const bx_conn_t *c = shown();
    if (!c || !c->handle || c->state != BX_READY) { say("Connect to a device first"); return; }
    if (c->phase != BX_P_DONE) { say("Wait for the table walk to finish"); return; }
    if (flib->e[idx].writes) {
        if (dlg >= 0) return;
        /* writes ask first: the flow's own question, or a plain one */
        bxf_err_t err;
        char m[160];
        if (!bxf_parse(flib->e[idx].text, flib->e[idx].len, flib->e[idx].line0, &flib->scratch, NULL, &err) && flib->scratch.confirm[0])
            snprintf(m, sizeof(m), "%s", flib->scratch.confirm);
        else snprintf(m, sizeof(m), "%.40s writes to the device. Run it?", flib->e[idx].name);
        dlg = lap_confirm(m);
        dlg_kind = DLG_RUN;
        dlg_flow = idx;
        return;
    }
    flow_go(idx);
}
static void act_stop(void)
{
    if (frun && frun->state == BXF_RUNNING) { bxf_stop(frun, lap_millis()); say("%s", frun->why); }
    else say("No flow running");
}
static void flow_step(void)
{
    if (!frun || frun->state != BXF_RUNNING) return;
    bx_conn_t *c = conns[frun_slot];
    bxf_req_t q;
    if (!bxf_tick(frun, c, lap_millis(), &q)) {
        if (frun->state == BXF_DONE) say("%s: done in %.1f s", frun->f.name, (double)(frun->ended - frun->started) / 1000.0);
        else if (frun->state == BXF_FAILED) say("%s", frun->why);
        return;
    }
    const int r = q.op.kind == BX_OP_READ ? lap_gatt_read(c->handle, q.op.a)
                : q.op.kind == BX_OP_WRITE ? gatt_write(c, q.op.a, q.data, q.n, q.no_rsp)
                : lap_gatt_subscribe(c->handle, q.op.a, q.op.on);
    bxf_sent(frun, c, &q, r, lap_millis());
}

/* ---- the open dialog ------------------------------------------------------------------------- */
static void dialogs(void)
{
    if (dlg < 0) return;
    char t[64] = "";
    int v = 0;
    const int r = lap_dialog_poll(dlg, t, sizeof(t), &v);
    if (r == 0) return;
    const int kind = dlg_kind;
    dlg = -1;
    dlg_kind = DLG_NONE;
    bx_conn_t *c = dlg_slot >= 0 && dlg_slot < BX_MAX_CONN ? conns[dlg_slot] : NULL;
    (void)c;
    if (r != 1) {
        if (kind == DLG_NAME) say("Not named");
        if (kind == DLG_PASSKEY || kind == DLG_NUMCMP) say("No code given: the pairing will time out");
        return;
    }
    if (kind == DLG_NAME) {
        size_t k = strlen(t);
        while (k && t[k - 1] == ' ') t[--k] = 0;
        if (!k) { say("Not named"); return; }
        const int e = lap_ids_user_add(dlg_kw, dlg_key, t);
        if (!e) say("Named %s %.24s: %s", dlg_kw, dlg_key, t);
        else say("%s", e == -2 ? "The name table is full" : e == -3 ? "Card refused the name" : "That name could not be filed");
    } else if (kind == DLG_RUN) {
        if (v == 1) flow_go(dlg_flow);
        else say("Not run");
    } else if (kind == DLG_MORE) {
        if (v == 0) act_write();
        else if (v == 1) act_pair();
        else if (v == 2) save_begin();
        else if (v == 3) act_rewalk();
        else if (v == 4) disconnect_cur();
    }
#if defined(LAP_API_29)
    else if (kind == DLG_PAIR && c && c->handle) {
        if (v == 2) {
            const int e = lap_ble_forget(c->handle);
            if (e >= 0) { say("Forgetting this device's keys"); pair_text[0] = 0; }
            else say("Forget refused: %s", bx_error_text(e));
            return;
        }
        const int e = lap_ble_pair(c->handle, LAP_BLE_PAIR_MITM | (v == 0 ? LAP_BLE_PAIR_BOND : 0));
        if (e >= 0) { snprintf(pair_text, sizeof(pair_text), "pairing..."); say("Pairing slot %d: follow the device", dlg_slot + 1); }
        else say("Pair refused: %s", bx_error_text(e));
    } else if (kind == DLG_PASSKEY && c && c->handle) {
        lap_ble_passkey(c->handle, LAP_BLE_PASSKEY_INPUT, (uint32_t)strtoul(t, NULL, 10));
    } else if (kind == DLG_NUMCMP && c && c->handle) {
        lap_ble_passkey(c->handle, LAP_BLE_PASSKEY_NUMCMP, v == 1 ? 1u : 0u);
    }
#endif
}

/* ---- saving the table ---------------------------------------------------------------------- */
static void save_begin(void)
{
    bx_conn_t *c = shown();
    if (save.stage) { say("Still saving"); return; }
    if (!c || !c->nsvc) { say("Nothing to save yet"); return; }
    bx_note_t mine[BX_NOTES];
    int n = 0;
    for (int k = 0; k < nnotes; k++) {
        const bx_note_t *t = &notes[(note_head - nnotes + k + BX_NOTES) % BX_NOTES];
        if (t->slot == cur) mine[n++] = *t;
    }
    lap_time_t t;
    lap_time(&t);
    const size_t need = bx_json(c, mine, n, t.unix, NULL, 0);
    free(save.buf);
    save.buf = malloc(need + 1);
    if (!save.buf) { say("No memory to save"); return; }
    save.len = bx_json(c, mine, n, t.unix, save.buf, need + 1);
    save.done = 0;
    snprintf(save.path, sizeof(save.path), "/sdcard/ble_explorer/gatt-%02x%02x%02x%02x%02x%02x-%04d%02d%02d-%02d%02d%02d.json",
             c->addr[5], c->addr[4], c->addr[3], c->addr[2], c->addr[1], c->addr[0], t.year, t.month, t.day, t.hour, t.minute, t.second);
    save.job = lap_sd_mkdir("/sdcard/ble_explorer");
    save.stage = 1;
    say("Saving %s", save.path + 8);
}
static void save_step(void)
{
    if (!save.stage) return;
    if (save.job >= 0) {
        const int r = lap_sd_poll(save.job);
        if (r == LAP_SD_PENDING) return;
        if (r < 0 && save.stage > 1) { say("Save failed (%d): is there a card?", r); save.stage = 0; return; }
    }
    if (save.done >= save.len) {
        say("Saved %s (%u bytes)", save.path + 21, (unsigned)save.len);
        lap_log("saved %s", save.path);
        save.stage = 0;
        free(save.buf);
        save.buf = NULL;
        return;
    }
    const size_t n = save.len - save.done > 32768 ? 32768 : save.len - save.done;
    save.job = lap_sd_write(save.path, save.done ? LAP_SD_APPEND : 0, save.buf + save.done, (uint32_t)n);
    if (save.job < 0) { say("Save refused (%d)", save.job); save.stage = 0; return; }
    save.done += n;
    save.stage = 2;
}

/* ---- drawing -------------------------------------------------------------------------------- */
/* The link as the board has it (API 2.9): MTU, signal, PHY, security. */
static void link_text(const bx_conn_t *c, char *out, size_t cap)
{
    out[0] = 0;
#if defined(LAP_API_29)
    lap_ble_conn_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.size = sizeof(ci);
    ci.version = 1;
    if (!c->handle || c->state != BX_READY || lap_ble_conn_info(c->handle, &ci)) { snprintf(out, cap, "%s", pair_text); return; }
    char rs[12] = "";
    if (ci.rssi != 127) snprintf(rs, sizeof(rs), "%d dBm  ", ci.rssi);
    const char *phy = ci.phy_rx == 2 ? "2M" : ci.phy_rx == 3 ? "coded" : "1M";
    const char *sec = ci.encrypted ? (ci.authenticated ? (ci.bonded ? "locked+checked+kept" : "locked+checked") : ci.bonded ? "locked+kept" : "locked")
                                   : pair_text[0] ? pair_text : "open";
    snprintf(out, cap, "mtu %u  %s%s  %s", ci.mtu, rs, phy, sec);
#else
    (void)c;
    snprintf(out, cap, "%s", pair_text);
#endif
}
static const char *state_text(const bx_conn_t *c)
{
    switch (c->state) {
    case BX_CONNECTING: return "connecting";
    case BX_READY: return c->phase == BX_P_DONE ? "ready" : "walking";
    case BX_FAILED: return "failed";
    case BX_GONE: return "closed";
    default: return "-";
    }
}
/* The device: a box with its antenna, and waves off it. While connecting or
   walking the waves run outward; a ready link holds them lit; listening for
   adverts they breathe slowly; nothing heard, none. rows: 4 or more. */
static void device_art(int x, int y, int rows, int mode)      /* mode 0 idle, 1 listening, 2 busy, 3 ready, 4 failed */
{
    const uint8_t fr = LAP_ATTR(LAP_CYAN, LAP_BLACK), body = LAP_ATTR(LAP_WHITE, LAP_BLACK);
    const bool tall = rows >= 6;                    /* four rows drop the antenna's tip and the grille */
    int r = y;
    if (tall) put(x + 1, r++, 3, fr, "\\|/");
    const int mast = r;
    put(x + 2, r++, 1, fr, "|");
    put(x, r++, 5, body, ".-+-.");
    put(x, r, 1, body, "|");
    put(x + 1, r, 3, mode == 4 ? LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK) : mode >= 2 ? LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK) : body,
        "%s", mode == 4 ? " x " : mode >= 2 ? "[o]" : "[ ]");
    put(x + 4, r++, 1, body, "|");
    if (tall) put(x, r++, 5, body, "| ::|");
    put(x, r, 5, body, "'---'");
    const uint32_t now = lap_millis();
    for (int k = 0; k < 3; k++) {
        bool lit = false;
        uint8_t hue = LAP_CYAN;
        if (mode == 1) lit = (int)(now / 700u % 4u) > k;
        else if (mode == 2) { lit = (int)(now / 220u % 4u) > k; hue = LAP_YELLOW | LAP_BRIGHT; }
        else if (mode == 3) { lit = true; hue = k == (int)(now / 500u % 3u) ? LAP_GREEN | LAP_BRIGHT : LAP_GREEN; }
        if (!lit) continue;
        /* each wave a little taller than the last: ) then ) over ) */
        put(x + 4 + k * 2, mast, 1, LAP_ATTR(hue, LAP_BLACK), ")");
        if (k && mast > y) put(x + 4 + k * 2, mast - 1, 1, LAP_ATTR(hue, LAP_BLACK), "\\");
        if (k) put(x + 4 + k * 2, mast + 1, 1, LAP_ATTR(hue, LAP_BLACK), "/");
    }
}
static const char *const spin = "|/-\\";
static void header(void)
{
    const uint8_t a = LAP_ATTR(LAP_BLACK, LAP_CYAN);
    lap_fill(0, 0, W, 1, ' ', a);
    put(1, 0, W - 2, a, "BLE EXPLORER");
    char right[48];
    char lim[8] = "?";
    if (board_limit) snprintf(lim, sizeof(lim), "%d", board_limit);
    snprintf(right, sizeof(right), "%d/%s links%s", live(), lim, scan > 0 ? "  SCAN" : "");
    put(W - 1 - (int)strlen(right), 0, (int)strlen(right), a, "%s", right);
    /* the header art and, beside it, the connection shown */
    const bx_conn_t *c = shown();
    const int mode = !fitted ? 0 : !c ? (scan > 0 ? 1 : 0) : c->state == BX_CONNECTING || (c->state == BX_READY && c->phase != BX_P_DONE) ? 2 :
                     c->state == BX_READY ? 3 : 4;
    device_art(1, 1, hd, mode);
    const int fx = 15, fw = W - fx - 1;
    const uint32_t now = lap_millis();
    if (fw >= 14) {
        if (!c) {
            put(fx, 1, fw, LAP_UI_TEXT, "%s", fitted ? "no connection" : "Bluetooth not fitted");
            put(fx, 2, fw, LAP_UI_DIM, "%d device%s heard", ndev, ndev == 1 ? "" : "s");
            const bxdev_t *pk = fitted ? picked_dev() : NULL;
            if (pk) {
                /* what CONNECT will dial: the list re-sorts under the finger */
                char label[32], a[18];
                addr_text(pk->addr, a);
                put(fx, 3, fw, LAP_CYAN | LAP_BRIGHT, "pick %.14s %s", dev_label(pk, label, sizeof(label)), a);
            } else put(fx, 3, fw, LAP_UI_FAINT, "%s", fitted ? "pick one in SCAN, then CONNECT" : "");
            if (ndev && hd >= 6) {
                /* the loudest advertiser, on a meter */
                int best = 0;
                for (int i = 1; i < ndev; i++) if (devs[i].rssi > devs[best].rssi) best = i;
                char label[32];
                put(fx, 4, fw, LAP_UI_DIM, "loudest %.16s %d dBm", dev_label(&devs[best], label, sizeof(label)), devs[best].rssi);
                lap_ui_meter(fx, 5, fw > 30 ? 30 : fw, 1.0f - art_range(devs[best].rssi), LAP_GREEN | LAP_BRIGHT);
            }
        } else {
            char ad[18];
            addr_text(c->addr, ad);
            put(fx, 1, fw, LAP_UI_TEXT, "[%d] %s", cur + 1, conn_name(c));
            const bool busy = mode == 2;
            int x = lap_ui_chip(fx, 2, busy ? (c->state == BX_CONNECTING ? "CONNECTING" : "WALKING") : mode == 3 ? "READY" : "CLOSED",
                                !busy || (now / 300u) % 2u, mode == 3 ? LAP_GREEN : busy ? LAP_YELLOW : LAP_RED) + 1;
            if (x + 12 < W) put(x, 2, W - x - 1, LAP_UI_DIM, "%s", ad);
            if (hd >= 5 || busy) {
                const int ry = hd >= 5 ? 4 : 3;
                if (busy) {
                    /* the walk: a spinner, how far, and a meter */
                    const int pct = c->state == BX_READY ? bx_progress(c) : 0;
                    put(fx, ry, 6, LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK), "%c %3d%%", spin[now / 120u % 4u], pct);
                    lap_ui_meter(fx + 7, ry, fw - 7 > 30 ? 30 : fw - 7, (float)pct / 100.0f, LAP_YELLOW | LAP_BRIGHT);
                } else put(fx, ry, fw, LAP_UI_DIM, "%d services  %d chars  %d descs", c->nsvc, c->nchr, c->ndsc);
            }
            if (hd >= 5 && !busy) {
                char lk[64];
                link_text(c, lk, sizeof(lk));
                const char *st = c->errors ? "some reads refused" : c->truncated ? "table full" : "table complete";
                put(fx, 3, fw, LAP_UI_FAINT, "%s", st);
                if (lk[0] && fw > (int)strlen(st) + 4)
                    put(fx + (int)strlen(st) + 2, 3, fw - (int)strlen(st) - 2, LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK), "%s", lk);
            }
        }
        if (hd >= 6 && nnotes) {
            /* the newest notification, lit for half a second when it lands */
            const bx_note_t *n = &notes[(note_head - 1 + BX_NOTES) % BX_NOTES];
            char hex[3 * BX_VAL + 1], dec[64] = "";
            bx_hex(n->val, n->vlen, hex, sizeof(hex));
            if (n->text[0]) snprintf(dec, sizeof(dec), "%s", n->text);           /* a WeChat packet, decoded as it landed */
            else if (n->uuid16) bx_decode(n->uuid16, n->val, n->vlen, dec, sizeof(dec));
            const bool fresh = now - n->at_ms < 600u;
            put(fx, 6, fw, fresh ? LAP_ATTR(LAP_BLACK, LAP_GREEN | LAP_BRIGHT) : LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK), "~ %u notes  %s",
                (unsigned)nnotes, dec[0] ? dec : hex);
        } else if (hd >= 6) put(fx, 6, fw, LAP_UI_FAINT, "no notifications");
    }
    if (!desk) {
        /* the handheld's strip: < name > as finger-sized keys; GATT names the slot it shows */
        char label[24], n[16];
        if (tab == T_SCAN) snprintf(label, sizeof(label), "SCAN %d", ndev);
        else if (tab == T_GATT) snprintf(label, sizeof(label), "GATT [%d]", cur + 1);
        else if (tab == T_NOTIFY) snprintf(label, sizeof(label), "NOTIFY %d", nnotes);
        else snprintf(label, sizeof(label), "FLOWS %d", flib ? flib->n : 0);
        if (tab == T_GATT && live() > 1) snprintf(n, sizeof(n), "%d/%d  SLOT>", tab + 1, T_COUNT);
        else snprintf(n, sizeof(n), "%d/%d", tab + 1, T_COUNT);
        const lap_btn_t k[3] = {{"<", NULL, 0, false, false, false}, {label, n, 0, false, false, false}, {">", NULL, 0, false, false, false}};
        art_strip(ty, W, tr, k[1].label, k[1].value, k[1].badge);
        return;
    }
    lap_fill(0, ty, W, 1, ' ', LAP_ATTR(LAP_WHITE, LAP_BLACK));
    int x = 0;
    for (int t = 0; t < T_COUNT; t++) {
        char name[24];
        if (t == T_SCAN) snprintf(name, sizeof(name), " SCAN %d ", ndev);
        else if (t == T_GATT) snprintf(name, sizeof(name), " GATT ");
        else if (t == T_NOTIFY) snprintf(name, sizeof(name), " NOTIFY %d ", nnotes);
        else snprintf(name, sizeof(name), " FLOWS%s ", frun && frun->state == BXF_RUNNING ? " >" : "");
        tab_x[t] = x;
        put(x, ty, (int)strlen(name), t == tab ? LAP_ATTR(LAP_BLACK, LAP_YELLOW | LAP_BRIGHT) : LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%s", name);
        x += (int)strlen(name) + 1;
    }
    tab_x[T_COUNT] = x;
    /* the connection slots, right of the tabs */
    for (int i = 0; i < BX_MAX_CONN && x + 4 < W; i++) {
        const bx_conn_t *c = conns[i];
        const bool used = c && (c->handle || c->nsvc);
        const uint8_t at = i == cur ? LAP_ATTR(LAP_BLACK, used ? LAP_GREEN | LAP_BRIGHT : LAP_WHITE)
                                    : LAP_ATTR(used ? (c->state == BX_READY ? LAP_GREEN | LAP_BRIGHT : LAP_RED | LAP_BRIGHT) : LAP_BLUE, LAP_BLACK);
        put(x, ty, 3, at, "[%d]", i + 1);
        x += 4;
    }
}

static void scan_pane(int x, int y, int wd, int h)
{
    const uint8_t hd = LAP_ATTR(LAP_CYAN, LAP_BLACK);
    dlist_y = y + 1;
    dlist_h = h - 1;
    const int lw = wd - (18 + 5 + 6 + 4);
    put(x, y, wd, hd, "  %-*s %-17s %4s %5s", lw - 1, "DEVICE", "ADDRESS", "RSSI", "SEEN");
    if (!ndev) {
        if (h >= 9) device_art(x + (wd - 11) / 2, dlist_y + 1, 6, fitted && scan > 0 ? 1 : 0);
        put(x + 1, dlist_y + (h >= 9 ? 8 : 1), wd - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%s", fitted ? "Listening for adverts..." : "Bluetooth not fitted");
        return;
    }
    if (dsel >= ndev) dsel = ndev - 1;
    dsmax = ndev > dlist_h ? ndev - dlist_h : 0;
    if (dshow && !dragging) {
        if (dsel < dtop) dtop = dsel;
        if (dsel >= dtop + dlist_h) dtop = dsel - dlist_h + 1;
        dshow = false;
    }
    if (dtop > dsmax) dtop = dsmax;
    if (dtop < 0) dtop = 0;
    for (int r = 0; r < dlist_h && dtop + r < ndev; r++) {
        const bxdev_t *d = &devs[dtop + r];
        const bool on = dtop + r == dsel;
        int linked = -1;
        for (int i = 0; i < BX_MAX_CONN; i++) if (conns[i] && conns[i]->handle && !memcmp(conns[i]->addr, d->addr, 6)) linked = i;
        const uint8_t at = on ? LAP_ATTR(LAP_BLACK, LAP_WHITE) : LAP_ATTR(lap_millis() - d->last < 30000 ? LAP_WHITE : LAP_BLUE | LAP_BRIGHT, LAP_BLACK);
        char label[32], a[18];
        dev_label(d, label, sizeof(label));
        addr_text(d->addr, a);
        lap_fill(x, y + 1 + r, wd, 1, ' ', at);
        if (linked >= 0) lap_put_cell(x, y + 1 + r, (int16_t)('1' + linked), on ? at : LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK));
        put(x + 2, y + 1 + r, lw - 1, at, "%s", label);
        put(x + 1 + lw + 1, y + 1 + r, 17, on ? at : LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s", a);
        put(x + 1 + lw + 19, y + 1 + r, 4, at, "%4d", d->rssi);
        const uint32_t age = (lap_millis() - d->last) / 1000;
        put(x + 1 + lw + 24, y + 1 + r, 5, at, "%4lus", (unsigned long)age);
    }
}

/* One row of the tree in columns: name, properties, value. */
static void row_draw(const bx_conn_t *c, const row_t *rw, int x, int y, int wd, bool on)
{
    char u[40], dec[80], props[8], name[48];
    const int nw = wd * 45 / 100;
    uint8_t at, va;
    dec[0] = 0;
    if (rw->kind == 0) {
        const bx_svc_t *s = &c->svc[rw->i];
        const char *n = bx_service_name(&s->uuid);
        bx_uuid_text(&s->uuid, u, sizeof(u));
        at = on ? LAP_ATTR(LAP_BLACK, LAP_YELLOW | LAP_BRIGHT) : LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK);
        lap_fill(x, y, wd, 1, ' ', at);
        put(x, y, 2, on ? at : LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s", rw->tree & TREE_LAST ? "`-" : "+-");
        put(x + 2, y, wd - 2, at, "%s %s", n ? n : "Service", u);
        char h[16];
        snprintf(h, sizeof(h), "%u-%u", s->start, s->end);
        put(x + wd - (int)strlen(h), y, (int)strlen(h), at, "%s", h);
        return;
    }
    if (rw->kind == 1) {
        const bx_chr_t *h = &c->chr[rw->i];
        const char *n = bx_char_name(&h->uuid);
        bx_uuid_text(&h->uuid, u, sizeof(u));
        bx_props_text(h->props, props, sizeof(props));
        snprintf(name, sizeof(name), "%s", n ? n : u);
        if (h->error) snprintf(dec, sizeof(dec), "(%s)", bx_error_text(h->error));
        else if (h->has_value && !bx_decode(bx_uuid16(&h->uuid), h->val, h->vlen, dec, sizeof(dec))) bx_hex(h->val, h->vlen, dec, sizeof(dec));
        if (!h->error && h->has_value && h->vlen >= 2 && bx_uuid16(&h->uuid) == 0x2A01) {     /* the library's word for the look */
            const unsigned look = (unsigned)(h->val[0] | h->val[1] << 8);
            const char *a = lap_ids_get(LAP_IDS_APPEARANCE, look);
            if (a) snprintf(dec, sizeof(dec), "%s (0x%04x)", a, look);
        }
        at = on ? LAP_ATTR(LAP_BLACK, LAP_WHITE) : LAP_ATTR(LAP_WHITE, LAP_BLACK);
        va = on ? at : h->error ? LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK) : LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK);
        lap_fill(x, y, wd, 1, ' ', at);
        put(x, y, 4, on ? at : LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s%s", rw->tree & TREE_SVC_LAST ? "  " : "| ", rw->tree & TREE_LAST ? "`-" : "+-");
        put(x + 4, y, nw - 4, at, "%s", name);
        put(x + nw, y, 5, on ? at : LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK), "%-4s", props);
        put(x + nw + 5, y, 1, on ? at : LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK), h->notifying ? "*" : " ");
        put(x + nw + 7, y, wd - nw - 7, va, "%s", dec);
        return;
    }
    const bx_dsc_t *d = &c->dsc[rw->i];
    const char *n = bx_desc_name(&d->uuid);
    bx_uuid_text(&d->uuid, u, sizeof(u));
    if (d->error) snprintf(dec, sizeof(dec), "(%s)", bx_error_text(d->error));
    else if (d->has_value && !bx_decode_desc(bx_uuid16(&d->uuid), d->val, d->vlen, dec, sizeof(dec))) bx_hex(d->val, d->vlen, dec, sizeof(dec));
    at = on ? LAP_ATTR(LAP_BLACK, LAP_WHITE) : LAP_ATTR(LAP_CYAN, LAP_BLACK);
    lap_fill(x, y, wd, 1, ' ', at);
    put(x, y, 6, on ? at : LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s%s%s", rw->tree & TREE_SVC_LAST ? "  " : "| ", rw->tree & TREE_CHR_LAST ? "  " : "| ",
        rw->tree & TREE_LAST ? "`-" : "+-");
    put(x + 6, y, nw, at, "%s", n ? n : u);       /* a descriptor has no properties column: its name may run into it */
    put(x + nw + 7, y, wd - nw - 7, on ? at : LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK), "%s", dec);
}

static void pick_detail(const bx_conn_t *c, int x, int y, int wd, int h)
{
    const uint8_t k = LAP_ATTR(LAP_CYAN, LAP_BLACK), v = LAP_ATTR(LAP_WHITE, LAP_BLACK), g = LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK);
    const int bottom = y + h;
    lap_hline(x, y, wd, '-', LAP_ATTR(LAP_BLUE, LAP_BLACK));
    y++;
    if (!c || gsel >= nrows || h < 3) return;
    const row_t *rw = &rows[gsel];
    char u[40], hex[3 * BX_VAL + 1], dec[96], props[8];
    if (rw->kind == 0) {
        const bx_svc_t *s = &c->svc[rw->i];
        const char *n = bx_service_name(&s->uuid);
        bx_uuid_text(&s->uuid, u, sizeof(u));
        int nc = 0;
        for (int i = 0; i < c->nchr; i++) nc += c->chr[i].svc == rw->i;
        put(x, y++, wd, v, "%s", n ? n : "Service");
        put(x, y++, wd, k, "uuid %s", u);
        if (y < bottom) put(x, y++, wd, k, "handles %u-%u, %d characteristics", s->start, s->end, nc);
        return;
    }
    if (rw->kind == 1) {
        const bx_chr_t *ch = &c->chr[rw->i];
        const char *n = bx_char_name(&ch->uuid);
        bx_uuid_text(&ch->uuid, u, sizeof(u));
        bx_props_text(ch->props, props, sizeof(props));
        put(x, y++, wd, v, "%s", n ? n : "Characteristic");
        put(x, y++, wd, k, "uuid %s", u);
        if (y < bottom) put(x, y++, wd, k, "handle %u (decl %u, end %u)  %s%s", ch->value, ch->decl, ch->end, props,
                            ch->cccd ? (ch->notifying ? "  notifying" : "  can notify") : "");
        bx_hex(ch->val, ch->vlen, hex, sizeof(hex));
        if (ch->has_value && y < bottom) put(x, y++, wd, g, "%s", hex);
        if (ch->has_value && y < bottom && bx_decode(bx_uuid16(&ch->uuid), ch->val, ch->vlen, dec, sizeof(dec))) put(x, y++, wd, v, "%s", dec);
        if (ch->error && y < bottom) put(x, y++, wd, LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK), "%s", bx_error_text(ch->error));
        if (ch->notes && y < bottom) put(x, y++, wd, k, "%lu notifications", (unsigned long)ch->notes);
        return;
    }
    const bx_dsc_t *d = &c->dsc[rw->i];
    const char *n = bx_desc_name(&d->uuid);
    bx_uuid_text(&d->uuid, u, sizeof(u));
    put(x, y++, wd, v, "%s", n ? n : "Descriptor");
    put(x, y++, wd, k, "uuid %s, handle %u", u, d->handle);
    bx_hex(d->val, d->vlen, hex, sizeof(hex));
    if (d->has_value && y < bottom) put(x, y++, wd, g, "%s", hex);
    if (d->error && y < bottom) put(x, y++, wd, LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK), "%s", bx_error_text(d->error));
}

static void notify_pane(int x, int y, int wd, int h);
static void gatt_pane(int x, int y, int wd, int h)
{
    const uint8_t hd = LAP_ATTR(LAP_CYAN, LAP_BLACK);
    bx_conn_t *c = shown();
    if (!c) {
        put(x + 1, y + 1, wd - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), "No connection in slot %d.", cur + 1);
        put(x + 1, y + 3, wd - 2, hd, "Pick a device in SCAN and CONNECT.");
        if (board_limit) put(x + 1, y + 5, wd - 2, hd, "This board allows %d connection%s for apps.", board_limit, board_limit == 1 ? "" : "s");
        glist_h = 0;
        return;
    }
    char a[18];
    addr_text(c->addr, a);
    put(x, y, wd, LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK), "[%d] %s", cur + 1, conn_name(c));
    char st[48];
    if (c->state == BX_READY && c->phase != BX_P_DONE) snprintf(st, sizeof(st), "%c walking %d%%", spin[lap_millis() / 120u % 4u], bx_progress(c));
    else if (c->state == BX_FAILED || (c->state == BX_GONE && c->error)) snprintf(st, sizeof(st), "%s: %s", state_text(c), bx_error_text(c->error));
    else snprintf(st, sizeof(st), "%s", state_text(c));
    put(x + wd - (int)strlen(st), y, (int)strlen(st), c->state == BX_READY ? LAP_ATTR(LAP_GREEN, LAP_BLACK) : LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK), "%s", st);
    char lk[64];
    link_text(c, lk, sizeof(lk));
    char l1[96];
    snprintf(l1, sizeof(l1), "%s %s  %d svc  %d chr  %d dsc%s%s", a, c->addr_type & 1 ? "random" : "public", c->nsvc, c->nchr, c->ndsc,
             c->errors ? "  some refused" : "", c->truncated ? "  TABLE FULL" : "");
    put(x, y + 1, wd, hd, "%s", l1);
    /* the link (MTU, signal, PHY, security) on the right when it fits */
    if (lk[0] && wd >= (int)strlen(l1) + (int)strlen(lk) + 2)
        put(x + wd - (int)strlen(lk), y + 1, (int)strlen(lk), LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK), "%s", lk);
    build_rows(c);
    const int detail_h = h >= 24 ? 7 : 4;
    const int notes_h = !desk && h >= 40 ? (h - 2 - detail_h) / 3 : 0;   /* a tall handheld shows the latest notifications too */
    glist_y = y + 2;
    glist_h = h - 2 - detail_h - notes_h;
    gsmax = nrows > glist_h ? nrows - glist_h : 0;
    if (gshow && !dragging) {
        if (gsel < gtop) gtop = gsel;
        if (gsel >= gtop + glist_h) gtop = gsel - glist_h + 1;
        gshow = false;
    }
    if (gtop > gsmax) gtop = gsmax;
    if (gtop < 0) gtop = 0;
    for (int r = 0; r < glist_h && gtop + r < nrows; r++) row_draw(c, &rows[gtop + r], x, glist_y + r, wd, gtop + r == gsel);
    if (!nrows) {
        put(x + 1, glist_y + 1, wd - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%s", c->state == BX_READY ? "Discovering services..." :
                                                                          c->state == BX_CONNECTING ? "Connecting..." : "No table.");
        if (c->state == BX_READY || c->state == BX_CONNECTING) {
            /* a tree growing a branch at a time while the walk finds its way */
            static const char *const grow[4] = {"+-", "| +-", "| | +-", "`-"};
            const int n = (int)(lap_millis() / 300u % 5u);
            for (int i = 0; i < n && i < 4 && i + 3 < glist_h; i++) put(x + 2, glist_y + 3 + i, wd - 4, LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s %s", grow[i], i == n - 1 ? "..." : "");
        }
    }
    if (nrows > glist_h) {
        char more[32];
        snprintf(more, sizeof(more), " %d/%d ", gsel + 1, nrows);
        put(x + wd - (int)strlen(more), glist_y + glist_h - 1, (int)strlen(more), hd, "%s", more);
    }
    pick_detail(c, x, glist_y + glist_h, wd, detail_h);
    if (notes_h) {
        lap_hline(x, glist_y + glist_h + detail_h, wd, '-', LAP_ATTR(LAP_BLUE, LAP_BLACK));
        notify_pane(x, glist_y + glist_h + detail_h + 1, wd, notes_h - 1);
    }
}

static void notify_pane(int x, int y, int wd, int h)
{
    const uint8_t hd = LAP_ATTR(LAP_CYAN, LAP_BLACK);
    put(x, y, wd, hd, "  TIME  # HANDLE %-20s VALUE", "CHARACTERISTIC");
    nlist_y = y + 1;
    nlist_h = h - 1;
    nsmax = nnotes > nlist_h ? nnotes - nlist_h : 0;
    if (!nnotes) { put(x + 1, y + 2, wd - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), "No notifications yet: pick one in GATT and press N."); return; }
    if (ntop > nnotes - 1) ntop = nnotes - 1;
    for (int r = 0; r < h - 1 && ntop + r < nnotes; r++) {
        const bx_note_t *n = &notes[(note_head - 1 - ntop - r + 2 * BX_NOTES) % BX_NOTES];
        char hex[3 * BX_VAL + 1], dec[64] = "";
        bx_hex(n->val, n->vlen, hex, sizeof(hex));
        bx_uuid_t u = {{(uint8_t)n->uuid16, (uint8_t)(n->uuid16 >> 8)}, 2};
        const char *cn = n->uuid16 ? bx_char_name(&u) : NULL;
        if (n->text[0]) snprintf(dec, sizeof(dec), "%s", n->text);           /* a WeChat packet, decoded as it landed */
        else if (n->uuid16) bx_decode(n->uuid16, n->val, n->vlen, dec, sizeof(dec));
        put(x, y + 1 + r, wd, r ? LAP_ATTR(LAP_WHITE, LAP_BLACK) : LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK), "%6.1f %d %6u %-20.20s %s%s%s",
            (double)n->at_ms / 1000.0, n->slot + 1, n->handle, cn ? cn : "?", dec[0] ? dec : hex, dec[0] ? "  " : "", dec[0] ? hex : "");
    }
}

/* FLOWS: the list (the ones that fit the device shown first), or the run's
   steps as they go. */
static const char *origin_word(int o) { return o == BXF_ORIGIN_PACK ? "pack" : o == BXF_ORIGIN_USER ? "yours" : "built-in"; }
static void flow_list(int x, int y, int wd, int h)
{
    const bx_conn_t *c = shown();
    const uint8_t hd = LAP_ATTR(LAP_CYAN, LAP_BLACK);
    nfit = bxf_order(flib, c, c ? conn_name(c) : NULL, forder);
    const int detail_h = h >= 20 ? 6 : h >= 16 ? 5 : 3;
    flist_y = y + 1;
    flist_h = h - 1 - detail_h;
    if (flist_h < 1) flist_h = 1;
    const int ow = 9, sw = 6, nw = wd - 3 - ow - sw;
    put(x, y, wd, hd, "  %-*s %-*s %s", nw, nfit ? "FLOW  (+ fits this device)" : "FLOW", ow - 1, "FROM", "STEPS");
    if (fsel >= flib->n) fsel = flib->n ? flib->n - 1 : 0;
    fsmax = flib->n > flist_h ? flib->n - flist_h : 0;
    if (fshow && !dragging) {
        if (fsel < ftop) ftop = fsel;
        if (fsel >= ftop + flist_h) ftop = fsel - flist_h + 1;
        fshow = false;
    }
    if (ftop > fsmax) ftop = fsmax;
    if (ftop < 0) ftop = 0;
    for (int r = 0; r < flist_h && ftop + r < flib->n; r++) {
        const int k = ftop + r;
        const bxf_entry_t *e = &flib->e[forder[k]];
        const bool on = k == fsel, fit = k < nfit;
        const uint8_t at = on ? LAP_ATTR(LAP_BLACK, fit ? LAP_GREEN | LAP_BRIGHT : LAP_WHITE)
                              : LAP_ATTR(fit ? LAP_WHITE | LAP_BRIGHT : LAP_WHITE, LAP_BLACK);
        lap_fill(x, flist_y + r, wd, 1, ' ', at);
        put(x, flist_y + r, 1, on ? at : LAP_ATTR(fit ? LAP_GREEN | LAP_BRIGHT : LAP_BLUE, LAP_BLACK), fit ? "+" : "-");
        put(x + 2, flist_y + r, nw, at, "%s", e->name);
        const uint8_t oc = e->origin == BXF_ORIGIN_USER ? LAP_MAGENTA | LAP_BRIGHT : e->origin == BXF_ORIGIN_PACK ? LAP_YELLOW | LAP_BRIGHT : LAP_CYAN;
        put(x + 2 + nw + 1, flist_y + r, ow - 1, on ? at : LAP_ATTR(oc, LAP_BLACK), "%s", origin_word(e->origin));
        put(x + wd - sw, flist_y + r, sw, on ? at : LAP_ATTR(e->writes ? LAP_RED | LAP_BRIGHT : LAP_GREEN, LAP_BLACK), "%2u %s", e->nsteps,
            e->writes ? "W" : "R");
    }
    if (!flib->n) put(x + 1, flist_y + 1, wd - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), "No flows.");
    if (flib->n > flist_h) {
        char more[32];
        snprintf(more, sizeof(more), " %d/%d ", fsel + 1, flib->n);
        put(x + wd - (int)strlen(more), flist_y + flist_h - 1, (int)strlen(more), hd, "%s", more);
    }
    /* the pick: what it does, what it is for, where the names stand */
    int dy = flist_y + flist_h;
    lap_hline(x, dy++, wd, '-', LAP_ATTR(LAP_BLUE, LAP_BLACK));
    if (fsel < flib->n) {
        const bxf_entry_t *e = &flib->e[forder[fsel]];
        dy += put_wrap(x, dy, wd, 2, LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK), e->about[0] ? e->about : e->name);
        char m[96] = "", u[40];
        int p = 0;
        for (int i = 0; i < e->nmatch && p < (int)sizeof(m) - 8; i++) {
            if (e->match[i].kind == BXF_M_SERVICE) bx_uuid_text(&e->match[i].uuid, u, sizeof(u));
            else snprintf(u, sizeof(u), "\"%s\"", e->match[i].glob);
            p += snprintf(m + p, sizeof(m) - (size_t)p, "%s%.38s", i ? ", " : "", u);
        }
        if (dy < flist_y + flist_h + detail_h)
            put(x, dy++, wd, LAP_ATTR(LAP_CYAN, LAP_BLACK), "for %s%s", m, e->writes ? "   W: writes, asks first" : "   R: reads only");
    }
    if (dy < flist_y + flist_h + detail_h && flib->bad)
        put(x, dy++, wd, LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK), "%d bad: %s line %d, %s", flib->bad, flib->err.where, flib->err.line, flib->err.msg);
    if (dy < flist_y + flist_h + detail_h) {
        char st[48];
        const int s = lap_ids_state(st, sizeof(st));
        char line[128];
        snprintf(line, sizeof(line), "names: %s, %d yours, %d devices%s", st, lap_ids_user_count(), lap_ids_device_count(),
                 fload.stage ? ", loading flows" : "");
        put_wrap(x, dy, wd, flist_y + flist_h + detail_h - dy, LAP_ATTR(s == 2 ? LAP_GREEN : s == 1 ? LAP_YELLOW | LAP_BRIGHT : LAP_BLUE | LAP_BRIGHT, LAP_BLACK), line);
    }
}
static void flow_log(int x, int y, int wd, int h)
{
    const bxf_run_t *r = frun;
    const uint32_t now = lap_millis();
    const bool live = r->state == BXF_RUNNING;
    const char *st = live ? "RUNNING" : r->state == BXF_DONE ? "DONE" : r->state == BXF_STOPPED ? "STOPPED" : "FAILED";
    const uint8_t hue = live ? LAP_YELLOW : r->state == BXF_DONE ? LAP_GREEN : LAP_RED;
    int cx = lap_ui_chip(x, y, st, true, hue) + 1;
    put(cx, y, wd - (cx - x), LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK), "%s  [%d] %s", r->f.name, frun_slot + 1,
        conns[frun_slot] ? conn_name(conns[frun_slot]) : "");
    char tm[24];
    snprintf(tm, sizeof(tm), "%.1f s", (double)((live ? now : r->ended) - r->started) / 1000.0);
    put(x + wd - (int)strlen(tm), y, (int)strlen(tm), LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s", tm);
    /* a step a row; the one running stays in view */
    const int rows_h = h - 2;
    int top = 0;
    if (r->at >= rows_h) top = r->at - rows_h + 1;
    if (top > r->f.nsteps - rows_h) top = r->f.nsteps > rows_h ? r->f.nsteps - rows_h : 0;
    const int sw = wd * 58 / 100;
    for (int k = 0; k < rows_h && top + k < r->f.nsteps; k++) {
        const int i = top + k;
        const bxf_step_t *s = &r->f.step[i];
        const bxf_result_t *res = &r->res[i];
        const bool here = live && i == r->at;
        const int ry = y + 1 + k;
        static const char glyph[] = ".>+x~";
        const uint8_t g = res->st == BXF_S_OK ? LAP_GREEN | LAP_BRIGHT : res->st == BXF_S_FAIL ? LAP_RED | LAP_BRIGHT
                        : res->st == BXF_S_SKIP || here ? LAP_YELLOW | LAP_BRIGHT : LAP_BLUE;
        put(x, ry, 3, LAP_ATTR(LAP_CYAN, LAP_BLACK), "%2d", i + 1);
        put(x + 3, ry, 1, LAP_ATTR(g, LAP_BLACK), "%c", here ? '>' : glyph[res->st < 5 ? res->st : 0]);
        const uint8_t sc = s->op == BXF_LOG ? LAP_MAGENTA | LAP_BRIGHT : s->op == BXF_WRITE ? LAP_RED | LAP_BRIGHT
                         : s->op == BXF_WAIT || s->op == BXF_WAIT_NOTIFY ? LAP_CYAN : LAP_WHITE;
        put(x + 5, ry, sw - 5, LAP_ATTR(here ? LAP_WHITE | LAP_BRIGHT : sc, LAP_BLACK), "%.*s", (int)s->srclen, s->src);
        const int vx = x + sw + 1, vw = wd - sw - 1;
        if (here && (s->op == BXF_WAIT || s->op == BXF_WAIT_NOTIFY) && res->st == BXF_S_NOW && s->ms) {
            /* a wait: how much of it has passed, on a line meter */
            const float f = (float)(now - r->t0) / (float)s->ms;
            put(vx, ry, 7, LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK), "%4.1f s", (double)(now - r->t0) / 1000.0);
            if (vw > 12) lap_ui_meter(vx + 7, ry, vw - 7 > 24 ? 24 : vw - 7, f > 1.0f ? 1.0f : f, LAP_YELLOW | LAP_BRIGHT);
        } else if (here) put(vx, ry, vw, LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK), "%s", r->out ? "sent, waiting" : "");
        else put(vx, ry, vw, LAP_ATTR(g, LAP_BLACK), "%s", res->text);
    }
    int skipped = 0;
    for (int i = 0; i < r->f.nsteps; i++) skipped += r->res[i].st == BXF_S_SKIP;
    char end[64];
    if (skipped) snprintf(end, sizeof(end), "Done, %d step%s passed over (marked ~).", skipped, skipped == 1 ? "" : "s");
    else snprintf(end, sizeof(end), "Every step done. Notifications are in NOTIFY.");
    if (!live && h >= 3)
        put(x, y + h - 1, wd, LAP_ATTR(r->state == BXF_DONE ? LAP_GREEN : LAP_RED | LAP_BRIGHT, LAP_BLACK), "%s",
            r->state == BXF_DONE ? end : r->why);
    else if (h >= 3) put(x, y + h - 1, wd, LAP_UI_FAINT, "step %d of %d   STOP ends it", r->at + 1, r->f.nsteps);
}
static void flows_pane(int x, int y, int wd, int h)
{
    flist_h = 0;
    if (!flib || !frun) { put(x + 1, y + 1, wd - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), "No memory for flows."); return; }
    if (fview_run && frun->state != BXF_IDLE) flow_log(x, y, wd, h);
    else flow_list(x, y, wd, h);
}

/* The key bar: eight on a wide screen, three or four a tab on the handheld.
   Each key carries the letter it stands for; a press runs that letter. */
#define KEY(l, c) {l, NULL, c, false, false, false}
static int key_set(lap_btn_t *k)
{
    const bool running = frun && frun->state == BXF_RUNNING;
    if (desk && tab == T_FLOWS) {
        const lap_btn_t b[8] = {KEY("CONNECT", 'c'), KEY(fview_run && !running ? "LIST" : "RUN", '\r'), KEY("STOP", 'q'), KEY("RELOAD", 'o'),
                                KEY("NAME", 'm'), KEY("PAIR", 'p'), KEY("SAVE", 's'), KEY("DISCONNECT", 'x')};
        memcpy(k, b, sizeof(b));
        k[2].dim = !running;
        return 8;
    }
    if (desk) {
        const lap_btn_t b[8] = {KEY("CONNECT", 'c'), KEY("READ", 'r'), KEY("WRITE 01", 'w'), KEY("NOTIFY", 'n'),
                                KEY("NAME", 'm'), KEY("PAIR", 'p'), KEY("SAVE", 's'), KEY("DISCONNECT", 'x')};
        memcpy(k, b, sizeof(b));
        return 8;
    }
    if (tab == T_SCAN) {
        const lap_btn_t b[4] = {KEY("CONNECT", 'c'), KEY("NAME", 'm'), KEY("DISCONN", 'x'), KEY("SAVE", 's')};
        memcpy(k, b, sizeof(b));
        return 4;
    }
    if (tab == T_GATT) {
        const lap_btn_t b[4] = {KEY("READ", 'r'), KEY("NOTIFY", 'n'), KEY("NAME", 'm'), KEY("MORE", 'k')};
        memcpy(k, b, sizeof(b));
        return 4;
    }
    if (tab == T_FLOWS) {
        const lap_btn_t b[3] = {KEY(fview_run && !running ? "LIST" : "RUN", '\r'), KEY("STOP", 'q'), KEY("RELOAD", 'o')};
        memcpy(k, b, sizeof(b));
        k[1].dim = !running;
        return 3;
    }
    const lap_btn_t b[3] = {KEY("CLEAR", 'z'), KEY("NOTIFY", 'n'), KEY("SAVE", 's')};
    memcpy(k, b, sizeof(b));
    return 3;
}
static int key_count(void)
{
    lap_btn_t k[8];
    return key_set(k);
}

static void draw(void)
{
    W = lap_cols(); H = lap_rows();
    desk = W >= 90 && H >= 20;
    if (!devs) return;
    if (fitted && (scan == LAP_IO_EBUSY || scan == LAP_IO_EAGAIN) && !(lap_frame() % 25)) scan = lap_ble_scan_start();
    if (scan > 0) {
        uint8_t payload[LAP_BLE_AD_MAX];
        lap_ble_advert_t a;
        int n;
        for (int i = 0; i < 16 && (n = lap_ble_advert_next(scan, &a, payload, sizeof(payload))) >= 0; i++) heard(&a, payload, n);
        lap_operation_t s;
        if (!lap_ble_scan_status(scan, &s) && s.error) say("Scan error %ld", (long)s.error);
        if (!(lap_frame() % 30) && ndev > 1) {     /* strongest first, keeping the pick */
            const bxdev_t keep = devs[dsel < ndev ? dsel : 0];
            qsort(devs, (size_t)ndev, sizeof(bxdev_t), by_rssi);
            for (int i = 0; i < ndev; i++) if (!memcmp(devs[i].addr, keep.addr, 6)) dsel = i;
        }
    }
    lap_ids_pump();                         /* the name library's one card job a frame */
    dialogs();
    events_in();
    for (int i = 0; i < BX_MAX_CONN; i++) if (conns[i] && conns[i]->handle) {
        const bool flowing = frun && frun->state == BXF_RUNNING && frun_slot == i;
        if (bx_overdue(conns[i], lap_millis()) && conns[i]->phase == BX_P_DONE && !flowing) say("Slot %d: no reply, given up after 5 s", i + 1);
        drive(conns[i]);
    }
    flow_step();
    flows_load_step();
    save_step();

    const int sw = lap_swipe();
    if (sw == LAP_SWIPE_LEFT) tab = (tab + 1) % T_COUNT;
    else if (sw == LAP_SWIPE_RIGHT) tab = (tab + T_COUNT - 1) % T_COUNT;
    if (tab != seen_tab || cur != seen_cur) {     /* another list: no drag or flick carries over */
        lap_scroll_stop(&ddrag); lap_scroll_stop(&gdrag); lap_scroll_stop(&ndrag); lap_scroll_stop(&fdrag);
        seen_tab = tab; seen_cur = cur;
    }
    /* the layout: title, tabs (a strip of keys on the handheld), the body, the key bar */
    const int nkeys = key_count();          /* five would halve the keys on a handheld */
    kr = lap_ui_keys_rows(nkeys);
    tr = desk ? 1 : lap_ui_keys_rows(3);
    hd = desk && H < 34 ? 4 : 6;            /* the header art's rows */
    ty = 1 + hd;
    body0 = ty + tr;
    const bool help = desk && H >= 34;      /* a tall wide screen keeps a line of keyboard help over the keys */
    body1 = H - kr - (help ? 1 : 0);
    /* the lists where they were drawn last frame; each takes only a finger that went down on it */
    dragging = lap_scroll_follow(&ddrag, &dtop, dsmax, dlist_y, dlist_y + dlist_h);
    dragging |= lap_scroll_follow(&gdrag, &gtop, gsmax, glist_y, glist_y + glist_h);
    dragging |= lap_scroll_follow(&ndrag, &ntop, nsmax, nlist_y, nlist_y + nlist_h);
    dragging |= lap_scroll_follow(&fdrag, &ftop, fsmax, flist_y, flist_y + flist_h);
    dlist_h = glist_h = nlist_h = flist_h = 0;

    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    header();
    const int body = body1 - body0;
    char t[40];
    if (desk) {
        /* three frames: the devices, the table, the notifications */
        const int lw = W * 40 / 100;
        const int nh = H >= 30 ? 8 : 6;
        snprintf(t, sizeof(t), " DEVICES %d ", ndev);
        lap_ui_bezel(0, body0, lw, body, t);
        scan_pane(1, body0 + 1, lw - 2, body - 2);
        /* the table's frame shows FLOWS while that tab is picked */
        if (tab == T_FLOWS) snprintf(t, sizeof(t), " FLOWS %d ", flib ? flib->n : 0);
        else snprintf(t, sizeof(t), " GATT [%d] ", cur + 1);
        lap_ui_bezel(lw - 1, body0, W - lw + 1, body - nh + 1, t);     /* the frames share their rules */
        if (tab == T_FLOWS) flows_pane(lw, body0 + 1, W - lw - 1, body - nh - 1);
        else gatt_pane(lw, body0 + 1, W - lw - 1, body - nh - 1);
        snprintf(t, sizeof(t), " NOTIFY %d ", nnotes);
        lap_ui_bezel(lw - 1, body0 + body - nh, W - lw + 1, nh, t);
        notify_pane(lw, body0 + body - nh + 1, W - lw - 1, nh - 2);
    } else {
        if (tab == T_SCAN) snprintf(t, sizeof(t), " SCAN %d ", ndev);
        else if (tab == T_GATT) snprintf(t, sizeof(t), " GATT [%d] ", cur + 1);
        else if (tab == T_NOTIFY) snprintf(t, sizeof(t), " NOTIFY %d ", nnotes);
        else snprintf(t, sizeof(t), " FLOWS %d ", flib ? flib->n : 0);
        lap_ui_bezel(0, body0, W, body, t);
        if (tab == T_SCAN) scan_pane(1, body0 + 1, W - 2, body - 2);
        else if (tab == T_GATT) gatt_pane(1, body0 + 1, W - 2, body - 2);
        else if (tab == T_NOTIFY) notify_pane(1, body0 + 1, W - 2, body - 2);
        else flows_pane(1, body0 + 1, W - 2, body - 2);
    }

    /* the last message, over the title for a few seconds (a tap there clears it) */
    if (message[0] && lap_millis() - message_at < 8000u) {
        lap_fill(0, 0, W, 1, ' ', LAP_ATTR(LAP_BLACK, LAP_WHITE));
        put(1, 0, W - 2, LAP_ATTR(LAP_BLACK, LAP_WHITE), "%s", message);
    }
    if (help) put(1, H - kr - 1, W - 2, LAP_ATTR(LAP_BLUE | LAP_BRIGHT, LAP_BLACK), "%s",
                  tab == T_FLOWS ? "ENTER run  Q stop  O reload flows  M name  P pair  C connect  X disconnect  1-3 slot  S save JSON"
                                 : "C connect  X disc  1-3 slot  R read  W write 01  N/U notify  M name  P pair  F flows  D walk  S save");
    lap_btn_t k[8];
    key_set(k);
    lap_ui_keys(0, H - kr, W, kr, k, nkeys, -1);
}

/* ---- input ------------------------------------------------------------------------------------ */
static void next_slot(void)
{
    for (int k = 1; k <= BX_MAX_CONN; k++) {
        const int i = (cur + k) % BX_MAX_CONN;
        if (conns[i] && (conns[i]->handle || conns[i]->nsvc)) { cur = i; gsel = gtop = 0; return; }
    }
}
static const bxdev_t *picked_dev(void) { return ndev && dsel < ndev ? &devs[dsel] : NULL; }
/* MORE on the handheld's GATT keys: the rarer actions, one level down. */
static void act_more(void)
{
    if (dlg >= 0) return;
    static const char *const more[] = {"Write 01 to the picked row", "Pair this device", "Save the table as JSON", "Walk the table again",
                                       "Disconnect this slot"};
    dlg = lap_choose(more, 5);
    dlg_kind = DLG_MORE;
}
static bool do_key(int ch);
static void soft(int b, int n)
{
    lap_btn_t k[8];
    const int nk = key_set(k);
    if (b < 0 || b >= nk || k[b].dim) return;
    if (k[b].key == 'n') act_notify(-1);            /* the NOTIFY key toggles */
    else do_key(k[b].key);
    (void)n;
}
static void move(int by)
{
    if (!desk && tab == T_NOTIFY) { ntop += by; if (ntop > nsmax) ntop = nsmax; if (ntop < 0) ntop = 0; }
    else if (tab == T_SCAN) { dsel += by; if (dsel >= ndev) dsel = ndev - 1; if (dsel < 0) dsel = 0; dshow = true; }
    else if (tab == T_FLOWS) {
        if (fview_run) return;
        const int n = flib ? flib->n : 0;
        fsel += by; if (fsel >= n) fsel = n - 1; if (fsel < 0) fsel = 0; fshow = true;
    }
    else { gsel += by; if (gsel >= nrows) gsel = nrows - 1; if (gsel < 0) gsel = 0; gshow = true; }
}
/* A page: the rows the list in front shows, less one to keep your place. */
static int page(void)
{
    const int h = !desk && tab == T_NOTIFY ? nlist_h : tab == T_SCAN ? dlist_h : tab == T_FLOWS ? flist_h : glist_h;
    return h > 2 ? h - 1 : 1;
}
static bool key(lap_key_t k, char ch)
{
    switch (k) {
    case LAP_KEY_UP: case LAP_KEY_WHEEL_UP: move(-1); return true;
    case LAP_KEY_DOWN: case LAP_KEY_WHEEL_DOWN: move(1); return true;
    case LAP_KEY_PGUP: move(-page()); return true;
    case LAP_KEY_PGDN: move(page()); return true;
    case LAP_KEY_LEFT: tab = (tab + T_COUNT - 1) % T_COUNT; return true;
    case LAP_KEY_RIGHT: case LAP_KEY_TAB: tab = (tab + 1) % T_COUNT; return true;
    case LAP_KEY_ENTER:
        if (tab == T_SCAN) connect_dev(picked_dev());
        else if (tab == T_GATT) act_read();
        else if (tab == T_FLOWS) act_run();
        return true;
    case LAP_KEY_BACK:
        if (tab == T_FLOWS && fview_run) { fview_run = false; return true; }    /* the run's log, then the list */
        if (tab != T_SCAN) { tab = T_SCAN; return true; }
        return false;
    case LAP_KEY_CHAR: break;
    default: return false;
    }
    return do_key(ch);
}
static bool do_key(int ch)
{
    switch (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch) {
    case 'c': connect_dev(picked_dev()); break;
    case 'x': disconnect_cur(); break;
    case 'd': case 'g': case 'e': act_rewalk(); break;
    case 'r': act_read(); break;
    case 'w': act_write(); break;           /* explicit: writes the one byte 01 */
    case 'n': act_notify(1); break;
    case 'u': act_notify(0); break;
    case 's': save_begin(); break;
    case 'm': act_name(); break;
    case 'p': act_pair(); break;
    case 'k': act_more(); break;
    case 'f': tab = T_FLOWS; break;
    case 'q': act_stop(); break;
    case 'o': flows_reload(); say("Loading flows again"); break;
    case 'z': nnotes = 0; ntop = 0; lap_scroll_stop(&ndrag); break;
    case '\r': act_run(); break;
    case '1': case '2': case '3': cur = ch - '1'; gsel = gtop = 0; if (!desk) tab = T_GATT; break;
    default: return false;
    }
    return true;
}
static bool touch(int col, int row)
{
    if (row >= H - kr) {
        const int b = dragging ? -1 : lap_ui_key_hit(col, row);
        if (b >= 0) soft(b, key_count());
        return true;
    }
    if (!desk && row >= ty && row < ty + tr) {
        const int t = col * 3 / (W > 0 ? W : 1);    /* the strip's three keys: < name > */
        if (t != 1) tab = (tab + (t < 1 ? T_COUNT - 1 : 1)) % T_COUNT;
        else if (tab == T_GATT) next_slot();         /* GATT [n]: the next connection's table */
        return true;
    }
    if (row == 0) { message[0] = 0; return true; }
    if (dragging) return true;
    if (desk && row == ty) {
        for (int t = 0; t < T_COUNT; t++) if (col >= tab_x[t] && col < tab_x[t + 1]) { tab = t; return true; }
        if (col >= tab_x[T_COUNT]) {
            const int s = (col - tab_x[T_COUNT]) / 4;
            if (s >= 0 && s < BX_MAX_CONN) { cur = s; gsel = gtop = 0; if (!desk) tab = T_GATT; }
        }
        return true;
    }
    const int lw = desk ? W * 40 / 100 : W;
    if ((desk || tab == T_SCAN) && col < lw && row >= dlist_y && row < dlist_y + dlist_h) {
        const int i = dtop + row - dlist_y;
        if (i < ndev) { if (i == dsel) connect_dev(&devs[i]); else { dsel = i; dshow = true; } }
        if (desk) tab = T_SCAN;
        return true;
    }
    if ((desk ? col > lw : tab == T_GATT) && row >= glist_y && row < glist_y + glist_h) {
        const int i = gtop + row - glist_y;
        if (i < nrows) { gsel = i; gshow = true; }
        if (desk) tab = T_GATT;
        return true;
    }
    if (tab == T_FLOWS && (desk ? col > lw : true) && row >= flist_y && row < flist_y + flist_h && flib) {
        const int i = ftop + row - flist_y;
        if (i < flib->n) { if (i == fsel) act_run(); else { fsel = i; fshow = true; } }   /* a second tap runs it */
        return true;
    }
    /* Every tap on this screen is the app's: one left unused reaches the
       board as UP, ENTER or DOWN by where it fell, and ENTER acts. */
    return true;
}

static void enter(void)
{
    lap_caps_t c;
    lap_caps(&c, sizeof(c));
    devs = calloc(MAXDEV, sizeof(bxdev_t));
    if (!devs) { lap_log("no memory"); lap_exit(); return; }
    ndev = dsel = dtop = gsel = gtop = ntop = 0;
    memset(&ddrag, 0, sizeof(ddrag)); memset(&gdrag, 0, sizeof(gdrag)); memset(&ndrag, 0, sizeof(ndrag));
    dlist_y = dlist_h = glist_y = glist_h = nlist_y = nlist_h = dsmax = gsmax = nsmax = 0;
    dragging = false; dshow = gshow = true;
    kr = tr = 1; body0 = body1 = 2; hd = 6; ty = 7;
    seen_tab = T_SCAN; seen_cur = 0;
    nnotes = note_head = 0;
    events = 0;
    cur = board_limit = 0;
    tab = T_SCAN;
    memset(conns, 0, sizeof(conns));
    memset(&save, 0, sizeof(save));
    memset(airsync, 0, sizeof(airsync));
    /* names: the shared library, asked first; the built-in SIG names answer while it looks */
    lap_ids_open();
    bx_set_namer(card_name);
    /* flows: the built-in list now, the pack and the owner's files as the card allows */
    memset(&fload, 0, sizeof(fload));
    memset(&fdrag, 0, sizeof(fdrag));
    fsel = ftop = flist_y = flist_h = fsmax = nfit = frun_slot = 0;
    fview_run = false; fshow = true;
    dlg = -1; dlg_kind = DLG_NONE; dlg_slot = 0;
    pair_text[0] = 0;
    flib = calloc(1, sizeof(bxf_lib_t));
    frun = calloc(1, sizeof(bxf_run_t));
    flows_reload();
    fitted = (c.hardware & LAP_HW_BLE) != 0;
    scan = -1;
    if (fitted) {
        scan = lap_ble_scan_start();
        say("Waiting for adverts");
    } else say("Bluetooth not fitted");
}
static void leave(void)
{
    if (scan > 0) lap_ble_scan_stop(scan);
    if (dlg >= 0) lap_dialog_cancel(dlg);
    dlg = -1;
    for (int i = 0; i < BX_MAX_CONN; i++) {
        if (conns[i] && conns[i]->handle) lap_ble_disconnect(conns[i]->handle);
        free(conns[i]);
        conns[i] = NULL;
    }
    free(devs);
    devs = NULL;
    free(save.buf);
    save.buf = NULL;
    flows_free();
    free(flib);
    free(frun);
    flib = NULL;
    frun = NULL;
    bx_set_namer(NULL);
    lap_ids_close();
}
LAP_ICON_PX("....................", "....77777777........", "...7744444477.......", "..77e44ff44477......",
            "..7e444f4f4447......", ".7744f4f44f4477.....", ".74444fff4f4447.....", ".744444ff444447.....",
            ".74444fff4f4447.....", ".7744f4f44f4477.....", "..74444f4f4447......", "..77444ff44477......",
            "...77444444773b.....", "....77777777.33b....", "..............33b...", "...............33b..",
            "................33b.", ".................33b");

LAP_APP(.id = "ble_explorer", .name = "BLE EXPLORER", .sub = "scan, GATT and flows", .version = "2.2",
        .author = "LakeShark", .icon = LAP_ICON_TOWER, .color = LAP_CYAN, .enter = enter, .leave = leave,
        .draw = draw, .key = key, .touch = touch);
