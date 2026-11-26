/* The shared signature table (ls_sigs.h).

   The built-in rows are the card file's own format (data/wardrive/sigs.txt
   holds the same rows with the sources written out; test_wd_sig keeps the
   two equal). wd_sig_load copies them, the extra layer and the card's text
   into one PSRAM buffer, splits it in place and compiles each pattern once,
   so a match is a walk over a few hundred small structs: no parsing, no
   allocation.

   Lived in the WARDRIVE module as wardrive/wd_sig.c until SWEEP started
   classifying with it; now firmware, with its own lock and allocator, and
   WARDRIVE's card directory reached through a weak reference. */
#include "ls_sigs.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#define SIG_DIR_DEFAULT "/sdcard/wardrive"
#else
#define SIG_DIR_DEFAULT ""
#define EXT_RAM_BSS_ATTR
#endif

/* WARDRIVE's card directory when its feed is linked into the same image (the
   simulator, a build without modules, the bench); NULL otherwise. */
#pragma weak wd_feed_dir
const char *wd_feed_dir(void);

static void *sig_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(1, n);
#endif
}

static void sig_free(void *p)
{
#ifdef ESP_PLATFORM
    if (p) heap_caps_free(p);
#else
    free(p);
#endif
}

#ifdef ESP_PLATFORM
static StaticSemaphore_t s_lock_mem;
static SemaphoreHandle_t s_mutex;
static unsigned s_once;
static void tlock(void)
{
    unsigned e = 0;
    if (__atomic_compare_exchange_n(&s_once, &e, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_lock_mem);
        __atomic_store_n(&s_once, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&s_once, __ATOMIC_ACQUIRE) != 2) vTaskDelay(1);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}
static void tunlock(void) { xSemaphoreGive(s_mutex); }
#else
static char s_flag;
static void tlock(void) { while (__atomic_test_and_set(&s_flag, __ATOMIC_ACQUIRE)) {} }
static void tunlock(void) { __atomic_clear(&s_flag, __ATOMIC_RELEASE); }
#endif

static const char BUILTIN[] =
    "flock-cam|ALPR|WIFI|2|FLOCK CAMERA|name:Flock-######,name:Flock,name:Flock Camera net.|flock-you, flock-you-esp32\n"
    "flock-oui|ALPR|WIFI+BLE|2|FLOCK SAFETY|oui:B41E52|IEEE via flock-you, ouispy\n"
    "flock-penguin|ALPR|WIFI+BLE|2|FLOCK PENGUIN BATTERY|name:Penguin-%%%%%%%%%%,co:09C8|flock-you, ESP32Marauder, BT SIG\n"
    "flock-battery|ALPR|WIFI+BLE|2|FLOCK EXT BATTERY|name:FS Ext Battery|flock-you, ESP32Marauder\n"
    "flock-pigvision|ALPR|WIFI|1|PIGVISION (FLOCK)|name:Pigvision*|ESP32Marauder, flock-you-esp32\n"
    "raven|SENSOR|BLE|2|RAVEN GUNSHOT SENSOR|uuid:3100,uuid:3200,uuid:3300,uuid:3400,uuid:3500|flock-you\n"
    "axon|BODYCAM|WIFI+BLE|2|AXON BODY CAM / TASER|oui:0025DF,co:034D,uuid:FC81,uuid:FE6B,uuid:FE6C|lookout.py, ouispy, BT SIG\n"
    "ipro-cam|BODYCAM|WIFI+BLE|1|I-PRO BODY CAM|oui:D42DC5|ouispy\n"
    "bodycam-name|BODYCAM|BLE|1|BODYCAM (BY NAME)|name:*bodycam*|LakeShark SWEEP\n"
    "cradlepoint|POLICE|WIFI|1|CRADLEPOINT ROUTER|oui:003044,oui:00E01C,name:IBR600-###,name:MBR1400-###|ouispy, Cradlepoint\n"
    "findmy|TRACKER|BLE|2|APPLE FIND MY|co:004C/12,uuid:FD44|LakeShark SWEEP+BUGHUNT, ESP32Marauder, BT SIG\n"
    "tile|TRACKER|BLE|2|TILE|co:067C,uuid:FEED,uuid:FEEC,uuid:FD84|LakeShark SWEEP, BT SIG\n"
    "smarttag|TRACKER|BLE|2|SAMSUNG SMARTTAG|uuid:FD5A,name:*SmartTag*|LakeShark BUGHUNT+SWEEP, BT SIG\n"
    "findhub|TRACKER|BLE|2|GOOGLE FIND HUB TAG|uuid:FEAA/40,uuid:FEAA/41|Google FMDN, LakeShark SWEEP\n"
    "dult|TRACKER|BLE|2|TRACKER (DULT)|uuid:FCB2|IETF DULT, LakeShark SWEEP\n"
    "chipolo|TRACKER|BLE|2|CHIPOLO|uuid:FE33|BT SIG, LakeShark BUGHUNT\n"
    "remote-id|DRONE|DRONE|1|REMOTE ID DRONE|any|ASTM F3411, LakeShark DRONES\n"
    "remote-id-ble|DRONE|BLE|1|REMOTE ID (BLE)|uuid:FFFA|ASTM F3411, BT SIG\n"
    "dji|DRONE|WIFI+BLE+DRONE|1|DJI|oui:0C9AE6,oui:8C5823,oui:04A85A,oui:58B858,oui:E47A2C,oui:60601F,oui:481CB9,oui:34D262,co:08AA|ouispy, BT SIG\n"
    "tello|DRONE|WIFI|1|DJI TELLO|name:TELLO-*,name:RMTT-*|Tello SDK\n"
    "parrot|DRONE|WIFI|1|PARROT|oui:00121C,oui:00267E,oui:9003B7,oui:903AE6,oui:A0143D|ouispy\n"
    "skydio|DRONE|WIFI|1|SKYDIO|oui:381D14|ouispy\n"
    "skimmer-hc|SKIMMER|BLE|1|BT SERIAL (SKIMMER?)|name:HC-05,name:HC-06,name:HC-03|Skimmer Scanner, ESP32Marauder\n"
    "skimmer-rn|SKIMMER|BLE|1|RN41/42 (SKIMMER?)|name:RNBT-????,oui:000666|USENIX Sec 19\n"
    "flipper|HACKTOOL|BLE|1|FLIPPER ZERO|name:*Flipper*,uuid:3081,uuid:3082,uuid:3083,oui:80E126,oui:80E127|flipper fw, ouispy, LakeShark SWEEP\n"
    "pineapple|HACKTOOL|WIFI|1|WIFI PINEAPPLE|oui:001337,name:Pineapple_####|ouispy, Hak5 docs\n"
    "deauther|HACKTOOL|WIFI|1|ESP8266 DEAUTHER|name:pwned|deauther\n"
    "pwnagotchi|HACKTOOL|WIFI|1|PWNAGOTCHI|mac:DEADBEEFDEAD,name:*pwnagotchi*|pwngrid, LakeShark SWEEP\n"
    "omg-cable|HACKTOOL|WIFI|1|O.MG CABLE|name:O.MG|O.MG\n"
    "meta-glasses|GLASSES|BLE|1|META / RAY-BAN GLASSES|co:01AB,co:058E,co:0D53,uuid:FD5F,name:*Ray-Ban*,name:*rayban*,name:*Wayfarer*,name:*Oakley Meta*,oui:7C2A9E,oui:CC660A,oui:F40343,oui:5CE91E,oui:985949,oui:80AA1C,oui:384712|nearby-glasses, ESP32Marauder, ouispy, BT SIG\n"
    "heycyan|GLASSES|BLE|1|HEYCYAN GLASSES|name:*HeyCyan*|nearby-glasses\n"
    "spectacles|GLASSES|BLE|1|SNAP SPECTACLES|co:03C2|nearby-glasses\n"
    "tesla-key|VEHICLE|BLE|1|TESLA|name:S################C,name:S################D,name:S################P,name:S################R|vehicle-command, trifinite\n"
    "obd-dongle|VEHICLE|WIFI|0|OBD-II DONGLE|name:OBDII,name:OBD2,name:WIFI_OBDII,name:*VLINK*|carscanner\n"
    "ring|CAMERA|WIFI|0|RING CAMERA|name:Ring-######,name:Ring Setup*,oui:187F88,oui:242BD6,oui:343EA4,oui:54E019,oui:5C475E,oui:649A63,oui:90486C,oui:9C7613,oui:AC9FC3,oui:C4DBAD,oui:CC3BFB|Ring, ouispy\n"
    "verkada|CAMERA|WIFI|1|VERKADA CAMERA|oui:E0A700|ouispy\n"
    "axis|CAMERA|WIFI|0|AXIS CAMERA|oui:00408C,oui:ACCC8E|LakeShark SWEEP\n";

enum { P_NAME, P_OUI, P_MAC, P_CO, P_UUID, P_ANY, P_U128 };

typedef struct {
    const char *glob;           /* P_NAME */
    uint16_t row, v;            /* v: company or UUID */
    int16_t b;                  /* the byte after it, -1 any */
    uint8_t type, addr[16];     /* addr: OUI (3), MAC (6) or a 128-bit UUID as written (16) */
} pat_t;

typedef struct {
    wd_sig_info_t i;
    uint16_t p0, np;
} row_t;

typedef struct {
    char *text;
    row_t *rows;
    pat_t *pats;
    int nrows, npats;
} table_t;

static table_t T;
static table_t T_old;           /* the table before T: freed at the next load, so wd_sig_at pointers outlive one load */
static EXT_RAM_BSS_ATTR char s_note[80];
static uint32_t s_hash;         /* the card file last loaded, 0 none */
static bool s_init;
static char *s_extra, *s_user;  /* the extra layer and the card text last loaded (PSRAM copies) */
static size_t s_extra_n, s_user_n;
static int s_extra_rows;
static EXT_RAM_BSS_ATTR char s_extra_from[24];
static EXT_RAM_BSS_ATTR char s_dir[96];

const char *wd_sig_builtin(void) { return BUILTIN; }
const char *wd_sig_note(void) { return s_note; }
/* Under the lock so a swap never pairs one table's count with another's rows;
   the pointer then outlives one more load (T_old). */
int wd_sig_count(void) { tlock(); const int n = T.nrows; tunlock(); return n; }
const wd_sig_info_t *wd_sig_at(int i)
{
    tlock();
    const wd_sig_info_t *r = i >= 0 && i < T.nrows ? &T.rows[i].i : NULL;
    tunlock();
    return r;
}

/* ------------------------------------------------------------ glob */

/* The length of the token at p if it takes c, else 0. */
static int token(const char *p, char c)
{
    const unsigned char u = (unsigned char)c;
    switch (*p) {
    case '?': return 1;
    case '#': return isxdigit(u) ? 1 : 0;
    case '%': return isdigit(u) ? 1 : 0;
    case '\\':
        if (p[1]) return tolower((unsigned char)p[1]) == tolower(u) ? 2 : 0;
        return tolower('\\') == tolower(u) ? 1 : 0;
    default: return *p && tolower((unsigned char)*p) == tolower(u) ? 1 : 0;
    }
}

bool wd_sig_glob(const char *p, const char *s)
{
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*p == '*') { star = ++p; mark = s; continue; }
        const int k = token(p, *s);
        if (k) { p += k; s++; continue; }
        if (star) { p = star; s = ++mark; continue; }
        return false;
    }
    while (*p == '*') p++;
    return !*p;
}

/* ------------------------------------------------------------ words */

uint16_t wd_sig_tag(const char *w, size_t n)
{
    static const struct { const char *w; uint16_t bit; } W[] = {
        { "CAMERA", WD_T_CAMERA }, { "ALPR", WD_T_ALPR }, { "BODYCAM", WD_T_BODYCAM }, { "POLICE", WD_T_POLICE },
        { "TRACKER", WD_T_TRACKER }, { "DRONE", WD_T_DRONE }, { "VEHICLE", WD_T_VEHICLE }, { "SKIMMER", WD_T_SKIMMER },
        { "HACKTOOL", WD_T_HACKTOOL }, { "GLASSES", WD_T_WEARABLE }, { "WEARABLE", WD_T_WEARABLE },
        { "SENSOR", WD_T_INFRA }, { "INFRA", WD_T_INFRA }, { "WATCH", WD_T_WATCH },
    };
    for (size_t i = 0; i < sizeof(W) / sizeof(W[0]); i++)
        if (strlen(W[i].w) == n && !strncmp(W[i].w, w, n)) return W[i].bit;
    return 0;
}

static int kind_word(const char *w, size_t n)
{
    static const struct { const char *w; int k; } W[] = {
        { "WIFI", WD_WIFI }, { "BLE", WD_BLE }, { "BT", WD_BLE }, { "CELL", WD_CELL }, { "DRONE", WD_DRONE },
        { "UAS", WD_DRONE }, { "RF", WD_RF }, { "SUBGHZ", WD_RF }, { "AIR", WD_AIR }, { "LORA", WD_LORA },
    };
    for (size_t i = 0; i < sizeof(W) / sizeof(W[0]); i++)
        if (strlen(W[i].w) == n && !strncmp(W[i].w, w, n)) return W[i].k;
    return -1;
}

/* "A+B+C" (or spaces) through f; false on an unknown word. */
static bool words(const char *s, uint16_t *out, bool kinds)
{
    *out = 0;
    if (kinds && !strcmp(s, "*")) { *out = (1u << WD_KINDS) - 1; return true; }
    while (*s) {
        while (*s == '+' || *s == ' ') s++;
        const char *e = s;
        while (*e && *e != '+' && *e != ' ') e++;
        if (e == s) break;
        if (kinds) {
            const int k = kind_word(s, (size_t)(e - s));
            if (k < 0) return false;
            *out |= (uint16_t)(1u << k);
        } else {
            const uint16_t t = wd_sig_tag(s, (size_t)(e - s));
            if (!t) return false;
            *out |= t;
        }
        s = e;
    }
    return *out != 0;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper((unsigned char)c);
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* n hex bytes from s (':' and '-' between them allowed); the end, or NULL. */
static const char *hexbytes(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        if (i && (*s == ':' || *s == '-')) s++;
        const int a = hexv(s[0]), b = a >= 0 ? hexv(s[1]) : -1;
        if (b < 0) return NULL;
        out[i] = (uint8_t)(a << 4 | b);
        s += 2;
    }
    return s;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

/* ------------------------------------------------------------ loading */

static bool pattern(char *s, pat_t *p)
{
    memset(p, 0, sizeof(*p));
    p->b = -1;
    if (!strcmp(s, "any")) { p->type = P_ANY; return true; }
    char *v = strchr(s, ':');
    if (!v) return false;
    *v++ = 0;
    if (!strcmp(s, "name")) { p->type = P_NAME; p->glob = v; return *v != 0; }
    if (!strcmp(s, "oui")) { p->type = P_OUI; const char *e = hexbytes(v, p->addr, 3); return e && !*e; }
    if (!strcmp(s, "mac")) { p->type = P_MAC; const char *e = hexbytes(v, p->addr, 6); return e && !*e; }
    if (!strcmp(s, "uuid")) {
        /* a 128-bit UUID, 12345678-1234-5678-9abc-123456789abc */
        const char *e = hexbytes(v, p->addr, 16);
        if (e && !*e) { p->type = P_U128; return true; }
        memset(p->addr, 0, sizeof(p->addr));
    }
    if (!strcmp(s, "co") || !strcmp(s, "uuid")) {
        p->type = s[0] == 'c' ? P_CO : P_UUID;
        uint8_t w[2];
        const char *e = hexbytes(v, w, 2);
        if (!e) return false;
        p->v = (uint16_t)(w[0] << 8 | w[1]);
        if (*e == '/') {
            uint8_t b;
            e = hexbytes(e + 1, &b, 1);
            if (!e) return false;
            p->b = b;
        }
        return !*e;
    }
    return false;
}

static int find_id(const table_t *t, const char *id)
{
    for (int i = 0; i < t->nrows; i++) if (!strcmp(t->rows[i].i.id, id)) return i;
    return -1;
}

static void say(const char *fmt, int line, const char *what)
{
    if (s_note[0]) return;      /* the first bad line is the one to fix */
    snprintf(s_note, sizeof(s_note), fmt, line, what);
}

/* One line, split in place. false: a bad line (noted when `user`). */
static bool line_in(table_t *t, char *ln, int no, bool user, int cap_rows, int cap_pats)
{
    ln = trim(ln);
    if (!*ln || *ln == '#') return true;
    char *f[7];
    int n = 0;
    f[n++] = ln;
    for (char *c = ln; *c && n < 7; c++) if (*c == '|') { *c = 0; f[n++] = c + 1; }
    for (int i = 0; i < n; i++) f[i] = trim(f[i]);
    const char *id = f[0];
    if (!*id) { if (user) say("sigs.txt line %d: %s", no, "no id"); return false; }
    const int old = find_id(t, id);
    if (n == 2 && !strcmp(f[1], "off")) {
        if (old >= 0) t->rows[old].i.kinds = 0;
        return true;
    }
    if (n != 7) { if (user) say("sigs.txt line %d: %s", no, "needs 7 fields split by |"); return false; }
    row_t r;
    memset(&r, 0, sizeof(r));
    r.i.id = id;
    uint16_t k;
    if (!words(f[1], &r.i.tags, false)) { if (user) say("sigs.txt line %d: unknown tag in %s", no, f[1]); return false; }
    if (!words(f[2], &k, true)) { if (user) say("sigs.txt line %d: unknown kind in %s", no, f[2]); return false; }
    r.i.kinds = (uint8_t)k;
    if (f[3][0] < '0' || f[3][0] > '2' || f[3][1]) { if (user) say("sigs.txt line %d: %s", no, "alert is 0, 1 or 2"); return false; }
    r.i.alert = (uint8_t)(f[3][0] - '0');
    r.i.label = f[4];
    r.i.source = f[6];
    const int row = old >= 0 ? old : t->nrows;
    if (row >= cap_rows) return false;
    r.p0 = (uint16_t)t->npats;
    /* the patterns, split at commas that are not escaped */
    char *s = f[5];
    while (*s) {
        char *e = s;
        while (*e && *e != ',') { if (*e == '\\' && e[1]) e++; e++; }
        const bool last = !*e;
        *e = 0;
        char *ps = trim(s);
        if (*ps) {
            if (t->npats >= cap_pats) return false;
            pat_t *p = &t->pats[t->npats];
            if (!pattern(ps, p)) {
                if (user) say("sigs.txt line %d: bad pattern %s", no, ps);
                t->npats = r.p0;
                return false;
            }
            p->row = (uint16_t)row;
            t->npats++;
        }
        if (last) break;
        s = e + 1;
    }
    r.np = (uint16_t)(t->npats - r.p0);
    if (!r.np) { if (user) say("sigs.txt line %d: %s", no, "no patterns"); return false; }
    t->rows[row] = r;
    if (row == t->nrows) t->nrows++;
    return true;
}

static void table_free(table_t *t)
{
    sig_free(t->text);
    sig_free(t->rows);
    sig_free(t->pats);
    memset(t, 0, sizeof(*t));
}

static bool not_blank(const char *c)
{
    while (*c == ' ' || *c == '\t') c++;
    return *c && *c != '#' && *c != '\r';
}

/* Build builtin + extra + user and swap it in. Under the lock: a load and a
   match never overlap, and two loads never interleave. */
static int build(const char *extra, size_t elen, const char *user, size_t len)
{
    if (!extra) elen = 0;
    if (!user) len = 0;
    const size_t nb = sizeof(BUILTIN) - 1, total = nb + 1 + elen + 1 + len + 1;
    table_t t;
    memset(&t, 0, sizeof(t));
    t.text = sig_alloc(total);
    if (!t.text) { snprintf(s_note, sizeof(s_note), "no memory for the signatures"); return -1; }
    char *part[3];
    part[0] = t.text;
    memcpy(part[0], BUILTIN, nb);
    part[1] = part[0] + nb + 1;
    if (elen) memcpy(part[1], extra, elen);
    part[2] = part[1] + elen + 1;
    if (len) memcpy(part[2], user, len);
    int lines = 3, commas = 0;
    for (size_t i = 0; i < total; i++) {
        if (t.text[i] == '\n') lines++;
        else if (t.text[i] == ',') commas++;
    }
    const int cap_rows = lines, cap_pats = lines + commas;
    t.rows = sig_alloc(sizeof(row_t) * (size_t)cap_rows);
    t.pats = sig_alloc(sizeof(pat_t) * (size_t)cap_pats);
    if (!t.rows || !t.pats) { table_free(&t); snprintf(s_note, sizeof(s_note), "no memory for the signatures"); return -1; }
    s_note[0] = 0;
    int user_rows = 0, extra_rows = 0;
    for (int k = 0; k < 3; k++) {
        char *p = part[k];
        int no = 0;
        while (*p) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            no++;
            const bool counts = not_blank(p);
            if (line_in(&t, p, no, k == 2, cap_rows, cap_pats) && counts) {
                if (k == 1) extra_rows++;
                if (k == 2) user_rows++;
            }
            if (!e) break;
            p = e + 1;
        }
    }
    char note[80];
    if (s_note[0]) snprintf(note, sizeof(note), "%s", s_note);
    else {
        int n = snprintf(note, sizeof(note), "%d signatures", t.nrows);
        if (user_rows && n > 0 && n < (int)sizeof(note))
            n += snprintf(note + n, sizeof(note) - (size_t)n, ", %d from the card", user_rows);
        if (extra_rows && n > 0 && n < (int)sizeof(note))
            snprintf(note + n, sizeof(note) - (size_t)n, ", %d from %s", extra_rows, s_extra_from[0] ? s_extra_from : "apps");
    }
    table_free(&T_old);
    T_old = T;
    T = t;
    s_extra_rows = extra_rows;
    snprintf(s_note, sizeof(s_note), "%s", note);
    return T.nrows;
}

/* A PSRAM copy of n bytes (NULL for none). */
static bool keep(char **dst, size_t *dn, const char *src, size_t n)
{
    if (src == *dst && n == *dn) return true;
    char *c = NULL;
    if (src && n) {
        c = sig_alloc(n + 1);
        if (!c) return false;
        memcpy(c, src, n);
    }
    sig_free(*dst);
    *dst = c;
    *dn = c ? n : 0;
    return true;
}

int wd_sig_load(const char *user, size_t len)
{
    if (!user) len = 0;
    tlock();
    int r = build(s_extra, s_extra_n, user, len);
    if (r >= 0 && !keep(&s_user, &s_user_n, user, len)) { s_user_n = 0; sig_free(s_user); s_user = NULL; }
    tunlock();
    return r;
}

int wd_sig_set_extra(const char *text, size_t len, const char *from)
{
    if (!text) len = 0;
    tlock();
    snprintf(s_extra_from, sizeof(s_extra_from), "%s", from ? from : "");
    int r = -1;
    if (keep(&s_extra, &s_extra_n, text, len)) r = build(s_extra, s_extra_n, s_user, s_user_n);
    else snprintf(s_note, sizeof(s_note), "no memory for the signatures");
    tunlock();
    return r;
}

int wd_sig_extra_rows(void) { return s_extra_rows; }

void wd_sig_ensure(void)
{
    tlock();
    if (!T.rows) build(s_extra, s_extra_n, s_user, s_user_n);
    tunlock();
}

void wd_sig_set_dir(const char *dir)
{
    tlock();
    snprintf(s_dir, sizeof(s_dir), "%s", dir ? dir : "");
    tunlock();
}

static uint32_t fnv(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h | 1;
}

void wd_sig_init(void)
{
    char *buf = NULL;
    size_t n = 0;
    char dir[sizeof(s_dir)];
    tlock();
    snprintf(dir, sizeof(dir), "%s", s_dir);
    tunlock();
    const char *d = dir[0] ? dir : (wd_feed_dir ? wd_feed_dir() : SIG_DIR_DEFAULT);
    if (d && d[0]) {
        char path[160];
        snprintf(path, sizeof(path), "%s/sigs.txt", d);
        FILE *f = fopen(path, "rb");
        if (f) {
            enum { MAX = 32 * 1024 };
            buf = sig_alloc(MAX + 1);
            if (buf) n = fread(buf, 1, MAX, f);
            fclose(f);
        }
    }
    const uint32_t h = n ? fnv(buf, n) : 0;
    if (!s_init || h != s_hash) {
        if (wd_sig_load(buf, n) >= 0) { s_init = true; s_hash = h; }
    }
    sig_free(buf);
}

/* ------------------------------------------------------------ matching */

static bool hit(const pat_t *p, const wd_obs_t *o, const wd_sig_ble_t *b)
{
    switch (p->type) {
    case P_ANY: return true;
    case P_NAME: return o->name[0] && wd_sig_glob(p->glob, o->name);
    case P_OUI: return !(b && b->no_oui) && !memcmp(o->addr, p->addr, 3);
    case P_MAC: return !memcmp(o->addr, p->addr, 6);
    case P_CO: return b && b->company && b->company == p->v && (p->b < 0 || b->mfg0 == p->b);
    case P_UUID: return b && b->uuid16 && b->uuid16 == p->v && (p->b < 0 || b->svc0 == p->b);
    case P_U128:
        if (!b || !b->uuid128) return false;
        for (int u = 0; u < b->n128; u++) {
            const uint8_t *q = b->uuid128 + 16 * u;
            int k = 0;
            while (k < 16 && q[k] == p->addr[15 - k]) k++;
            if (k == 16) return true;
        }
        return false;
    default: return false;
    }
}

uint16_t wd_sig_match_ble(const wd_obs_t *o, const wd_sig_ble_t *b, uint16_t *tags)
{
    if (!o || o->kind >= WD_KINDS) return 0;
    const uint8_t km = (uint8_t)(1u << o->kind);
    int best = -1;
    tlock();
    for (int r = 0; r < T.nrows; r++) {
        const row_t *w = &T.rows[r];
        if (!(w->i.kinds & km)) continue;
        for (int k = 0; k < w->np; k++) {
            if (!hit(&T.pats[w->p0 + k], o, b)) continue;
            if (tags) *tags |= w->i.tags;
            if (best < 0 || w->i.alert > T.rows[best].i.alert) best = r;
            break;
        }
    }
    tunlock();
    return (uint16_t)(best + 1);
}

uint16_t wd_sig_match(const wd_obs_t *o, uint16_t *tags) { return wd_sig_match_ble(o, NULL, tags); }
