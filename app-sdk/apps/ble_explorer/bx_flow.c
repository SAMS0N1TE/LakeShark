/* BLE EXPLORER flows: the parser, the list and the runner. See bx_flow.h
   and the README for the format. */
#include "bx_flow.h"
#include <stdio.h>
#include <string.h>

/* ---- text helpers ---------------------------------------------------------------------------- */
static char low(char ch) { return ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch; }
/* memchr, which apps cannot import */
static const char *find(const char *s, char ch, size_t n)
{
    for (size_t i = 0; i < n; i++) if (s[i] == ch) return s + i;
    return NULL;
}
static bool same_word(const char *a, size_t n, const char *w)
{
    size_t i = 0;
    for (; i < n && w[i]; i++) if (low(a[i]) != w[i]) return false;
    return i == n && !w[i];
}
static bool same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (low(*a) != low(*b)) return false;
    return !*a && !*b;
}
static bool space(char ch) { return ch == ' ' || ch == '\t'; }

/* One line: [s, s+n) with its comment and outer spaces gone. A '#' starts a
   comment at the line's start or after a space, outside quotes. */
static void clean(const char **s, size_t *n)
{
    const char *p = *s;
    size_t k = *n;
    while (k && (space(*p) || *p == '\r')) { p++; k--; }
    bool q = false;
    for (size_t i = 0; i < k; i++) {
        if (p[i] == '\\' && q) { i++; continue; }
        if (p[i] == '"') q = !q;
        if (!q && p[i] == '#' && (i == 0 || space(p[i - 1]))) { k = i; break; }
    }
    while (k && (space(p[k - 1]) || p[k - 1] == '\r')) k--;
    *s = p;
    *n = k;
}
/* The next word of [*s, end): returned with its length, *s moved past it. */
static const char *word(const char **s, const char *end, size_t *n)
{
    const char *p = *s;
    while (p < end && space(*p)) p++;
    const char *w = p;
    while (p < end && !space(*p)) p++;
    *n = (size_t)(p - w);
    *s = p;
    return w;
}
static void rest(const char **s, const char *end, char *out, size_t cap)
{
    const char *p = *s;
    while (p < end && space(*p)) p++;
    size_t n = (size_t)(end - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}
static int hexv(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = low(ch);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}
static bool number(const char *w, size_t n, uint32_t *out)
{
    uint32_t v = 0;
    if (!n || n > 6) return false;
    for (size_t i = 0; i < n; i++) {
        if (w[i] < '0' || w[i] > '9') return false;
        v = v * 10 + (uint32_t)(w[i] - '0');
    }
    *out = v;
    return true;
}

/* "2a06" or "1802/2a06" (the characteristic in that service). */
static bool uuid_ref(const char *w, size_t n, bxf_step_t *st)
{
    const char *slash = find(w, '/', n);
    st->in_svc = slash != NULL;
    if (!slash) return n && bx_uuid_parse(w, n, &st->uuid);
    const size_t a = (size_t)(slash - w);
    return a && a + 1 < n && bx_uuid_parse(w, a, &st->svc) && bx_uuid_parse(slash + 1, n - a - 1, &st->uuid);
}

static int bad(bxf_err_t *err, int line, const char *fmt, const char *w, size_t n)
{
    if (!err) return -1;
    err->line = line;
    char t[20];
    if (n >= sizeof(t)) n = sizeof(t) - 1;
    memcpy(t, w ? w : "", w ? n : 0);
    t[w ? n : 0] = 0;
    snprintf(err->msg, sizeof(err->msg), fmt, t);
    return -1;
}

/* The bytes of a write: hex (10 04 01, 100401, 0x10), "text" with \n \r \t
   \\ \" \xNN \0, and $time (Current Time, 10 bytes). */
static int data_in(const char *p, const char *end, bxf_step_t *st, const bxf_time_t *t, const char **why)
{
    st->n = 0;
    while (p < end) {
        while (p < end && space(*p)) p++;
        if (p >= end) break;
        if (*p == '"') {
            p++;
            for (;;) {
                if (p >= end) { *why = "no closing quote"; return -1; }
                if (*p == '"') { p++; break; }
                uint8_t b = (uint8_t)*p++;
                if (b == '\\') {
                    if (p >= end) { *why = "no closing quote"; return -1; }
                    const char e = *p++;
                    if (e == 'n') b = '\n';
                    else if (e == 'r') b = '\r';
                    else if (e == 't') b = '\t';
                    else if (e == '0') b = 0;
                    else if (e == 'x') {
                        const int h = p + 1 < end ? hexv(p[0]) : -1, l = p + 1 < end ? hexv(p[1]) : -1;
                        if (h < 0 || l < 0) { *why = "bad \\x in text"; return -1; }
                        b = (uint8_t)(h << 4 | l);
                        p += 2;
                    } else b = (uint8_t)e;
                }
                if (st->n >= BX_VAL) { *why = "too many bytes for one write"; return -1; }
                st->data[st->n++] = b;
            }
            if (p < end && !space(*p)) { *why = "text runs into a word"; return -1; }
            continue;
        }
        size_t n;
        const char *w = word(&p, end, &n);
        if (same_word(w, n, "$time")) {
            if (st->n + 10 > BX_VAL) { *why = "too many bytes for one write"; return -1; }
            uint8_t *d = st->data + st->n;
            memset(d, 0, 10);
            if (t) {
                d[0] = (uint8_t)t->year; d[1] = (uint8_t)(t->year >> 8);
                d[2] = (uint8_t)t->month; d[3] = (uint8_t)t->day;
                d[4] = (uint8_t)t->hour; d[5] = (uint8_t)t->minute; d[6] = (uint8_t)t->second;
                d[7] = (uint8_t)(t->weekday == 0 ? 7 : t->weekday);       /* 1 Monday .. 7 Sunday */
                d[9] = 1;                                                    /* adjusted by hand */
            }
            st->n += 10;
            st->time = true;
            continue;
        }
        if (n > 2 && w[0] == '0' && low(w[1]) == 'x') { w += 2; n -= 2; }
        if (!n || n % 2) { *why = "hex comes in pairs"; return -1; }
        for (size_t i = 0; i < n; i += 2) {
            const int h = hexv(w[i]), l = hexv(w[i + 1]);
            if (h < 0 || l < 0) { *why = "bad hex"; return -1; }
            if (st->n >= BX_VAL) { *why = "too many bytes for one write"; return -1; }
            st->data[st->n++] = (uint8_t)(h << 4 | l);
        }
    }
    if (!st->n) { *why = "write needs data"; return -1; }
    return 0;
}

int bxf_parse(const char *text, size_t len, int line0, bxf_flow_t *f, const bxf_time_t *time, bxf_err_t *err)
{
    memset(f, 0, sizeof(*f));
    const char *p = text, *tend = text + len;
    int line = line0;
    bool named = false;
    for (; p < tend; line++) {
        const char *nl = find(p, '\n', (size_t)(tend - p));
        const char *ls = p;
        size_t ln = (size_t)((nl ? nl : tend) - p);
        p = nl ? nl + 1 : tend;
        clean(&ls, &ln);
        if (!ln) continue;
        const char *s = ls, *end = ls + ln;
        size_t n;
        const char *w = word(&s, end, &n);
        if (same_word(w, n, "flow")) {
            if (named) return bad(err, line, "a second flow line", NULL, 0);
            rest(&s, end, f->name, sizeof(f->name));
            if (!f->name[0]) return bad(err, line, "flow needs a name", NULL, 0);
            named = true;
            continue;
        }
        if (!named) return bad(err, line, "'%s' before the flow line", w, n);
        if (same_word(w, n, "about")) { rest(&s, end, f->about, sizeof(f->about)); continue; }
        if (same_word(w, n, "confirm")) { rest(&s, end, f->confirm, sizeof(f->confirm)); continue; }
        if (same_word(w, n, "match")) {
            if (f->nmatch >= BXF_MATCH) return bad(err, line, "more than 6 match lines", NULL, 0);
            bxf_match_t *m = &f->match[f->nmatch];
            w = word(&s, end, &n);
            if (same_word(w, n, "service")) {
                w = word(&s, end, &n);
                if (!bx_uuid_parse(w, n, &m->uuid)) return bad(err, line, "bad UUID '%s'", w, n);
                m->kind = BXF_M_SERVICE;
            } else if (same_word(w, n, "name")) {
                rest(&s, end, m->glob, sizeof(m->glob));
                if (!m->glob[0]) return bad(err, line, "match name needs a pattern", NULL, 0);
                m->kind = BXF_M_NAME;
            } else return bad(err, line, "match service or match name", NULL, 0);
            f->nmatch++;
            continue;
        }
        /* a step */
        if (f->nsteps >= BXF_STEPS) return bad(err, line, "more than 32 steps", NULL, 0);
        bxf_step_t *st = &f->step[f->nsteps];
        memset(st, 0, sizeof(*st));
        st->src = ls;
        st->srclen = (uint8_t)(ln > 255 ? 255 : ln);
        if (n == 1 && w[0] == '?') { st->optional = true; w = word(&s, end, &n); }
        if (same_word(w, n, "log")) {
            st->op = BXF_LOG;
        } else if (same_word(w, n, "read") || same_word(w, n, "write") || same_word(w, n, "notify")) {
            st->op = same_word(w, n, "read") ? BXF_READ : same_word(w, n, "write") ? BXF_WRITE : BXF_NOTIFY;
            w = word(&s, end, &n);
            if (!uuid_ref(w, n, st)) return bad(err, line, n ? "bad UUID '%s'" : "a UUID is missing", w, n);
            if (st->op == BXF_WRITE) {
                const char *why = NULL;
                if (data_in(s, end, st, time, &why)) return bad(err, line, why, NULL, 0);
                f->writes = true;
            } else if (st->op == BXF_NOTIFY) {
                w = word(&s, end, &n);
                if (same_word(w, n, "on")) st->on = true;
                else if (!same_word(w, n, "off")) return bad(err, line, "notify needs on or off", NULL, 0);
                word(&s, end, &n);
                if (n) return bad(err, line, "too many words", NULL, 0);
            } else {
                word(&s, end, &n);
                if (n) return bad(err, line, "too many words", NULL, 0);
            }
        } else if (same_word(w, n, "wait")) {
            w = word(&s, end, &n);
            if (same_word(w, n, "notify")) {
                st->op = BXF_WAIT_NOTIFY;
                w = word(&s, end, &n);
                if (!uuid_ref(w, n, st)) return bad(err, line, n ? "bad UUID '%s'" : "a UUID is missing", w, n);
                w = word(&s, end, &n);
            } else st->op = BXF_WAIT;
            if (!number(w, n, &st->ms) || !st->ms) return bad(err, line, "wait needs a time in ms", NULL, 0);
            if (st->ms > BXF_WAIT_MAX) return bad(err, line, "a wait is 30000 ms at most", NULL, 0);
            word(&s, end, &n);
            if (n) return bad(err, line, "too many words", NULL, 0);
        } else return bad(err, line, "unknown step '%s'", w, n);
        f->nsteps++;
    }
    if (!named) return bad(err, line0, "no flow line", NULL, 0);
    if (!f->nsteps) return bad(err, line0, "no steps", NULL, 0);
    return 0;
}

/* ---- the list ---------------------------------------------------------------------------------- */
void bxf_lib_init(bxf_lib_t *L)
{
    L->n = L->bad = L->replaced = 0;
    memset(&L->err, 0, sizeof(L->err));
}

static int take(bxf_lib_t *L, const char *t, size_t n, int line0, uint8_t origin, const char *where)
{
    bxf_err_t err;
    if (bxf_parse(t, n, line0, &L->scratch, NULL, &err)) {
        if (!L->bad++) { L->err = err; snprintf(L->err.where, sizeof(L->err.where), "%s", where ? where : ""); }
        return 0;
    }
    const bxf_flow_t *f = &L->scratch;
    int i = 0;
    while (i < L->n && !same_name(L->e[i].name, f->name)) i++;
    if (i < L->n) L->replaced++;
    else if (L->n < BXF_FLOWS) L->n++;
    else return 0;
    bxf_entry_t *e = &L->e[i];
    memcpy(e->name, f->name, sizeof(e->name));
    memcpy(e->about, f->about, sizeof(e->about));
    memcpy(e->match, f->match, sizeof(e->match));
    e->nmatch = f->nmatch;
    e->nsteps = f->nsteps;
    e->writes = f->writes;
    e->origin = origin;
    e->text = t;
    e->len = (uint32_t)n;
    e->line0 = (uint16_t)line0;
    return 1;
}

int bxf_lib_add(bxf_lib_t *L, const char *text, size_t len, uint8_t origin, const char *where)
{
    int got = 0, line = 1, start_line = 0;
    const char *p = text, *end = text + len, *start = NULL;
    while (p < end) {
        const char *nl = find(p, '\n', (size_t)(end - p));
        const char *ls = p;
        size_t ln = (size_t)((nl ? nl : end) - p);
        const char *here = p;
        p = nl ? nl + 1 : end;
        clean(&ls, &ln);
        if (ln) {
            const char *s = ls;
            size_t n;
            const char *w = word(&s, ls + ln, &n);
            if (same_word(w, n, "flow")) {
                if (start) got += take(L, start, (size_t)(here - start), start_line, origin, where);
                start = here;
                start_line = line;
            } else if (!start) {
                if (!L->bad++) {
                    L->err.line = line;
                    snprintf(L->err.where, sizeof(L->err.where), "%s", where ? where : "");
                    snprintf(L->err.msg, sizeof(L->err.msg), "a step before any flow line");
                }
                start = NULL;
            }
        }
        line++;
    }
    if (start) got += take(L, start, (size_t)(end - start), start_line, origin, where);
    return got;
}

bool bxf_glob(const char *pat, const char *s)
{
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (;; s++) {
            if (bxf_glob(pat + 1, s)) return true;
            if (!*s) return false;
        }
    }
    if (!*s) return false;
    if (*pat != '?' && low(*pat) != low(*s)) return false;
    return bxf_glob(pat + 1, s + 1);
}

bool bxf_matches(const bxf_entry_t *e, const bx_conn_t *c, const char *name)
{
    for (int m = 0; m < e->nmatch; m++) {
        const bxf_match_t *k = &e->match[m];
        if (k->kind == BXF_M_NAME && name && name[0] && bxf_glob(k->glob, name)) return true;
        if (k->kind == BXF_M_SERVICE && c)
            for (int i = 0; i < c->nsvc; i++) if (bx_uuid_same(&c->svc[i].uuid, &k->uuid)) return true;
    }
    return false;
}

int bxf_order(const bxf_lib_t *L, const bx_conn_t *c, const char *name, int *out)
{
    int k = 0;
    for (int i = 0; i < L->n; i++) if (bxf_matches(&L->e[i], c, name)) out[k++] = i;
    const int fit = k;
    for (int i = 0; i < L->n; i++) if (!bxf_matches(&L->e[i], c, name)) out[k++] = i;
    return fit;
}

/* ---- running ----------------------------------------------------------------------------------- */
const char *bxf_op_word(int op)
{
    switch (op) {
    case BXF_LOG: return "log";
    case BXF_READ: return "read";
    case BXF_WRITE: return "write";
    case BXF_NOTIFY: return "notify";
    case BXF_WAIT: return "wait";
    case BXF_WAIT_NOTIFY: return "wait notify";
    default: return "?";
    }
}

static int find_chr(const bx_conn_t *c, const bxf_step_t *s)
{
    for (int i = 0; i < c->nchr; i++) {
        if (!bx_uuid_same(&c->chr[i].uuid, &s->uuid)) continue;
        if (s->in_svc && (c->chr[i].svc >= c->nsvc || !bx_uuid_same(&c->svc[c->chr[i].svc].uuid, &s->svc))) continue;
        return i;
    }
    return -1;
}
static void look(bxf_run_t *r, const bx_conn_t *c)
{
    for (int i = 0; i < c->nchr && i < BX_MAX_CHR; i++) r->base[i] = c->chr[i].notes;
}
static void value_text(const bx_chr_t *h, char *out, size_t cap)
{
    if (!h->has_value) { snprintf(out, cap, "no value"); return; }
    if (!bx_decode(bx_uuid16(&h->uuid), h->val, h->vlen, out, cap)) bx_hex(h->val, h->vlen, out, cap);
}

int bxf_begin(bxf_run_t *r, const bxf_entry_t *e, const bx_conn_t *c, const bxf_time_t *time, uint32_t now)
{
    memset(r, 0, sizeof(*r));
    r->state = BXF_IDLE;
    if (!c || c->state != BX_READY) { snprintf(r->why, sizeof(r->why), "Not connected"); return -1; }
    if (c->phase != BX_P_DONE) { snprintf(r->why, sizeof(r->why), "Wait for the table walk to finish"); return -1; }
    bxf_err_t err;
    if (bxf_parse(e->text, e->len, e->line0, &r->f, time, &err)) {
        snprintf(r->why, sizeof(r->why), "Line %d: %s", err.line, err.msg);
        return -1;
    }
    r->origin = e->origin;
    r->state = BXF_RUNNING;
    r->started = now;
    r->chr = -1;
    look(r, c);
    return 0;
}

static void next(bxf_run_t *r, uint32_t now)
{
    r->at++;
    if (r->at >= r->f.nsteps) { r->state = BXF_DONE; r->ended = now; }
}
static void ok(bxf_run_t *r, uint32_t now)
{
    r->res[r->at].st = BXF_S_OK;
    next(r, now);
}
static void fail(bxf_run_t *r, const char *why, uint32_t now)
{
    bxf_result_t *res = &r->res[r->at];
    snprintf(res->text, sizeof(res->text), "%s", why);
    if (r->f.step[r->at].optional) { res->st = BXF_S_SKIP; next(r, now); return; }
    res->st = BXF_S_FAIL;
    r->state = BXF_FAILED;
    r->ended = now;
    snprintf(r->why, sizeof(r->why), "Step %d: %s", r->at + 1, why);
}

bool bxf_tick(bxf_run_t *r, bx_conn_t *c, uint32_t now, bxf_req_t *req)
{
    if (r->state != BXF_RUNNING) return false;
    if (r->at >= r->f.nsteps) { r->state = BXF_DONE; r->ended = now; return false; }
    if (!c || c->state != BX_READY) {
        r->out = false;
        r->res[r->at].st = BXF_S_FAIL;
        snprintf(r->res[r->at].text, sizeof(r->res[r->at].text), "link lost");
        r->state = BXF_FAILED;
        r->ended = now;
        snprintf(r->why, sizeof(r->why), "Step %d: link lost", r->at + 1);
        return false;
    }
    const bxf_step_t *s = &r->f.step[r->at];
    bxf_result_t *res = &r->res[r->at];
    char u[40];
    bx_uuid_text(&s->uuid, u, sizeof(u));
    if (r->out) {
        if (c->done_n == r->done_seen) return false;      /* still out; bx_overdue gives up after 5 s */
        r->out = false;
        if (c->done_err) { fail(r, bx_error_text(c->done_err), now); return false; }
        if (s->op == BXF_READ && r->chr >= 0) value_text(&c->chr[r->chr], res->text, sizeof(res->text));
        else if (s->op == BXF_WRITE) snprintf(res->text, sizeof(res->text), "sent %u byte%s", s->n, s->n == 1 ? "" : "s");
        else if (s->op == BXF_NOTIFY) snprintf(res->text, sizeof(res->text), "%s", s->on ? "on" : "off");
        ok(r, now);
        return false;
    }
    int i;
    switch (s->op) {
    case BXF_LOG: {
        const char *t = s->src, *end = s->src + s->srclen;
        size_t n;
        word(&t, end, &n);
        if (n == 1 && s->src[0] == '?') word(&t, end, &n);
        rest(&t, end, res->text, sizeof(res->text));
        ok(r, now);
        return false;
    }
    case BXF_WAIT:
        if (res->st == BXF_S_PENDING) { res->st = BXF_S_NOW; r->t0 = now; return false; }
        if (now - r->t0 >= s->ms) { snprintf(res->text, sizeof(res->text), "waited %lu ms", (unsigned long)s->ms); ok(r, now); }
        return false;
    case BXF_WAIT_NOTIFY:
        i = find_chr(c, s);
        if (i < 0) {
            char t[48];
            snprintf(t, sizeof(t), "no %.20s here", u);
            fail(r, t, now);
            return false;
        }
        if (res->st == BXF_S_PENDING) { res->st = BXF_S_NOW; r->t0 = now; }
        if (i < BX_MAX_CHR && c->chr[i].notes > r->base[i]) {
            value_text(&c->chr[i], res->text, sizeof(res->text));
            look(r, c);
            ok(r, now);
        } else if (now - r->t0 >= s->ms) {
            char t[48];
            snprintf(t, sizeof(t), "nothing in %lu ms", (unsigned long)s->ms);
            fail(r, t, now);
        }
        return false;
    default: break;
    }
    if (c->busy) return false;                            /* one request at a time */
    i = find_chr(c, s);
    char t[48];
    if (i < 0) { snprintf(t, sizeof(t), "no %.20s here", u); fail(r, t, now); return false; }
    const bx_chr_t *h = &c->chr[i];
    memset(req, 0, sizeof(*req));
    req->op.manual = true;
    if (s->op == BXF_READ) {
        if (!(h->props & 0x02)) { fail(r, "not readable", now); return false; }
        req->op.kind = BX_OP_READ;
        req->op.a = h->value;
    } else if (s->op == BXF_WRITE) {
        if (!(h->props & 0x0C)) { fail(r, "not writable", now); return false; }
        req->op.kind = BX_OP_WRITE;
        req->op.a = h->value;
        memcpy(req->data, s->data, s->n);
        req->n = s->n;
        req->no_rsp = !(h->props & 0x08);
    } else {
        if (!h->cccd) { fail(r, "cannot notify", now); return false; }
        req->op.kind = BX_OP_SUBSCRIBE;
        req->op.a = h->cccd;
        req->op.on = s->on;
    }
    r->chr = i;
    res->st = BXF_S_NOW;
    return true;
}

void bxf_sent(bxf_run_t *r, bx_conn_t *c, const bxf_req_t *req, int rc, uint32_t now)
{
    if (r->state != BXF_RUNNING) return;
    if (rc >= 0) {
        bx_started(c, &req->op);
        r->out = true;
        r->done_seen = c->done_n;
        r->t0 = now;
        look(r, c);
        return;
    }
    if (rc == LAP_IO_EBUSY || rc == LAP_IO_EAGAIN) { r->res[r->at].st = BXF_S_PENDING; return; }   /* again next frame */
    bx_refused(c, &req->op, rc);
    fail(r, bx_error_text(rc), now);
}

void bxf_stop(bxf_run_t *r, uint32_t now)
{
    if (r->state != BXF_RUNNING) return;
    if (r->at < r->f.nsteps) {
        r->res[r->at].st = BXF_S_FAIL;
        snprintf(r->res[r->at].text, sizeof(r->res[r->at].text), "stopped");
    }
    r->state = BXF_STOPPED;
    r->ended = now;
    snprintf(r->why, sizeof(r->why), "Stopped at step %d", r->at + 1);
}
