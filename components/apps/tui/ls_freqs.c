/* See ls_freqs.h. The model and the card; nothing in here moves a radio. */
#include "ls_freqs.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ls_textpack.h"
#include "scan_import.h"
#include "tuilib/tui_core.h"

#ifdef _WIN32                   /* the bench on Windows */
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0775)
#endif

/* ---------------------------------------------------------------- modes -- */

static const char *const MODES[LS_FM__COUNT] = {
    "NFM", "WFM", "AM", "USB", "LSB", "CW", "P25", "DMR", "NXDN", "TETRA", "DSTAR",
    "YSF", "FSK", "OOK", "LORA", "ADSB", "AIS", "APRS", "POCSAG", "ACARS", "VDL2",
    "APT", "LRPT", "GPS", "IRIDIUM", "LTE",
};

const char *ls_freq_mode_name(int mode)
{
    return mode >= 0 && mode < LS_FM__COUNT ? MODES[mode] : "?";
}

int ls_freq_mode_parse(const char *text)
{
    /* What CHIRP, gqrx, SDR++ and people typing call the same things. */
    static const struct { const char *alias; uint8_t mode; } ALIAS[] = {
        { "FM", LS_FM_NFM }, { "NBFM", LS_FM_NFM }, { "NARROW FM", LS_FM_NFM },
        { "WBFM", LS_FM_WFM }, { "NAM", LS_FM_AM }, { "AM-SYNC", LS_FM_AM },
        { "SAM", LS_FM_AM }, { "DSB", LS_FM_AM }, { "CW-L", LS_FM_CW }, { "CW-U", LS_FM_CW },
        { "NCW", LS_FM_CW }, { "CWR", LS_FM_CW }, { "DV", LS_FM_DSTAR }, { "D-STAR", LS_FM_DSTAR },
        { "DN", LS_FM_YSF }, { "C4FM", LS_FM_YSF }, { "ADS-B", LS_FM_ADSB },
        { "RTTY", LS_FM_FSK }, { "GFSK", LS_FM_FSK }, { "ASK", LS_FM_OOK }, { "VDL", LS_FM_VDL2 },
    };
    if (!text) return -1;
    while (*text == ' ') text++;
    if (!*text) return -1;
    for (int i = 0; i < LS_FM__COUNT; i++) if (!strcasecmp(text, MODES[i])) return i;
    for (unsigned i = 0; i < sizeof(ALIAS) / sizeof(ALIAS[0]); i++)
        if (!strcasecmp(text, ALIAS[i].alias)) return ALIAS[i].mode;
    /* gqrx: "WFM (stereo)", "Narrow FM". */
    if (!strncasecmp(text, "WFM", 3)) return LS_FM_WFM;
    if (strstr(text, "Narrow")) return LS_FM_NFM;
    return -1;
}

void ls_freq_mhz_text(uint32_t hz, char *out, size_t n)
{
    char d[8];
    snprintf(d, sizeof(d), "%06lu", (unsigned long)(hz % 1000000u));
    int k = 6;
    while (k > 3 && d[k - 1] == '0') k--;
    d[k] = 0;
    snprintf(out, n, "%lu.%s", (unsigned long)(hz / 1000000u), d);
}

/* A guess for a line that names no mode: what that part of the dial is. */
static int guess_mode(uint32_t hz)
{
    if (hz < 30000000u || (hz >= 108000000u && hz < 137000000u)) return LS_FM_AM;
    if (hz >= 87500000u && hz < 108000000u) return LS_FM_WFM;
    return LS_FM_NFM;
}

/* ------------------------------------------------------------ CSV lines -- */

const char LS_FREQ_CSV_HEADER[] = "freq_mhz,mode,bw_khz,name,group,tags,lat,lon,radius_km,tone,notes";
static const char *const COLS[11] = {
    "freq_mhz", "mode", "bw_khz", "name", "group", "tags", "lat", "lon", "radius_km", "tone", "notes",
};

/* Cut one record at `sep` outside quotes, in place: "" inside quotes is a
   quote, blanks around an unquoted field are dropped. */
static int split(char *s, char sep, char **f, int max)
{
    int n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        char *start = s, *w = s;
        if (*s == '"') {
            for (s++; *s; ) {
                if (*s == '"') { if (s[1] != '"') { s++; break; } s++; }
                *w++ = *s++;
            }
            while (*s && *s != sep) s++;
        } else {
            while (*s && *s != sep) *w++ = *s++;
            while (w > start && (w[-1] == ' ' || w[-1] == '\t')) w--;
        }
        char *next = *s ? s + 1 : NULL;
        *w = 0;
        if (n < max) f[n++] = start;
        if (!next) return n;
        s = next;
    }
}

static bool num(const char *s, double *v)
{
    char *end;
    if (!s || !*s) return false;
    *v = strtod(s, &end);
    while (*end == ' ') end++;
    return end != s && !*end && isfinite(*v);
}

/* Text for the cells: accents folded to their letters (the card is written
   by people in Spanish; the cells draw ASCII), no control characters. */
static void put(char *dst, size_t n, char *src)
{
    if (*src) tui_utf8_fold_str(src);
    snprintf(dst, n, "%s", src);
    for (char *p = dst; *p; p++) if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
}

static void upper(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }

static void blank_entry(ls_freq_t *e)
{
    memset(e, 0, sizeof(*e));
    e->lat_e5 = e->lon_e5 = LS_FREQ_NOWHERE;
}

static bool set_freq(ls_freq_t *e, double mhz)
{
    if (!(mhz >= 0.009 && mhz <= 6000.0)) return false;
    e->hz = (uint32_t)llround(mhz * 1e6);
    return true;
}

static bool set_place(ls_freq_t *e, const char *lat, const char *lon, const char *radius)
{
    double a, b, r = 50;
    if (!*lat && !*lon) return true;
    if (!num(lat, &a) || !num(lon, &b) || fabs(a) > 90 || fabs(b) > 180) return false;
    if (radius && *radius && (!num(radius, &r) || r < 0 || r > 20000)) return false;
    e->lat_e5 = (int32_t)llround(a * 1e5);
    e->lon_e5 = (int32_t)llround(b * 1e5);
    e->radius_km = (uint16_t)(r < 1 ? 1 : r);
    return true;
}

static void finish(ls_freq_t *e)
{
    if (!e->name[0]) ls_freq_mhz_text(e->hz, e->name, sizeof(e->name));
    upper(e->group);
}

void ls_freq_csv_begin(ls_freq_csv_t *c)
{
    for (int i = 0; i < 11; i++) c->col[i] = (int8_t)i;
    c->seen = false;
}

static void strip(char *line)
{
    line[strcspn(line, "\r\n")] = 0;
}

int ls_freq_csv_line(ls_freq_csv_t *c, char *line, ls_freq_t *out)
{
    strip(line);
    if (!strncmp(line, "\xef\xbb\xbf", 3)) line += 3;   /* a BOM from a spreadsheet */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return 0;
    char *f[16];
    const int n = split(line, ',', f, 16);
    if (!c->seen) {
        c->seen = true;
        double v;
        if (!num(f[0], &v)) {
            /* A header: the columns it names, wherever they are. */
            for (int k = 0; k < 11; k++) {
                c->col[k] = -1;
                for (int i = 0; i < n; i++) if (!strcasecmp(f[i], COLS[k])) c->col[k] = (int8_t)i;
            }
            if (c->col[0] >= 0) return 0;
            /* Not a header after all: garbage, and the columns as written. */
            ls_freq_csv_begin(c);
            c->seen = true;
            return -1;
        }
    }
    char *g[11];
    for (int k = 0; k < 11; k++) g[k] = c->col[k] >= 0 && c->col[k] < n ? f[c->col[k]] : (char *)"";
    ls_freq_t e;
    blank_entry(&e);
    double v;
    if (!num(g[0], &v) || !set_freq(&e, v)) return -1;
    const int mode = *g[1] ? ls_freq_mode_parse(g[1]) : guess_mode(e.hz);
    if (mode < 0) return -1;
    e.mode = (uint8_t)mode;
    if (*g[2]) {
        if (!num(g[2], &v) || v < 0 || v > 6500) return -1;
        e.bw = (uint16_t)lround(v * 10);
    }
    if (!set_place(&e, g[6], g[7], g[8])) return -1;
    put(e.name, sizeof(e.name), g[3]);
    put(e.group, sizeof(e.group), g[4]);
    put(e.tags, sizeof(e.tags), g[5]);
    put(e.tone, sizeof(e.tone), g[9]);
    put(e.notes, sizeof(e.notes), g[10]);
    finish(&e);
    *out = e;
    return 1;
}

static size_t field(char *out, size_t n, size_t k, const char *s, bool comma)
{
    if (comma && k + 1 < n) out[k++] = ',';
    const bool quote = strpbrk(s, ",\"") || (*s && (s[0] == ' ' || s[strlen(s) - 1] == ' '));
    if (quote && k + 1 < n) out[k++] = '"';
    for (; *s && k + 2 < n; s++) {
        if (*s == '"') out[k++] = '"';
        out[k++] = *s;
    }
    if (quote && k + 1 < n) out[k++] = '"';
    out[k < n ? k : n - 1] = 0;
    return k;
}

int ls_freq_csv_format(const ls_freq_t *e, char *out, size_t n)
{
    return ls_freq_csv_format_as(NULL, e, out, n);
}

/* -34.60370 from -3460370: the five decimals the format keeps, exactly. */
static void degrees(char *out, size_t n, int32_t e5)
{
    const uint32_t a = (uint32_t)(e5 < 0 ? -(int64_t)e5 : e5);
    snprintf(out, n, "%s%lu.%05lu", e5 < 0 ? "-" : "", (unsigned long)(a / 100000u), (unsigned long)(a % 100000u));
}

/* The eleven fields of an entry as text. */
typedef struct { char mhz[16], bw[12], lat[14], lon[14], rad[8]; const char *v[11]; } texts_t;
static void texts(const ls_freq_t *e, texts_t *t)
{
    memset(t, 0, sizeof(*t));
    ls_freq_mhz_text(e->hz, t->mhz, sizeof(t->mhz));
    if (e->bw % 10) snprintf(t->bw, sizeof(t->bw), "%u.%u", (unsigned)e->bw / 10, (unsigned)e->bw % 10);
    else if (e->bw) snprintf(t->bw, sizeof(t->bw), "%u", (unsigned)e->bw / 10);
    if (e->lat_e5 != LS_FREQ_NOWHERE) {
        degrees(t->lat, sizeof(t->lat), e->lat_e5);
        degrees(t->lon, sizeof(t->lon), e->lon_e5);
        snprintf(t->rad, sizeof(t->rad), "%u", (unsigned)e->radius_km);
    }
    const char *v[11] = { t->mhz, ls_freq_mode_name(e->mode), t->bw, e->name, e->group, e->tags,
                          t->lat, t->lon, t->rad, e->tone, e->notes };
    memcpy(t->v, v, sizeof(v));
}

/* One line in a file's columns: the eleven where its header put them, and
   `orig` (the line being replaced, if any) in every column it has that is
   not one of them, so an extra column somebody keeps survives an edit. */
static int join(const ls_freq_csv_t *c, const char *const v[11], char *const *orig, int norig,
                char *out, size_t n)
{
    if (!out || n < 16) return 0;
    int8_t at[16];
    int cols = 11;
    for (int i = 0; i < 16; i++) at[i] = (int8_t)(i < 11 ? i : -1);
    if (c && c->seen) {
        cols = 0;
        memset(at, -1, sizeof(at));
        for (int k = 0; k < 11; k++)
            if (c->col[k] >= 0 && c->col[k] < 16) {
                at[c->col[k]] = (int8_t)k;
                if (c->col[k] >= cols) cols = c->col[k] + 1;
            }
    }
    if (norig > cols) cols = norig > 16 ? 16 : norig;
    size_t k = 0;
    for (int i = 0; i < cols; i++)
        k = field(out, n, k, at[i] >= 0 ? v[at[i]] : orig && i < norig ? orig[i] : "", i > 0);
    return k + 2 < n ? (int)k : 0;
}

int ls_freq_csv_format_as(const ls_freq_csv_t *c, const ls_freq_t *e, char *out, size_t n)
{
    texts_t t;
    texts(e, &t);
    return join(c, t.v, NULL, 0, out, n);
}

/* ------------------------------------------------------- other formats -- */

ls_freq_kind_t ls_freq_kind_of(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\xef' || *s == '\xbb' || *s == '\xbf') s++;
    if (!strncmp(s, "LSCAN1", 6)) return LS_FI_LSCAN;
    if (*s == '{' || *s == '[') return LS_FI_SDRPP;
    if (strstr(s, "Location") && strstr(s, "Frequency")) return LS_FI_CHIRP;
    if (strchr(s, ';')) return LS_FI_GQRX;
    return LS_FI_CSV;
}

const char *ls_freq_kind_name(ls_freq_kind_t k)
{
    static const char *const N[] = { "CSV", "LSCAN", "CHIRP", "GQRX", "SDR++" };
    return k <= LS_FI_SDRPP ? N[k] : "?";
}

/* CHIRP's columns, by name: the order moved between CHIRP versions. */
enum { CH_NAME, CH_FREQ, CH_DUPLEX, CH_OFFSET, CH_TONE, CH_RTONE, CH_CTONE, CH_DTCS,
       CH_POL, CH_MODE, CH_COMMENT, CH__N };
static const char *const CHIRP_COLS[CH__N] = {
    "Name", "Frequency", "Duplex", "Offset", "Tone", "rToneFreq", "cToneFreq", "DtcsCode",
    "DtcsPolarity", "Mode", "Comment",
};

typedef struct {
    ls_freq_kind_t kind;
    bool decided;
    ls_freq_csv_t csv;
    int8_t chirp[CH__N];
    const char *group;
} reader_t;

static int chirp_line(reader_t *r, char *line, ls_freq_t *e)
{
    char *f[24];
    const int n = split(line, ',', f, 24);
    if (r->chirp[CH_FREQ] < 0) {
        for (int k = 0; k < CH__N; k++) {
            r->chirp[k] = -1;
            for (int i = 0; i < n; i++) if (!strcmp(f[i], CHIRP_COLS[k])) r->chirp[k] = (int8_t)i;
        }
        return r->chirp[CH_FREQ] >= 0 ? 0 : -1;
    }
    char *g[CH__N];
    for (int k = 0; k < CH__N; k++) g[k] = r->chirp[k] >= 0 && r->chirp[k] < n ? f[r->chirp[k]] : (char *)"";
    double v;
    if (!num(g[CH_FREQ], &v) || !set_freq(e, v)) return -1;
    const int mode = *g[CH_MODE] ? ls_freq_mode_parse(g[CH_MODE]) : LS_FM_NFM;
    if (mode < 0) return -1;
    e->mode = (uint8_t)mode;
    put(e->name, sizeof(e->name), g[CH_NAME]);
    /* The tone a receiver needs to open: TSQL's, else the repeater's. */
    if (!strcmp(g[CH_TONE], "DTCS"))
        snprintf(e->tone, sizeof(e->tone), "D%s%s", g[CH_DTCS], g[CH_POL][0] == 'R' ? "I" : "N");
    else if (!strcmp(g[CH_TONE], "TSQL") || !strcmp(g[CH_TONE], "Tone"))
        snprintf(e->tone, sizeof(e->tone), "%s", !strcmp(g[CH_TONE], "TSQL") ? g[CH_CTONE] : g[CH_RTONE]);
    /* The repeater's shift, which a receive list does not use but a person
       reading it wants. */
    char note[LS_FREQ_NOTES] = "";
    double off;
    if ((*g[CH_DUPLEX] == '+' || *g[CH_DUPLEX] == '-') && num(g[CH_OFFSET], &off) && off > 0)
        snprintf(note, sizeof(note), "shift %c%g MHz", *g[CH_DUPLEX], off);
    if (*g[CH_COMMENT]) {
        size_t k = strlen(note);
        snprintf(note + k, sizeof(note) - k, "%s%s", k ? " " : "", g[CH_COMMENT]);
    }
    put(e->notes, sizeof(e->notes), note);
    snprintf(e->group, sizeof(e->group), "%s", r->group);
    return 1;
}

/* gqrx bookmarks.csv: "Frequency; Name; Modulation; Bandwidth; Tags", the
   tag table above it two fields wide. */
static int gqrx_line(reader_t *r, char *line, ls_freq_t *e)
{
    if (*line == '#') return 0;
    char *f[6];
    const int n = split(line, ';', f, 6);
    double v, bw;
    if (n < 3) return 0;
    if (!num(f[0], &v) || !set_freq(e, v / 1e6)) return -1;
    const int mode = ls_freq_mode_parse(f[2]);
    e->mode = (uint8_t)(mode >= 0 ? mode : guess_mode(e->hz));
    if (n > 3 && num(f[3], &bw) && bw > 0 && bw < 650000) e->bw = (uint16_t)lround(bw / 100);
    put(e->name, sizeof(e->name), f[1]);
    if (n > 4 && *f[4]) {
        put(e->tags, sizeof(e->tags), f[4]);
        char *comma = strchr(f[4], ',');
        if (comma) *comma = 0;
        if (strcasecmp(f[4], "Untagged")) { put(e->group, sizeof(e->group), f[4]); return 1; }
    }
    snprintf(e->group, sizeof(e->group), "%s", r->group);
    return 1;
}

static int lscan_line(reader_t *r, char *line, ls_freq_t *e)
{
    strip(line);
    if (!*line || *line == '#' || !strncmp(line, "LSCAN1", 6)) return 0;
    scan_channel_t ch;
    if (!scan_import_line(line, &ch)) return -1;
    e->hz = ch.freq_hz;
    e->mode = ch.mode == SCAN_MODE_P25 ? LS_FM_P25 : LS_FM_NFM;
    snprintf(e->name, sizeof(e->name), "%s", ch.name);
    if (ch.radius_m) {
        e->lat_e5 = (int32_t)(ch.lat_e7 / 100);
        e->lon_e5 = (int32_t)(ch.lon_e7 / 100);
        e->radius_km = (uint16_t)((ch.radius_m + 999) / 1000);
    }
    snprintf(e->tags, sizeof(e->tags), "zone%u%s", ch.zone, ch.flags & SCAN_FLAG_PRIORITY ? " priority" : "");
    snprintf(e->group, sizeof(e->group), "%s", r->group);
    return 1;
}

/* SDR++'s frequency manager: lists of named bookmarks, each an object with
   frequency, bandwidth and mode (0 NFM, 1 WFM, 2 AM, 3 DSB, 4 USB, 5 CW,
   6 LSB, 7 RAW). Read without a JSON library: whichever object holds a
   "frequency" is a bookmark, named by its key; its list is two keys up. */
static void sdrpp(const char *p, const char *end, const char *group, ls_freq_sink_t sink,
                  void *ctx, ls_freq_tally_t *t)
{
    static const uint8_t SDRPP_MODE[8] = { LS_FM_NFM, LS_FM_WFM, LS_FM_AM, LS_FM_AM,
                                           LS_FM_USB, LS_FM_CW, LS_FM_LSB, LS_FM_NFM };
    enum { DEPTH = 8, KEY = 32 };
    char key[DEPTH][KEY], pending[KEY] = "";
    double fr[DEPTH] = { 0 }, bw[DEPTH] = { 0 }, md[DEPTH] = { 0 };
    int depth = 0;
    while (p < end) {
        const char c = *p++;
        if (c == '"') {
            char s[KEY]; int k = 0;
            for (; p < end && *p != '"'; p++) {
                if (*p == '\\' && p + 1 < end) p++;
                if (k < KEY - 1) s[k++] = *p;
            }
            s[k] = 0; p++;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p < end && *p == ':') { snprintf(pending, sizeof(pending), "%s", s); p++; }
            else pending[0] = 0;
        } else if (c == '{' || c == '[') {
            if (depth < DEPTH) { snprintf(key[depth], KEY, "%s", c == '{' ? pending : ""); fr[depth] = 0; bw[depth] = 0; md[depth] = 0; }
            depth++;
            pending[0] = 0;
        } else if (c == '}' || c == ']') {
            depth--;
            if (depth >= 0 && depth < DEPTH && fr[depth] > 0) {
                ls_freq_t e;
                blank_entry(&e);
                if (!set_freq(&e, fr[depth] / 1e6)) { t->skipped++; if (!t->first_bad) t->first_bad = 1; }
                else {
                    const int m = (int)md[depth];
                    e.mode = m >= 0 && m < 8 ? SDRPP_MODE[m] : LS_FM_NFM;
                    if (bw[depth] > 0 && bw[depth] < 650000) e.bw = (uint16_t)lround(bw[depth] / 100);
                    put(e.name, sizeof(e.name), key[depth]);
                    if (depth >= 2 && !strcmp(key[depth - 1], "bookmarks")) put(e.group, sizeof(e.group), key[depth - 2]);
                    else snprintf(e.group, sizeof(e.group), "%s", group);
                    finish(&e);
                    t->loaded++;
                    if (!sink(&e, ctx)) return;
                }
            }
            if (depth < 0) depth = 0;
            pending[0] = 0;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            char *stop;
            const double v = strtod(p - 1, &stop);
            p = stop;
            const int d = depth - 1;
            if (d >= 0 && d < DEPTH) {
                if (!strcmp(pending, "frequency")) fr[d] = v;
                else if (!strcmp(pending, "bandwidth")) bw[d] = v;
                else if (!strcmp(pending, "mode")) md[d] = v;
            }
            pending[0] = 0;
        }
    }
}

bool ls_freq_read(FILE *f, const char *group, ls_freq_sink_t sink, void *ctx,
                  ls_freq_tally_t *t, ls_freq_kind_t *kind)
{
    enum { LINE = 512, JSON_MAX = 512 * 1024 };
    if (!f) return false;
    char *line = heap_caps_malloc(LINE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) return false;
    reader_t r = { .kind = LS_FI_CSV, .group = group ? group : "" };
    ls_freq_csv_begin(&r.csv);
    memset(r.chirp, -1, sizeof(r.chirp));
    memset(t, 0, sizeof(*t));
    int number = 0;
    while (fgets(line, LINE, f)) {
        number++;
        if (!strchr(line, '\n') && !feof(f)) {
            /* Longer than any line of a frequency list: say so, go past it. */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            t->skipped++;
            if (!t->first_bad) t->first_bad = number;
            continue;
        }
        if (!r.decided) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '\r' || *s == '\n' || !*s) continue;
            /* A comment decides nothing, except gqrx's own header. */
            if (*s == '#' && !strstr(s, "Tag name") && !strstr(s, "Frequency ;") && !strstr(s, "Frequency;")) continue;
            r.kind = *s == '#' ? LS_FI_GQRX : ls_freq_kind_of(s);
            r.decided = true;
            if (r.kind == LS_FI_SDRPP) {
                fseek(f, 0, SEEK_END);
                const long size = ftell(f);
                char *text = size > 0 && size <= JSON_MAX ? heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
                if (text) {
                    fseek(f, 0, SEEK_SET);
                    const size_t got = fread(text, 1, (size_t)size, f);
                    text[got] = 0;   /* strtod stops here on a cut-off file */
                    sdrpp(text, text + got, r.group, sink, ctx, t);
                    heap_caps_free(text);
                } else t->truncated = true;
                break;
            }
        }
        ls_freq_t e;
        blank_entry(&e);
        int rc;
        switch (r.kind) {
        case LS_FI_LSCAN: rc = lscan_line(&r, line, &e); break;
        case LS_FI_CHIRP: strip(line); rc = chirp_line(&r, line, &e); break;
        case LS_FI_GQRX:  strip(line); rc = gqrx_line(&r, line, &e); break;
        default:          rc = ls_freq_csv_line(&r.csv, line, &e); break;
        }
        if (rc < 0) { t->skipped++; if (!t->first_bad) t->first_bad = number; }
        if (rc <= 0) continue;
        if (!e.group[0]) snprintf(e.group, sizeof(e.group), "%s", r.group);
        finish(&e);
        e.line = (uint16_t)(number < 65535 ? number : 0);
        t->loaded++;
        if (!sink(&e, ctx)) break;
    }
    heap_caps_free(line);
    if (kind) *kind = r.kind;
    return true;
}

bool ls_freq_lscan_format(const ls_freq_t *e, char *out, size_t n)
{
    if (e->mode != LS_FM_NFM && e->mode != LS_FM_P25) return false;
    char name[SCAN_NAME_LEN];
    int k = 0;
    for (const char *p = e->name; *p && k < SCAN_NAME_LEN - 1; p++)
        name[k++] = *p == '|' ? '/' : (*p < 32 || *p > 126) ? '-' : *p;
    while (k && name[k - 1] == ' ') k--;
    name[k] = 0;
    if (!k) ls_freq_mhz_text(e->hz, name, sizeof(name));
    const bool placed = e->lat_e5 != LS_FREQ_NOWHERE;
    char lat[14] = "0", lon[14] = "0";
    if (placed) { degrees(lat, sizeof(lat), e->lat_e5); degrees(lon, sizeof(lon), e->lon_e5); }
    snprintf(out, n, "%s|%lu|%s|0|%s|%s|%u|%d\n", name, (unsigned long)e->hz,
             e->mode == LS_FM_P25 ? "P25" : "NFM", lat, lon,
             placed ? (unsigned)(e->radius_km > 500 ? 500 : e->radius_km) : 0u,
             strstr(e->tags, "priority") ? 1 : 0);
    scan_channel_t check;
    return scan_import_line(out, &check);
}

/* --------------------------------------------------------------- views -- */

const char *ls_freq_band_name(int band)
{
    static const char *const N[LS_FB__COUNT] = { "ALL", "HF", "VHF", "UHF", "700-960", "L-BAND" };
    return band >= 0 && band < LS_FB__COUNT ? N[band] : "?";
}

const char *ls_freq_sort_name(int sort)
{
    static const char *const N[LS_FS__COUNT] = { "AS LISTED", "FREQUENCY", "NAME", "NEAREST" };
    return sort >= 0 && sort < LS_FS__COUNT ? N[sort] : "?";
}

bool ls_freq_in_band(int band, uint32_t hz)
{
    static const uint32_t EDGE[LS_FB__COUNT + 1] = { 0, 0, 30000000u, 300000000u, 700000000u,
                                                     960000000u, UINT32_MAX };
    if (band <= LS_FB_ALL || band >= LS_FB__COUNT) return true;
    return hz >= EDGE[band] && (hz < EDGE[band + 1] || band == LS_FB_L);
}

/* Single precision: the P4's FPU has no double, and a float haversine is
   good to metres across a country. */
double ls_freq_km(double lat0, double lon0, double lat1, double lon1)
{
    const float r = 0.017453292f;
    const float a = (float)lat0 * r, b = (float)lat1 * r, d = (float)(lon1 - lon0) * r;
    const float s = sinf((b - a) / 2), t = sinf(d / 2);
    float h = s * s + cosf(a) * cosf(b) * t * t;
    h = h < 0 ? 0 : h > 1 ? 1 : h;
    return 12742.0f * asinf(sqrtf(h));
}

const char *ls_freq_bearing(double lat0, double lon0, double lat1, double lon1)
{
    static const char P[8][3] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    const float r = 0.017453292f, d = (float)(lon1 - lon0) * r, a = (float)lat0 * r, b = (float)lat1 * r;
    const float deg = atan2f(sinf(d) * cosf(b), cosf(a) * sinf(b) - sinf(a) * cosf(b) * cosf(d)) / r;
    return P[(int)((deg + 382.5f) / 45) & 7];
}

float ls_freq_distance(const ls_freq_t *e, double lat, double lon)
{
    if (e->lat_e5 == LS_FREQ_NOWHERE) return -1;
    return (float)ls_freq_km(lat, lon, e->lat_e5 * 1e-5f, e->lon_e5 * 1e-5f);
}

static bool has(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

bool ls_freq_matches(const ls_freq_t *e, const ls_freq_filter_t *f, bool fix, double lat, double lon)
{
    if (!ls_freq_in_band(f->band, e->hz)) return false;
    if (f->group[0] && strcasecmp(e->group, f->group)) return false;
    if (f->near && fix) {
        const float d = ls_freq_distance(e, lat, lon);
        if (d < 0 || d > (e->radius_km ? e->radius_km : 50)) return false;
    }
    if (f->find[0]) {
        char mhz[16];
        ls_freq_mhz_text(e->hz, mhz, sizeof(mhz));
        if (!has(e->name, f->find) && !has(e->tags, f->find) && !has(e->notes, f->find) &&
            !has(e->group, f->find) && strcasecmp(ls_freq_mode_name(e->mode), f->find) &&
            strncmp(mhz, f->find, strlen(f->find))) return false;
    }
    return true;
}

static const struct { const char *key, *tag; uint8_t hue; } GROUPS[] = {
    { "AVIA", "AIR", TUI_CYAN }, { "MARI", "SEA", TUI_BLUE }, { "AMAT", "HAM", TUI_GREEN },
    { "PMR", "PMR", TUI_YELLOW }, { "ISM", "ISM", TUI_MAGENTA }, { "LORA", "MSH", TUI_CYAN },
    { "SATE", "SAT", TUI_WHITE }, { "WEAT", "WX", TUI_YELLOW }, { "BROADCAST F", "BFM", TUI_MAGENTA },
    { "BROADCAST A", "BAM", TUI_MAGENTA }, { "PUBL", "PUB", TUI_RED }, { "RAIL", "RLY", TUI_YELLOW },
    { "UTIL", "UTL", TUI_WHITE }, { "EMER", "SOS", TUI_RED }, { "NAVI", "NAV", TUI_GREEN },
    { "MINE", "ME", TUI_YELLOW },
};

static int group_row(const char *g)
{
    for (unsigned i = 0; g && i < sizeof(GROUPS) / sizeof(GROUPS[0]); i++)
        if (!strncasecmp(g, GROUPS[i].key, strlen(GROUPS[i].key))) return (int)i;
    return -1;
}
const char *ls_freq_group_tag(const char *group)
{
    const int i = group_row(group);
    return i >= 0 ? GROUPS[i].tag : "---";
}
uint8_t ls_freq_group_hue(const char *group)
{
    const int i = group_row(group);
    return i >= 0 ? GROUPS[i].hue : TUI_WHITE;
}

ls_freq_route_t ls_freq_route(int mode, uint32_t hz)
{
    (void)hz;
    static const char NO_DECODER[] = "no decoder for it here; FM NFM lets you hear it";
    switch (mode) {
    case LS_FM_NFM:    return (ls_freq_route_t){ LS_FR_FM, "fm", "listen", NULL };
    case LS_FM_WFM:    return (ls_freq_route_t){ LS_FR_FM, "fm", "wfm", NULL };
    case LS_FM_APRS:   return (ls_freq_route_t){ LS_FR_FM, "fm", "aprs", NULL };
    case LS_FM_POCSAG: return (ls_freq_route_t){ LS_FR_FM, "fm", "pocsag", NULL };
    case LS_FM_ACARS:  return (ls_freq_route_t){ LS_FR_FM, "fm", "acars", NULL };
    case LS_FM_AIS:    return (ls_freq_route_t){ LS_FR_FM, "fm", "ais", "AIS listens on both channels at once" };
    case LS_FM_AM:     return (ls_freq_route_t){ LS_FR_HF, "fm", "am", NULL };
    case LS_FM_USB:    return (ls_freq_route_t){ LS_FR_HF, "fm", "usb", NULL };
    case LS_FM_LSB:    return (ls_freq_route_t){ LS_FR_HF, "fm", "lsb", NULL };
    case LS_FM_CW:     return (ls_freq_route_t){ LS_FR_HF, "fm", "cw", NULL };
    case LS_FM_P25:    return (ls_freq_route_t){ LS_FR_P25, "p25", NULL, NULL };
    case LS_FM_DMR:    return (ls_freq_route_t){ LS_FR_P25, "p25", NULL, "DMR/6 there shows Tier II activity; no DMR voice" };
    case LS_FM_NXDN: case LS_FM_TETRA: case LS_FM_DSTAR: case LS_FM_YSF:
        return (ls_freq_route_t){ LS_FR_FM, "fm", "listen", NO_DECODER };
    case LS_FM_OOK: case LS_FM_FSK:
        return (ls_freq_route_t){ LS_FR_SUBGHZ, "subghz", NULL, NULL };
    case LS_FM_LORA:   return (ls_freq_route_t){ LS_FR_LABS, "labs", NULL, NULL };
    case LS_FM_ADSB:   return (ls_freq_route_t){ LS_FR_OPEN, "adsb", NULL, "ADS-B listens on 1090 by itself" };
    case LS_FM_GPS:    return (ls_freq_route_t){ LS_FR_OPEN, "gps", NULL, "the GPS receiver has its own antenna" };
    case LS_FM_LTE:    return (ls_freq_route_t){ LS_FR_OPEN, "cell", NULL, "CELL WATCH picks its band in BAND" };
    case LS_FM_APT:    return (ls_freq_route_t){ LS_FR_FM, "fm", "wfm", "no APT decoder; WFM lets you hear the pass" };
    case LS_FM_LRPT:   return (ls_freq_route_t){ LS_FR_NONE, NULL, NULL, "no LRPT decoder on this unit" };
    case LS_FM_VDL2:   return (ls_freq_route_t){ LS_FR_NONE, NULL, NULL, "no VDL2 decoder; ACARS is the one it has" };
    case LS_FM_IRIDIUM:return (ls_freq_route_t){ LS_FR_NONE, NULL, NULL, "IRIDIUM is an SD app: HOME > USER" };
    default:           return (ls_freq_route_t){ LS_FR_NONE, NULL, NULL, "unknown mode" };
    }
}

/* ---------------------------------------------------------------- store -- */

/* What every unit knows with no card at all: the frequencies that are the
   same everywhere, and the license-free and ISM ones of Argentina. The full
   country list ships as a bank. Packed with the rest of the long text. */
const char *ls_freqs_builtin_text(void)
{
    return LS_TEXT(t_ls_freqs_builtin,
        "freq_mhz,mode,bw_khz,name,group,tags,lat,lon,radius_km,tone,notes\n"
        "121.5,AM,,AIR DISTRESS,AVIATION,,,,,,guard: ELTs and emergencies\n"
        "123.45,AM,,AIR TO AIR,AVIATION,,,,,,\n"
        "243,AM,,MILITARY AIR DISTRESS,AVIATION,,,,,,\n"
        "131.55,ACARS,,ACARS PRIMARY,AVIATION,,,,,,\n"
        "1090,ADSB,2000,ADS-B 1090ES,AVIATION,,,,,,\n"
        "156.8,NFM,,MARINE CH16,MARINE,,,,,,distress and calling\n"
        "156.3,NFM,,MARINE CH06,MARINE,,,,,,ship to ship\n"
        "156.65,NFM,,MARINE CH13,MARINE,,,,,,bridge to bridge\n"
        "161.975,AIS,,AIS 1,MARINE,,,,,,\n"
        "162.025,AIS,,AIS 2,MARINE,,,,,,\n"
        "27.065,AM,,CB CH9 EMERGENCY,PMR/LICENSE-FREE,tx-ok,,,,,banda ciudadana\n"
        "27.185,AM,,CB CH19,PMR/LICENSE-FREE,tx-ok,,,,,\n"
        "446.00625,NFM,12.5,PMR446 1,PMR/LICENSE-FREE,listen,,,,,heard in AR; not licensed here\n"
        "446.01875,NFM,12.5,PMR446 2,PMR/LICENSE-FREE,listen,,,,,\n"
        "446.03125,NFM,12.5,PMR446 3,PMR/LICENSE-FREE,listen,,,,,\n"
        "446.04375,NFM,12.5,PMR446 4,PMR/LICENSE-FREE,listen,,,,,\n"
        "446.05625,NFM,12.5,PMR446 5,PMR/LICENSE-FREE,listen,,,,,\n"
        "446.06875,NFM,12.5,PMR446 6,PMR/LICENSE-FREE,listen,,,,,\n"
        "446.08125,NFM,12.5,PMR446 7,PMR/LICENSE-FREE,listen,,,,,\n"
        "446.09375,NFM,12.5,PMR446 8,PMR/LICENSE-FREE,listen,,,,,\n"
        "145.8,NFM,,ISS VOICE,SATELLITE,,,,,,downlink\n"
        "145.825,APRS,,ISS APRS,SATELLITE,,,,,,digipeater\n"
        "144.93,APRS,,APRS ARGENTINA,AMATEUR,,,,,,\n"
        "433.92,OOK,,ISM 433.92,ISM/IOT,,,,,,remotes and sensors\n"
        "315,OOK,,ISM 315 REMOTES,ISM/IOT,,,,,,\n"
        "915.2,LORA,125,LORAWAN AU915 UP 0,LORA/MESH,,,,,,AR uses AU915-928\n"
        "923.3,LORA,500,LORAWAN AU915 RX2,LORA/MESH,,,,,,\n"
        "137.1,LRPT,,METEOR-M LRPT,WEATHER,,,,,,\n"
        "137.9,LRPT,,METEOR-M LRPT B,WEATHER,,,,,,\n"
        "137.62,APT,,NOAA 15 APT,WEATHER,,,,,,NOAA APT retired 2025\n"
        "1575.42,GPS,,GPS L1,NAVIGATION,,,,,,\n"
        "1626,IRIDIUM,,IRIDIUM,SATELLITE,,,,,,\n");
}

static char s_root[64] = "/sdcard/freqs";
static EXT_RAM_BSS_ATTR char s_bank[LS_FREQ_BANKS][24];
static int s_banks;
/* The entries and the view scratch, taken from PSRAM the first time FREQS
   is used, so a unit that never opens it pays nothing. */
static ls_freq_t *s_e;
static float *s_key;
static int s_n, s_which = LS_FREQS_ALL;
static FILE *s_f;
static int s_file;              /* next bank to open while loading ALL */
static const char *s_bp;        /* read point in the built-in text, NULL: not reading it */
static ls_freq_csv_t s_csv;
static int s_lineno;
static bool s_loading, s_dirty;
static uint32_t s_gen;
static ls_freq_tally_t s_tally;
static EXT_RAM_BSS_ATTR char s_line[512];

/* The UI task loads a bank a few lines a frame; the console's `freq` and a
   freq.save called from any task scan the card and write to it meanwhile.
   The bank list, what is loaded and the line buffer change under this. */
static StaticSemaphore_t s_lock_memory;
static SemaphoreHandle_t s_lock;
/* Made at boot (ls_freqs_register), before any task can reach the store;
   the fallback only serves the host bench, which never registers. */
void ls_freqs_lock_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_memory);
}
static void lock(void)
{
    if (!s_lock) ls_freqs_lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

void ls_freqs_set_root(const char *dir) { snprintf(s_root, sizeof(s_root), "%s", dir); }
const char *ls_freqs_root(void) { return s_root; }

static void path_of(const char *bank, char *out, size_t n) { snprintf(out, n, "%s/%s.csv", s_root, bank); }

static int by_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static bool scan_now(void)
{
    static EXT_RAM_BSS_ATTR char found[LS_FREQ_BANKS][24];
    /* What is loaded is held by name: a bank added in front of it (an
       import, a card swapped) must not turn it into its neighbour. */
    char was[24] = "";
    if (s_which >= 0 && s_which < s_banks) snprintf(was, sizeof(was), "%s", s_bank[s_which]);
    int n = 0;
    memset(found, 0, sizeof(found));
    DIR *d = opendir(s_root);
    if (!d) { s_dirty |= s_banks > 0; s_banks = 0; return false; }
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *name = de->d_name;
        const size_t len = strlen(name);
        if (name[0] == '.' || len < 5 || len - 4 >= sizeof(found[0]) || strcasecmp(name + len - 4, ".csv")) continue;
        /* mine.csv is where SAVE writes: on a card with more banks than fit
           it takes the last place rather than drop out of view. */
        if (n >= LS_FREQ_BANKS) {
            if (strcasecmp(name, "mine.csv")) continue;
            n = LS_FREQ_BANKS - 1;
        }
        snprintf(found[n], sizeof(found[n]), "%.*s", (int)(len - 4), name);
        n++;
    }
    closedir(d);
    qsort(found, (size_t)n, sizeof(found[0]), by_name);
    if (n != s_banks || memcmp(s_bank, found, sizeof(found))) s_dirty = true;
    memcpy(s_bank, found, sizeof(found));
    s_banks = n;
    if (was[0]) {
        const int i = ls_freqs_bank_find(was);
        s_which = i >= 0 ? i : LS_FREQS_ALL;
    }
    return true;
}
bool ls_freqs_scan(void)
{
    lock();
    const bool card = scan_now();
    unlock();
    return card;
}
int ls_freqs_banks(void) { return s_banks; }
const char *ls_freqs_bank(int i)
{
    if (i == LS_FREQS_ALL) return "ALL BANKS";
    if (i == LS_FREQS_BUILTIN || i == LS_FREQ_BANK_BUILTIN) return "BUILT-IN";
    return i >= 0 && i < s_banks ? s_bank[i] : "?";
}
int ls_freqs_bank_find(const char *name)
{
    for (int i = 0; i < s_banks; i++) if (!strcasecmp(s_bank[i], name)) return i;
    return -1;
}

static bool room(void)
{
    if (!s_e) s_e = heap_caps_calloc(LS_FREQ_MAX, sizeof(ls_freq_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_key) s_key = heap_caps_calloc(LS_FREQ_MAX, sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_e && s_key;
}

static void close_source(void)
{
    if (s_f) fclose(s_f);
    s_f = NULL;
    s_bp = NULL;
}

static void load_now(int which)
{
    close_source();
    s_n = 0;
    memset(&s_tally, 0, sizeof(s_tally));
    s_dirty = false;
    s_which = which;
    s_file = which >= 0 ? which : 0;
    s_loading = room();
    /* The set is gone already: a view holding indices into it rebuilds now,
       not when the reload finishes. */
    s_gen++;
}
void ls_freqs_load(int which)
{
    lock();
    load_now(which);
    unlock();
}

/* The next thing to read: a bank file, or the built-in set when that is what
   was asked for, or when the card has no banks. */
static bool open_next(void)
{
    if (s_which == LS_FREQS_BUILTIN || (s_which == LS_FREQS_ALL && !s_banks && s_file == 0)) {
        s_bp = ls_freqs_builtin_text();
        s_file = s_banks + 1;
    } else {
        const int last = s_which >= 0 ? s_which : s_banks - 1;
        while (!s_f && s_file <= last && s_file < s_banks) {
            char path[96];
            path_of(s_bank[s_file], path, sizeof(path));
            s_f = fopen(path, "r");
            if (!s_f) s_file++;
        }
        if (!s_f) return false;
    }
    ls_freq_csv_begin(&s_csv);
    s_lineno = 0;
    return true;
}

static bool next_line(void)
{
    if (s_bp) {
        if (!*s_bp) return false;
        size_t k = strcspn(s_bp, "\n");
        snprintf(s_line, sizeof(s_line), "%.*s", (int)(k < sizeof(s_line) - 1 ? k : sizeof(s_line) - 1), s_bp);
        s_bp += k + (s_bp[k] == '\n');
        return true;
    }
    if (!fgets(s_line, sizeof(s_line), s_f)) return false;
    if (!strchr(s_line, '\n') && !feof(s_f)) {
        int c;
        while ((c = fgetc(s_f)) != EOF && c != '\n') {}
        s_line[0] = '!';   /* past any frequency list's width: refused below */
        s_line[1] = 0;
    }
    return true;
}

bool ls_freqs_pump(int lines)
{
    lock();
    if (s_dirty && !s_loading) load_now(s_which);
    while (s_loading && lines-- > 0) {
        if (!s_f && !s_bp && !open_next()) {
            s_loading = false;
            s_gen++;
            break;
        }
        if (!next_line()) {
            const bool built = s_bp != NULL;
            close_source();
            s_file++;
            if (built || (s_which >= 0 && s_file > s_which)) { s_loading = false; s_gen++; }
            continue;
        }
        s_lineno++;
        ls_freq_t e;
        const int rc = ls_freq_csv_line(&s_csv, s_line, &e);
        if (rc < 0) { s_tally.skipped++; if (!s_tally.first_bad) s_tally.first_bad = s_lineno; }
        if (rc <= 0) continue;
        if (s_n >= LS_FREQ_MAX) {
            s_tally.truncated = true;
            close_source();
            s_loading = false;
            s_gen++;
            break;
        }
        e.bank = s_f ? (uint8_t)s_file : LS_FREQ_BANK_BUILTIN;
        e.line = (uint16_t)(s_f && s_lineno < 65535 ? s_lineno : 0);
        s_e[s_n++] = e;
        s_tally.loaded++;
    }
    const bool more = s_loading;
    unlock();
    return more;
}

bool ls_freqs_loading(void) { return s_loading; }
int ls_freqs_which(void) { return s_which; }
uint32_t ls_freqs_generation(void) { return s_gen; }
int ls_freqs_count(void) { return s_n; }
const ls_freq_t *ls_freqs_at(int i) { return i >= 0 && i < s_n ? &s_e[i] : NULL; }
const ls_freq_tally_t *ls_freqs_tally(void) { return &s_tally; }
void ls_freqs_dirty(void) { s_dirty = true; }
bool ls_freqs_is_dirty(void) { return s_dirty; }

static int s_sort;
static int by_key(const void *a, const void *b)
{
    const int i = *(const uint16_t *)a, j = *(const uint16_t *)b;
    int c = 0;
    if (s_sort == LS_FS_NAME) c = strcasecmp(s_e[i].name, s_e[j].name);
    else if (s_sort == LS_FS_FREQ) c = (s_e[i].hz > s_e[j].hz) - (s_e[i].hz < s_e[j].hz);
    else if (s_sort == LS_FS_NEAR) c = (s_key[i] > s_key[j]) - (s_key[i] < s_key[j]);
    return c ? c : i - j;
}

int ls_freqs_view(const ls_freq_filter_t *f, bool fix, double lat, double lon, uint16_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < s_n && n < cap; i++) {
        if (!ls_freq_matches(&s_e[i], f, fix, lat, lon)) continue;
        if (f->sort == LS_FS_NEAR) {
            const float d = fix ? ls_freq_distance(&s_e[i], lat, lon) : -1;
            s_key[i] = d < 0 ? 1e9f : d;
        }
        out[n++] = (uint16_t)i;
    }
    s_sort = f->sort == LS_FS_NEAR && !fix ? LS_FS_LISTED : f->sort;
    if (s_sort != LS_FS_LISTED) qsort(out, (size_t)n, sizeof(out[0]), by_key);
    return n;
}

/* ------------------------------------------------------------- writing -- */

static bool make_root(void)
{
    struct stat st;
    if (stat(s_root, &st) == 0) return S_ISDIR(st.st_mode);
    return make_dir(s_root) == 0;
}

static bool append_now(const char *bank, const ls_freq_t *e, char *why, size_t n)
{
    char path[96], line[256];
    if (!make_root()) { snprintf(why, n, "No SD card: nothing saved"); return false; }
    path_of(bank, path, sizeof(path));
    FILE *f = fopen(path, "a+");
    if (!f) { snprintf(why, n, "Could not open %s.csv", bank); return false; }
    /* The file's own columns, from its first line that is not a comment. */
    ls_freq_csv_t csv;
    ls_freq_csv_begin(&csv);
    rewind(f);
    while (!csv.seen && fgets(s_line, sizeof(s_line), f)) {
        ls_freq_t skip;
        (void)ls_freq_csv_line(&csv, s_line, &skip);
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (!ls_freq_csv_format_as(&csv, e, line, sizeof(line))) {
        fclose(f);
        snprintf(why, n, "That entry will not fit a line");
        return false;
    }
    bool ok = true;
    if (size <= 0) ok = fprintf(f, "%s\n", LS_FREQ_CSV_HEADER) > 0;
    else {
        /* A file edited by hand may end without a newline; the new line must
           not join the last one. */
        fseek(f, size - 1, SEEK_SET);
        const int last = fgetc(f);
        fseek(f, 0, SEEK_END);
        if (last != '\n') ok = fputc('\n', f) != EOF;
    }
    ok = ok && fprintf(f, "%s\n", line) > 0;
    ok = (fclose(f) == 0) && ok;
    if (ok && ls_freqs_bank_find(bank) < 0) scan_now();
    char mhz[16];
    ls_freq_mhz_text(e->hz, mhz, sizeof(mhz));
    if (ok) snprintf(why, n, "Saved %s %s to %s", mhz, e->name, bank);
    else snprintf(why, n, "Could not write %s.csv", bank);
    if (ok) s_dirty = true;
    return ok;
}
bool ls_freqs_append(const char *bank, const ls_freq_t *e, char *why, size_t n)
{
    lock();
    const bool ok = append_now(bank, e, why, n);
    unlock();
    return ok;
}

static void replace_file(const char *tmp, const char *path)
{
    remove(path);
    rename(tmp, path);
}

static bool rewrite_now(int i, const ls_freq_t *with, char *why, size_t n)
{
    const ls_freq_t *e = ls_freqs_at(i);
    if (s_loading) { snprintf(why, n, "Still loading"); return false; }
    if (!e || e->bank >= s_banks || !e->line) {
        snprintf(why, n, "%s", e && e->bank == LS_FREQ_BANK_BUILTIN ? "The built-in set cannot change; SAVE copies to mine"
                                                                     : "Nothing to change");
        return false;
    }
    if (s_tally.truncated) { snprintf(why, n, "Bank cut short at %d; edit the file instead", LS_FREQ_MAX); return false; }
    char path[96], tmp[100];
    static EXT_RAM_BSS_ATTR char out[sizeof(s_line)];
    path_of(s_bank[e->bank], path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *in = fopen(path, "r"), *o = in ? fopen(tmp, "w") : NULL;
    if (!o) { if (in) fclose(in); snprintf(why, n, "Could not open %s.csv", s_bank[e->bank]); return false; }
    ls_freq_csv_t csv;
    ls_freq_csv_begin(&csv);
    int number = 0;
    bool done = false, ok = true;
    while (ok && fgets(s_line, sizeof(s_line), in)) {
        number++;
        if (number == e->line) {
            /* Only the line that was read as this entry: a file changed on
               the card since it was loaded is not edited blind. */
            char copy[sizeof(s_line)];
            ls_freq_t was;
            snprintf(copy, sizeof(copy), "%s", s_line);
            if (ls_freq_csv_line(&csv, copy, &was) == 1 && was.hz == e->hz && !strcmp(was.name, e->name)) {
                done = true;
                if (with) {
                    /* Only what was changed is written from memory; every
                       other field is the line's own text, so a note longer
                       than the screen keeps and an extra column stays. */
                    static EXT_RAM_BSS_ATTR texts_t a, b;
                    char *f[16];
                    snprintf(copy, sizeof(copy), "%s", s_line);
                    strip(copy);
                    const int nf = split(copy, ',', f, 16);
                    texts(e, &a);
                    texts(with, &b);
                    for (int k = 0; k < 11; k++)
                        if (!strcmp(a.v[k], b.v[k]) && csv.col[k] >= 0 && csv.col[k] < nf) b.v[k] = f[csv.col[k]];
                    ok = join(&csv, b.v, f, nf, out, sizeof(out)) && fprintf(o, "%s\n", out) > 0;
                }
                continue;
            }
        } else if (number < e->line) {
            char copy[sizeof(s_line)];
            ls_freq_t was;
            snprintf(copy, sizeof(copy), "%s", s_line);
            (void)ls_freq_csv_line(&csv, copy, &was);
        }
        ok = fputs(s_line, o) != EOF;
    }
    fclose(in);
    ok = (fclose(o) == 0) && ok;
    if (!ok || !done) {
        remove(tmp);
        snprintf(why, n, "%s", done ? "Could not write the card" : "The file changed on the card; reloaded");
        s_dirty = true;
        return false;
    }
    replace_file(tmp, path);
    snprintf(why, n, "%s %s in %s", with ? "Changed" : "Deleted", e->name, s_bank[e->bank]);
    s_dirty = true;
    return true;
}
bool ls_freqs_rewrite(int i, const ls_freq_t *with, char *why, size_t n)
{
    lock();
    const bool ok = rewrite_now(i, with, why, n);
    unlock();
    return ok;
}

static EXT_RAM_BSS_ATTR uint32_t s_seen[LS_FREQ_MAX];

bool ls_freq_out_open(ls_freq_out_t *o, const char *path, bool lscan)
{
    memset(o, 0, sizeof(*o));
    o->lscan = lscan;
    o->f = fopen(path, "w");
    return o->f && fprintf(o->f, "%s\n", lscan ? "LSCAN1\n# from FREQS" : LS_FREQ_CSV_HEADER) > 0;
}

void ls_freq_out_put(ls_freq_out_t *o, const ls_freq_t *e)
{
    char line[256];
    if (!o->f) return;
    if (o->lscan) {
        /* The scanner refuses a list with the same channel twice. */
        const uint32_t key = e->hz ^ (e->mode == LS_FM_P25 ? 0x80000000u : 0);
        for (int j = 0; j < o->seen; j++) if (s_seen[j] == key) { o->left++; return; }
        if (!ls_freq_lscan_format(e, line, sizeof(line))) { o->left++; return; }
        if (o->seen < LS_FREQ_MAX) s_seen[o->seen++] = key;
        if (fputs(line, o->f) >= 0) o->wrote++;
    } else if (ls_freq_csv_format(e, line, sizeof(line)) && fprintf(o->f, "%s\n", line) > 0) o->wrote++;
    else o->left++;
}

bool ls_freq_out_close(ls_freq_out_t *o, const char *path, char *why, size_t n)
{
    const bool ok = o->f && fclose(o->f) == 0;
    o->f = NULL;
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (ok) snprintf(why, n, "Wrote %d to %s%s", o->wrote, base,
                     o->left ? (o->lscan ? ", NFM/P25 only" : ", some left out") : "");
    else snprintf(why, n, "Could not write %s", base);
    return ok && o->wrote > 0;
}

bool ls_freqs_export(const uint16_t *idx, int n, const char *path, bool lscan, char *why, size_t wn)
{
    if (!make_root()) { snprintf(why, wn, "No SD card"); return false; }
    char dir[96];
    snprintf(dir, sizeof(dir), "%s/export", s_root);
    make_dir(dir);
    ls_freq_out_t o;
    if (!ls_freq_out_open(&o, path, lscan)) {
        if (o.f) fclose(o.f);
        snprintf(why, wn, "Could not create %s", path);
        return false;
    }
    for (int k = 0; k < n; k++) {
        const ls_freq_t *e = ls_freqs_at(idx[k]);
        if (e) ls_freq_out_put(&o, e);
    }
    return ls_freq_out_close(&o, path, why, wn);
}

static char s_imported[24];
const char *ls_freqs_imported(void) { return s_imported; }

typedef struct { FILE *out; int wrote; } import_t;
static bool import_sink(const ls_freq_t *e, void *ctx)
{
    import_t *im = ctx;
    char line[256];
    if (ls_freq_csv_format(e, line, sizeof(line)) && fprintf(im->out, "%s\n", line) > 0) im->wrote++;
    return true;
}

bool ls_freqs_import(const char *src, char *why, size_t n)
{
    FILE *in = fopen(src, "r");
    if (!in) { snprintf(why, n, "Cannot open %s", src); return false; }
    if (!make_root()) { fclose(in); snprintf(why, n, "No SD card"); return false; }
    /* The bank is named for the file: letters, digits, - and _. */
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char stem[20], path[96];
    int k = 0;
    for (const char *p = base; *p && *p != '.' && k < (int)sizeof(stem) - 3; p++)
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') stem[k++] = (char)tolower((unsigned char)*p);
    stem[k] = 0;
    if (!k) snprintf(stem, sizeof(stem), "imported");
    char name[24];
    struct stat st;
    snprintf(name, sizeof(name), "%s", stem);
    /* Never over a bank already there: stem, stem-2 .. stem-99, then no. */
    bool free_name = false;
    for (int i = 2; i <= 100; i++) {
        path_of(name, path, sizeof(path));
        if (stat(path, &st) != 0) { free_name = true; break; }
        snprintf(name, sizeof(name), "%s-%d", stem, i);
    }
    if (!free_name) {
        fclose(in);
        snprintf(why, n, "%s to %s-99 are all taken; delete one first", stem, stem);
        return false;
    }
    import_t im = { fopen(path, "w"), 0 };
    if (!im.out) { fclose(in); snprintf(why, n, "Cannot create %s.csv", name); return false; }
    ls_freq_tally_t t;
    ls_freq_kind_t kind;
    fprintf(im.out, "%s\n# imported from %s\n", LS_FREQ_CSV_HEADER, base);
    ls_freq_read(in, "IMPORTED", import_sink, &im, &t, &kind);
    fclose(in);
    const bool ok = (fclose(im.out) == 0) && im.wrote > 0;
    s_imported[0] = 0;
    if (!ok) remove(path);
    else {
        snprintf(s_imported, sizeof(s_imported), "%s", name);
        ls_freqs_scan();
    }
    if (ok && t.skipped)
        snprintf(why, n, "%s: %d into %s, %d skipped (line %d)", ls_freq_kind_name(kind), im.wrote, name,
                 t.skipped, t.first_bad);
    else if (ok) snprintf(why, n, "%s: %d into %s", ls_freq_kind_name(kind), im.wrote, name);
    else snprintf(why, n, "Nothing readable in %s (%d refused)", base, t.skipped);
    return ok;
}
