/* TAP: display filters. Wireshark's field names and operators beside TAP's
   short forms, compiled to a small postfix program.

   As in Wireshark: a bare field is true when the frame has it, a comparison
   is false when it has not, == is true when any of a field's values match
   and != when none do. Strings compare exactly and with case; `matches` is
   a small case-blind pattern (. * + ? ^ $ [ ] \d \w \s). The short forms
   (addr, ssid, name, contains "x") stay case-blind and match by prefix. */
#include "tap.h"

enum { F_TAG, F_NUM, F_ADDR, F_STR, F_SADDR, F_BYTES, F_TEXT, F_AND, F_OR, F_NOT };
enum { C_EQ, C_NE, C_GT, C_LT, C_GE, C_LE, C_HAS, C_RE, C_EXISTS };
enum { N_NO, N_FLEN, N_CAPLEN, N_TREL, N_MARKED, N_TYPE, N_SUB, N_TYPESUB, N_PROT, N_RETRY, N_TODS, N_FROMDS, N_WCH, N_FREQ,
       N_WRSSI, N_RATE, N_REASON, N_MSGNR, N_PDU, N_COMPANY, N_BRSSI, N_BCH, N_RSSI, N_LEN, N_CH, N_SF };
enum { A_ANY, A_SA, A_DA, A_TA, A_RA, A_BSSID, A_ADV };
enum { S_SSID, S_BTNAME, S_NAME, S_INFO, S_PROTO, S_SRC, S_DST, S_ANYADDR };

static const struct { const char *name; uint8_t op, id; } fields[] = {
    {"frame.number", F_NUM, N_NO}, {"frame.len", F_NUM, N_FLEN}, {"frame.cap_len", F_NUM, N_CAPLEN},
    {"frame.time_relative", F_NUM, N_TREL}, {"frame.marked", F_NUM, N_MARKED},
    {"wlan.fc.type", F_NUM, N_TYPE}, {"wlan.fc.subtype", F_NUM, N_SUB}, {"wlan.fc.type_subtype", F_NUM, N_TYPESUB},
    {"wlan.fc.protected", F_NUM, N_PROT}, {"wlan.fc.retry", F_NUM, N_RETRY}, {"wlan.fc.tods", F_NUM, N_TODS},
    {"wlan.fc.fromds", F_NUM, N_FROMDS}, {"wlan_radio.channel", F_NUM, N_WCH}, {"radiotap.channel.freq", F_NUM, N_FREQ},
    {"wlan_radio.frequency", F_NUM, N_FREQ}, {"radiotap.dbm_antsignal", F_NUM, N_WRSSI}, {"wlan_radio.signal_dbm", F_NUM, N_WRSSI},
    {"radiotap.datarate", F_NUM, N_RATE}, {"wlan.fixed.reason_code", F_NUM, N_REASON},
    {"wlan_rsna_eapol.keydes.msgnr", F_NUM, N_MSGNR}, {"btle.advertising_header.pdu_type", F_NUM, N_PDU},
    {"btcommon.eir_ad.entry.company_id", F_NUM, N_COMPANY}, {"btle_rf.signal_dbm", F_NUM, N_BRSSI},
    {"btle_rf.channel", F_NUM, N_BCH}, {"loratap.channel.sf", F_NUM, N_SF},
    {"wlan.addr", F_ADDR, A_ANY}, {"wlan.sa", F_ADDR, A_SA}, {"wlan.da", F_ADDR, A_DA}, {"wlan.ta", F_ADDR, A_TA},
    {"wlan.ra", F_ADDR, A_RA}, {"wlan.bssid", F_ADDR, A_BSSID}, {"btle.advertising_address", F_ADDR, A_ADV},
    {"wlan.ssid", F_STR, S_SSID}, {"btcommon.eir_ad.entry.device_name", F_STR, S_BTNAME},
    {"_ws.col.info", F_STR, S_INFO}, {"_ws.col.protocol", F_STR, S_PROTO},
    /* the short forms */
    {"rssi", F_NUM, N_RSSI}, {"len", F_NUM, N_LEN}, {"ch", F_NUM, N_CH}, {"channel", F_NUM, N_CH}, {"no", F_NUM, N_NO},
    {"addr", F_SADDR, S_ANYADDR}, {"src", F_SADDR, S_SRC}, {"dst", F_SADDR, S_DST},
    {"ssid", F_STR, S_NAME}, {"name", F_STR, S_NAME}, {"info", F_STR, S_INFO}};
static const struct { const char *w; uint32_t mask; } words[] = {
    {"wifi", T_WIFI}, {"wlan", T_WIFI}, {"802.11", T_WIFI}, {"radiotap", T_WIFI}, {"wlan_radio", T_WIFI},
    {"mgmt", T_MGMT}, {"wlan.mgt", T_MGMT}, {"ctrl", T_CTRL}, {"data", T_DATA},
    {"beacon", T_BEACON}, {"probe", T_PROBE}, {"eapol", T_EAPOL}, {"deauth", T_DEAUTH}, {"disassoc", T_DEAUTH}, {"auth", T_AUTH},
    {"ble", T_BLE}, {"bt", T_BLE}, {"btle", T_BLE}, {"btle_rf", T_BLE}, {"btcommon", T_BLE},
    {"ibeacon", T_IBEACON}, {"eddystone", T_EDDYSTONE}, {"scanrsp", T_SCANRSP},
    {"rid", T_RID}, {"lora", T_LORA}, {"loratap", T_LORA}, {"meshcore", T_MESHCORE}, {"meshtastic", T_MESHTASTIC},
    {"decode", T_DECODE}, {"protected", T_PROTECTED}, {"bad", T_BAD}, {"malformed", T_BAD}, {"_ws.malformed", T_BAD},
    {"retry", T_RETRY}, {"cut", T_CUT}, {"llc", T_LLC}, {"ip", T_IP}, {"arp", T_ARP}, {"ipv6", T_IP6}, {"frame", 0xFFFFFFFFu}};

/* kind: 0 end, 1 word, 2 operator, 3 quoted string. tok keeps its case. */
typedef struct { const char *s; tap_filter_t *f; char tok[40]; int kind; } parse_t;
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static bool same(const char *a, const char *b)
{
    while (*a && lower(*a) == *b) { a++; b++; }
    return !*a && !*b;
}
static void next(parse_t *p)
{
    while (*p->s == ' ') p->s++;
    size_t n = 0;
    p->tok[0] = 0; p->kind = 0;
    if (!*p->s) return;
    if (*p->s == '"') {
        p->s++;
        while (*p->s && *p->s != '"') { if (n + 1 < sizeof(p->tok)) p->tok[n++] = *p->s; p->s++; }
        if (*p->s == '"') p->s++;
        p->kind = 3;
    } else if (strchr("()=!<>&|~{},", *p->s)) {
        p->tok[n++] = *p->s++;
        if (*p->s && strchr("=&|", *p->s) && strchr("=!<>&|", p->tok[0])) p->tok[n++] = *p->s++;
        if (n == 2 && *p->s == '=' && (p->tok[0] == '=' || p->tok[0] == '!')) p->s++;      /* === and !== */
        p->kind = 2;
    } else {
        while (*p->s && *p->s != ' ' && !strchr("()=!<>&|~{},\"", *p->s)) { if (n + 1 < sizeof(p->tok)) p->tok[n++] = *p->s; p->s++; }
        p->kind = 1;
    }
    p->tok[n] = 0;
}
static bool is_op(const parse_t *p, const char *sym, const char *word)
{
    return (p->kind == 2 && sym && !strcmp(p->tok, sym)) || (p->kind == 1 && word && same(p->tok, word));
}
static bool fail(parse_t *p, const char *why, const char *what)
{
    if (!p->f->err[0]) snprintf(p->f->err, sizeof(p->f->err), "%s%s%.24s", why, what && what[0] ? " " : "", what ? what : "");
    return false;
}
static tap_fnode_t *emit(parse_t *p, int op)
{
    if (p->f->count >= TAP_FNODES) { fail(p, "the filter is too long", NULL); return NULL; }
    tap_fnode_t *n = &p->f->n[p->f->count++];
    memset(n, 0, sizeof(*n));
    n->op = (uint8_t)op;
    return n;
}
static int compare(parse_t *p)
{
    static const char *const ops[6] = {"==", "!=", ">", "<", ">=", "<="}, *const names[6] = {"eq", "ne", "gt", "lt", "ge", "le"};
    if (is_op(p, "~", "matches")) { next(p); return C_RE; }
    if (is_op(p, NULL, "contains")) { next(p); return C_HAS; }
    if (is_op(p, "=", NULL)) { next(p); return C_EQ; }
    for (int i = 0; i < 6; i++) if (is_op(p, ops[i], names[i])) { next(p); return i; }
    return -1;
}
static int hexval(char c) { c = lower(c); return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
/* aa:bb:cc, aa-bb-cc, aabb.ccdd or aabbcc into bytes; 0 when it is not that. */
static int hexbytes(const char *t, char *out, int cap)
{
    int n = 0;
    while (*t) {
        if (hexval(t[0]) < 0 || hexval(t[1]) < 0 || n >= cap) return 0;
        out[n++] = (char)(hexval(t[0]) * 16 + hexval(t[1]));
        t += 2;
        if (*t == ':' || *t == '-' || *t == '.') { t++; if (!*t) return 0; }
    }
    return n;
}
/* A number: decimal, 0x hex, true/false; seconds with a fraction become
   microseconds when the field is a time. */
static bool number(const char *t, bool time, int64_t *out)
{
    if (same(t, "true")) { *out = 1; return true; }
    if (same(t, "false")) { *out = 0; return true; }
    if (time) {
        int64_t us = 0, scale = 100000;
        const bool neg = *t == '-';
        if (neg) t++;
        if (!*t) return false;
        for (; *t >= '0' && *t <= '9'; t++) us = us * 10 + (*t - '0') * 1000000;
        if (*t == '.') for (t++; *t >= '0' && *t <= '9'; t++, scale /= 10) us += (*t - '0') * scale;
        if (*t) return false;
        *out = neg ? -us : us;
        return true;
    }
    char *end;
    if (!*t) return false;
    *out = strtol(t, &end, 0);
    return !*end;
}
/* The value after a comparison, into n. */
static bool value(parse_t *p, tap_fnode_t *n, const char *field)
{
    if (p->kind != 1 && p->kind != 3) return fail(p, "expected a value after", field);
    if (n->op == F_NUM) {
        if (n->cmp == C_HAS || n->cmp == C_RE) return fail(p, "is a number: use == != > <:", field);
        if (p->kind == 3 || !number(p->tok, n->field == N_TREL, &n->num)) return fail(p, "is not a number:", p->tok);
    } else if (n->op == F_ADDR) {
        if (n->cmp != C_EQ && n->cmp != C_NE) return fail(p, "is an address: use == or !=:", field);
        if (same(p->tok, "broadcast")) { memset(n->str, 0xFF, 6); n->slen = 6; }
        else n->slen = (uint8_t)hexbytes(p->tok, n->str, 6);
        if (!n->slen) return fail(p, "is not an address:", p->tok);
    } else if (n->op == F_BYTES) {
        n->slen = p->kind == 1 ? (uint8_t)hexbytes(p->tok, n->str, sizeof(n->str) - 1) : 0;
        if (!n->slen) { n->slen = (uint8_t)strlen(p->tok); memcpy(n->str, p->tok, n->slen); }
    } else {
        if (n->cmp != C_EQ && n->cmp != C_NE && n->cmp != C_HAS && n->cmp != C_RE) return fail(p, "is text: use == != contains matches:", field);
        const bool blind = n->op == F_SADDR || n->op == F_TEXT || (n->op == F_STR && n->field == S_NAME);
        for (n->slen = 0; p->tok[n->slen]; n->slen++) n->str[n->slen] = blind && n->cmp != C_RE ? lower(p->tok[n->slen]) : p->tok[n->slen];
    }
    if (n->cmp == C_RE) {
        for (const char *c = n->str; *c; c++) {
            if (*c == '\\' && c[1]) { c++; continue; }
            if (strchr("()|{}", *c)) return fail(p, "matches: ( ) | { } are not supported", NULL);
        }
    }
    n->str[n->slen] = 0;
    next(p);
    return true;
}
static bool expr(parse_t *p);
static bool primary(parse_t *p)
{
    if (is_op(p, "(", NULL)) {
        next(p);
        if (!expr(p)) return false;
        if (!is_op(p, ")", NULL)) return fail(p, "missing )", NULL);
        next(p);
        return true;
    }
    if (is_op(p, "!", "not")) {
        next(p);
        return primary(p) && emit(p, F_NOT);
    }
    if (p->kind != 1) return fail(p, p->kind ? "unexpected" : "the filter is not finished", p->tok);
    char w[40];
    snprintf(w, sizeof(w), "%s", p->tok);
    next(p);
    tap_fnode_t proto = {0};
    int op = -1;
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]) && op < 0; i++)
        if (same(w, fields[i].name)) { op = fields[i].op; proto.field = fields[i].id; }
    if (same(w, "marked") && op < 0) { tap_fnode_t *n = emit(p, F_NUM); if (n) { n->field = N_MARKED; n->cmp = C_EQ; n->num = 1; } return n != NULL; }
    if (same(w, "contains") && op < 0) {        /* contains "text": anywhere, case-blind */
        tap_fnode_t *n = emit(p, F_TEXT);
        if (!n) return false;
        n->cmp = C_HAS;
        return value(p, n, w);
    }
    if (same(w, "proto") || same(w, "radio")) {
        const int cmp = compare(p);
        if (cmp != C_EQ && cmp != C_NE) return fail(p, "use == or != after", w);
        tap_fnode_t *n = emit(p, F_TAG);
        if (!n) return false;
        for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) if (same(p->tok, words[i].w)) n->mask = words[i].mask;
        if (!n->mask) return fail(p, "unknown protocol", p->tok);
        next(p);
        return cmp == C_EQ || emit(p, F_NOT);
    }
    if (op < 0) {
        uint32_t mask = 0;
        for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]) && !mask; i++) if (same(w, words[i].w)) mask = words[i].mask;
        if (!mask) return fail(p, strchr(w, '.') ? "unknown field" : "unknown word", w);
        const int cmp = compare(p);
        if (cmp < 0) { tap_fnode_t *n = emit(p, F_TAG); if (n) n->mask = mask; return n != NULL; }
        if (cmp != C_HAS && cmp != C_RE) return fail(p, "is a protocol: use contains or matches:", w);
        tap_fnode_t *n = emit(p, F_BYTES);      /* frame contains "x", wlan contains 88:8e */
        if (!n) return false;
        n->mask = mask; n->cmp = (uint8_t)cmp;
        return value(p, n, w);
    }
    if (is_op(p, NULL, "in")) {                 /* field in {a, b, c} */
        next(p);
        if (!is_op(p, "{", NULL)) return fail(p, "expected { after in", NULL);
        next(p);
        int k = 0;
        while (!is_op(p, "}", NULL)) {
            if (is_op(p, ",", NULL)) { next(p); continue; }
            tap_fnode_t *n = emit(p, op);
            if (!n) return false;
            n->field = proto.field; n->cmp = C_EQ;
            if (!value(p, n, w)) return false;
            if (k++ && !emit(p, F_OR)) return false;
        }
        next(p);
        return k ? true : fail(p, "nothing between { }", NULL);
    }
    const int cmp = compare(p);
    tap_fnode_t *n = emit(p, op);
    if (!n) return false;
    n->field = proto.field;
    if (cmp < 0) {
        if (op == F_SADDR) return fail(p, "expected == after", w);
        n->cmp = C_EXISTS;
        return true;
    }
    n->cmp = (uint8_t)cmp;
    return value(p, n, w);
}
static bool conjn(parse_t *p)
{
    if (!primary(p)) return false;
    while (is_op(p, "&&", "and")) {
        next(p);
        if (!primary(p) || !emit(p, F_AND)) return false;
    }
    return true;
}
static bool expr(parse_t *p)
{
    if (!conjn(p)) return false;
    while (is_op(p, "||", "or")) {
        next(p);
        if (!conjn(p) || !emit(p, F_OR)) return false;
    }
    return true;
}
bool tap_filter_compile(const char *s, tap_filter_t *f)
{
    parse_t p = {.s = s ? s : "", .f = f};
    f->count = 0; f->err[0] = 0;
    next(&p);
    if (!p.kind) return true;
    if (!expr(&p)) { f->count = 0; return false; }
    if (p.kind) { fail(&p, is_op(&p, ")", NULL) ? "unmatched" : "expected and/or before", p.tok); f->count = 0; return false; }
    return true;
}

/* ---- matching ------------------------------------------------------------- */
/* Search of a needle in text or bytes; blind: the needle is lowered already. */
static bool has(const char *hay, size_t hn, const char *needle, size_t nn, bool blind)
{
    if (!nn) return true;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t k = 0;
        while (k < nn && (blind ? lower(hay[i + k]) : hay[i + k]) == needle[k]) k++;
        if (k == nn) return true;
    }
    return false;
}
/* The small pattern matcher: an atom is a character, ., \x, or [set]. */
static const char *atom_end(const char *re)
{
    if (*re == '\\' && re[1]) return re + 2;
    if (*re == '[') {
        const char *q = re + 1;
        if (*q == '^') q++;
        if (*q == ']') q++;
        while (*q && *q != ']') q++;
        return *q ? q + 1 : q;
    }
    return re + 1;
}
static bool atom_is(const char *a, char c)
{
    c = lower(c);
    if (*a == '.') return true;
    if (*a == '\\' && a[1]) {
        switch (a[1]) {
        case 'd': return c >= '0' && c <= '9';
        case 'w': return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_';
        case 's': return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        default: return lower(a[1]) == c;
        }
    }
    if (*a == '[') {
        const bool neg = a[1] == '^';
        const char *q = a + 1 + (neg ? 1 : 0);
        bool hit = false;
        for (bool first = true; *q && (*q != ']' || first); first = false) {
            const char lo = lower(*q);
            if (q[1] == '-' && q[2] && q[2] != ']') { if (c >= lo && c <= lower(q[2])) hit = true; q += 3; }
            else { if (c == lo) hit = true; q++; }
        }
        return hit != neg;
    }
    return lower(*a) == c;
}
static bool re_here(const char *re, const char *t, const char *te)
{
    if (!*re) return true;
    if (*re == '$' && !re[1]) return t == te;
    const char *e = atom_end(re);
    if (*e == '*' || *e == '+' || *e == '?') {
        if (*e == '?') return (t < te && atom_is(re, *t) && re_here(e + 1, t + 1, te)) || re_here(e + 1, t, te);
        const char *q = t;
        while (q < te && atom_is(re, *q)) q++;
        for (const char *min = t + (*e == '+' ? 1 : 0); q >= min; q--) if (re_here(e + 1, q, te)) return true;
        return false;
    }
    return t < te && atom_is(re, *t) && re_here(e, t + 1, te);
}
static bool re_search(const char *re, const char *t, size_t n)
{
    const char *te = t + n;
    if (*re == '^') return re_here(re + 1, t, te);
    for (const char *s = t; s <= te; s++) if (re_here(re, s, te)) return true;
    return false;
}
/* A field's numbers for this frame, up to 2; 0 when the frame has none. */
static int nums(const tap_fnode_t *n, const tap_pkt_t *p, const uint8_t *d, const tap_dis_t *o, const tap_fctx_t *c, int64_t *v)
{
    const bool wifi = p->radio == R_WIFI, hdr = wifi && p->len >= 10, ble = p->radio == R_BLE;
    switch (n->field) {
    case N_NO: v[0] = (int64_t)p->no + 1; return 1;
    case N_FLEN: v[0] = p->orig + tap_file_overhead(p); return 1;
    case N_CAPLEN: v[0] = p->len + tap_file_overhead(p); return 1;
    case N_TREL: v[0] = c ? (int64_t)(p->ts_us - c->t0) : 0; return 1;
    case N_MARKED: v[0] = c && c->marked; return 1;
    case N_TYPE: v[0] = o->fc_type; return hdr;
    case N_SUB: v[0] = o->fc_sub; return hdr;
    case N_TYPESUB: v[0] = o->fc_type << 4 | o->fc_sub; return hdr;
    case N_PROT: v[0] = hdr && (d[1] & 0x40); return hdr;
    case N_RETRY: v[0] = hdr && (d[1] & 0x08); return hdr;
    case N_TODS: v[0] = hdr && (d[1] & 0x01); return hdr;
    case N_FROMDS: v[0] = hdr && (d[1] & 0x02); return hdr;
    case N_WCH: v[0] = p->ch; return wifi;
    case N_FREQ: v[0] = p->ch == 14 ? 2484 : p->ch >= 1 && p->ch <= 13 ? 2407 + 5 * p->ch : 5000 + 5 * p->ch; return wifi;
    case N_WRSSI: v[0] = p->rssi; return wifi;
    case N_RATE: v[0] = p->rate / 2; return wifi && p->rate;
    case N_REASON: v[0] = o->reason; return (o->tags & T_DEAUTH) != 0;
    case N_MSGNR: v[0] = o->eapol_msg; return o->eapol_msg != 0;
    case N_PDU: { static const uint8_t air[8] = {0, 1, 6, 2, 4, 5, 6, 7}; v[0] = air[p->kind & 7]; return ble; }
    case N_COMPANY: v[0] = o->company[0]; v[1] = o->company[1]; return o->companies;
    case N_BRSSI: v[0] = p->rssi; return ble;
    case N_BCH: v[0] = p->ch == 38 ? 12 : p->ch == 39 ? 39 : 0; return ble;
    case N_SF: v[0] = p->sf; return p->radio == R_LORA;
    case N_LEN: v[0] = p->orig; return 1;
    case N_CH: v[0] = p->ch; return 1;
    default: v[0] = p->rssi; return 1;
    }
}
static bool num_is(int64_t v, const tap_fnode_t *n)
{
    switch (n->cmp) { case C_EQ: return v == n->num; case C_NE: return v != n->num; case C_GT: return v > n->num;
        case C_LT: return v < n->num; case C_GE: return v >= n->num; default: return v <= n->num; }
}
static int addrs(int id, const tap_dis_t *o, const uint8_t **v)
{
    int k = 0;
    const unsigned h = o->have;
    if (id == A_ADV) { if (h & H_ADV) v[k++] = o->sa; return k; }
    if (h & H_ADV) return 0;
    if ((id == A_ANY || id == A_RA) && (h & H_RA)) v[k++] = o->ra;
    if ((id == A_ANY || id == A_TA) && (h & H_TA)) v[k++] = o->ta;
    if ((id == A_ANY || id == A_SA) && (h & H_SA)) v[k++] = o->sa;
    if ((id == A_ANY || id == A_DA) && (h & H_DA)) v[k++] = o->da;
    if ((id == A_ANY || id == A_BSSID) && (h & H_BSS)) v[k++] = o->bss;
    return k;
}
static const char *str_of(int id, const tap_dis_t *o)
{
    switch (id) {
    case S_SSID: return (o->tags & T_WIFI) && (o->name[0] || (o->tags & (T_BEACON | T_PROBE))) ? o->name : NULL;
    case S_BTNAME: return (o->tags & T_BLE) && o->name[0] ? o->name : NULL;
    case S_NAME: return o->name;
    case S_INFO: return o->info;
    default: return o->proto;
    }
}
static bool text_is(const char *have, const tap_fnode_t *n, bool blind)
{
    const size_t hn = strlen(have);
    if (n->cmp == C_RE) return re_search(n->str, have, hn);
    if (n->cmp == C_HAS) return has(have, hn, n->str, n->slen, blind);
    const bool eq = hn == n->slen && has(have, hn, n->str, n->slen, blind);
    return n->cmp == C_NE ? !eq : eq;
}
static bool addr_is(const char *have, const tap_fnode_t *n)
{
    /* a prefix is enough: addr == aa:bb:cc */
    size_t k = 0;
    if (!have[0]) return false;
    while (k < n->slen && lower(have[k]) == n->str[k]) k++;
    return k == n->slen;
}
bool tap_text_match(const char *t, const tap_pkt_t *p, const uint8_t *d, const tap_dis_t *o)
{
    const size_t n = strlen(t);
    return has((const char *)d, p->len, t, n, true) || has(o->info, strlen(o->info), t, n, true) || has(o->name, strlen(o->name), t, n, true) ||
           has(o->src, strlen(o->src), t, n, true) || has(o->dst, strlen(o->dst), t, n, true) || has(o->bssid, strlen(o->bssid), t, n, true) ||
           has(o->proto, strlen(o->proto), t, n, true);
}
bool tap_filter_match(const tap_filter_t *f, const tap_pkt_t *p, const uint8_t *d, const tap_dis_t *o, const tap_fctx_t *c)
{
    bool st[TAP_FNODES];
    int sp = 0;
    if (!f->count) return true;
    for (int i = 0; i < f->count; i++) {
        const tap_fnode_t *n = &f->n[i];
        bool v = false;
        switch (n->op) {
        case F_AND: if (sp >= 2) { sp--; st[sp - 1] = st[sp - 1] && st[sp]; } continue;
        case F_OR: if (sp >= 2) { sp--; st[sp - 1] = st[sp - 1] || st[sp]; } continue;
        case F_NOT: if (sp >= 1) st[sp - 1] = !st[sp - 1]; continue;
        case F_TAG: v = (o->tags & n->mask) != 0; break;
        case F_NUM: {
            int64_t x[2];
            const int k = nums(n, p, d, o, c, x);
            if (n->cmp == C_EXISTS) v = k > 0;
            else if (n->cmp == C_NE) { v = k > 0; for (int j = 0; j < k; j++) v = v && num_is(x[j], n); }
            else for (int j = 0; j < k; j++) v = v || num_is(x[j], n);
            break;
        }
        case F_ADDR: {
            const uint8_t *a[5];
            const int k = addrs(n->field, o, a);
            bool any = false;
            for (int j = 0; j < k; j++) any = any || !memcmp(a[j], n->str, n->slen);
            v = n->cmp == C_EXISTS ? k > 0 : n->cmp == C_NE ? k > 0 && !any : any;
            break;
        }
        case F_STR: {
            const char *s = str_of(n->field, o);
            v = s && (n->cmp == C_EXISTS ? (n->field != S_NAME || s[0]) : text_is(s, n, n->field == S_NAME));
            break;
        }
        case F_SADDR:
            v = n->field == S_SRC ? addr_is(o->src, n) : n->field == S_DST ? addr_is(o->dst, n)
                                                       : addr_is(o->src, n) || addr_is(o->dst, n) || addr_is(o->bssid, n);
            if (n->cmp == C_NE) v = !v;
            break;
        case F_BYTES:
            v = (o->tags & n->mask) && (n->cmp == C_RE ? re_search(n->str, (const char *)d, p->len) : has((const char *)d, p->len, n->str, n->slen, false));
            break;
        default:
            v = has((const char *)d, p->len, n->str, n->slen, true) || has(o->info, strlen(o->info), n->str, n->slen, true) ||
                has(o->name, strlen(o->name), n->str, n->slen, true);
            break;
        }
        if (sp < TAP_FNODES) st[sp++] = v;
    }
    return sp > 0 && st[sp - 1];
}
