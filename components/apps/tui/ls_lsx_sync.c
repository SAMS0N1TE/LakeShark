/* HOME BASE sync over LSX: the PULL / DELTA / PUSH exchange, the home's unit
   table and the field unit's side (ls_lsx_sync.h). */
#include "ls_lsx_sync.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
#else
static void *big_alloc(size_t n) { return malloc(n); }
#endif

enum { OP_PULL = 1, OP_DELTA, OP_PUSH };

/* Object meta: op, since, next, pending (13 bytes). */
static int meta_put(uint8_t *m, uint8_t op, uint32_t since, uint32_t next, uint32_t pending)
{
    m[0] = op;
    memcpy(m + 1, &since, 4);
    memcpy(m + 5, &next, 4);
    memcpy(m + 9, &pending, 4);
    return 13;
}

static bool meta_get(const uint8_t *m, int n, uint8_t *op, uint32_t *since, uint32_t *next, uint32_t *pending)
{
    if (n < 13) return false;
    *op = m[0];
    memcpy(since, m + 1, 4);
    memcpy(next, m + 5, 4);
    memcpy(pending, m + 9, 4);
    return true;
}

struct ls_lsx_sync {
    int role;
    ls_lsx_t *x;
    const ls_lsx_store_t *store;
    uint32_t now;
    ls_lsx_unit_t units[LSX_UNITS];
    bool dirty;

    /* The session. */
    uint8_t phase;
    uint32_t peer;
    char peer_name[LSX_NAME];
    int unit;                   /* home: index into units, -1 none */
    uint32_t recs_in, recs_out;
    uint32_t bytes_done;        /* object bytes finished this session */
    uint32_t pending_recs, last_obj_len, last_obj_recs;
    bool close_sent, open, hold;

    /* One buffer for what this end sends (a DELTA or a PUSH), kept after it
       has gone: the same `since` asked for again gets the same bytes, which
       is what lets LSX resume a cut-off transfer. */
    uint8_t *buf;
    bool buf_busy;
    uint32_t buf_since, buf_next, buf_len;
    bool buf_valid;
    bool pull_waiting;
    uint32_t pull_since;
    uint32_t push_next, push_pending;
    uint32_t sent_len, sent_recs;   /* the DELTA in flight */

    /* Field. */
    uint32_t last_sync_s;
    uint32_t home_heard;
    bool home_seen;
    char home_name[LSX_NAME];

    /* 'lsx push'. */
    uint8_t *test_buf;
    uint32_t test_len, test_t0, test_bytes, test_ms;
};

ls_lsx_sync_t *ls_lsx_sync_create(int role)
{
    ls_lsx_sync_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->role = role;
    s->unit = -1;
    return s;
}

void ls_lsx_sync_destroy(ls_lsx_sync_t *s)
{
    if (!s) return;
    free(s->buf);
    free(s->test_buf);
    free(s);
}

void ls_lsx_sync_bind(ls_lsx_sync_t *s, ls_lsx_t *x) { s->x = x; }
void ls_lsx_sync_set_store(ls_lsx_sync_t *s, const ls_lsx_store_t *store)
{
    s->store = store;
    s->buf_valid = false;
}

static ls_lsx_unit_t *unit_get(ls_lsx_sync_t *s, uint32_t id, bool make, int *idx)
{
    int free_i = -1, old_i = 0;
    for (int i = 0; i < LSX_UNITS; i++) {
        if (s->units[i].id == id) { if (idx) *idx = i; return &s->units[i]; }
        if (!s->units[i].id && free_i < 0) free_i = i;
        if (s->units[i].last_sync_s < s->units[old_i].last_sync_s) old_i = i;
    }
    if (!make) return NULL;
    const int i = free_i >= 0 ? free_i : old_i;
    memset(&s->units[i], 0, sizeof(s->units[i]));
    s->units[i].id = id;
    s->dirty = true;
    if (idx) *idx = i;
    return &s->units[i];
}

static bool ensure_buf(ls_lsx_sync_t *s)
{
    if (!s->buf) s->buf = big_alloc(LSX_SYNC_CHUNK);
    return s->buf != NULL;
}

static uint32_t now_s(const ls_lsx_sync_t *s) { return s->now / 1000; }

/* ---- home ---------------------------------------------------------------- */

static void send_pull(ls_lsx_sync_t *s)
{
    ls_lsx_unit_t *u = &s->units[s->unit];
    uint8_t m[16];
    const int n = meta_put(m, OP_PULL, u->pulled, 0, LSX_SYNC_CHUNK);
    s->phase = LSX_PH_PULL;
    ls_lsx_send_object(s->x, LSX_PORT_ATLAS, m, n, NULL, 0, NULL);
}

static void finish(ls_lsx_sync_t *s)
{
    s->phase = LSX_PH_DONE;
    if (s->unit >= 0) {
        ls_lsx_unit_t *u = &s->units[s->unit];
        u->last_sync_s = now_s(s) ? now_s(s) : 1;
        u->syncs++;
        s->dirty = true;
    }
}

static void start_push(ls_lsx_sync_t *s)
{
    ls_lsx_unit_t *u = &s->units[s->unit];
    if (!s->store || !ensure_buf(s)) { finish(s); return; }
    uint32_t next = u->pushed;
    const size_t n = s->store->delta(s->store->ctx, u->pushed, s->buf, LSX_SYNC_CHUNK, &next);
    if (!n || next == u->pushed) { finish(s); return; }
    const uint32_t seq = s->store->seq(s->store->ctx);
    uint8_t m[16];
    const int ml = meta_put(m, OP_PUSH, u->pushed, next, seq > next ? seq - next : 0);
    s->push_next = next;
    s->push_pending = seq > next ? seq - next : 0;
    s->buf_len = (uint32_t)n;
    s->buf_busy = true;
    s->buf_valid = false;     /* a PUSH is never asked for again by `since` */
    s->phase = LSX_PH_PUSH;
    s->pending_recs = s->push_pending;
    s->last_obj_len = (uint32_t)n;
    s->last_obj_recs = next - u->pushed;
    if (ls_lsx_send_object(s->x, LSX_PORT_ATLAS, m, ml, s->buf, (uint32_t)n, (void *)(uintptr_t)OP_PUSH) != 0) {
        s->buf_busy = false;
        finish(s);
    }
}

static void home_delta(ls_lsx_sync_t *s, const uint8_t *d, uint32_t len, uint32_t since, uint32_t next,
                       uint32_t pending)
{
    if (s->unit < 0) return;
    ls_lsx_unit_t *u = &s->units[s->unit];
    if (since != u->pulled) return;           /* an answer to an older question */
    if (len && s->store) {
        const int r = s->store->apply(s->store->ctx, d, len, s->peer);
        if (r < 0) { finish(s); return; }     /* not records this atlas reads: stop here */
        u->recs_in += (uint32_t)r;
        s->recs_in += (uint32_t)r;
    }
    u->bytes_in += len;
    s->bytes_done += len;
    s->pending_recs = pending;
    if (len) {
        s->last_obj_len = len;
        s->last_obj_recs = next > since ? next - since : 1;
    }
    s->dirty = true;
    if (!len || next == since) { start_push(s); return; }
    u->pulled = next;
    send_pull(s);
}

/* ---- field ----------------------------------------------------------------- */

static void field_pull(ls_lsx_sync_t *s, uint32_t since)
{
    if (s->buf_busy) { s->pull_waiting = true; s->pull_since = since; return; }
    uint8_t m[16];
    uint32_t next = since, pending = 0;
    size_t n = 0;
    if (s->buf_valid && s->buf_since == since) {
        n = s->buf_len;
        next = s->buf_next;
    } else if (s->store && ensure_buf(s)) {
        n = s->store->delta(s->store->ctx, since, s->buf, LSX_SYNC_CHUNK, &next);
        s->buf_since = since;
        s->buf_next = next;
        s->buf_len = (uint32_t)n;
        s->buf_valid = true;
    }
    const uint32_t seq = s->store ? s->store->seq(s->store->ctx) : 0;
    pending = seq > next ? seq - next : 0;
    s->pending_recs = pending;
    s->last_obj_len = (uint32_t)n;
    s->last_obj_recs = next > since ? next - since : 0;
    s->phase = LSX_PH_PULL;
    s->sent_len = (uint32_t)n;
    s->sent_recs = next - since;
    const int ml = meta_put(m, OP_DELTA, since, next, pending);
    s->buf_busy = n > 0;
    if (ls_lsx_send_object(s->x, LSX_PORT_ATLAS, m, ml, n ? s->buf : NULL, (uint32_t)n, (void *)(uintptr_t)OP_DELTA) != 0)
        s->buf_busy = false;
}

/* ---- engine callbacks -------------------------------------------------------- */

static bool cb_probe(void *ctx, const ls_lsx_peer_t *p)
{
    ls_lsx_sync_t *s = ctx;
    const uint8_t role = p->role & LSX_ROLE_MASK;
    if (s->role != LSX_ROLE_HOME || !s->store || (role != LSX_ROLE_FIELD && role != LSX_ROLE_PARTY)) return false;
    ls_lsx_unit_t *u = unit_get(s, p->id, true, NULL);
    memcpy(u->name, p->name, LSX_NAME);
    u->rssi = p->rssi;
    u->caps = p->caps;
    if (p->role & LSX_ROLE_FORCED || !u->syncs) return true;
    const uint32_t since = now_s(s) - u->last_sync_s;
    if (p->hint != u->pulled && since >= 60) return true;                  /* it found things */
    if (s->store->seq(s->store->ctx) != u->pushed && since >= LSX_RESYNC_S) return true;   /* others did */
    return false;
}

static void cb_peer(void *ctx, const ls_lsx_peer_t *p)
{
    ls_lsx_sync_t *s = ctx;
    if ((p->role & LSX_ROLE_MASK) == LSX_ROLE_HOME) {
        s->home_seen = true;
        s->home_heard = s->now;
        memcpy(s->home_name, p->name, LSX_NAME);
    }
}

static void cb_session(void *ctx, uint32_t peer, int ev)
{
    ls_lsx_sync_t *s = ctx;
    if (ev == LSX_EV_OPEN) {
        s->open = true;
        s->peer = peer;
        s->recs_in = s->recs_out = s->bytes_done = 0;
        s->pending_recs = s->last_obj_len = s->last_obj_recs = 0;
        s->close_sent = false;
        s->buf_busy = false;
        s->pull_waiting = false;
        ls_lsx_status_t st;
        ls_lsx_status(s->x, &st);
        memcpy(s->peer_name, st.peer_name, LSX_NAME);
        s->phase = LSX_PH_IDLE;
        if (s->role == LSX_ROLE_HOME) {
            unit_get(s, peer, true, &s->unit);
            memcpy(s->units[s->unit].name, st.peer_name, LSX_NAME);
            if (s->store) send_pull(s);
            else finish(s);
        } else {
            s->home_seen = true;
            s->home_heard = s->now;
            memcpy(s->home_name, st.peer_name, LSX_NAME);
        }
        return;
    }
    if (ev == LSX_EV_CLOSED || ev == LSX_EV_LOST) {
        if (s->role == LSX_ROLE_FIELD && ev == LSX_EV_CLOSED && s->open) s->last_sync_s = now_s(s) ? now_s(s) : 1;
        s->open = false;
        s->buf_busy = false;
        s->phase = LSX_PH_IDLE;
        s->unit = -1;
        free(s->test_buf);
        s->test_buf = NULL;
    }
}

static void cb_object(void *ctx, uint32_t peer, uint8_t port, const uint8_t *meta, int mlen, const uint8_t *d,
                      uint32_t len)
{
    ls_lsx_sync_t *s = ctx;
    (void)peer;
    if (port == LSX_PORT_TEST) { s->test_bytes = len; return; }
    uint8_t op;
    uint32_t since, next, pending;
    if (port != LSX_PORT_ATLAS || !meta_get(meta, mlen, &op, &since, &next, &pending)) return;
    if (s->role == LSX_ROLE_HOME && op == OP_DELTA) home_delta(s, d, len, since, next, pending);
    else if (s->role != LSX_ROLE_HOME && op == OP_PULL) field_pull(s, since);
    else if (s->role != LSX_ROLE_HOME && op == OP_PUSH) {
        s->phase = LSX_PH_PUSH;
        s->pending_recs = pending;
        s->last_obj_len = len;
        s->last_obj_recs = next > since ? next - since : 1;
        s->bytes_done += len;
        if (len && s->store) {
            const int r = s->store->apply(s->store->ctx, d, len, peer);
            if (r > 0) s->recs_in += (uint32_t)r;
        }
    }
}

static void cb_sent(void *ctx, uint32_t peer, uint8_t port, void *tag, int result)
{
    ls_lsx_sync_t *s = ctx;
    (void)peer;
    if (port == LSX_PORT_TEST) {
        if (result == LSX_SENT_OK) { s->test_bytes = s->test_len; s->test_ms = s->now - s->test_t0; }
        free(s->test_buf);
        s->test_buf = NULL;
        return;
    }
    const uintptr_t op = (uintptr_t)tag;
    if (op == OP_DELTA) {
        s->buf_busy = false;
        if (result == LSX_SENT_OK) {
            s->bytes_done += s->sent_len;
            s->recs_out += s->sent_recs;
        }
        if (s->pull_waiting) { s->pull_waiting = false; field_pull(s, s->pull_since); }
    } else if (op == OP_PUSH && s->role == LSX_ROLE_HOME) {
        s->buf_busy = false;
        if (result != LSX_SENT_OK || s->unit < 0) return;
        ls_lsx_unit_t *u = &s->units[s->unit];
        u->recs_out += s->push_next - u->pushed;
        s->recs_out += s->push_next - u->pushed;
        u->bytes_out += s->buf_len;
        s->bytes_done += s->buf_len;
        u->pushed = s->push_next;
        s->dirty = true;
        if (s->push_pending) start_push(s);
        else finish(s);
    }
}

static uint32_t cb_hint(void *ctx)
{
    ls_lsx_sync_t *s = ctx;
    return s->store ? s->store->seq(s->store->ctx) : 0;
}

ls_lsx_app_t ls_lsx_sync_app(ls_lsx_sync_t *s)
{
    return (ls_lsx_app_t){ s, cb_probe, cb_peer, cb_session, cb_object, cb_sent, NULL, cb_hint };
}

void ls_lsx_sync_tick(ls_lsx_sync_t *s, uint32_t now)
{
    s->now = now;
    if (s->open && s->role != LSX_ROLE_HOME) s->home_heard = now;   /* in session with it */
    if (s->role == LSX_ROLE_HOME && s->open && s->phase == LSX_PH_DONE && !s->close_sent && !s->hold && !s->test_buf) {
        s->close_sent = true;
        ls_lsx_close(s->x);
    }
}

void ls_lsx_sync_hold(ls_lsx_sync_t *s, bool on) { s->hold = on; }

void ls_lsx_sync_force(ls_lsx_sync_t *s)
{
    if (s->x) ls_lsx_probe_now(s->x);
}

void ls_lsx_sync_status(const ls_lsx_sync_t *s, ls_lsx_sync_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->role = (uint8_t)s->role;
    out->phase = s->phase;
    out->peer = s->open ? s->peer : 0;
    memcpy(out->peer_name, s->peer_name, LSX_NAME);
    out->recs_in = s->recs_in;
    out->recs_out = s->recs_out;
    out->last_sync_s = s->last_sync_s;
    out->last_home_ms = s->home_seen ? s->now - s->home_heard : UINT32_MAX;
    memcpy(out->home_name, s->home_name, LSX_NAME);
    out->test_bytes = s->test_bytes;
    out->test_ms = s->test_ms;
    out->store = s->store != NULL;
    out->now_s = now_s(s);
    out->progress_pct = -1;
    if (!s->open) return;
    if (s->phase == LSX_PH_DONE) { out->progress_pct = 100; return; }
    /* From bytes: what has crossed, the object on the wire, and what the
       pending records come to at the size records have had so far. */
    ls_lsx_status_t st;
    ls_lsx_status(s->x, &st);
    const uint32_t wire_done = st.obj_rx_len ? st.obj_rx_done : st.obj_tx_done;
    const uint32_t wire_len = st.obj_rx_len ? st.obj_rx_len : st.obj_tx_len ? st.obj_tx_len : s->buf_busy ? s->buf_len : 0;
    uint32_t per = s->last_obj_recs ? s->last_obj_len / s->last_obj_recs : 0;
    uint64_t rest = (uint64_t)s->pending_recs * per;
    /* The object coming in says itself what is left behind it. */
    uint8_t op;
    uint32_t since, next, pending;
    if (st.obj_rx_len && meta_get(st.obj_rx_meta, st.obj_rx_mlen, &op, &since, &next, &pending) && next > since) {
        per = st.obj_rx_len / (next - since);
        rest = (uint64_t)pending * per;
    }
    const uint64_t done = (uint64_t)s->bytes_done + wire_done;
    const uint64_t total = (uint64_t)s->bytes_done + wire_len + rest;
    out->progress_pct = total ? (int)(done * 100 / total) : 0;
    if (out->progress_pct > 99) out->progress_pct = 99;
}

int ls_lsx_sync_units(const ls_lsx_sync_t *s, ls_lsx_unit_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < LSX_UNITS && n < max; i++)
        if (s->units[i].id) out[n++] = s->units[i];
    return n;
}

#define BLOB_MAGIC 0x4C535855u   /* "LSXU", version 1 */

size_t ls_lsx_sync_units_blob(const ls_lsx_sync_t *s, void *out, size_t cap)
{
    const size_t need = 4 + sizeof(s->units);
    if (cap < need) return 0;
    const uint32_t m = BLOB_MAGIC;
    memcpy(out, &m, 4);
    memcpy((uint8_t *)out + 4, s->units, sizeof(s->units));
    return need;
}

void ls_lsx_sync_units_load(ls_lsx_sync_t *s, const void *blob, size_t len)
{
    uint32_t m = 0;
    if (len != 4 + sizeof(s->units)) return;
    memcpy(&m, blob, 4);
    if (m != BLOB_MAGIC) return;
    memcpy(s->units, (const uint8_t *)blob + 4, sizeof(s->units));
    /* Uptime restarted: a sync remembered from before the boot is long ago. */
    for (int i = 0; i < LSX_UNITS; i++)
        if (s->units[i].last_sync_s) s->units[i].last_sync_s = 1;
}

bool ls_lsx_sync_take_dirty(ls_lsx_sync_t *s)
{
    const bool d = s->dirty;
    s->dirty = false;
    return d;
}

int ls_lsx_sync_push_test(ls_lsx_sync_t *s, uint32_t bytes)
{
    if (!s->x || s->test_buf || !bytes || bytes > LSX_OBJ_MAX) return -1;
    s->test_buf = big_alloc(bytes);
    if (!s->test_buf) return -1;
    for (uint32_t i = 0; i < bytes; i++) s->test_buf[i] = (uint8_t)(i * 7 + (i >> 8));
    s->test_len = bytes;
    s->test_t0 = s->now;
    s->test_bytes = s->test_ms = 0;
    if (ls_lsx_send_object(s->x, LSX_PORT_TEST, NULL, 0, s->test_buf, bytes, NULL) != 0) {
        free(s->test_buf);
        s->test_buf = NULL;
        return -1;
    }
    return 0;
}

/* ---- the test store ---------------------------------------------------------- */

#define TREC 32

typedef struct {
    ls_lsx_store_t st;
    uint32_t count, salt, applied;
} test_store_t;

static uint32_t ts_seq(void *ctx) { return ((test_store_t *)ctx)->count; }

static size_t ts_delta(void *ctx, uint32_t since, uint8_t *out, size_t cap, uint32_t *next)
{
    test_store_t *t = ctx;
    size_t n = 0;
    uint32_t q = since;
    while (q < t->count && n + TREC <= cap) {
        q++;
        uint8_t *r = out + n;
        memcpy(r, &q, 4);
        memcpy(r + 4, &t->salt, 4);
        for (int i = 8; i < TREC; i++) r[i] = (uint8_t)(q * 31 + t->salt + (uint32_t)i);
        n += TREC;
    }
    *next = q;
    return n;
}

static int ts_apply(void *ctx, const uint8_t *buf, size_t len, uint32_t origin)
{
    (void)buf;
    (void)origin;
    test_store_t *t = ctx;
    if (len % TREC) return -1;
    t->applied += (uint32_t)(len / TREC);
    return (int)(len / TREC);
}

const ls_lsx_store_t *ls_lsx_test_store(uint32_t count, uint32_t salt)
{
    test_store_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->count = count;
    t->salt = salt;
    t->st = (ls_lsx_store_t){ t, ts_seq, ts_delta, ts_apply };
    return &t->st;
}

void ls_lsx_test_store_add(const ls_lsx_store_t *store, uint32_t n)
{
    if (store) ((test_store_t *)store->ctx)->count += n;
}

uint32_t ls_lsx_test_store_applied(const ls_lsx_store_t *store)
{
    return store ? ((const test_store_t *)store->ctx)->applied : 0;
}
