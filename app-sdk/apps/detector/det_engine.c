/* DETECTOR's engine, from the built-in one (firmware 2.21): the same rules
 * file, predicates, per-entity and per-rule cooldowns, the global token
 * bucket, quiet hours and the shared alert line. Single-threaded: the app
 * calls it from its screen or its service tick. See det.h. */
#include "det.h"
#include <stdio.h>
#include <string.h>

static bool digit(int c) { return c >= '0' && c <= '9'; }
static bool alnum(int c) { return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int hexval(int c) { return digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; }

static const char defaults[] =
    "{\"version\":1,\"sweep_rules\":\"/sdcard/sweep/rules.json\",\"defaults\":{\"enabled\":false,\"cooldown_s\":60,\"dedupe\":\"rule_entity\",\"rule_cooldown_s\":120},\"rules\":["
    "{\"id\":\"remote-id\",\"enabled\":true,\"sources\":[\"rid.ble\"],\"match\":{\"validated_message\":true},\"sinks\":[\"vibrate\",\"banner\",\"sd\"]},"
    "{\"id\":\"with-you-trackers\",\"enabled\":true,\"sources\":[\"sweep.ble\",\"sweep.wifi\"],\"match\":{\"category_in\":[\"TRACKER\"],\"section_in\":[\"with_you\"]},\"sinks\":[\"vibrate\",\"banner\"]},"
    "{\"id\":\"cameras\",\"enabled\":true,\"sources\":[\"sweep.ble\",\"sweep.wifi\"],\"match\":{\"category_in\":[\"CAMERA\"]},\"sinks\":[\"banner\"]},"
    "{\"id\":\"bodycam-example\",\"sources\":[\"sweep.ble\"],\"match\":{\"category_in\":[\"BODYCAM\"]},\"sinks\":[\"banner\"]},"
    "{\"id\":\"wifi-example\",\"sources\":[\"sweep.wifi\"],\"match\":{\"category_in\":[\"CAMERA\"]},\"sinks\":[\"sd\"]},"
    "{\"id\":\"drone-link-example\",\"enabled\":false,\"sources\":[\"lora.dronelink\"],\"match\":{\"band_in\":[\"868\",\"915\"],\"min_rssi_dbm\":-100,\"min_hits_per_min\":3},\"sinks\":[\"banner\",\"sd\"]},"
    "{\"id\":\"air-emergency-squawk\",\"enabled\":true,\"rule_cooldown_s\":0,\"sources\":[\"adsb\"],\"match\":{\"squawk_in\":[\"7500\",\"7600\",\"7700\"]},\"sinks\":[\"vibrate\",\"banner\"]},"
    "{\"id\":\"air-emergency-status\",\"enabled\":true,\"rule_cooldown_s\":0,\"sources\":[\"adsb\"],\"match\":{\"emergency_status\":true},\"sinks\":[\"vibrate\",\"banner\"]},"
    "{\"id\":\"air-nearby\",\"sources\":[\"adsb\"],\"match\":{\"distance_km_within\":10},\"sinks\":[\"banner\"]},"
    "{\"id\":\"air-low\",\"sources\":[\"adsb\"],\"match\":{\"altitude_baro_ft_below\":1500},\"sinks\":[\"banner\"]},"
    "{\"id\":\"air-category\",\"sources\":[\"adsb\"],\"match\":{\"adsb_category_in\":[\"rotorcraft\"]},\"sinks\":[\"banner\"]}]}";

/* ---- names ---------------------------------------------------------------- */

const char *det_source_name(unsigned s) {
    switch (s) {
    case DET_RID: return "rid.ble";
    case DET_BLE: return "sweep.ble";
    case DET_WIFI: return "sweep.wifi";
    case DET_LORA: return "lora.dronelink";
    case DET_ADSB: return "adsb";
    case DET_DRONE_WATCH: return "drone.watch";
    case DET_REC: return "rec.watch";
    case DET_CATCH: return "rec.catch";
    case DET_POCSAG: return "pocsag";
    case DET_P25: return "p25";
    case DET_MESH_PEER: return "mesh.peer";
    case DET_MESH_ALERT: return "mesh.alert";
    default: return "detector.summary";
    }
}
const char *det_category_name(unsigned c) {
    static const char *const names[] = {"CAMERA", "BODYCAM", "DRONE", "TRACKER", "ATTACK"};
    return c < DET_CATS ? names[c] : "UNKNOWN";
}
static const char *const air_categories[16] = {
    "light", "small", "large", "high_vortex", "heavy", "high_performance", "rotorcraft",
    "glider", "lighter_than_air", "parachutist", "ultralight", "uav", "space",
    "emergency_vehicle", "service_vehicle", "obstacle"};
const char *det_air_category_name(unsigned bit) { return bit < 16 ? air_categories[bit] : "?"; }
/* Set A is type code 4, B 3, C 2; category 0 and reserved entries name nothing. */
int det_air_category_bit(const char *c) {
    if (!c || !c[0] || !digit(c[1]) || c[2]) return -1;
    const unsigned ca = (unsigned)(c[1] - '0');
    const unsigned tc = c[0] == 'A' ? 4 : c[0] == 'B' ? 3 : c[0] == 'C' ? 2 : 0;
    if (tc == 4 && ca >= 1 && ca <= 7) return (int)ca - 1;
    if (tc == 3 && ca >= 1 && ca <= 7 && ca != 5) return ca == 6 ? 11 : ca == 7 ? 12 : (int)ca + 6;
    if (tc == 2 && ca >= 1 && ca <= 5) return ca == 1 ? 13 : ca == 2 ? 14 : 15;
    return -1;
}
const char *det_note_name(unsigned n) {
    static const char *const w[] = {"-", "SENT", "HELD 1/min", "REPEAT", "TX OFF", "NO CHANNEL", "RECEIVED", "QUIET", "FAILED"};
    return n < sizeof(w) / sizeof(*w) ? w[n] : "?";
}

/* ---- the rules file -------------------------------------------------------
 * Fixed-depth grammar. No recursion, unknown or duplicate keys, coercion or
 * partial numbers. */
typedef struct { const char *p, *end; } json_t;
static void ws(json_t *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\r' || *j->p == '\n' || *j->p == '\t')) j->p++; }
static bool take(json_t *j, char c) { ws(j); if (j->p == j->end || *j->p != c) return false; j->p++; return true; }
static bool str(json_t *j, char *out, size_t cap) {
    if (!take(j, '"')) return false;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') { if (j->p == j->end) return false; c = (unsigned char)*j->p++; if (c != '"' && c != '\\' && c != '/') return false; }
        if (c < 32 || c > 126 || n + 1 >= cap) return false;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return take(j, '"');
}
static bool boolean(json_t *j, bool *out) {
    ws(j);
    size_t n = (size_t)(j->end - j->p);
    if (n >= 4 && !memcmp(j->p, "true", 4)) { *out = true; j->p += 4; return true; }
    if (n >= 5 && !memcmp(j->p, "false", 5)) { *out = false; j->p += 5; return true; }
    return false;
}
static bool digits(json_t *j, uint32_t *out, unsigned most) {
    ws(j);
    if (j->p == j->end || !digit(*j->p)) return false;
    uint32_t v = 0; bool zero = *j->p == '0'; unsigned n = 0;
    while (j->p < j->end && digit(*j->p)) { if (++n > most || (zero && n > 1)) return false; v = v * 10 + (uint32_t)(*j->p++ - '0'); }
    *out = v;
    return true;
}
static bool number(json_t *j, uint32_t *out) { return digits(j, out, 6); }
static bool hex6(const char *s, uint32_t *v) {
    if (strlen(s) != 6) return false;
    uint32_t x = 0;
    for (; *s; s++) { int d = hexval((unsigned char)*s); if (d < 0) return false; x = x << 4 | (uint32_t)d; }
    *v = x;
    return true;
}
/* Aircraft watch lists: bounded, strictly spelled, no duplicates. An empty
   list is accepted and matches nothing. */
static bool air_list(json_t *j, det_rule_t *r, int kind) {
    if (!take(j, '[')) return false;
    r->air_mask |= kind == 0 ? DET_AIR_SQUAWK : kind == 1 ? DET_AIR_CALL : kind == 4 ? DET_AIR_CAT : DET_AIR_ICAO;
    if (take(j, ']')) return true;
    do {
        char s[32];
        if (kind == 3) {
            uint32_t lo, hi;
            if (!take(j, '[') || !str(j, s, sizeof(s)) || !hex6(s, &lo) || !take(j, ',') || !str(j, s, sizeof(s)) ||
                !hex6(s, &hi) || lo > hi || !take(j, ']'))
                return false;
            if (r->air_icaos >= DET_AIR_LIST) return false;
            r->air_icao_lo[r->air_icaos] = lo; r->air_icao_hi[r->air_icaos++] = hi;
            continue;
        }
        if (!str(j, s, sizeof(s))) return false;
        if (kind == 0) {
            if (strlen(s) != 4 || r->air_squawks >= DET_AIR_LIST) return false;
            unsigned v = 0;
            for (const char *p = s; *p; p++) { if (*p < '0' || *p > '7') return false; v = v * 10 + (unsigned)(*p - '0'); }
            for (unsigned i = 0; i < r->air_squawks; i++) if (r->air_squawk[i] == v) return false;
            r->air_squawk[r->air_squawks++] = (uint16_t)v;
        } else if (kind == 1) {
            size_t n = strlen(s);
            if (!n || n > 8 || r->air_calls >= DET_AIR_LIST) return false;
            for (const char *p = s; *p; p++) if (!((*p >= 'A' && *p <= 'Z') || digit(*p))) return false;
            for (unsigned i = 0; i < r->air_calls; i++) if (!strcmp(r->air_call[i], s)) return false;
            strcpy(r->air_call[r->air_calls++], s);
        } else if (kind == 2) {
            uint32_t v;
            if (!hex6(s, &v) || r->air_icaos >= DET_AIR_LIST) return false;
            r->air_icao_lo[r->air_icaos] = r->air_icao_hi[r->air_icaos] = v; r->air_icaos++;
        } else {
            unsigned bit = 16;
            for (unsigned c = 0; c < 16; c++) if (!strcmp(s, air_categories[c])) bit = c;
            if (bit == 16 || (r->air_cats & (1u << bit))) return false;
            r->air_cats |= (uint16_t)(1u << bit);
        }
    } while (take(j, ','));
    return take(j, ']');
}
static bool wide_list(json_t *j, uint16_t *out, int kind) {
    if (!take(j, '[')) return false;
    *out = 0;
    if (kind == 2 && take(j, ']')) return true;
    static const char *const sinks[] = {"banner", "vibrate", "sd", "tts", "tone", "flipper", "mesh"};
    do {
        char s[32]; unsigned bit = 0;
        if (!str(j, s, sizeof(s))) return false;
        if (kind == 0) { for (unsigned b = 1; b <= DET_MESH_ALERT; b <<= 1) if (!strcmp(s, det_source_name(b))) bit = b; }
        if (kind == 1) { for (unsigned c = 0; c < DET_CATS; c++) if (!strcmp(s, det_category_name(c))) bit = 1u << c; }
        if (kind == 2) { for (unsigned i = 0; i < 7; i++) if (!strcmp(s, sinks[i])) bit = 1u << i; }
        if (kind == 3) { if (!strcmp(s, "with_you")) bit = 1; if (!strcmp(s, "new")) bit = 2; if (!strcmp(s, "passing")) bit = 4; }
        if (kind == 4) { if (!strcmp(s, "868")) bit = 1; if (!strcmp(s, "915")) bit = 2; }
        if (kind == 5) { if (!strcmp(s, "new")) bit = 1; if (!strcmp(s, "lost")) bit = 2; }
        if (!bit || (*out & bit)) return false;
        *out |= (uint16_t)bit;
    } while (take(j, ','));
    return take(j, ']');
}
static bool list(json_t *j, uint8_t *out, int kind) { uint16_t v; if (!wide_list(j, &v, kind)) return false; *out = (uint8_t)v; return true; }
/* Up to eight numbers of up to eight digits, or four words. */
static bool sig_list(json_t *j, det_rule_t *r, bool words) {
    if (!take(j, '[')) return false;
    do {
        if (words) {
            if (r->words >= 4) return false;
            char *w = r->word[r->words];
            if (!str(j, w, sizeof(r->word[0])) || !w[0]) return false;
            for (char *p = w; *p; p++) *p = (char)upper((unsigned char)*p);
            r->words++;
        } else if (r->nums >= 8 || !digits(j, &r->num[r->nums++], 8)) return false;
    } while (take(j, ','));
    return take(j, ']');
}
static bool predicate(json_t *j, det_rule_t *r) {
    if (!take(j, '{')) return false;
    unsigned seen = 0;
    do {
        char k[32]; unsigned b = 0;
        if (!str(j, k, sizeof(k)) || !take(j, ':')) return false;
        if (!strcmp(k, "category_in")) { b = 1; if (!list(j, &r->categories, 1)) return false; }
        else if (!strcmp(k, "band_in")) { b = 8; if (!list(j, &r->bands, 4)) return false; }
        else if (!strcmp(k, "min_hits_per_min")) { b = 16; if (!number(j, &r->min_hits_per_min)) return false; }
        else if (!strcmp(k, "min_rssi_dbm")) {
            b = 32; bool negative = take(j, '-'); uint32_t v;
            if (!number(j, &v) || v > 127 || (!negative && v)) return false;
            r->min_rssi_dbm = (int8_t)-(int)v; r->has_min_rssi = true;
        }
        else if (!strcmp(k, "section_in")) { b = 4; if (!list(j, &r->sections, 3)) return false; }
        else if (!strcmp(k, "validated_message")) { b = 2; r->has_validated = true; if (!boolean(j, &r->validated)) return false; }
        else if (!strcmp(k, "squawk_in")) { b = 64; if (!air_list(j, r, 0)) return false; }
        else if (!strcmp(k, "callsign_prefix_in")) { b = 128; if (!air_list(j, r, 1)) return false; }
        else if (!strcmp(k, "icao_in")) { b = 256; if (!air_list(j, r, 2)) return false; }
        else if (!strcmp(k, "icao_ranges")) { b = 512; if (!air_list(j, r, 3)) return false; }
        else if (!strcmp(k, "adsb_category_in")) { b = 1024; if (!air_list(j, r, 4)) return false; }
        else if (!strcmp(k, "emergency_status")) { b = 2048; bool v; if (!boolean(j, &v) || !v) return false; r->air_mask |= DET_AIR_EMERG; }
        else if (!strcmp(k, "distance_km_within")) {
            b = 4096; uint32_t v;
            if (!number(j, &v) || v < 1 || v > 1000) return false;
            r->air_km = (uint16_t)v; r->air_mask |= DET_AIR_KM;
        }
        else if (!strcmp(k, "altitude_baro_ft_below")) {
            b = 8192; uint32_t v;
            if (!number(j, &v) || v > 60000) return false;
            r->air_alt_below_ft = (int32_t)v; r->air_mask |= DET_AIR_ALT;
        }
        else if (!strcmp(k, "capcode_in") || !strcmp(k, "talkgroup_in") || !strcmp(k, "unit_in")) {
            b = 16384; r->num_key = k[0] == 'c' ? 1 : k[0] == 't' ? 2 : 3; r->sig_mask |= DET_SIG_NUM;
            if (!sig_list(j, r, false)) return false;
        }
        else if (!strcmp(k, "text_contains_any_ci")) { b = 32768; r->sig_mask |= DET_SIG_WORDS; if (!sig_list(j, r, true)) return false; }
        else if (!strcmp(k, "frequency_khz")) {
            b = 65536; r->sig_mask |= DET_SIG_FREQ;
            if (!take(j, '[') || !digits(j, &r->khz_lo, 7) || !take(j, ',') || !digits(j, &r->khz_hi, 7) ||
                r->khz_lo > r->khz_hi || !take(j, ']'))
                return false;
        }
        else if (!strcmp(k, "novel_pattern") || !strcmp(k, "emergency")) {
            bool v; b = k[0] == 'n' ? 131072 : 262144;
            if (!boolean(j, &v) || !v) return false;
            r->sig_mask |= k[0] == 'n' ? DET_SIG_NOVEL : DET_SIG_EMERG;
        }
        else if (!strcmp(k, "event_in")) { b = 524288; r->sig_mask |= DET_SIG_EVENT; if (!list(j, &r->events, 5)) return false; }
        else return false;
        if (seen & b) return false;
        seen |= b;
    } while (take(j, ','));
    return take(j, '}');
}
static bool object(json_t *j, det_rule_t *r, bool def) {
    if (!take(j, '{')) return false;
    unsigned seen = 0;
    do {
        char k[32], s[32]; unsigned b = 0;
        if (!str(j, k, sizeof(k)) || !take(j, ':')) return false;
        if (!strcmp(k, "enabled")) { b = 1; if (!boolean(j, &r->enabled)) return false; }
        else if (!strcmp(k, "cooldown_s")) { b = 2; if (!number(j, &r->cooldown_s) || r->cooldown_s > 86400) return false; }
        else if (!strcmp(k, "rule_cooldown_s")) { b = 128; if (!number(j, &r->rule_cooldown_s) || r->rule_cooldown_s > 86400) return false; }
        else if (!strcmp(k, "dedupe")) { b = 4; if (!str(j, s, sizeof(s)) || strcmp(s, "rule_entity")) return false; }
        else if (!def && !strcmp(k, "id")) {
            b = 8;
            if (!str(j, r->id, sizeof(r->id)) || !r->id[0]) return false;
            for (const char *p = r->id; *p; p++) if (!alnum((unsigned char)*p) && *p != '-' && *p != '_') return false;
        }
        else if (!def && !strcmp(k, "sources")) { b = 16; if (!wide_list(j, &r->sources, 0)) return false; }
        else if (!def && !strcmp(k, "match")) { b = 32; if (!predicate(j, r)) return false; }
        else if (!def && !strcmp(k, "sinks")) { b = 64; if (!list(j, &r->sinks, 2)) return false; }
        else return false;
        if (seen & b) return false;
        seen |= b;
    } while (take(j, ','));
    if (!take(j, '}')) return false;
    if (def) return true;
    if ((seen & 120) != 120) return false;
    /* An aircraft rule watches only aircraft and says what for; signal
       rules likewise take signal sources and predicates only. */
    const bool other = r->categories || r->bands || r->min_hits_per_min || r->has_min_rssi || r->sections || r->has_validated;
    if (r->sources & DET_ADSB) return r->sources == DET_ADSB && !other && r->air_mask && !r->sig_mask;
    if (r->sources & DET_SIGNALS) return !(r->sources & ~DET_SIGNALS) && !other && !r->air_mask;
    return !r->air_mask && !r->sig_mask;
}
/* "quiet_hours":{"start":22,"end":7,"utc_offset":-5}, "mesh":{"channel":1,"share":true} */
static bool prefs_object(det_t *d, json_t *j, bool mesh) {
    if (!take(j, '{')) return false;
    unsigned seen = 0;
    do {
        char k[16]; unsigned b;
        if (!str(j, k, sizeof(k)) || !take(j, ':')) return false;
        if (mesh && !strcmp(k, "share")) { b = 1; if (!boolean(j, &d->staged_share)) return false; }
        else {
            bool neg = take(j, '-'); uint32_t v;
            if (!number(j, &v) || v > 23) return false;
            const int x = neg ? -(int)v : (int)v;
            if (mesh && !strcmp(k, "channel")) { b = 2; if (x < 1 || x >= 4) return false; d->staged_prefs.mesh_chan = (uint8_t)x; }
            else if (!mesh && !strcmp(k, "utc_offset")) { b = 4; if (x < -12 || x > 14) return false; d->staged_prefs.utc_offset = (int8_t)x; }
            else if (!mesh && (!strcmp(k, "start") || !strcmp(k, "end"))) {
                b = k[0] == 's' ? 1 : 2;
                if (neg) return false;
                if (b == 1) d->staged_prefs.quiet_start = (int8_t)x; else d->staged_prefs.quiet_end = (int8_t)x;
            } else return false;
        }
        if (seen & b) return false;
        seen |= b;
    } while (take(j, ','));
    return take(j, '}');
}
static bool parse(det_t *d, const char *text, size_t len, unsigned *n) {
    json_t j = {text, text + len};
    unsigned seen = 0;
    det_rule_t def = {.cooldown_s = 60, .rule_cooldown_s = 120};
    /* Defaults may come after the rules: two passes, both validating. */
    for (int pass = 0; pass < 2; pass++) {
        j.p = text; seen = 0; *n = 0;
        if (!take(&j, '{')) return false;
        do {
            char k[32], s[64]; uint32_t v; unsigned b = 0;
            if (!str(&j, k, sizeof(k)) || !take(&j, ':')) return false;
            if (!strcmp(k, "version")) { b = 1; if (!number(&j, &v) || v != 1) return false; }
            else if (!strcmp(k, "sweep_rules")) { b = 2; if (!str(&j, s, sizeof(s)) || strcmp(s, "/sdcard/sweep/rules.json")) return false; }
            else if (!strcmp(k, "quiet_hours")) { b = 16; if (!prefs_object(d, &j, false)) return false; }
            else if (!strcmp(k, "mesh")) { b = 32; if (!prefs_object(d, &j, true)) return false; }
            else if (!strcmp(k, "defaults")) {
                b = 4; det_rule_t x = {.cooldown_s = 60, .rule_cooldown_s = 120};
                if (!object(&j, &x, true)) return false;
                def = x;
            } else if (!strcmp(k, "rules")) {
                b = 8;
                if (!take(&j, '[')) return false;
                if (!take(&j, ']')) {
                    do {
                        if (*n >= DET_RULES) return false;
                        d->staging[*n] = def;
                        if (!object(&j, &d->staging[*n], false)) return false;
                        for (unsigned i = 0; i < *n; i++) if (!strcmp(d->staging[i].id, d->staging[*n].id)) return false;
                        (*n)++;
                    } while (take(&j, ','));
                    if (!take(&j, ']')) return false;
                }
            } else return false;
            if (seen & b) return false;
            seen |= b;
        } while (take(&j, ','));
        if (!take(&j, '}')) return false;
        ws(&j);
        if (j.p != j.end || (seen & 9) != 9) return false;
    }
    d->root_seen = seen;
    return true;
}
bool det_parse(det_t *d, const char *text, size_t len) {
    unsigned n = 0;
    d->staged_prefs = d->prefs;
    d->staged_share = false;
    const bool ok = text && len <= DET_RULES_BYTES && parse(d, text, len, &n);
    if (ok) {
        memcpy(d->rules, d->staging, n * sizeof(*d->rules));
        d->n_rules = n;
        d->revision++;
        memset(d->cooldown, 0, sizeof(d->cooldown));
        memset(d->window, 0, sizeof(d->window));
        d->error[0] = 0;
        if (d->root_seen & 16) d->staged_prefs.quiet_on = true;
        if (d->root_seen & 48) d->prefs = d->staged_prefs;
        if (d->staged_share) d->sinks |= DET_MESH;
    } else {
        snprintf(d->error, sizeof(d->error), "Rules rejected: invalid/unsupported v1 schema; previous rules retained");
    }
    return ok;
}

void det_init(det_t *d) {
    det_deliver_fn deliver = d->deliver;
    void *user = d->user;
    memset(d, 0, sizeof(*d));
    d->deliver = deliver;
    d->user = user;
    d->sources = DET_SOURCES;
    d->sinks = DET_BANNER | DET_VIBRATE | DET_TTS | DET_TONE;
    d->prefs = (det_prefs_t){.quiet_start = 22, .quiet_end = 7, .mesh_chan = 1, .mesh_rx_sinks = DET_BANNER | DET_TONE};
    d->tokens = 4;
    det_parse(d, defaults, strlen(defaults));
}
bool det_rule_set(det_t *d, unsigned i, const det_rule_t *r) {
    if (i >= d->n_rules || strcmp(d->rules[i].id, r->id) || r->cooldown_s > 86400 || r->rule_cooldown_s > 86400 || (r->sinks & ~127))
        return false;
    det_rule_t *x = &d->rules[i];
    x->enabled = r->enabled; x->cooldown_s = r->cooldown_s; x->rule_cooldown_s = r->rule_cooldown_s; x->sinks = r->sinks;
    /* A threshold a rule has can be tuned in the session; a predicate cannot be added. */
    if ((x->air_mask & DET_AIR_KM) && r->air_km >= 1 && r->air_km <= 1000) x->air_km = r->air_km;
    if ((x->air_mask & DET_AIR_ALT) && r->air_alt_below_ft >= 0 && r->air_alt_below_ft <= 60000) x->air_alt_below_ft = r->air_alt_below_ft;
    d->revision++;
    return true;
}
bool det_prefs_set(det_t *d, const det_prefs_t *p) {
    const bool ok = p->quiet_start >= 0 && p->quiet_start < 24 && p->quiet_end >= 0 && p->quiet_end < 24 &&
                    p->utc_offset >= -12 && p->utc_offset <= 14 && p->mesh_chan >= 1 && p->mesh_chan < 4 &&
                    !(p->mesh_rx_sinks & ~(DET_BANNER | DET_VIBRATE | DET_TTS | DET_TONE));
    if (ok) d->prefs = *p;
    return ok;
}
bool det_quiet_at(bool on, int start, int end, int offset, int64_t utc) {
    if (!on || start == end) return false;
    if (utc < 1704067200) return true;          /* unknown wall time: assume quiet */
    const int h = (int)(((utc / 3600 + offset) % 24 + 24) % 24);
    return start < end ? (h >= start && h < end) : (h >= start || h < end);
}
bool det_quiet(const det_t *d) {
    const det_prefs_t *p = &d->prefs;
    return det_quiet_at(p->quiet_on, p->quiet_start, p->quiet_end, p->utc_offset, d->utc);
}
bool det_wants(const det_t *d, unsigned source) {
    if (!(d->sources & source)) return false;
    for (unsigned i = 0; i < d->n_rules; i++) if (d->rules[i].enabled && (d->rules[i].sources & source)) return true;
    return false;
}
uint32_t det_seen_mask(const det_t *d) {
    uint32_t m = d->sources & (DET_RID | DET_BLE | DET_WIFI | DET_REC | DET_CATCH | DET_POCSAG | DET_P25);
    if (det_wants(d, DET_LORA)) m |= DET_LORA;
    /* Drone watch always: with no rule of its own the built-in one raises it. */
    if (d->sources & DET_DRONE_WATCH) m |= DET_DRONE_WATCH;
    return m;
}

/* ---- cooldowns and the alert itself -------------------------------------- */

static bool gate(det_t *d, unsigned rule, uint64_t entity, int64_t now, uint32_t seconds) {
    int slot = -1;
    for (int i = 0; i < DET_COOLDOWNS; i++) {
        if (d->cooldown[i].until && d->cooldown[i].entity == entity && d->cooldown[i].rule == rule) {
            if (now < d->cooldown[i].until) return false;
            slot = i;
            break;
        }
        if (slot < 0 && d->cooldown[i].until <= now) slot = i;
    }
    if (slot < 0) return false;                 /* full: fail closed, never evict a live cooldown */
    d->cooldown[slot].entity = entity;
    d->cooldown[slot].until = now + (int64_t)seconds * 1000000;
    d->cooldown[slot].rule = (uint8_t)rule;
    return true;
}
static bool token(det_t *d, int64_t now) {
    if (now > d->bucket_at) {
        const int64_t add = (now - d->bucket_at) / 2000000;
        if (add) { d->tokens = add >= 4 ? 4 : d->tokens + (unsigned)add; if (d->tokens > 4) d->tokens = 4; d->bucket_at = now; }
    }
    if (!d->tokens) return false;
    d->tokens--;
    return true;
}
static void finish(det_t *d, det_event_t *e) {
    uint8_t effective = e->selected & d->sinks;
    /* A received alert is never shared again: no forwarding, no loops. */
    if (e->source == DET_MESH_ALERT && (effective & DET_MESH)) { effective &= (uint8_t)~DET_MESH; e->mesh_note = DET_NOTE_RELAYED; }
    effective &= (uint8_t)~DET_FLIPPER;
    /* Quiet hours hold back sound, speech, vibration and (unless allowed)
       the mesh. The banner and the SD log carry on. */
    const uint8_t hush = DET_TTS | DET_TONE | DET_VIBRATE | (d->prefs.quiet_mesh ? 0 : DET_MESH);
    if ((effective & hush) && det_quiet(d)) {
        if (effective & DET_MESH) e->mesh_note = DET_NOTE_QUIET;
        effective &= (uint8_t)~hush;
        e->quiet = true;
    }
    e->allowed = effective;
    if (e->selected & ~effective & ~DET_FLIPPER) d->suppressed++;
    e->suppressed = d->suppressed;
    e->delivered = d->deliver ? d->deliver(d, e, effective) : 0;
    e->failed = effective & ~e->delivered;
    if (d->history_n < DET_HISTORY) d->history_n++;
    memmove(d->history + 1, d->history, (d->history_n - 1) * sizeof(*d->history));
    d->history[0] = *e;
}
/* A matching rule joins the event unless its per-entity cooldown or its
   rate window holds it back; held matches are counted, not lost. */
static void join_rule(det_t *d, det_event_t *e, const det_rule_t *r, unsigned i, uint64_t key, int64_t now) {
    if (now < d->window[i].until || !gate(d, i, key, now, r->cooldown_s)) {
        if (d->window[i].pending < UINT32_MAX) d->window[i].pending++;
        d->suppressed++;
        return;
    }
    d->window[i].until = now + (int64_t)r->rule_cooldown_s * 1000000;
    e->coalesced = UINT32_MAX - e->coalesced < d->window[i].pending ? UINT32_MAX : e->coalesced + d->window[i].pending;
    e->rules |= UINT64_C(1) << (i < 64 ? i : 63);
    e->selected |= r->sinks;
    const size_t n = strlen(e->reason);
    snprintf(e->reason + n, sizeof(e->reason) - n, "%s%s", n ? "," : "", r->id);
}
static void join(det_t *d, det_event_t *e, unsigned i, uint64_t key, int64_t now) { join_rule(d, e, &d->rules[i], i, key, now); }
/* Counts the match, applies the global token bucket and hands it on. */
static void emit_policy(det_t *d, det_event_t *e, int64_t now, unsigned builtin) {
    if (!e->rules && !e->builtin) return;
    d->matches++;
    if (!token(d, now)) {
        if (builtin) { if (d->window[builtin].pending < UINT32_MAX) d->window[builtin].pending++; }
        else for (unsigned i = 0; i < d->n_rules; i++)
            if ((e->rules & (UINT64_C(1) << i)) && d->window[i].pending < UINT32_MAX) d->window[i].pending++;
        d->suppressed++;
        return;
    }
    if (builtin) d->window[builtin].pending = 0;
    else for (unsigned i = 0; i < d->n_rules; i++) if (e->rules & (UINT64_C(1) << i)) d->window[i].pending = 0;
    e->sequence = ++d->sequence;
    e->suppressed = d->suppressed;
    finish(d, e);
}
static det_event_t *fresh(det_t *d, uint16_t source, int64_t now) {
    det_event_t *e = &d->work;
    memset(e, 0, sizeof(*e));
    e->at = now;
    e->source = source;
    e->category = 255;
    e->air_category = -1;
    e->altitude_ft = INT32_MIN;
    e->distance_x10_km = DET_NO_DISTANCE;
    e->revision = d->revision;
    return e;
}

/* ---- the passes ----------------------------------------------------------- */

void det_observe(det_t *d, const det_obs_t *o, int64_t now) {
    d->observations++;
    if (!(d->sources & o->source)) return;
    det_event_t *e = fresh(d, o->source, now);
    e->entity = o->entity; e->category = o->category; e->rssi = o->rssi; e->band = o->band;
    e->rate_guess = o->rate_guess; e->hits_per_min = o->hits_per_min; e->validated = o->validated;
    for (unsigned i = 0; i < d->n_rules; i++) {
        const det_rule_t *r = &d->rules[i];
        if (!r->enabled || !(r->sources & o->source) ||
            (r->categories && (o->category >= DET_CATS || !(r->categories & (1u << o->category)))) ||
            (r->bands && (o->source != DET_LORA || !(r->bands & (o->band == 868 ? 1 : 2)))) ||
            (r->has_min_rssi && o->rssi < r->min_rssi_dbm) ||
            (r->min_hits_per_min && (o->source != DET_LORA || o->hits_per_min < r->min_hits_per_min)) ||
            (r->sections && (o->section > 2 || !(r->sections & (1u << o->section)))) ||
            (r->has_validated && (o->source != DET_RID || r->validated != o->validated)))
            continue;
        join(d, e, i, o->entity, now);
    }
    emit_policy(d, e, now, 0);
}
void det_drone_watch(det_t *d, const det_obs_t *o, int64_t now) {
    d->observations++;
    det_event_t *e = fresh(d, DET_DRONE_WATCH, now);
    e->entity = o->entity; e->category = DET_CAT_DRONE; e->rssi = o->rssi;
    e->relative_rssi = !o->validated; e->band = o->band;
    bool custom = false;
    for (unsigned i = 0; i < d->n_rules; i++) {
        const det_rule_t *r = &d->rules[i];
        if (r->sources != DET_DRONE_WATCH) continue;
        custom = true;
        if (r->enabled && (!r->categories || (r->categories & (1u << DET_CAT_DRONE))) &&
            (!r->has_min_rssi || (o->validated && o->rssi >= r->min_rssi_dbm)) && !r->bands && !r->sections &&
            !r->has_validated && !r->min_hits_per_min)
            join(d, e, i, o->entity, now);
    }
    if (!custom) {
        /* Drone watch's own rule; a drone.watch rule in the file replaces it. */
        const det_rule_t r = {.id = "drone-link-nearby", .enabled = true, .cooldown_s = 0, .rule_cooldown_s = 2,
                              .sinks = DET_BANNER | DET_VIBRATE};
        join_rule(d, e, &r, DET_RULES, o->entity, now);
        if (e->reason[0]) e->builtin = 1;
    }
    if (o->approaching && (e->rules || e->builtin)) snprintf(e->reason, sizeof(e->reason), "drone link nearby: approaching (RSSI rising)");
    emit_policy(d, e, now, custom ? 0 : DET_RULES);
}

/* Signal predicates, ANDed, each only on the sources it describes. A pager
   match keeps the word that matched, never the page. */
static bool sig_match(const det_rule_t *r, const det_signal_t *s, det_event_t *e) {
    const uint8_t m = r->sig_mask;
    const unsigned src = s->source;
    if (m & DET_SIG_NUM) {
        if (r->num_key == 1 ? src != DET_POCSAG : src != DET_P25) return false;
        const uint32_t v = r->num_key == 3 ? s->b : s->a;
        bool hit = false;
        for (unsigned i = 0; i < r->nums; i++) hit |= r->num[i] == v;
        if (!hit) return false;
    }
    if ((m & (DET_SIG_FREQ | DET_SIG_NOVEL)) && !(src & (DET_REC | DET_CATCH))) return false;
    if ((m & DET_SIG_FREQ) && (s->a / 1000 < r->khz_lo || s->a / 1000 > r->khz_hi)) return false;
    if ((m & DET_SIG_NOVEL) && !(s->flag & 1)) return false;
    if ((m & DET_SIG_EMERG) && (!(src & (DET_P25 | DET_MESH_ALERT)) || !(s->flag & 1))) return false;
    if ((m & DET_SIG_EVENT) && (src != DET_MESH_PEER || !(r->events & (1u << s->kind)))) return false;
    if (m & DET_SIG_WORDS) {
        if (!(src & (DET_POCSAG | DET_MESH_ALERT))) return false;
        char up[sizeof(s->text)];
        size_t n = 0;
        for (; n + 1 < sizeof(up) && s->text[n]; n++) up[n] = (char)upper((unsigned char)s->text[n]);
        up[n] = 0;
        unsigned i = 0;
        while (i < r->words && !strstr(up, r->word[i])) i++;
        if (i == r->words) return false;
        if (src == DET_POCSAG) snprintf(e->text, sizeof(e->text), "%s", r->word[i]);
    }
    return true;
}
static uint64_t sig_entity(const det_signal_t *s) {
    uint64_t k = s->source == DET_P25 ? ((uint64_t)s->a << 32 | s->b) : s->source == DET_REC ? s->b
               : s->source == DET_CATCH ? s->a / 25000 : s->a;
    for (const char *p = s->who; *p; p++) k = (k ^ (uint8_t)*p) * UINT64_C(1099511628211);
    if (s->source == DET_MESH_PEER) k ^= (uint64_t)s->kind << 60;
    return (k ^ ((uint64_t)s->source << 48)) * UINT64_C(0x9E3779B97F4A7C15);
}
void det_signal(det_t *d, const det_signal_t *s, int64_t now) {
    d->observations++;
    if (!(d->sources & s->source)) return;
    det_event_t *e = fresh(d, s->source, now);
    e->rssi = s->rssi;
    e->num[0] = s->source == DET_MESH_PEER ? s->kind : s->a;
    e->num[1] = s->b;
    e->remote = s->kind;
    e->emergency = (s->source & (DET_P25 | DET_MESH_ALERT)) ? s->flag : 0;
    e->validated = (s->flag & 1) && (s->source & (DET_REC | DET_CATCH));
    memcpy(e->origin, s->who, sizeof(e->origin) - 1);
    memcpy(e->name, s->name, sizeof(e->name) - 1);
    if (s->source != DET_POCSAG) snprintf(e->text, sizeof(e->text), "%s", s->text);
    if (s->source == DET_MESH_ALERT) { e->distance_x10_km = s->distance_x10_km; e->bearing = s->bearing; }
    const uint64_t entity = sig_entity(s);
    e->entity = entity;
    bool matched = false;
    for (unsigned i = 0; i < d->n_rules; i++) {
        const det_rule_t *r = &d->rules[i];
        if (r->enabled && (r->sources & s->source) && sig_match(r, s, e)) { matched = true; join(d, e, i, entity, now); }
    }
    /* An alert another board shared that no rule takes raises here by itself,
       with the Mesh share options' sinks. */
    unsigned builtin = 0;
    if (s->source == DET_MESH_ALERT && !matched) {
        const det_rule_t r = {.id = "mesh-alert", .enabled = true, .sinks = d->prefs.mesh_rx_sinks};
        builtin = DET_RULES + 1;
        join_rule(d, e, &r, builtin, entity, now);
        if (e->reason[0]) e->builtin = 2;
    }
    emit_policy(d, e, now, builtin);
}

bool det_air_alarm(unsigned squawk, unsigned emergency) {
    return emergency || squawk == 7500 || squawk == 7600 || squawk == 7700;
}
bool det_air_match(const det_rule_t *r, const det_air_t *a) {
    if (!r || !a || !r->air_mask) return false;
    if (r->air_mask & DET_AIR_SQUAWK) {
        bool hit = false;
        for (unsigned i = 0; i < r->air_squawks; i++) hit |= a->squawk && a->squawk == r->air_squawk[i];
        if (!hit) return false;
    }
    if ((r->air_mask & DET_AIR_EMERG) && !a->emergency) return false;
    if (r->air_mask & DET_AIR_CALL) {
        bool hit = false;
        for (unsigned i = 0; i < r->air_calls && a->callsign[0]; i++) hit |= !strncmp(a->callsign, r->air_call[i], strlen(r->air_call[i]));
        if (!hit) return false;
    }
    if (r->air_mask & DET_AIR_ICAO) {
        bool hit = false;
        for (unsigned i = 0; i < r->air_icaos; i++) hit |= a->icao >= r->air_icao_lo[i] && a->icao <= r->air_icao_hi[i];
        if (!hit) return false;
    }
    if (r->air_mask & DET_AIR_CAT) { if (a->category < 0 || !(r->air_cats & (1u << a->category))) return false; }
    if (r->air_mask & DET_AIR_KM) { if (a->distance_x10_km == DET_NO_DISTANCE || a->distance_x10_km > r->air_km * 10u) return false; }
    if (r->air_mask & DET_AIR_ALT) { if (a->altitude_ft == INT32_MIN || a->altitude_ft >= r->air_alt_below_ft) return false; }
    return true;
}
void det_air(det_t *d, const det_air_t *a, int64_t now) {
    /* Once a second per aircraft. */
    int slot = -1, oldest = 0;
    for (int i = 0; i < DET_AIR_SLOTS; i++) {
        if (d->air_seen[i].icao == a->icao + 1) { slot = i; break; }
        if (d->air_seen[i].at < d->air_seen[oldest].at) oldest = i;
    }
    if (slot >= 0 && now - d->air_seen[slot].at < 1000000) return;
    if (slot < 0) slot = oldest;
    d->air_seen[slot].icao = a->icao + 1;
    d->air_seen[slot].at = now;
    d->observations++;
    if (!(d->sources & DET_ADSB)) return;
    det_event_t *e = fresh(d, DET_ADSB, now);
    const uint64_t entity = (UINT64_C(0xADB5) << 32 | a->icao) * UINT64_C(0x9E3779B97F4A7C15);
    e->entity = entity; e->icao = a->icao; e->emergency = a->emergency; e->air_category = a->category;
    e->squawk = a->squawk; e->distance_x10_km = a->distance_x10_km; e->altitude_ft = a->altitude_ft;
    memcpy(e->callsign, a->callsign, sizeof(e->callsign));
    for (unsigned i = 0; i < d->n_rules; i++) {
        const det_rule_t *r = &d->rules[i];
        if (!r->enabled || r->sources != DET_ADSB || !det_air_match(r, a)) continue;
        /* A changed squawk or emergency state is a new alert; the same one is not. */
        const uint64_t key = r->air_mask & (DET_AIR_SQUAWK | DET_AIR_EMERG)
                                 ? entity ^ ((uint64_t)a->squawk << 40) ^ ((uint64_t)a->emergency << 56) : entity;
        join(d, e, i, key, now);
    }
    emit_policy(d, e, now, 0);
}
void det_test(det_t *d, int64_t now) {
    if (!token(d, now)) { d->suppressed++; return; }
    det_event_t *e = fresh(d, 0, now);
    e->sequence = ++d->sequence;
    e->test = true;
    e->selected = d->sinks;
    snprintf(e->reason, sizeof(e->reason), "Operator sink test");
    finish(d, e);
}

/* ---- words ---------------------------------------------------------------- */

static void air_words(char *out, size_t cap, uint32_t icao, const char *call, unsigned squawk, int emergency, int32_t alt, unsigned dist) {
    char id[12], tail[48] = "";
    if (call[0]) snprintf(id, sizeof(id), "%s", call); else snprintf(id, sizeof(id), "%06X", (unsigned)icao);
    size_t n = 0;
    if (squawk) n += (size_t)snprintf(tail + n, sizeof(tail) - n, " sq%04u", squawk);
    if (emergency && n < sizeof(tail)) n += (size_t)snprintf(tail + n, sizeof(tail) - n, " EMG");
    if (alt != INT32_MIN && n < sizeof(tail)) n += (size_t)snprintf(tail + n, sizeof(tail) - n, " %dft", (int)alt);
    if (dist != DET_NO_DISTANCE && n < sizeof(tail)) snprintf(tail + n, sizeof(tail) - n, " %u.%ukm", dist / 10, dist % 10);
    snprintf(out, cap, "%s%s", id, tail);
}
void det_air_text(const det_air_t *a, char *out, size_t cap) {
    air_words(out, cap, a->icao, a->callsign, a->squawk, a->emergency, a->altitude_ft, a->distance_x10_km);
}
void det_air_event_text(const det_event_t *e, char *out, size_t cap) {
    air_words(out, cap, e->icao, e->callsign, e->squawk, e->emergency, e->altitude_ft, e->distance_x10_km);
}
const char *det_label(const det_event_t *e) {
    if (e->test) return "TEST";
    switch (e->source) {
    case DET_ADSB: return "AIRCRAFT";
    case DET_RID: return "REMOTE ID";
    case DET_LORA: return "LoRa activity";
    case DET_DRONE_WATCH: return "DRONE";
    case DET_REC: case DET_CATCH: return "SUB-GHZ";
    case DET_POCSAG: return "PAGER";
    case DET_P25: return "P25";
    case DET_MESH_PEER: return "MESH NODE";
    case DET_MESH_ALERT: return "MESH ALERT";
    }
    return e->category < DET_CATS ? det_category_name(e->category) : "ACTIVITY";
}
bool det_emergency(const det_event_t *e) {
    return e->source == DET_ADSB ? det_air_alarm(e->squawk, e->emergency)
                                 : (e->source & (DET_P25 | DET_MESH_ALERT)) && (e->emergency & 1);
}
void det_describe(const det_event_t *e, char *out, size_t cap) {
    const char *label = det_label(e);
    char facts[64] = "";
    switch (e->source) {
    case DET_ADSB: det_air_event_text(e, facts, sizeof(facts)); break;
    case DET_POCSAG: snprintf(facts, sizeof(facts), "cap %u%s%.15s", (unsigned)e->num[0], e->text[0] ? " word " : "", e->text); break;
    case DET_P25: snprintf(facts, sizeof(facts), "TG %u unit %u%s", (unsigned)e->num[0], (unsigned)e->num[1], e->emergency & 1 ? " EMERGENCY" : ""); break;
    case DET_REC: case DET_CATCH:
        snprintf(facts, sizeof(facts), "%u.%03u MHz %.24s%s", (unsigned)(e->num[0] / 1000000), (unsigned)(e->num[0] / 1000 % 1000),
                 e->text, e->validated ? " new" : "");
        break;
    case DET_MESH_PEER: snprintf(facts, sizeof(facts), "%.16s %s", e->name[0] ? e->name : e->origin, e->num[0] ? "lost" : "new"); break;
    case DET_MESH_ALERT: snprintf(out, cap, "%s", e->text); return;
    default: if (!e->test) snprintf(facts, sizeof(facts), "%d %s", e->rssi, e->relative_rssi ? "dB" : "dBm");
    }
    snprintf(out, cap, "%s %s %s", label, facts, e->reason);
}
/* Short, for a small synthesizer: what, and from whom; never content. */
void det_speech(const det_event_t *e, char *out, size_t cap) {
    if (e->source == DET_MESH_ALERT) {
        det_event_t r;
        memset(&r, 0, sizeof(r));
        r.source = (uint16_t)(1u << (e->remote & 15));
        r.category = 255;
        snprintf(out, cap, "Mesh alert from %s. %s%s.", e->name[0] ? e->name : "a node", det_label(&r), e->emergency & 1 ? ", emergency" : "");
    } else {
        snprintf(out, cap, "%s %s.", det_label(e), det_emergency(e) ? "emergency" : "alert");
    }
}
int det_log_line(const det_t *d, const det_event_t *e, uint64_t boot, char *out, size_t cap) {
    char utc[24] = "null", signal[16] = "null", air[200] = "", ids[600] = "";
    if (d->utc >= 1704067200) snprintf(utc, sizeof(utc), "%lu", (unsigned long)d->utc);
    if (e->source && e->source != DET_ADSB) snprintf(signal, sizeof(signal), "%d", e->rssi);
    if (e->source == DET_ADSB) {
        /* Broadcast identifiers only; position, range and our fix are never logged. */
        char alt[16] = "null";
        if (e->altitude_ft != INT32_MIN) snprintf(alt, sizeof(alt), "%d", (int)e->altitude_ft);
        snprintf(air, sizeof(air), "\"icao\":\"%06X\",\"callsign\":\"%s\",\"squawk\":%u,\"emergency_state\":%u,\"altitude_baro_ft\":%s,\"adsb_category\":%s%s%s,",
                 (unsigned)e->icao, e->callsign, (unsigned)e->squawk, (unsigned)e->emergency, alt, e->air_category < 0 ? "null" : "\"",
                 e->air_category < 0 ? "" : det_air_category_name((unsigned)e->air_category), e->air_category < 0 ? "" : "\"");
    }
    size_t n = 0;
    for (unsigned i = 0; i < d->n_rules && i < 64 && n + 40 < sizeof(ids); i++)
        if (e->rules & (UINT64_C(1) << i)) n += (size_t)snprintf(ids + n, sizeof(ids) - n, "%s\"%s\"", n ? "," : "", d->rules[i].id);
    if (e->builtin && n + 24 < sizeof(ids)) snprintf(ids + n, sizeof(ids) - n, "%s\"%s\"", n ? "," : "", e->builtin == 1 ? "drone-link-nearby" : "mesh-alert");
    const int w = snprintf(out, cap,
        "{\"version\":1,\"revision\":%u,\"boot\":\"%08lX%08lX\",\"sequence\":%u,\"uptime_ms\":%lu,\"utc\":%s,\"source\":\"%s\",\"rule_ids\":[%s],"
        "\"entity\":\"%08lX%08lX\",\"category\":\"%s\",\"rssi\":%s,\"validated_message\":%s,\"signal_units\":\"%s\",\"band\":%u,"
        "\"rate_guess_hz\":%u,\"hits_per_min\":%u,%s\"gps_valid\":false,\"test\":%s,\"selected\":%u,\"allowed\":%u,\"accepted\":%u,"
        "\"failed\":%u,\"coalesced\":%u,\"suppressed\":%u}\n",
        (unsigned)e->revision, (unsigned long)(boot >> 32), (unsigned long)(uint32_t)boot, (unsigned)e->sequence,
        (unsigned long)(e->at / 1000), utc, e->test ? "test" : det_source_name(e->source), ids,
        (unsigned long)(e->entity >> 32), (unsigned long)(uint32_t)e->entity, det_category_name(e->category), signal,
        e->validated ? "true" : "false", e->relative_rssi ? "dB relative" : "dBm", e->band, e->rate_guess, (unsigned)e->hits_per_min,
        air, e->test ? "true" : "false", e->selected, e->allowed, e->delivered, e->failed, (unsigned)e->coalesced, (unsigned)e->suppressed);
    return w < 0 || (size_t)w >= cap ? -1 : w;
}

/* ---- the shared alert line ------------------------------------------------ */

static bool hex8(const char *p, size_t n, uint32_t *v) {
    if (n != 8) return false;
    uint32_t x = 0;
    for (size_t i = 0; i < n; i++) { int h = hexval((unsigned char)p[i]); if (h < 0) return false; x = x << 4 | (uint32_t)h; }
    *v = x;
    return true;
}
static void clean(char *out, const char *in, size_t cap) {
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) out[n++] = (*in < 32 || *in > 126 || *in == '|') ? '/' : *in;
    out[n] = 0;
}
int det_alert_encode(const det_alert_t *m, char *out, size_t cap) {
    char sum[DET_ALERT_SUMMARY], pos[24] = "";
    clean(sum, m->summary, sizeof(sum));
    if (m->has_pos) snprintf(pos, sizeof(pos), "%ld,%ld", (long)m->lat_e4, (long)m->lon_e4);
    const int n = snprintf(out, cap, DET_ALERT_TAG "1|%.8s|%08lX|%u|%s%s|%s|%s", m->origin, (unsigned long)m->seq, (unsigned)m->source,
                           m->flags & DET_ALERT_F_EMERG ? "E" : "", m->flags & DET_ALERT_F_TEST ? "T" : "", pos, sum);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}
/* One signed decimal filling [p,end), within +-lim. */
static bool coord(const char *p, const char *end, long lim, int32_t *v) {
    if (p == end) return false;
    bool neg = *p == '-';
    if (neg) p++;
    if (p == end) return false;
    long x = 0;
    for (; p < end; p++) { if (!digit(*p) || x > 100000000) return false; x = x * 10 + (*p - '0'); }
    if (neg) x = -x;
    if (x < -lim || x > lim) return false;
    *v = (int32_t)x;
    return true;
}
int det_alert_decode(const char *text, det_alert_t *m) {
    const size_t tag = strlen(DET_ALERT_TAG);
    if (!text || strncmp(text, DET_ALERT_TAG, tag)) return DET_ALERT_NOT_ALERT;
    /* The version first: a newer board may lay out the rest differently. */
    const char *p = text + tag;
    unsigned v = 0, n = 0;
    while (digit(p[n]) && n < 3) v = v * 10 + (unsigned)(p[n++] - '0');
    if (!n || p[n] != '|') return DET_ALERT_BAD;
    if (v != 1) return v > 1 ? DET_ALERT_NEWER : DET_ALERT_BAD;
    const char *f[7]; size_t len[7];
    for (int i = 0; i < 7; i++) {
        const char *bar = i < 6 ? strchr(p, '|') : NULL;
        if (i < 6 && !bar) return DET_ALERT_BAD;
        f[i] = p;
        len[i] = bar ? (size_t)(bar - p) : strlen(p);
        p = bar ? bar + 1 : p + len[i];
    }
    memset(m, 0, sizeof(*m));
    uint32_t o;
    if (!hex8(f[1], len[1], &o) || !hex8(f[2], len[2], &m->seq)) return DET_ALERT_BAD;
    memcpy(m->origin, f[1], 8);
    if (!len[3] || len[3] > 2 || !digit(f[3][0]) || (len[3] == 2 && !digit(f[3][1]))) return DET_ALERT_BAD;
    unsigned s = 0;
    for (size_t i = 0; i < len[3]; i++) s = s * 10 + (unsigned)(f[3][i] - '0');
    if (s > 15) return DET_ALERT_BAD;
    m->source = (uint8_t)s;
    for (size_t i = 0; i < len[4]; i++) { if (f[4][i] == 'E') m->flags |= DET_ALERT_F_EMERG; else if (f[4][i] == 'T') m->flags |= DET_ALERT_F_TEST; }
    if (len[5]) {
        const char *comma = NULL;
        for (size_t i = 0; i < len[5] && !comma; i++) if (f[5][i] == ',') comma = f[5] + i;
        if (!comma || !coord(f[5], comma, 900000, &m->lat_e4) || !coord(comma + 1, f[5] + len[5], 1800000, &m->lon_e4)) return DET_ALERT_BAD;
        m->has_pos = true;
    }
    if (!len[6] || len[6] >= DET_ALERT_SUMMARY) return DET_ALERT_BAD;
    for (size_t i = 0; i < len[6]; i++) if (f[6][i] < 32 || f[6][i] > 126) return DET_ALERT_BAD;
    memcpy(m->summary, f[6], len[6]);
    m->summary[len[6]] = 0;
    return DET_ALERT_OK;
}
uint32_t det_alert_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}
int det_alert_gate(det_alert_gate_t *g, uint32_t hash, int64_t now) {
    for (unsigned i = 0; i < 8; i++) if (g->hash[i] == hash && now - g->at[i] < DET_ALERT_REPEAT_US) return DET_ALERT_REPEAT;
    if (now < g->next_at) return DET_ALERT_WAIT;
    g->next_at = now + DET_ALERT_GAP_US;
    g->hash[g->i] = hash; g->at[g->i] = now; g->i = (g->i + 1) % 8;
    return DET_ALERT_GO;
}
bool det_alert_seen(det_alert_seen_t *s, uint32_t origin, uint32_t seq) {
    for (unsigned i = 0; i < 32; i++) if (s->origin[i] == origin && s->seq[i] == seq) return true;
    s->origin[s->i] = origin; s->seq[s->i] = seq; s->i = (s->i + 1) % 32;
    return false;
}
