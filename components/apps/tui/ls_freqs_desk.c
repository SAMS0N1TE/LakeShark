/* See ls_freqs_desk.h. Nothing here draws and nothing here moves a radio. */
#include "ls_freqs_desk.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"

#ifdef _WIN32                   /* the bench on Windows */
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0775)
#endif

/* ------------------------------------------------------------- columns -- */

const char *ls_freqd_col_name(int col)
{
    static const char *const N[LS_FC__COUNT] = { "*", "MHz", "NAME", "MODE", "BAND", "GROUP", "TONE",
                                                 "DIST", "BANK", "TAGS", "NOTES" };
    return col >= 0 && col < LS_FC__COUNT ? N[col] : "LISTED";
}

int ls_freqd_band(uint32_t hz)
{
    for (int b = LS_FB_HF; b < LS_FB__COUNT; b++) if (ls_freq_in_band(b, hz)) return b;
    return LS_FB_ALL;
}

/* qsort has no room for what it sorts by. UI task only. */
static struct { int col; bool desc, fix; double lat, lon; } s_by;
/* Distances worked out once a sort, by loaded index: a great circle a
   comparison is seconds of a list of two thousand. */
static float *s_dist;

static float reach(const ls_freq_t *e)
{
    const float d = s_by.fix ? ls_freq_distance(e, s_by.lat, s_by.lon) : -1;
    return d < 0 ? 1e9f : d;
}

static int by_col(const void *a, const void *b)
{
    const int i = *(const uint16_t *)a, j = *(const uint16_t *)b;
    const ls_freq_t *x = ls_freqs_at(i), *y = ls_freqs_at(j);
    int c = 0;
    if (!x || !y || s_by.col < 0) return i - j;      /* as listed: the files' own order */
    switch (s_by.col) {
    case LS_FC_STAR: c = (int)ls_freqd_starred(y) - (int)ls_freqd_starred(x); break;   /* starred first */
    case LS_FC_FREQ: c = (x->hz > y->hz) - (x->hz < y->hz); break;
    case LS_FC_NAME: c = strcasecmp(x->name, y->name); break;
    case LS_FC_MODE: c = strcmp(ls_freq_mode_name(x->mode), ls_freq_mode_name(y->mode)); break;
    case LS_FC_BAND: c = ls_freqd_band(x->hz) - ls_freqd_band(y->hz); break;
    case LS_FC_GROUP: c = strcasecmp(x->group, y->group); break;
    case LS_FC_TONE: {
        /* An entry with no tone goes after every one that has one. */
        if (!x->tone[0] || !y->tone[0]) c = (int)!x->tone[0] - (int)!y->tone[0];
        else {
            const double p = atof(x->tone), q = atof(y->tone);
            c = p != q ? (p > q) - (p < q) : strcmp(x->tone, y->tone);
        }
        break;
    }
    case LS_FC_DIST: {
        const float p = s_dist ? s_dist[i] : reach(x), q = s_dist ? s_dist[j] : reach(y);
        c = (p > q) - (p < q);
        break;
    }
    case LS_FC_BANK: c = (int)x->bank - (int)y->bank; break;
    case LS_FC_TAGS: c = strcasecmp(x->tags, y->tags); break;
    case LS_FC_NOTES: c = strcasecmp(x->notes, y->notes); break;
    default: break;
    }
    if (s_by.desc) c = -c;
    /* The same frequency keeps one order whichever way a column is turned:
       then by MHz, then as the files have them. */
    if (!c && s_by.col != LS_FC_FREQ) c = (x->hz > y->hz) - (x->hz < y->hz);
    return c ? c : i - j;
}

void ls_freqd_sort(uint16_t *idx, int n, int col, bool desc, bool fix, double lat, double lon)
{
    if (n < 2) return;
    s_by.col = col; s_by.desc = desc; s_by.fix = fix; s_by.lat = lat; s_by.lon = lon;
    if (col < 0 || col >= LS_FC__COUNT) s_by.col = -1;
    s_dist = col == LS_FC_DIST ? heap_caps_malloc((size_t)ls_freqs_count() * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    for (int k = 0; s_dist && k < n; k++) {
        const ls_freq_t *e = ls_freqs_at(idx[k]);
        if (e) s_dist[idx[k]] = reach(e);
    }
    qsort(idx, (size_t)n, sizeof(idx[0]), by_col);
    heap_caps_free(s_dist);
    s_dist = NULL;
    if (s_by.col < 0 && desc)
        for (int a = 0, b = n - 1; a < b; a++, b--) { const uint16_t t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
}

/* ---------------------------------------------------------------- star -- */

/* Where the word "fav" stands in a tag list, or NULL. */
static const char *fav_at(const char *tags)
{
    for (const char *p = tags; (p = strstr(p, "fav")); p += 3) {
        const bool head = p == tags || p[-1] == ' ' || p[-1] == ',' || p[-1] == ';';
        const bool tail = !p[3] || p[3] == ' ' || p[3] == ',' || p[3] == ';';
        if (head && tail) return p;
    }
    return NULL;
}

bool ls_freqd_starred(const ls_freq_t *e) { return e && fav_at(e->tags) != NULL; }

void ls_freqd_set_star(ls_freq_t *e, bool on)
{
    const char *at = fav_at(e->tags);
    if (on == (at != NULL)) return;
    char t[LS_FREQ_TAGS];
    if (on) {
        /* In front, where a list cut for a narrow column still shows it. A
           tag list too full for it loses its tail, not the star. */
        snprintf(t, sizeof(t), "fav%s%.*s", e->tags[0] ? " " : "", (int)sizeof(t) - 5, e->tags);
    } else {
        const size_t head = (size_t)(at - e->tags);
        const char *rest = at + 3;
        while (*rest == ' ' || *rest == ',' || *rest == ';') rest++;
        snprintf(t, sizeof(t), "%.*s%s", (int)head, e->tags, rest);
        size_t k = strlen(t);
        while (k && (t[k - 1] == ' ' || t[k - 1] == ',' || t[k - 1] == ';')) t[--k] = 0;
    }
    snprintf(e->tags, sizeof(e->tags), "%s", t);
}

/* ---------------------------------------------------------- duplicates -- */

/* The same channel twice: one frequency and mode in one place. A tower
   frequency used again at an airport a province away is not a duplicate;
   two lines for the same repeater are. A place is the same within about a
   kilometre, and an entry with none matches one with none. */
static bool same_channel(const ls_freq_t *x, const ls_freq_t *y)
{
    if (x->hz != y->hz || x->mode != y->mode) return false;
    const bool px = x->lat_e5 != LS_FREQ_NOWHERE, py = y->lat_e5 != LS_FREQ_NOWHERE;
    if (px != py) return false;
    if (!px) return true;
    return labs((long)x->lat_e5 - y->lat_e5) < 1000 && labs((long)x->lon_e5 - y->lon_e5) < 1000;
}

int ls_freqd_dups(const uint16_t *idx, int n, uint8_t *bits, size_t nbytes)
{
    int found = 0;
    memset(bits, 0, nbytes);
    for (int a = 0; a < n; a++) {
        const ls_freq_t *x = ls_freqs_at(idx[a]);
        if (!x || (size_t)(idx[a] >> 3) >= nbytes) continue;
        if (bits[idx[a] >> 3] & (1u << (idx[a] & 7))) continue;      /* already paired with an earlier one */
        bool any = false;
        for (int b = a + 1; b < n; b++) {
            const ls_freq_t *y = ls_freqs_at(idx[b]);
            if (!y || !same_channel(x, y) || (size_t)(idx[b] >> 3) >= nbytes) continue;
            if (!(bits[idx[b] >> 3] & (1u << (idx[b] & 7)))) found++;
            bits[idx[b] >> 3] |= (uint8_t)(1u << (idx[b] & 7));
            any = true;
        }
        if (any) { bits[idx[a] >> 3] |= (uint8_t)(1u << (idx[a] & 7)); found++; }
    }
    return found;
}

/* -------------------------------------------------------------- fields -- */

static const struct { const char *label, *hint; uint8_t max; } FIELD[LS_FF__COUNT] = {
    { "MHz",    "0.009 to 6000",                     12 },
    { "Name",   "what the list calls it",            LS_FREQ_NAME - 1 },
    { "Mode",   "NFM WFM AM USB LSB CW P25 DMR OOK FSK LORA ...", 8 },
    { "BW kHz", "blank, or the channel width",       7 },
    { "Group",  "AVIATION, MARINE, AMATEUR, MINE ...", LS_FREQ_GROUP - 1 },
    { "Tone",   "88.5, or D023N for DCS",            LS_FREQ_TONE - 1 },
    { "Tags",   "words; fav is the star, tx-ok",     LS_FREQ_TAGS - 1 },
    { "Notes",  "anything",                          LS_FREQ_NOTES - 1 },
    { "Lat",    "blank for everywhere; -31.4100",    10 },
    { "Lon",    "with Lat; -64.1900",                11 },
    { "Reach",  "km it is heard from its place",     5 },
};

const char *ls_freqd_field_label(int f) { return f >= 0 && f < LS_FF__COUNT ? FIELD[f].label : ""; }
const char *ls_freqd_field_hint(int f) { return f >= 0 && f < LS_FF__COUNT ? FIELD[f].hint : ""; }
int ls_freqd_field_max(int f) { return f >= 0 && f < LS_FF__COUNT ? FIELD[f].max : 0; }

static void e5_text(int32_t v, char *out, size_t n)
{
    if (v == LS_FREQ_NOWHERE) { out[0] = 0; return; }
    const long a = labs((long)v);
    snprintf(out, n, "%s%ld.%05ld", v < 0 ? "-" : "", a / 100000, a % 100000);
    /* Four decimals at least, as the banks are written. */
    size_t k = strlen(out);
    if (k && out[k - 1] == '0') out[k - 1] = 0;
}

void ls_freqd_field_get(const ls_freq_t *e, int f, char *out, size_t n)
{
    if (!n) return;
    out[0] = 0;
    switch (f) {
    case LS_FF_FREQ: if (e->hz) ls_freq_mhz_text(e->hz, out, n); break;
    case LS_FF_NAME: snprintf(out, n, "%s", e->name); break;
    case LS_FF_MODE: snprintf(out, n, "%s", ls_freq_mode_name(e->mode)); break;
    case LS_FF_BW:
        if (e->bw % 10) snprintf(out, n, "%u.%u", e->bw / 10, e->bw % 10);
        else if (e->bw) snprintf(out, n, "%u", e->bw / 10);
        break;
    case LS_FF_GROUP: snprintf(out, n, "%s", e->group); break;
    case LS_FF_TONE: snprintf(out, n, "%s", e->tone); break;
    case LS_FF_TAGS: snprintf(out, n, "%s", e->tags); break;
    case LS_FF_NOTES: snprintf(out, n, "%s", e->notes); break;
    case LS_FF_LAT: e5_text(e->lat_e5, out, n); break;
    case LS_FF_LON: e5_text(e->lon_e5, out, n); break;
    case LS_FF_RADIUS: if (e->radius_km) snprintf(out, n, "%u", e->radius_km); break;
    default: break;
    }
}

/* A whole number of something, nothing after it but blanks. */
static bool number(const char *text, double *out)
{
    char *end;
    while (*text == ' ') text++;
    if (!*text) return false;
    *out = strtod(text, &end);
    while (*end == ' ') end++;
    return end != text && !*end && isfinite(*out);
}

static bool blank(const char *text)
{
    while (*text == ' ') text++;
    return !*text;
}

/* Text as a field keeps it: no ends, nothing a line of CSV cannot carry. */
static bool text_in(char *dst, size_t cap, const char *text, const char *what, char *why, size_t wn)
{
    while (*text == ' ') text++;
    size_t k = strlen(text);
    while (k && text[k - 1] == ' ') k--;
    if (k >= cap) { snprintf(why, wn, "%s holds %d characters", what, (int)cap - 1); return false; }
    for (size_t i = 0; i < k; i++)
        if ((unsigned char)text[i] < 32 || text[i] == '"') {
            snprintf(why, wn, "%s cannot hold a quote mark", what);
            return false;
        }
    memcpy(dst, text, k);
    dst[k] = 0;
    return true;
}

bool ls_freqd_field_set(ls_freq_t *e, int f, const char *text, char *why, size_t wn)
{
    double v;
    if (!text) text = "";
    switch (f) {
    case LS_FF_FREQ:
        if (!number(text, &v) || v < 0.009 || v > 6000) { snprintf(why, wn, "MHz: a number from 0.009 to 6000"); return false; }
        e->hz = (uint32_t)llround(v * 1e6);
        return true;
    case LS_FF_NAME: return text_in(e->name, sizeof(e->name), text, "Name", why, wn);
    case LS_FF_MODE: {
        const int m = ls_freq_mode_parse(text);
        if (m < 0) { snprintf(why, wn, "Mode: NFM, WFM, AM, USB, LSB, CW, P25, DMR, OOK, FSK, LORA ..."); return false; }
        e->mode = (uint8_t)m;
        return true;
    }
    case LS_FF_BW:
        if (blank(text)) { e->bw = 0; return true; }
        if (!number(text, &v) || v <= 0 || v > 6500) { snprintf(why, wn, "BW: blank, or kHz up to 6500"); return false; }
        e->bw = (uint16_t)lround(v * 10);
        return true;
    case LS_FF_GROUP: return text_in(e->group, sizeof(e->group), text, "Group", why, wn);
    case LS_FF_TONE: {
        if (blank(text)) { e->tone[0] = 0; return true; }
        char t[LS_FREQ_TONE];
        if (!text_in(t, sizeof(t), text, "Tone", why, wn)) return false;
        /* A CTCSS tone in Hz, or DCS the way the banks write it. */
        const bool dcs = (t[0] == 'D' || t[0] == 'd') && isdigit((unsigned char)t[1]);
        if (!dcs && (!number(t, &v) || v < 60 || v > 260)) {
            snprintf(why, wn, "Tone: 67 to 254.1 Hz, or D023N for DCS");
            return false;
        }
        if (dcs) t[0] = 'D';
        snprintf(e->tone, sizeof(e->tone), "%s", t);
        return true;
    }
    case LS_FF_TAGS: return text_in(e->tags, sizeof(e->tags), text, "Tags", why, wn);
    case LS_FF_NOTES: return text_in(e->notes, sizeof(e->notes), text, "Notes", why, wn);
    case LS_FF_LAT:
        if (blank(text)) { e->lat_e5 = LS_FREQ_NOWHERE; return true; }
        if (!number(text, &v) || v < -90 || v > 90) { snprintf(why, wn, "Lat: blank, or -90 to 90"); return false; }
        e->lat_e5 = (int32_t)lround(v * 1e5);
        return true;
    case LS_FF_LON:
        if (blank(text)) { e->lon_e5 = LS_FREQ_NOWHERE; return true; }
        if (!number(text, &v) || v < -180 || v > 180) { snprintf(why, wn, "Lon: blank, or -180 to 180"); return false; }
        e->lon_e5 = (int32_t)lround(v * 1e5);
        return true;
    case LS_FF_RADIUS:
        if (blank(text)) { e->radius_km = 0; return true; }
        if (!number(text, &v) || v < 0 || v > 9999) { snprintf(why, wn, "Reach: blank, or km up to 9999"); return false; }
        e->radius_km = (uint16_t)lround(v);
        return true;
    default:
        snprintf(why, wn, "No such field");
        return false;
    }
}

void ls_freqd_blank(ls_freq_t *e)
{
    memset(e, 0, sizeof(*e));
    e->lat_e5 = e->lon_e5 = LS_FREQ_NOWHERE;
    e->mode = LS_FM_NFM;
}

/* --------------------------------------------------------------- banks -- */

bool ls_freqd_bank_name(const char *typed, char *out, size_t n, char *why, size_t wn)
{
    size_t k = 0;
    /* What an import keeps of a file's name, and a blank becomes a dash. */
    for (const char *p = typed ? typed : ""; *p && k + 1 < n && k < 19; p++) {
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') out[k++] = (char)tolower((unsigned char)*p);
        else if (*p == ' ' && k && out[k - 1] != '-') out[k++] = '-';
    }
    while (k && out[k - 1] == '-') k--;
    out[k] = 0;
    if (!k) { snprintf(why, wn, "A bank's name: letters, digits, - and _"); return false; }
    return true;
}

static void bank_path(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s.csv", ls_freqs_root(), name);
}

bool ls_freqd_bank_new(const char *typed, char *why, size_t wn)
{
    char name[24], path[96];
    struct stat st;
    if (!ls_freqd_bank_name(typed, name, sizeof(name), why, wn)) return false;
    bank_path(name, path, sizeof(path));
    if (stat(path, &st) == 0) { snprintf(why, wn, "There is a bank called %s already", name); return false; }
    if (ls_freqs_banks() >= LS_FREQ_BANKS) { snprintf(why, wn, "FREQS lists %d banks; that is all of them", LS_FREQ_BANKS); return false; }
    if (stat(ls_freqs_root(), &st) != 0) make_dir(ls_freqs_root());
    FILE *f = fopen(path, "w");
    const bool ok = f && fprintf(f, "%s\n", LS_FREQ_CSV_HEADER) > 0;
    if (!((f ? fclose(f) == 0 : false) && ok)) { snprintf(why, wn, "No SD card: no bank made"); return false; }
    ls_freqs_scan();
    ls_freqs_dirty();
    snprintf(why, wn, "New bank %s", name);
    return true;
}

bool ls_freqd_bank_rename(int bank, const char *typed, char *why, size_t wn)
{
    char name[24], from[96], to[96];
    struct stat st;
    if (bank < 0 || bank >= ls_freqs_banks()) { snprintf(why, wn, "Choose a bank on the card first"); return false; }
    if (!ls_freqd_bank_name(typed, name, sizeof(name), why, wn)) return false;
    if (!strcmp(name, ls_freqs_bank(bank))) { snprintf(why, wn, "It is called %s already", name); return false; }
    bank_path(ls_freqs_bank(bank), from, sizeof(from));
    bank_path(name, to, sizeof(to));
    if (stat(to, &st) == 0) { snprintf(why, wn, "There is a bank called %s already", name); return false; }
    if (rename(from, to) != 0) { snprintf(why, wn, "Could not rename %s", ls_freqs_bank(bank)); return false; }
    snprintf(why, wn, "%s is now %s", ls_freqs_bank(bank), name);
    /* What a delete kept names the old file: it has nowhere to go back to. */
    ls_freqs_undo_forget();
    ls_freqs_scan();
    ls_freqs_dirty();
    return true;
}

bool ls_freqd_bank_trash(int bank, char *why, size_t wn)
{
    char from[96], dir[96], to[128];
    if (bank < 0 || bank >= ls_freqs_banks()) { snprintf(why, wn, "Choose a bank on the card first"); return false; }
    bank_path(ls_freqs_bank(bank), from, sizeof(from));
    snprintf(dir, sizeof(dir), "%s/trash", ls_freqs_root());
    make_dir(dir);
    snprintf(to, sizeof(to), "%s/%s.csv", dir, ls_freqs_bank(bank));
    remove(to);                               /* an older one of the same name gives way */
    if (rename(from, to) != 0) { snprintf(why, wn, "Could not move %s to the trash", ls_freqs_bank(bank)); return false; }
    snprintf(why, wn, "%s.csv is in freqs/trash", ls_freqs_bank(bank));
    ls_freqs_undo_forget();
    ls_freqs_scan();
    ls_freqs_dirty();
    return true;
}

/* ------------------------------------------------------ copy and move -- */

/* What a bank already holds: the little of each entry same_channel reads. */
typedef struct { uint32_t hz; int32_t lat_e5, lon_e5; uint8_t mode; } chan_t;
typedef struct { chan_t *key; int n, cap; } keys_t;
static bool key_sink(const ls_freq_t *e, void *ctx)
{
    keys_t *k = ctx;
    if (k->n < k->cap) k->key[k->n++] = (chan_t){ e->hz, e->lat_e5, e->lon_e5, e->mode };
    return true;
}
static bool key_has(const keys_t *k, const ls_freq_t *e)
{
    for (int i = 0; i < k->n; i++) {
        const ls_freq_t y = { .hz = k->key[i].hz, .lat_e5 = k->key[i].lat_e5, .lon_e5 = k->key[i].lon_e5, .mode = k->key[i].mode };
        if (same_channel(e, &y)) return true;
    }
    return false;
}

/* The entries of `idx` that are to go to `bank`, by value, in PSRAM; the
   caller frees. `took` gets the loaded index of each. */
static ls_freq_t *gather(const uint16_t *idx, int n, const char *bank, bool skip_dups, uint16_t *took,
                         int *count, int *same, int *dups)
{
    *count = *same = *dups = 0;
    if (n < 1) return NULL;
    ls_freq_t *list = heap_caps_malloc((size_t)n * sizeof(ls_freq_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!list) return NULL;
    const int to = ls_freqs_bank_find(bank);
    keys_t keys = { NULL, 0, 0 };
    if (skip_dups) {
        keys.cap = LS_FREQ_MAX + n;
        keys.key = heap_caps_malloc((size_t)keys.cap * sizeof(chan_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!keys.key) keys.cap = 0;
        char path[96];
        bank_path(bank, path, sizeof(path));
        FILE *f = keys.key && to >= 0 ? fopen(path, "r") : NULL;
        if (f) {
            ls_freq_tally_t t;
            ls_freq_read(f, "", key_sink, &keys, &t, NULL);
            fclose(f);
        }
    }
    for (int k = 0; k < n; k++) {
        const ls_freq_t *e = ls_freqs_at(idx[k]);
        if (!e) continue;
        if (to >= 0 && e->bank == to) { (*same)++; continue; }
        if (skip_dups && key_has(&keys, e)) { (*dups)++; continue; }
        if (skip_dups) key_sink(e, &keys);          /* nor twice from this list */
        if (took) took[*count] = idx[k];
        list[(*count)++] = *e;
    }
    heap_caps_free(keys.key);
    return list;
}

static void tail(char *why, size_t wn, int same, int dups)
{
    const size_t k = strlen(why);
    if (dups && k < wn) snprintf(why + k, wn - k, "; %d already there", dups);
    else if (same && k < wn) snprintf(why + k, wn - k, "; %d were in it", same);
}

int ls_freqd_copy(const uint16_t *idx, int n, const char *bank, bool skip_dups, char *why, size_t wn)
{
    int count, same, dups;
    ls_freq_t *list = gather(idx, n, bank, skip_dups, NULL, &count, &same, &dups);
    if (!list) { snprintf(why, wn, "%s", n < 1 ? "Nothing selected" : "No memory for that many"); return 0; }
    if (!count) {
        snprintf(why, wn, "%s", dups ? "Every one of them is in that bank already" : "They are in that bank already");
        heap_caps_free(list);
        return 0;
    }
    const bool ok = ls_freqs_append_many(bank, list, count, why, wn);
    heap_caps_free(list);
    if (!ok) return 0;
    snprintf(why, wn, "Copied %d to %s", count, bank);
    tail(why, wn, same, dups);
    return count;
}

int ls_freqd_move(const uint16_t *idx, int n, const char *bank, char *why, size_t wn)
{
    int count, same, dups;
    if (n < 1) { snprintf(why, wn, "Nothing selected"); return 0; }
    uint16_t *took = heap_caps_malloc((size_t)n * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ls_freq_t *list = took ? gather(idx, n, bank, false, took, &count, &same, &dups) : NULL;
    if (!list) { heap_caps_free(took); snprintf(why, wn, "No memory for that many"); return 0; }
    int moved = 0;
    if (!count) snprintf(why, wn, "They are in that bank already");
    else {
        /* Out of their files first, while the lines they were read from are
           still where they were; a write that then fails puts them back. */
        int fixed = 0;
        for (int k = 0; k < count; k++) fixed += list[k].bank == LS_FREQ_BANK_BUILTIN;
        const int gone = ls_freqs_remove(took, count, why, wn);
        if (gone != count - fixed) {
            /* A file changed under the list: nothing is half moved. */
            if (gone) ls_freqs_undo(why, wn);
            snprintf(why, wn, "A file changed on the card; nothing moved");
        } else if (!ls_freqs_append_many(bank, list, count, why, wn)) {
            char back[64];
            if (gone) ls_freqs_undo(back, sizeof(back));
        } else {
            moved = count;
            ls_freqs_undo_forget();
            snprintf(why, wn, "Moved %d to %s", gone, bank);
            if (fixed) {
                const size_t k = strlen(why);
                snprintf(why + k, wn - k, "; %d built-in copied", fixed);
            }
            tail(why, wn, same, 0);
        }
    }
    heap_caps_free(list);
    heap_caps_free(took);
    return moved;
}

/* --------------------------------------------------------------- CHIRP -- */

bool ls_freqd_export_chirp(const uint16_t *idx, int n, const char *path, char *why, size_t wn)
{
    char dir[96];
    snprintf(dir, sizeof(dir), "%s/export", ls_freqs_root());
    make_dir(ls_freqs_root());
    make_dir(dir);
    FILE *f = fopen(path, "w");
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (!f) { snprintf(why, wn, "Could not create %s", base); return false; }
    bool ok = fputs("Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,DtcsPolarity,"
                    "Mode,TStep,Skip,Comment,URCALL,RPT1CALL,RPT2CALL,DVCODE\n", f) >= 0;
    int wrote = 0, left = 0;
    for (int k = 0; ok && k < n; k++) {
        const ls_freq_t *e = ls_freqs_at(idx[k]);
        if (!e) continue;
        const char *mode = e->mode == LS_FM_NFM ? (e->bw && e->bw <= 125 ? "NFM" : "FM")
                         : e->mode == LS_FM_WFM ? "WFM" : e->mode == LS_FM_AM ? "AM" : NULL;
        if (!mode) { left++; continue; }
        /* Tone the way the importer reads it back: TSQL for a CTCSS tone,
           DTCS for D023N / D023I. */
        const char *tone = "", *pol = "NN";
        char ctcss[12] = "88.5", dcs[8] = "023";
        if (e->tone[0] == 'D' && isdigit((unsigned char)e->tone[1])) {
            tone = "DTCS";
            snprintf(dcs, sizeof(dcs), "%03d", atoi(e->tone + 1));
            if (strchr(e->tone, 'I')) pol = "RN";
        } else if (e->tone[0]) {
            tone = "TSQL";
            snprintf(ctcss, sizeof(ctcss), "%.1f", atof(e->tone));
        }
        /* "shift -0.6" in the notes is the repeater's, as an import left it. */
        char duplex[2] = "", offset[16] = "0.000000", name[LS_FREQ_NAME], note[LS_FREQ_NOTES];
        const char *s = strstr(e->notes, "shift ");
        if (s && (s[6] == '+' || s[6] == '-') && atof(s + 7) > 0) {
            duplex[0] = s[6];
            snprintf(offset, sizeof(offset), "%.6f", atof(s + 7));
        }
        snprintf(name, sizeof(name), "%s", e->name);
        snprintf(note, sizeof(note), "%s", e->notes);
        for (char *p = name; *p; p++) if (*p == ',' || *p == '"') *p = ' ';
        for (char *p = note; *p; p++) if (*p == ',' || *p == '"') *p = ';';
        ok = fprintf(f, "%d,%s,%lu.%06lu,%s,%s,%s,%s,%s,%s,%s,%s,5.00,,%s,,,,\n", wrote, name,
                     (unsigned long)(e->hz / 1000000u), (unsigned long)(e->hz % 1000000u), duplex, offset, tone,
                     ctcss, ctcss, dcs, pol, mode, note) > 0;
        wrote += ok;
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok) snprintf(why, wn, "Could not write %s", base);
    else if (left) snprintf(why, wn, "Wrote %d to %s; %d not FM or AM left out", wrote, base, left);
    else snprintf(why, wn, "Wrote %d to %s", wrote, base);
    return ok && wrote > 0;
}
