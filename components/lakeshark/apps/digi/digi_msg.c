/* 77-bit FT8/FT4 message packing. Written from the protocol description
   (QEX 2020, "The FT4 and FT8 Communication Protocols"); constants and bit
   order checked against ft8_lib by Karlis Goba (MIT). */
#include "digi_msg.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"

#define NTOKENS  2063592u
#define MAX22    4194304u
#define MAXGRID4 32400u

static const char C_FULL[]  = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?"; /* 42 */
static const char C_A38[]   = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";     /* 38 */
static const char C_A37[]   = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";      /* 37 */
static const char C_A36[]   = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";       /* 36 */
static const char C_L27[]   = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";                /* 27 */

static int idx_of(const char *set, char c)
{
    if (!c) return -1;
    const char *p = strchr(set, c);
    return p ? (int)(p - set) : -1;
}
static bool ends_with(const char *s, const char *t)
{
    size_t a = strlen(s), b = strlen(t);
    return a >= b && !strcmp(s + a - b, t);
}

/* ---- hashes ------------------------------------------------------------ */

uint32_t digi_hash22(const char *call)
{
    uint64_t n = 0;
    int i = 0;
    for (; call[i] && i < 11; ++i) {
        int j = idx_of(C_A38, call[i]);
        if (j < 0) j = 0;
        n = n * 38u + (uint64_t)j;
    }
    for (; i < 11; ++i) n *= 38u;
    return (uint32_t)((47055833459ull * n) >> (64 - 22)) & 0x3FFFFFu;
}

void digi_hash_init(digi_hash_t *h) { if (h) memset(h, 0, sizeof(*h)); }

uint32_t digi_hash_save(digi_hash_t *h, const char *call)
{
    uint32_t v = digi_hash22(call);
    if (!h || strlen(call) < 3) return v;
    for (int i = 0; i < h->used; ++i)
        if (h->h22[i] == v && !strcmp(h->call[i], call)) return v;
    int slot = h->next;
    h->h22[slot] = v;
    snprintf(h->call[slot], sizeof(h->call[slot]), "%s", call);
    h->next = (slot + 1) % DIGI_HASH_SLOTS;
    if (h->used < DIGI_HASH_SLOTS) h->used++;
    return v;
}

/* bits: 22, 12 or 10. Prints "<CALL>" or "<...>". */
static void hash_lookup(const digi_hash_t *h, uint32_t v, int bits, char *out, size_t cap)
{
    if (h)
        for (int k = 0; k < h->used; ++k) {
            int i = (h->next - 1 - k + DIGI_HASH_SLOTS) % DIGI_HASH_SLOTS;
            if ((h->h22[i] >> (22 - bits)) == v) {
                snprintf(out, cap, "<%s>", h->call[i]);
                return;
            }
        }
    snprintf(out, cap, "<...>");
}

/* ---- callsigns ---------------------------------------------------------- */

bool digi_call_align6(const char *call, char out[7])
{
    char c[12];
    size_t n = strlen(call);
    if (n < 3 || n > 7) return false;
    /* The two prefix work-arounds the protocol defines. */
    if (!strncmp(call, "3DA0", 4) && n <= 7) snprintf(c, sizeof(c), "3D0%s", call + 4);
    else if (!strncmp(call, "3X", 2) && n >= 3 && isalpha((unsigned char)call[2]))
        snprintf(c, sizeof(c), "Q%s", call + 2);
    else snprintf(c, sizeof(c), "%s", call);
    n = strlen(c);
    char a[7] = "      ";
    if (n >= 3 && isdigit((unsigned char)c[2]) && n <= 6) memcpy(a, c, n);
    else if (n >= 2 && isdigit((unsigned char)c[1]) && n <= 5) memcpy(a + 1, c, n);
    else return false;
    if (idx_of(C_A37, a[0]) < 0 || idx_of(C_A36, a[1]) < 0 || !isdigit((unsigned char)a[2]))
        return false;
    for (int i = 3; i < 6; ++i) if (idx_of(C_L27, a[i]) < 0) return false;
    /* No letters after a space in the suffix. */
    for (int i = 3; i < 5; ++i) if (a[i] == ' ' && a[i + 1] != ' ') return false;
    memcpy(out, a, 7);
    return true;
}

static int32_t pack_basecall(const char *call)
{
    char a[7];
    if (!digi_call_align6(call, a)) return -1;
    int32_t n = idx_of(C_A37, a[0]);
    n = n * 36 + idx_of(C_A36, a[1]);
    n = n * 10 + (a[2] - '0');
    n = n * 27 + idx_of(C_L27, a[3]);
    n = n * 27 + idx_of(C_L27, a[4]);
    n = n * 27 + idx_of(C_L27, a[5]);
    return n;
}

/* CQ modifiers: "CQ 123" or "CQ DX" / "CQ TEST" (1-4 letters). */
static int cq_modifier(const char *m)
{
    int nd = 0, nl = 0, v = 0, n = (int)strlen(m);
    for (int i = 0; i < n; ++i) {
        if (isdigit((unsigned char)m[i])) nd++;
        else if (isupper((unsigned char)m[i])) { nl++; v = 27 * v + (m[i] - 'A' + 1); }
        else return -1;
    }
    if (nd == 3 && nl == 0) return atoi(m);
    if (nd == 0 && nl >= 1 && nl <= 4) return 1000 + v;
    return -1;
}

/* A call as n28 plus its /R or /P flag; -1 when it has no 28-bit form. */
static int32_t pack28(const char *call, digi_hash_t *h, int *ip)
{
    *ip = 0;
    if (!strcmp(call, "DE")) return 0;
    if (!strcmp(call, "QRZ")) return 1;
    if (!strcmp(call, "CQ")) return 2;
    if (!strncmp(call, "CQ ", 3)) {
        int v = cq_modifier(call + 3);
        return v < 0 ? -1 : 3 + v;
    }
    char base[12];
    snprintf(base, sizeof(base), "%s", call);
    if (ends_with(base, "/P") || ends_with(base, "/R")) {
        *ip = 1;
        base[strlen(base) - 2] = 0;
    }
    int32_t n = pack_basecall(base);
    if (n >= 0) {
        digi_hash_save(h, call);
        return (int32_t)(NTOKENS + MAX22) + n;
    }
    /* <CALL> sends a 22-bit hash. */
    size_t L = strlen(call);
    if (L >= 5 && call[0] == '<' && call[L - 1] == '>') {
        char c[12];
        snprintf(c, sizeof(c), "%.*s", (int)(L - 2), call + 1);
        *ip = 0;
        return (int32_t)(NTOKENS + digi_hash_save(h, c));
    }
    return -1;
}

static void unpack28(uint32_t n28, int ip, int i3, digi_hash_t *h, char *out, size_t cap)
{
    if (n28 < NTOKENS) {
        if (n28 == 0) snprintf(out, cap, "DE");
        else if (n28 == 1) snprintf(out, cap, "QRZ");
        else if (n28 == 2) snprintf(out, cap, "CQ");
        else if (n28 <= 1002) snprintf(out, cap, "CQ %03u", (unsigned)(n28 - 3));
        else if (n28 <= 532443) {
            uint32_t n = n28 - 1003;
            char m[5] = {0};
            for (int i = 3; i >= 0; --i) { m[i] = C_L27[n % 27]; n /= 27; }
            const char *p = m;
            while (*p == ' ') ++p;
            snprintf(out, cap, "CQ %s", p);
        } else snprintf(out, cap, "CQ ?");
        return;
    }
    n28 -= NTOKENS;
    if (n28 < MAX22) { hash_lookup(h, n28, 22, out, cap); return; }
    uint32_t n = n28 - MAX22;
    char c[7];
    c[6] = 0;
    c[5] = C_L27[n % 27]; n /= 27;
    c[4] = C_L27[n % 27]; n /= 27;
    c[3] = C_L27[n % 27]; n /= 27;
    c[2] = (char)('0' + n % 10); n /= 10;
    c[1] = C_A36[n % 36]; n /= 36;
    c[0] = C_A37[n % 37];
    char t[12];
    int k = 0;
    for (int i = 0; i < 6; ++i) if (c[i] != ' ') t[k++] = c[i];
    t[k] = 0;
    char full[16];
    if (!strncmp(t, "3D0", 3) && strlen(t) > 3) snprintf(full, sizeof(full), "3DA0%s", t + 3);
    else if (t[0] == 'Q' && isalpha((unsigned char)t[1])) snprintf(full, sizeof(full), "3X%s", t + 1);
    else snprintf(full, sizeof(full), "%s", t);
    if (ip) strncat(full, i3 == 2 ? "/P" : "/R", sizeof(full) - strlen(full) - 1);
    digi_hash_save(h, full);
    snprintf(out, cap, "%s", full);
}

bool digi_grid4_valid(const char *g)
{
    return strlen(g) == 4 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' &&
           isdigit((unsigned char)g[2]) && isdigit((unsigned char)g[3]) && strcmp(g, "RR73");
}

/* A report like "-12", "+05", "R-09"; returns false when not one. */
static bool parse_report(const char *s, int *dd)
{
    if (*s != '+' && *s != '-') return false;
    if (!isdigit((unsigned char)s[1])) return false;
    for (const char *p = s + 1; *p; ++p) if (!isdigit((unsigned char)*p)) return false;
    *dd = atoi(s);
    return *dd >= -30 && *dd <= 99 && strlen(s) <= 3;
}

/* ---- bit writer --------------------------------------------------------- */

static void put_bits(uint8_t *b, int *pos, uint64_t v, int n)
{
    for (int i = n - 1; i >= 0; --i, ++*pos)
        if ((v >> i) & 1u) b[*pos / 8] |= (uint8_t)(0x80u >> (*pos % 8));
}
static uint64_t get_bits(const uint8_t *b, int *pos, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; ++i, ++*pos)
        v = (v << 1) | ((b[*pos / 8] >> (7 - *pos % 8)) & 1u);
    return v;
}

/* ---- pack --------------------------------------------------------------- */

static int split(char *s, char *tok[], int max)
{
    int n = 0;
    for (char *p = strtok(s, " "); p && n < max; p = strtok(NULL, " ")) tok[n++] = p;
    return n;
}

static bool pack_free(const char *text, uint8_t p[10])
{
    size_t n = strlen(text);
    if (n > 13) return false;
    /* 13 base-42 digits into a 71-bit number, as a 9-byte big integer. */
    uint8_t b[9] = {0};
    for (int i = 0; i < 13; ++i) {
        int c = idx_of(C_FULL, i < (int)n ? text[i] : ' ');
        if (c < 0) return false;
        unsigned carry = (unsigned)c;
        for (int k = 8; k >= 0; --k) {
            carry += b[k] * 42u;
            b[k] = (uint8_t)carry;
            carry >>= 8;
        }
    }
    memset(p, 0, 10);
    int pos = 0;
    put_bits(p, &pos, b[0] & 0x7Fu, 7);
    for (int k = 1; k < 9; ++k) put_bits(p, &pos, b[k], 8);
    /* n3 = 0, i3 = 0 */
    return true;
}

static bool pack_std(const char *c1, const char *c2, const char *extra, uint8_t p[10], digi_hash_t *h)
{
    int ipa, ipb;
    int32_t a = pack28(c1, h, &ipa), b = pack28(c2, h, &ipb);
    if (a < 0 || b < 0) return false;
    int i3 = 1;
    bool pa = ends_with(c1, "/P"), pb = ends_with(c2, "/P");
    bool ra = ends_with(c1, "/R"), rb = ends_with(c2, "/R");
    if (pa || pb) { if (ra || rb) return false; i3 = 2; }
    uint32_t g;
    int ir = 0, dd;
    if (!extra || !*extra) g = MAXGRID4 + 1;
    else if (!strcmp(extra, "RRR")) g = MAXGRID4 + 2;
    else if (!strcmp(extra, "RR73")) g = MAXGRID4 + 3;
    else if (!strcmp(extra, "73")) g = MAXGRID4 + 4;
    else if (digi_grid4_valid(extra))
        g = ((uint32_t)(extra[0] - 'A') * 18 + (uint32_t)(extra[1] - 'A')) * 100 +
            (uint32_t)(extra[2] - '0') * 10 + (uint32_t)(extra[3] - '0');
    else if (extra[0] == 'R' && extra[1] == ' ' && digi_grid4_valid(extra + 2)) {
        const char *e = extra + 2;
        ir = 1;
        g = ((uint32_t)(e[0] - 'A') * 18 + (uint32_t)(e[1] - 'A')) * 100 +
            (uint32_t)(e[2] - '0') * 10 + (uint32_t)(e[3] - '0');
    } else if (extra[0] == 'R' && parse_report(extra + 1, &dd)) { ir = 1; g = MAXGRID4 + 35 + (uint32_t)dd; }
    else if (parse_report(extra, &dd)) g = MAXGRID4 + 35 + (uint32_t)dd;
    else return false;
    memset(p, 0, 10);
    int pos = 0;
    put_bits(p, &pos, (uint32_t)a, 28); put_bits(p, &pos, (uint32_t)ipa, 1);
    put_bits(p, &pos, (uint32_t)b, 28); put_bits(p, &pos, (uint32_t)ipb, 1);
    put_bits(p, &pos, (uint32_t)ir, 1); put_bits(p, &pos, g, 15);
    put_bits(p, &pos, (uint32_t)i3, 3);
    return true;
}

static bool pack58(const char *call, uint64_t *n58)
{
    uint64_t v = 0;
    size_t n = strlen(call);
    if (n < 3 || n > 11) return false;
    for (size_t i = 0; i < n; ++i) {
        int j = idx_of(C_A38, call[i]);
        if (j < 0) return false;
        v = v * 38u + (uint64_t)j;
    }
    *n58 = v;
    return true;
}

static bool pack_nonstd(const char *c1, const char *c2, const char *extra, uint8_t p[10], digi_hash_t *h)
{
    bool cq = !strcmp(c1, "CQ");
    int nrpt = 0, flip = 0;
    uint32_t n12 = 0;
    char plain[14];
    if (cq) {
        snprintf(plain, sizeof(plain), "%s", c2);
    } else {
        /* One call is bracketed (sent as a 12-bit hash), the other in full. */
        size_t l1 = strlen(c1), l2 = strlen(c2);
        const char *hashed;
        if (l1 > 2 && c1[0] == '<' && c1[l1 - 1] == '>') { hashed = c1; snprintf(plain, sizeof(plain), "%s", c2); flip = 0; }
        else if (l2 > 2 && c2[0] == '<' && c2[l2 - 1] == '>') { hashed = c2; snprintf(plain, sizeof(plain), "%s", c1); flip = 1; }
        else return false;
        char hc[12];
        snprintf(hc, sizeof(hc), "%.*s", (int)strlen(hashed) - 2, hashed + 1);
        n12 = digi_hash_save(h, hc) >> 10;
        if (!extra || !*extra) nrpt = 0;
        else if (!strcmp(extra, "RRR")) nrpt = 1;
        else if (!strcmp(extra, "RR73")) nrpt = 2;
        else if (!strcmp(extra, "73")) nrpt = 3;
        else return false;
    }
    uint64_t n58;
    if (!pack58(plain, &n58)) return false;
    digi_hash_save(h, plain);
    memset(p, 0, 10);
    int pos = 0;
    put_bits(p, &pos, n12, 12); put_bits(p, &pos, n58, 58);
    put_bits(p, &pos, (uint32_t)flip, 1); put_bits(p, &pos, (uint32_t)nrpt, 2);
    put_bits(p, &pos, cq ? 1u : 0u, 1); put_bits(p, &pos, 4, 3);
    return true;
}

bool digi_msg_pack(const char *text, uint8_t payload[10], digi_hash_t *h)
{
    char s[64];
    size_t n = 0;
    for (const char *q = text; *q && n < sizeof(s) - 1; ++q)
        s[n++] = (char)toupper((unsigned char)*q);
    s[n] = 0;
    char copy[64];
    snprintf(copy, sizeof(copy), "%s", s);
    char *tok[6];
    int nt = split(copy, tok, 6);
    if (nt == 0) return false;

    /* Join "CQ DX" / "CQ 123" into one first field when a call follows. */
    char c1[DIGI_CALL_MAX], *rest[4];
    int nr = 0;
    if (nt >= 3 && !strcmp(tok[0], "CQ") && cq_modifier(tok[1]) >= 0 &&
        !digi_grid4_valid(tok[2]) && tok[2][0] != '+' && tok[2][0] != '-') {
        snprintf(c1, sizeof(c1), "CQ %s", tok[1]);
        for (int i = 2; i < nt && nr < 4; ++i) rest[nr++] = tok[i];
    } else {
        snprintf(c1, sizeof(c1), "%s", tok[0]);
        for (int i = 1; i < nt && nr < 4; ++i) rest[nr++] = tok[i];
    }
    char extra[16] = "";
    if (nr == 3 && !strcmp(rest[1], "R") && digi_grid4_valid(rest[2]))
        snprintf(extra, sizeof(extra), "R %s", rest[2]);
    else if (nr == 2) snprintf(extra, sizeof(extra), "%s", rest[1]);
    if (nr >= 1 && nr <= 3 && (nr != 3 || extra[0] == 'R')) {
        if (pack_std(c1, rest[0], extra, payload, h)) return true;
        if (pack_nonstd(c1, rest[0], extra, payload, h)) return true;
    }
    return pack_free(s, payload);
}

/* ---- unpack ------------------------------------------------------------- */

static void grid_or_report(uint32_t g, int ir, digi_msg_fields_t *f)
{
    if (g <= MAXGRID4) {
        uint32_t v = g;
        char gr[5];
        gr[3] = (char)('0' + v % 10); v /= 10;
        gr[2] = (char)('0' + v % 10); v /= 10;
        gr[1] = (char)('A' + v % 18); v /= 18;
        gr[0] = (char)('A' + v % 18);
        gr[4] = 0;
        snprintf(f->grid, sizeof(f->grid), "%s", gr);
        snprintf(f->extra, sizeof(f->extra), "%s%s", ir ? "R " : "", gr);
        return;
    }
    int r = (int)(g - MAXGRID4);
    if (r == 1) f->extra[0] = 0;
    else if (r == 2) snprintf(f->extra, sizeof(f->extra), "RRR");
    else if (r == 3) snprintf(f->extra, sizeof(f->extra), "RR73");
    else if (r == 4) snprintf(f->extra, sizeof(f->extra), "73");
    else {
        f->report = r - 35;
        f->has_report = true;
        snprintf(f->extra, sizeof(f->extra), "%s%+03d", ir ? "R" : "", f->report);
    }
}

bool digi_msg_unpack(const uint8_t p[10], char *text, size_t cap, digi_hash_t *h,
                     digi_msg_fields_t *fo)
{
    digi_msg_fields_t f;
    memset(&f, 0, sizeof(f));
    int pos = 74;
    int i3 = (int)get_bits(p, &pos, 3);
    pos = 71;
    int n3 = (int)get_bits(p, &pos, 3);
    bool ok = true;
    if (text && cap) text[0] = 0;

    if (i3 == 1 || i3 == 2) {
        pos = 0;
        uint32_t a = (uint32_t)get_bits(p, &pos, 28); int ipa = (int)get_bits(p, &pos, 1);
        uint32_t b = (uint32_t)get_bits(p, &pos, 28); int ipb = (int)get_bits(p, &pos, 1);
        int ir = (int)get_bits(p, &pos, 1);
        uint32_t g = (uint32_t)get_bits(p, &pos, 15);
        f.type = DIGI_MSG_STD;
        unpack28(a, ipa, i3, h, f.call_to, sizeof(f.call_to));
        unpack28(b, ipb, i3, h, f.call_de, sizeof(f.call_de));
        grid_or_report(g, ir, &f);
        f.cq = !strncmp(f.call_to, "CQ", 2);
        snprintf(text, cap, "%s %s%s%s", f.call_to, f.call_de, f.extra[0] ? " " : "", f.extra);
    } else if (i3 == 4) {
        pos = 0;
        uint32_t n12 = (uint32_t)get_bits(p, &pos, 12);
        uint64_t n58 = get_bits(p, &pos, 58);
        int flip = (int)get_bits(p, &pos, 1), nrpt = (int)get_bits(p, &pos, 2);
        int cq = (int)get_bits(p, &pos, 1);
        char c11[12], plain[12], hashed[DIGI_CALL_MAX];
        c11[11] = 0;
        for (int i = 10; i >= 0; --i) { c11[i] = C_A38[n58 % 38]; n58 /= 38; }
        const char *s = c11;
        while (*s == ' ') ++s;
        snprintf(plain, sizeof(plain), "%s", s);
        for (int i = (int)strlen(plain) - 1; i >= 0 && plain[i] == ' '; --i) plain[i] = 0;
        digi_hash_save(h, plain);
        f.type = DIGI_MSG_NONSTD;
        if (cq) {
            f.cq = true;
            snprintf(f.call_to, sizeof(f.call_to), "CQ");
            snprintf(f.call_de, sizeof(f.call_de), "%s", plain);
        } else {
            hash_lookup(h, n12, 12, hashed, sizeof(hashed));
            snprintf(f.call_to, sizeof(f.call_to), "%s", flip ? plain : hashed);
            snprintf(f.call_de, sizeof(f.call_de), "%s", flip ? hashed : plain);
            static const char *const R[] = {"", "RRR", "RR73", "73"};
            snprintf(f.extra, sizeof(f.extra), "%s", R[nrpt]);
        }
        snprintf(text, cap, "%s %s%s%s", f.call_to, f.call_de, f.extra[0] ? " " : "", f.extra);
    } else if (i3 == 0 && n3 == 0) {
        uint8_t b[9];
        pos = 0;
        b[0] = (uint8_t)get_bits(p, &pos, 7);
        for (int k = 1; k < 9; ++k) b[k] = (uint8_t)get_bits(p, &pos, 8);
        char c[14];
        c[13] = 0;
        for (int i = 12; i >= 0; --i) {
            unsigned rem = 0;
            for (int k = 0; k < 9; ++k) {
                rem = (rem << 8) | b[k];
                b[k] = (uint8_t)(rem / 42u);
                rem %= 42u;
            }
            c[i] = C_FULL[rem];
        }
        int e = 12;
        while (e >= 0 && c[e] == ' ') c[e--] = 0;
        const char *s = c;
        while (*s == ' ') ++s;
        f.type = DIGI_MSG_FREE;
        snprintf(text, cap, "%s", s);
    } else if (i3 == 0 && n3 == 1) {
        pos = 0;
        uint32_t a = (uint32_t)get_bits(p, &pos, 28), b = (uint32_t)get_bits(p, &pos, 28);
        uint32_t n10 = (uint32_t)get_bits(p, &pos, 10), r5 = (uint32_t)get_bits(p, &pos, 5);
        char ca[DIGI_CALL_MAX], cb[DIGI_CALL_MAX], hc[DIGI_CALL_MAX];
        unpack28(a, 0, 1, h, ca, sizeof(ca));
        unpack28(b, 0, 1, h, cb, sizeof(cb));
        hash_lookup(h, n10, 10, hc, sizeof(hc));
        f.type = DIGI_MSG_DXPED;
        snprintf(f.call_to, sizeof(f.call_to), "%s", cb);
        snprintf(f.call_de, sizeof(f.call_de), "%s", hc);
        f.report = 2 * (int)r5 - 30;
        f.has_report = true;
        snprintf(f.extra, sizeof(f.extra), "%+03d", f.report);
        LS_SNPRINTF(t_digi_msg_s_rr73_s_s, text, cap, "%s RR73; %s %s %+03d", ca, cb, hc, f.report);
    } else if (i3 == 0 && n3 == 5) {
        pos = 0;
        char hex[24];
        int k = 0;
        hex[k++] = "0123456789ABCDEF"[get_bits(p, &pos, 3)];
        for (int i = 0; i < 17; ++i) hex[k++] = "0123456789ABCDEF"[get_bits(p, &pos, 4)];
        hex[k] = 0;
        const char *s = hex;
        while (*s == '0' && s[1]) ++s;
        f.type = DIGI_MSG_TELEMETRY;
        snprintf(text, cap, "%s", s);
    } else {
        f.type = i3 == 0 && n3 == 2 ? DIGI_MSG_EUVHF : i3 == 0 && (n3 == 3 || n3 == 4) ? DIGI_MSG_FIELDDAY :
                 i3 == 3 ? DIGI_MSG_RTTY : i3 == 5 ? DIGI_MSG_WWROF : DIGI_MSG_UNKNOWN;
        static const char *const NAME[] = {"", "", "EU VHF", "FIELD DAY", "", "", "RTTY RU", "", "WWROF", "?"};
        LS_SNPRINTF(t_digi_msg_s_message_i3_d, text, cap, "[%s message i3=%d n3=%d]", NAME[f.type], i3, n3);
        ok = f.type != DIGI_MSG_UNKNOWN;
    }
    if (fo) *fo = f;
    return ok;
}
