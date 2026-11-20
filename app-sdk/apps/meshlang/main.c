/* MESH LANGUAGES: the translator languages MESHTASTIC reads, over Wi-Fi.
   Portuguese, French, German, Italian, Dutch, Polish, Russian and Ukrainian
   are a file each on the card (155-198 KB). This lists them with their size
   and where they are (downloaded here, a PACKS pack, or not on the card),
   downloads one from terminalbay.com and removes one. The size and sha256
   come from the pack list (data.terminalbay.com/v1/catalog.txt); the file
   must match both before it is written to meshtastic/lang/<code>.bin, where
   MESHTASTIC reads it, and it is read back and compared after. HTTPS with
   the board's CA bundle is the trust. API 2.4 calls only: 2.23.x boards. */
#include "lakeshark_app.h"
#include "lakeshark_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define URL     "https://data.terminalbay.com/v1/"
#define DIR_DL  "/sdcard/meshtastic/lang"
#define CHUNK   65536u
#define NL      8

static const char *const CODE[NL] = { "pt", "fr", "de", "it", "nl", "pl", "ru", "uk" };
static const char *const NAME[NL] = { "PORTUGUESE", "FRENCH", "GERMAN", "ITALIAN", "DUTCH", "POLISH", "RUSSIAN", "UKRAINIAN" };
static const char *const HELLO[NL] = { "Ola", "Bonjour", "Hallo", "Ciao", "Hoi", "Czesc", "Privet", "Pryvit" };

/* The three places MESHTASTIC reads a language from, in its order. */
enum { AT_PACK, AT_DL, AT_HAND, ATS };
static const char *const AT_PATH[ATS] = { "/sdcard/lakeshark/packs/lang-", DIR_DL "/", "/sdcard/meshtastic/lang-" };
typedef struct {
    uint32_t have[ATS];         /* bytes of the file there, 0 none */
    uint32_t size;              /* the pack list's, 0 not listed */
    uint8_t sha[32];
    char path[40];              /* packs/lang-ru.bin */
} lang_t;
static lang_t L[NL];
static int sel;

/* ---- SHA-256 (FIPS 180-4), one shot ------------------------------------ */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t s[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}
static void sha256(const uint8_t *data, size_t n, uint8_t out[32])
{
    uint32_t s[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint64_t bits = (uint64_t)n * 8;
    while (n >= 64) { sha_block(s, data); data += 64; n -= 64; }
    uint8_t last[128];
    memset(last, 0, sizeof(last));
    memcpy(last, data, n);
    last[n] = 0x80;
    const size_t total = n + 9 <= 64 ? 64 : 128;
    for (int i = 0; i < 8; i++) last[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(s, last);
    if (total == 128) sha_block(s, last + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s[i] >> 24); out[4 * i + 1] = (uint8_t)(s[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s[i] >> 8); out[4 * i + 3] = (uint8_t)s[i];
    }
}

/* ---- words on the screen ----------------------------------------------- */
static char msg[64];
static bool msg_bad;
static uint32_t msg_ms;
static void say(bool bad, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg_bad = bad;
    msg_ms = lap_millis() | 1u;
}
static unsigned kb(uint32_t bytes) { return (unsigned)((bytes + 1023u) / 1024u); }
static uint8_t A(uint8_t fg) { return LAP_ATTR(fg, LAP_BLACK); }
static int put(int x, int y, int w, uint8_t attr, const char *fmt, ...)
{
    char t[128];
    if (w <= 0) return 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if ((int)strlen(t) > w) t[w] = 0;
    lap_put_str(x, y, t, attr);
    return (int)strlen(t);
}

/* ---- the card: what is where (four size jobs at a time) ---------------- */
static int pj[4] = { -1, -1, -1, -1 }, pk[4], probe_next = NL * ATS;
static void probe_all(void) { probe_next = 0; }
static bool probing(void)
{
    if (probe_next < NL * ATS) return true;
    for (int s = 0; s < 4; s++) if (pj[s] >= 0) return true;
    return false;
}
static void probe_pump(void)
{
    for (int s = 0; s < 4; s++) {
        if (pj[s] >= 0) {
            const int r = lap_sd_poll(pj[s]);
            if (r == LAP_SD_PENDING) continue;
            pj[s] = -1;
            L[pk[s] / ATS].have[pk[s] % ATS] = r > 0 ? (uint32_t)r : 0;
        }
        if (probe_next < NL * ATS) {
            char p[64];
            snprintf(p, sizeof(p), "%s%s.bin", AT_PATH[probe_next % ATS], CODE[probe_next / ATS]);
            const int j = lap_sd_size(p);
            if (j < 0) return;                      /* four waiting: next frame */
            pj[s] = j;
            pk[s] = probe_next++;
        }
    }
}
static bool on_card(int i) { return L[i].have[AT_DL] || L[i].have[AT_HAND]; }
static uint32_t card_size(int i) { return L[i].have[AT_DL] ? L[i].have[AT_DL] : L[i].have[AT_HAND]; }
/* Downloaded or copied, and the pack list has another size: an older or cut-off copy. */
static bool stale(int i) { return on_card(i) && L[i].size && card_size(i) != L[i].size; }

/* Card jobs whose answer does not matter, polled to free them. */
static int loose[4] = { -1, -1, -1, -1 };
static void forget(int j)
{
    for (int i = 0; j >= 0 && i < 4; i++) if (loose[i] < 0) { loose[i] = j; return; }
}

/* ---- Wi-Fi ------------------------------------------------------------- */
static bool wifi_up, net_denied;
static char ssid[33];
static void wifi_look(void)
{
    lap_wifi_status_t w;
    memset(&w, 0, sizeof(w));
    w.size = sizeof(w);
    w.version = 1;
    const int r = lap_wifi_status(&w);
    if (r == LAP_IO_EBUSY) return;                  /* the worker is mid-request: as it was */
    net_denied = r == LAP_IO_EACCES;
    wifi_up = r == 0 && w.connected;
    snprintf(ssid, sizeof(ssid), "%s", wifi_up ? w.ssid : "");
}

/* ---- HTTPS -------------------------------------------------------------- */
static lap_http_request_t req;
static uint8_t io[2048];
static void req_get(const char *path, uint32_t limit)
{
    memset(&req, 0, sizeof(req));
    req.size = sizeof(req);
    req.version = 1;
    req.method = LAP_HTTP_GET;
    req.timeout_ms = 30000;
    req.response_limit = limit;
    snprintf(req.url, sizeof(req.url), URL "%s", path);
}
/* The board's network worker holds its lock through a whole TLS handshake,
   and a call meanwhile is EBUSY: a wait. EBUSY only stops a request that
   itself ended so (the board short of internal memory for TLS). */
static int http_read(int h, void *to, uint32_t cap)
{
    const int n = lap_http_read(h, to, cap);
    if (n != LAP_IO_EBUSY) return n;
    lap_http_status_t s;
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.version = 1;
    return lap_http_status(h, &s) == 0 && s.state == LAP_OPERATION_DONE && s.error == LAP_IO_EBUSY ? LAP_IO_EBUSY
                                                                                               : LAP_IO_EAGAIN;
}
/* The status code once the headers are in; 0 before (or while the lock is held). */
static int http_code(int h)
{
    lap_http_status_t s;
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    s.version = 1;
    if (lap_http_status(h, &s) || s.state == LAP_OPERATION_PENDING) return 0;
    return s.status_code;
}
static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* ---- the pack list ------------------------------------------------------ */
enum { CAT_NONE, CAT_GO, CAT_READ, CAT_OK, CAT_FAIL };
static int cat_st, cat_h, want_get = -1;
static uint32_t cat_t0;
static unsigned cat_found;                      /* bits of the languages it lists */
static char line[160];
static uint16_t ln;
/* "pack lang-ru LANG <bbox> 185468 <sha256> packs/lang-ru.bin 20261119" */
static void cat_row(char *s)
{
    char *t[8];
    int n = 0, i;
    for (char *p = s; *p && n < 8;) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        t[n++] = p;
        while (*p && *p != ' ') p++;
    }
    if (n < 7 || strcmp(t[0], "pack") || strncmp(t[1], "lang-", 5)) return;
    for (i = 0; i < NL && strcmp(t[1] + 5, CODE[i]); i++) {}
    if (i == NL) return;
    const unsigned long size = strtoul(t[4], NULL, 10);
    if (size < 128 || size > 1048575u || strlen(t[5]) != 64 || strlen(t[6]) >= sizeof(L[i].path) || strstr(t[6], ".."))
        return;
    for (const char *p = t[6]; *p; p++)
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') && !strchr("._-/", *p)) return;
    uint8_t sha[32];
    for (int k = 0; k < 32; k++) {
        const int a = hexv(t[5][2 * k]), b = hexv(t[5][2 * k + 1]);
        if (a < 0 || b < 0) return;
        sha[k] = (uint8_t)(a << 4 | b);
    }
    memcpy(L[i].sha, sha, sizeof(sha));
    snprintf(L[i].path, sizeof(L[i].path), "%s", t[6]);
    L[i].size = (uint32_t)size;
    cat_found |= 1u << i;
}
static void cat_feed(const uint8_t *p, int n, bool end)
{
    for (int i = 0; i <= n; i++) {
        if (i == n && !end) break;
        const char c = i < n ? (char)p[i] : '\n';
        if (c == '\n' || c == '\r') {
            line[ln] = 0;
            if (ln) cat_row(line);
            ln = 0;
        } else if (ln + 1u < sizeof(line)) line[ln++] = c;
    }
}
static void cat_start(void)
{
    if (cat_st == CAT_GO || cat_st == CAT_READ) return;
    if (net_denied) { say(true, "No network for this app: open it again, ALLOW"); return; }
    if (!wifi_up) { say(true, "No Wi-Fi. Connect in SETTINGS, then RELOAD"); return; }
    cat_st = CAT_GO;
    cat_t0 = lap_millis();
    ln = 0;
}
static void cat_end(bool ok, const char *fmt, int code)
{
    if (cat_h > 0) lap_http_close(cat_h);
    cat_h = 0;
    cat_st = ok ? CAT_OK : CAT_FAIL;
    if (!ok) { say(true, fmt, code); want_get = -1; }
}
static void dl_begin(int i);
static void cat_pump(void)
{
    if (cat_st == CAT_GO) {
        req_get("catalog.txt", 1048576u);
        const int h = lap_http_open(&req);
        if (h == LAP_IO_EBUSY && (int32_t)(lap_millis() - cat_t0) < 10000) return;
        if (h < 0) { cat_end(false, "Could not ask terminalbay.com (%d)", h); return; }
        cat_h = h;
        cat_st = CAT_READ;
        return;
    }
    if (cat_st != CAT_READ) return;
    const int code = http_code(cat_h);
    if (code && code != 200) { cat_end(false, "terminalbay.com said %d", code); return; }
    for (int k = 0; k < 8; k++) {
        const int n = http_read(cat_h, io, sizeof(io));
        if (n == LAP_IO_EAGAIN) break;
        if (n < 0) { cat_end(false, n == LAP_IO_EBUSY ? "The board is busy, RELOAD (%d)" : "Could not reach terminalbay.com (%d)", n); return; }
        cat_feed(io, n, n == 0);
        if (n == 0 || cat_found == (1u << NL) - 1) {
            cat_end(cat_found != 0, "The pack list has no languages (%d)", 0);
            if (cat_found && want_get >= 0) { const int i = want_get; want_get = -1; dl_begin(i); }
            return;
        }
    }
    if ((int32_t)(lap_millis() - cat_t0) > 40000) cat_end(false, "terminalbay.com did not answer (%d)", 0);
}

/* ---- a download: confirm, the bytes, the hash, the card, read back ------ */
enum { DL_IDLE, DL_START, DL_GET, DL_SAVE };
static struct {
    int st, lang, h, job, step;
    uint32_t got, at, t0;
    uint8_t *buf, *chk;
} dl;
static int dlg = -1, dlg_kind, dlg_lang;       /* the confirm up, and what it is for */
enum { ASK_GET = 1, ASK_DEL };
static void dl_path(int i, char *b, size_t n) { snprintf(b, n, DIR_DL "/%s.bin", CODE[i]); }
static void dl_end(void)
{
    if (dl.h > 0) lap_http_close(dl.h);
    free(dl.buf);
    free(dl.chk);
    dl.buf = dl.chk = NULL;
    dl.h = 0;
    dl.st = DL_IDLE;
}
static void dl_fail(const char *fmt, ...)
{
    char why[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    const int i = dl.lang;
    dl_end();
    say(true, "%.10s: %s", NAME[i], why);
    lap_log("lang-%s: %s", CODE[i], why);
}
/* The confirm, once the pack list has the language's size. */
static void dl_begin(int i)
{
    if (dl.st != DL_IDLE || dlg >= 0) return;
    if (!L[i].size) {
        if (cat_st == CAT_OK) { say(true, "%s is not in the pack list yet", NAME[i]); return; }
        want_get = i;                               /* after the list */
        cat_start();
        return;
    }
    char m[128];
    snprintf(m, sizeof(m), "DOWNLOAD %s (%u KB) FROM TERMINALBAY.COM FOR MESHTASTIC'S TRANSLATOR?", NAME[i], kb(L[i].size));
    dlg = lap_confirm(m);
    dlg_kind = ASK_GET;
    dlg_lang = i;
}
static void ask_remove(int i)
{
    if (dlg >= 0 || dl.st != DL_IDLE) return;
    char m[128];
    snprintf(m, sizeof(m), "REMOVE %s (%u KB) FROM THE CARD?%s", NAME[i], kb(card_size(i)),
             L[i].have[AT_PACK] ? " THE PACK FROM PACKS STAYS." : "");
    dlg = lap_confirm(m);
    dlg_kind = ASK_DEL;
    dlg_lang = i;
}
static void remove_now(int i)
{
    char p[64];
    for (int a = AT_DL; a <= AT_HAND; a++)
        if (L[i].have[a]) {
            snprintf(p, sizeof(p), "%s%s.bin", AT_PATH[a], CODE[i]);
            const int j = lap_sd_remove(p);
            if (j < 0) { say(true, "The card is busy, try again"); return; }
            forget(j);
        }
    say(false, "%s removed, %u KB free again", NAME[i], kb(card_size(i)));
    probe_all();                                    /* after the removes: the jobs run in order */
}
static void dl_go(int i)
{
    if (net_denied || !wifi_up) { say(true, net_denied ? "No network for this app: open it again, ALLOW" : "No Wi-Fi. Connect in SETTINGS"); return; }
    dl.lang = i;
    dl.h = 0;
    dl.got = dl.at = 0;
    dl.job = -1;
    dl.t0 = lap_millis();
    dl.st = DL_START;
}
static void dl_save(void)
{
    char path[48];
    dl_path(dl.lang, path, sizeof(path));
    const uint32_t size = L[dl.lang].size;
    if (dl.job >= 0) {
        const int r = lap_sd_poll(dl.job);
        if (r == LAP_SD_PENDING) return;
        dl.job = -1;
        switch (dl.step) {
        case 0: case 1: dl.step++; break;           /* the folders; there already is fine */
        case 2:
            if (r <= 0) goto bad;
            dl.at += (uint32_t)r;
            if (dl.at >= size) dl.step = 3;
            break;
        case 3:
            if (r != (int)size || !(dl.chk = malloc(CHUNK))) goto bad;
            dl.at = 0;
            dl.step = 4;
            break;
        default:
            if (r <= 0 || dl.at + (uint32_t)r > size || memcmp(dl.chk, dl.buf + dl.at, (size_t)r)) goto bad;
            dl.at += (uint32_t)r;
            if (dl.at >= size) {
                const int i = dl.lang;
                dl_end();
                say(false, "%s downloaded, %u KB, checked", NAME[i], kb(size));
                lap_log("lang-%s downloaded (%u bytes)", CODE[i], (unsigned)size);
                probe_all();
                return;
            }
            break;
        }
    }
    const uint32_t left = size - dl.at, n = left > CHUNK ? CHUNK : left;
    int j;
    switch (dl.step) {
    case 0: j = lap_sd_mkdir("/sdcard/meshtastic"); break;
    case 1: j = lap_sd_mkdir(DIR_DL); break;
    case 2: j = lap_sd_write(path, dl.at ? LAP_SD_APPEND : 0, dl.buf + dl.at, n); break;
    case 3: j = lap_sd_size(path); break;
    default: j = lap_sd_read(path, dl.at, dl.chk, n); break;
    }
    if (j >= 0) dl.job = j;                         /* refused (four waiting): next frame */
    return;
bad:
    forget(lap_sd_remove(path));                    /* nothing half written stays */
    dl_fail("could not write the card");
    probe_all();
}
static void dl_pump(void)
{
    const uint32_t now = lap_millis();
    const lang_t *g = &L[dl.lang];
    switch (dl.st) {
    case DL_START: {
        if (!dl.buf && !(dl.buf = malloc(g->size))) { dl_fail("no memory for %u KB", kb(g->size)); return; }
        req_get(g->path, g->size + 1u);             /* a byte over: a longer file shows */
        const int h = lap_http_open(&req);
        if (h == LAP_IO_EBUSY && (int32_t)(now - dl.t0) < 10000) return;
        if (h < 0) { dl_fail("could not ask terminalbay.com (%d)", h); return; }
        dl.h = h;
        dl.got = 0;
        dl.t0 = now;
        dl.st = DL_GET;
        return;
    }
    case DL_GET: {
        const int code = http_code(dl.h);
        if (code && code != 200) { dl_fail("terminalbay.com said %d", code); return; }
        for (int k = 0; k < 16; k++) {
            const bool more = dl.got < g->size;
            uint32_t cap = more ? g->size - dl.got : sizeof(io);
            if (cap > 8192u) cap = 8192u;
            const int n = http_read(dl.h, more ? dl.buf + dl.got : io, cap);
            if (n == LAP_IO_EAGAIN) break;
            if (n == LAP_IO_EBUSY) { dl_fail("the board is busy, try again"); return; }
            if (n < 0) { dl_fail("stopped at %u of %u KB (%d)", dl.got / 1024u, kb(g->size), n); return; }
            if (n > 0 && !more) { dl_fail("longer than the pack list says"); return; }
            if (n > 0) { dl.got += (uint32_t)n; continue; }
            lap_http_close(dl.h);                   /* all of it */
            dl.h = 0;
            if (dl.got != g->size) { dl_fail("stopped at %u of %u KB", dl.got / 1024u, kb(g->size)); return; }
            uint8_t sum[32];
            sha256(dl.buf, g->size, sum);
            if (memcmp(sum, g->sha, sizeof(sum))) { dl_fail("damaged on the way (sha256)"); return; }
            /* a translator file names its language at bytes 4..7 */
            if (g->size < 8 || memcmp(dl.buf + 4, CODE[dl.lang], 2) || dl.buf[6]) { dl_fail("not a %s file", CODE[dl.lang]); return; }
            dl.step = 0;
            dl.at = 0;
            dl.job = -1;
            dl.st = DL_SAVE;
            return;
        }
        if ((int32_t)(now - dl.t0) > 45000) dl_fail("too slow, try again");
        return;
    }
    case DL_SAVE: dl_save(); return;
    default: return;
    }
}

/* ---- the app ------------------------------------------------------------ */
static int W, H, list_y, row_h, rows_shown, top;
static bool cat_auto;                           /* read the pack list when Wi-Fi is first seen up */
static lap_btn_t keys[3];
static char kv0[12];

static void enter(void)
{
    sel = top = 0;
    memset(L, 0, sizeof(L));
    dlg = -1;
    want_get = -1;
    cat_st = CAT_NONE;
    cat_found = 0;
    probe_all();
    wifi_look();
    cat_auto = true;                                /* the sizes, once Wi-Fi says it is up */
}
static void leave(void) { dl_end(); if (cat_h > 0) lap_http_close(cat_h); cat_h = 0; }

static bool removable(int i) { return on_card(i) && !(dl.st != DL_IDLE && dl.lang == i); }
static bool gettable(int i) { return dl.st == DL_IDLE && (!on_card(i) || stale(i)) && !net_denied; }
static void primary(int i)
{
    if (gettable(i)) dl_begin(i);
    else if (removable(i)) ask_remove(i);
    else if (L[i].have[AT_PACK]) say(false, "%s is a pack: remove it in PACKS", NAME[i]);
}
static void act(char c)
{
    switch (c) {
    case 'd': if (gettable(sel)) dl_begin(sel); else say(false, "%s is on the card already", NAME[sel]); break;
    case 'x': if (removable(sel)) ask_remove(sel); else if (L[sel].have[AT_PACK]) say(false, "A pack: remove it in PACKS"); break;
    case 'r': wifi_look(); probe_all(); cat_st = CAT_NONE; cat_found = 0; cat_start(); break;
    default: break;
    }
}

/* The art: a word balloon of hellos, each lit by where its language is:
   green on the card, cyan a pack, grey not here. */
static void art(int y)
{
    const int bw = W - 2 < 46 ? W - 2 : 46, x = (W - bw) / 2;
    if (bw < 20) return;
    lap_put_cell(x, y, '.', A(LAP_MAGENTA));
    for (int k = 1; k < bw - 1; k++) lap_put_cell(x + k, y, '-', A(LAP_MAGENTA));
    lap_put_cell(x + bw - 1, y, '.', A(LAP_MAGENTA));
    for (int r = 0; r < 2; r++) {
        lap_put_cell(x - 1 >= 0 ? x - 1 : x, y + 1 + r, '(', A(LAP_MAGENTA));
        int cx = x + 2;
        for (int k = 0; k < 4; k++) {
            const int i = r * 4 + k;
            const uint8_t at = L[i].have[AT_PACK] ? A(LAP_CYAN | LAP_BRIGHT) : on_card(i) ? A(LAP_GREEN | LAP_BRIGHT)
                             : LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK);
            if (cx + (int)strlen(HELLO[i]) < x + bw - 2) cx += put(cx, y + 1 + r, x + bw - 2 - cx, at, "%s", HELLO[i]) + 2;
        }
        if (r == 1 && cx + 9 < x + bw - 1) put(x + bw - 11, y + 2, 9, A(LAP_WHITE | LAP_BRIGHT), "> hello");
        lap_put_cell(x + bw - 1 + 1 < W ? x + bw : x + bw - 1, y + 1 + r, ')', A(LAP_MAGENTA));
    }
    lap_put_cell(x, y + 3, '\'', A(LAP_MAGENTA));
    lap_put_cell(x + 1, y + 3, '-', A(LAP_MAGENTA));
    lap_put_cell(x + 2, y + 3, '.', A(LAP_MAGENTA));
    for (int k = 3; k < bw - 1; k++) lap_put_cell(x + k, y + 3, '-', A(LAP_MAGENTA));
    lap_put_cell(x + bw - 1, y + 3, '\'', A(LAP_MAGENTA));
    put(x + 2, y + 4, 2, A(LAP_MAGENTA), "\\");
    put(x + 4, y + 4, bw - 4, A(LAP_GREEN | LAP_BRIGHT), "((o");
    put(x + 8, y + 4, bw - 8, LAP_ATTR(LAP_WHITE, LAP_BLACK), "MESHTASTIC reads these");
}

static void bar(int x, int y, int w, uint32_t done, uint32_t all, uint8_t hue)
{
    if (w < 4 || !all) return;
    const int lit = (int)((uint64_t)done * (uint64_t)(w - 2) / all);
    lap_put_cell(x, y, '[', LAP_ATTR(LAP_WHITE, LAP_BLACK));
    for (int k = 0; k < w - 2; k++)
        lap_put_cell(x + 1 + k, y, k < lit ? '=' : k == lit ? '>' : '.',
                     k < lit ? A(hue) : k == lit ? A(LAP_WHITE | LAP_BRIGHT) : LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK));
    lap_put_cell(x + w - 1, y, ']', LAP_ATTR(LAP_WHITE, LAP_BLACK));
}

static void row(int i, int y)
{
    const bool on = i == sel, busy = dl.st != DL_IDLE && dl.lang == i;
    const uint8_t base = on ? LAP_ATTR(LAP_BLACK, LAP_CYAN) : LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK);
    if (on) lap_fill(0, y, W, 1, ' ', base);
    lap_put_cell(1, y, on ? '>' : ' ', base);
    put(3, y, 12, base, "%s", NAME[i]);
    const uint32_t sz = on_card(i) ? card_size(i) : L[i].have[AT_PACK] ? L[i].have[AT_PACK] : L[i].size;
    if (sz) put(W - 21, y, 8, on ? base : LAP_ATTR(LAP_WHITE, LAP_BLACK), "%4u KB", kb(sz));
    else put(W - 21, y, 8, on ? base : LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "  -- KB");
    const char *st;
    uint8_t hue;
    if (busy) { st = "GETTING"; hue = LAP_YELLOW | LAP_BRIGHT; }
    else if (probing() && !on_card(i) && !L[i].have[AT_PACK]) { st = "..."; hue = LAP_BLACK | LAP_BRIGHT; }
    else if (stale(i)) { st = "UPDATE"; hue = LAP_YELLOW | LAP_BRIGHT; }
    else if (on_card(i)) { st = "ON CARD"; hue = LAP_GREEN | LAP_BRIGHT; }
    else if (L[i].have[AT_PACK]) { st = "PACK"; hue = LAP_CYAN | LAP_BRIGHT; }
    else { st = "GET"; hue = L[i].size ? (LAP_MAGENTA | LAP_BRIGHT) : (LAP_BLACK | LAP_BRIGHT); }
    put(W - 12, y, 11, on ? base : A(hue), "%11s", st);
    if (row_h < 2) return;
    /* the second line: where it is, or the download's bar */
    const uint8_t faint = LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK);
    if (busy) {
        if (dl.st == DL_SAVE) {
            const int n = put(3, y + 1, 14, A(LAP_CYAN | LAP_BRIGHT), "%s ", dl.step < 4 ? "to the card" : "checking");
            bar(3 + n, y + 1, W - 5 - n, dl.step < 2 ? 0 : dl.step == 3 ? L[i].size : dl.at, L[i].size, LAP_CYAN);
        } else {
            const int n = put(3, y + 1, 16, A(LAP_YELLOW | LAP_BRIGHT), "%u/%u KB ", dl.got / 1024u, kb(L[i].size));
            bar(3 + n, y + 1, W - 5 - n, dl.got, L[i].size, LAP_GREEN | LAP_BRIGHT);
        }
    } else if (L[i].have[AT_DL]) put(3, y + 1, W - 4, faint, "meshtastic/lang/%s.bin%s", CODE[i], L[i].have[AT_PACK] ? ", and a pack" : "");
    else if (L[i].have[AT_HAND]) put(3, y + 1, W - 4, faint, "meshtastic/lang-%s.bin, copied by hand", CODE[i]);
    else if (L[i].have[AT_PACK]) put(3, y + 1, W - 4, faint, "a pack from PACKS > LANGUAGES");
    else if (L[i].size) put(3, y + 1, W - 4, faint, "from terminalbay.com");
    else put(3, y + 1, W - 4, faint, cat_st == CAT_READ || cat_st == CAT_GO ? "reading the pack list" : "not on the card");
}

static void draw(void)
{
    W = lap_cols();
    H = lap_rows();
    /* the confirm's answer */
    if (dlg >= 0) {
        char t[4];
        int v = 0;
        const int r = lap_dialog_poll(dlg, t, sizeof(t), &v);
        if (r) {
            const int kind = dlg_kind, i = dlg_lang;
            dlg = -1;
            if (r == 1 && v == 1) {
                if (kind == ASK_GET) dl_go(i);
                else remove_now(i);
            }
        }
    }
    for (int i = 0; i < 4; i++)
        if (loose[i] >= 0 && lap_sd_poll(loose[i]) != LAP_SD_PENDING) loose[i] = -1;
    static uint32_t wifi_ms;
    if (lap_millis() - wifi_ms > 1000u || !wifi_ms) { wifi_ms = lap_millis() | 1u; wifi_look(); }
    if (cat_auto && wifi_up && cat_st == CAT_NONE) { cat_auto = false; cat_start(); }
    probe_pump();
    cat_pump();
    dl_pump();

    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    const int art_h = H >= 30 ? 6 : 0;
    if (art_h) art(0);
    int y = art_h;
    /* the network, and the pack list */
    if (net_denied) put(1, y, W - 2, A(LAP_RED | LAP_BRIGHT), "No network for this app: open it again, ALLOW");
    else if (!wifi_up) put(1, y, W - 2, A(LAP_RED | LAP_BRIGHT), "No Wi-Fi. Connect in SETTINGS, then RELOAD");
    else {
        const int n = put(1, y, W - 2, A(LAP_GREEN | LAP_BRIGHT), "Wi-Fi %.20s", ssid);
        int c = 0;
        for (int i = 0; i < NL; i++) c += (cat_found >> i) & 1;
        put(n + 3, y, W - n - 4, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%s",
            cat_st == CAT_OK ? (c == NL ? "pack list: all eight" : "pack list read") :
            cat_st == CAT_FAIL ? "pack list: not read" : cat_st == CAT_NONE ? "" : "reading the pack list");
    }
    y++;
    for (int k = 0; k < W; k++) lap_put_cell(k, y, '-', LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK));
    y++;
    /* the eight, two rows each where there is room */
    const int kr = lap_ui_keys_rows(3);
    const int room = H - kr - 1 - y;
    row_h = room >= NL * 2 ? 2 : 1;
    rows_shown = room / row_h;
    if (rows_shown > NL) rows_shown = NL;
    if (rows_shown < 1) rows_shown = 1;
    if (sel < top) top = sel;
    if (sel >= top + rows_shown) top = sel - rows_shown + 1;
    list_y = y;
    for (int k = 0; k < rows_shown && top + k < NL; k++) row(top + k, y + k * row_h);
    /* what happened, or what to do */
    const int my = H - kr - 1;
    if (msg[0] && lap_millis() - msg_ms < 8000u)
        put(1, my, W - 2, A(msg_bad ? LAP_YELLOW | LAP_BRIGHT : LAP_GREEN | LAP_BRIGHT), "%s", msg);
    else put(1, my, W - 2, LAP_ATTR(LAP_CYAN, LAP_BLACK), "Turn them on in MESHTASTIC > TRANSLATE > FROM");
    /* the keys */
    snprintf(kv0, sizeof(kv0), "%u KB", kb(L[sel].size));
    keys[0] = (lap_btn_t){ "DOWNLOAD", L[sel].size ? kv0 : NULL, 'd', false, !gettable(sel), false };
    keys[1] = (lap_btn_t){ "REMOVE", NULL, 'x', false, !removable(sel), false };
    keys[2] = (lap_btn_t){ "RELOAD", NULL, 'r', false, dl.st != DL_IDLE, false };
    lap_ui_keys(0, H - kr, W, kr, keys, 3, -1);
}

static bool key(lap_key_t k, char ch)
{
    switch (k) {
    case LAP_KEY_UP: case LAP_KEY_WHEEL_UP: if (sel > 0) sel--; return true;
    case LAP_KEY_DOWN: case LAP_KEY_WHEEL_DOWN: if (sel < NL - 1) sel++; return true;
    case LAP_KEY_ENTER: primary(sel); return true;
    case LAP_KEY_CHAR:
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (ch == 'd' || ch == 'x' || ch == 'r') { act(ch); return true; }
        return false;
    default: return false;
    }
}

static bool touch(int col, int row_)
{
    if (row_ >= H - lap_ui_keys_rows(3)) {
        const int b = lap_ui_key_hit(col, row_);
        if (b >= 0 && !keys[b].dim) act(keys[b].key);
        return true;
    }
    if (row_ >= list_y && row_ < list_y + rows_shown * row_h) {
        const int i = top + (row_ - list_y) / row_h;
        if (i >= 0 && i < NL) {
            if (i == sel) primary(i);                   /* a second tap acts */
            else sel = i;
        }
        return true;
    }
    return false;
}

LAP_APP(.id = "meshlang", .name = "MESH LANGUAGES", .sub = "translator languages", .version = "1.0",
        .author = "LakeShark", .color = LAP_MAGENTA,
        .icon_art = { " .------. ", "( Ola Hoi)", "( Privet )", " '-.----' ", "   \\ ((o ", "  pt..uk  " },
        .flags = LAP_CAP_NETWORK,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
