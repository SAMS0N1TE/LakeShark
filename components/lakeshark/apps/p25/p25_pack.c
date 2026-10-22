/* See p25_pack.h. State packs concatenate unmodified v2 profiles. */
#include "p25_pack.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

bool p25_pack_state_valid(const char *s)
{
    /* USPS states and DC. Small shared validation string, never a UI table. */
    const char *codes = "AL AK AZ AR CA CO CT DE DC FL GA HI ID IL IN IA KS KY LA ME MD MA MI MN MS MO MT NE NV NH NJ NM NY NC ND OH OK OR PA RI SC SD TN TX UT VT VA WA WV WI WY";
    if (!s || strlen(s) != 2) return false;
    for (const char *p = codes; *p; p += 3) {
        if (p[0] == s[0] && p[1] == s[1]) return true;
        if (!p[2]) break;
    }
    return false;
}

bool p25_pack_parse(p25_pack_db_t *db, p25_profile_parse_scratch_t *scratch,
                    const char *text, size_t len, const p25_profile_parse_config_t *cfg,
                    p25_profile_diagnostic_t *diag)
{
    if (!db || !scratch || !text || !len || len > P25_PACK_MAX_BYTES ||
        db->count > P25_PACK_RECORD_MAX || db->packs >= P25_PACK_STATE_MAX) return false;
    unsigned count = db->count, sites = db->sites;
    size_t start = 0;
    bool have = false;
    for (size_t pos = 0; pos <= len;) {
        size_t end = pos;
        while (end < len && text[end] != '\n') end++;
        bool next = pos < len && end - pos >= 8 && !memcmp(text + pos, "version=", 8);
        if ((next && have) || pos == len) {
            if (count >= P25_PACK_RECORD_MAX ||
                !p25_profile_parse(&db->records[count], scratch, text + start, pos - start, cfg, diag)) return false;
            p25_profile_t *p = &db->records[count];
            if (p->format_version != 2) return false;
            for (unsigned i = 0; i < p->control_count; i++) {
                if (!p->control_has_geo[i]) return false;
                sites++;
            }
            count++;
            start = pos;
        }
        if (next) { if (!have) start = 0; have = true; }
        if (pos == len) break;
        pos = end < len ? end + 1 : len;
    }
    if (!have || count == db->count) return false;
    db->count = count; db->sites = sites; db->packs++;
    return true;
}

static bool hex(const char *s, uint8_t *out)
{
    if (strlen(s) != 64) return false;
    for (unsigned i = 0; i < 32; i++) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = s[i * 2 + j];
            int x = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (x < 0) return false;
            v = v * 16 + (unsigned)x;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

bool p25_pack_catalog_parse(p25_pack_catalog_t *out, const char *text, size_t len,
                            p25_pack_verify_fn verify)
{
    if (!out) return false;
    out->count = 0;
    ls_ota_manifest_t m;
    if (!text || len > LS_OTA_DATA_MAX_TEXT || memchr(text, 0, len) || !verify || ls_ota_parse_data(text, len, P25_PACK_CATALOG_URL, &m) != LS_OTA_OK ||
        strcmp(m.board, "P25-sites") || strcmp(m.channel, "catalog") || !verify(text, &m)) return false;
    unsigned count = 0; bool schema = false;
    for (size_t pos = 0; pos < m.signed_len;) {
        size_t end = pos;
        while (end < m.signed_len && text[end] != '\n') end++;
        size_t n = end - pos;
        if (n && text[pos + n - 1] == '\r') n--;
        if (n >= 7 && !memcmp(text + pos, "schema=", 7)) {
            static const char expected[] = "schema=p25-sites-1";
            if (schema || n != sizeof(expected) - 1 || memcmp(text + pos, expected, n)) return false;
            schema = true;
        }
        if (n >= 6 && !memcmp(text + pos, "entry=", 6)) {
            if (count == P25_PACK_STATE_MAX || n >= 320) return false;
            char line[320]; memcpy(line, text + pos + 6, n - 6); line[n - 6] = 0;
            char *fields[7], *p = line;
            for (unsigned i = 0; i < 7; i++) {
                fields[i] = p;
                char *sep = strchr(p, '|');
                if ((i < 6) != (sep != NULL)) return false;
                if (sep) { *sep = 0; p = sep + 1; }
                if (!*fields[i]) return false;
            }
            p25_pack_entry_t *e = &out->entries[count];
            memset(e, 0, sizeof(*e));
            if (!p25_pack_state_valid(fields[0]) || strlen(fields[1]) >= sizeof(e->name) ||
                strlen(fields[2]) >= sizeof(e->file) || strlen(fields[5]) >= sizeof(e->version)) return false;
            char expected[8]; snprintf(expected, sizeof(expected), "%s.txt", fields[0]);
            if (strcmp(fields[2], expected)) return false; /* token never leaves trusted host */
            for (unsigned i = 0; i < count; i++) if (!strcmp(out->entries[i].state, fields[0])) return false;
            for (const char *q = fields[3]; *q; q++) if (!isdigit((unsigned char)*q)) return false;
            unsigned long bytes = strtoul(fields[3], NULL, 10);
            if (strlen(fields[3]) > 7 || !bytes || bytes > P25_PACK_MAX_BYTES || !hex(fields[4], e->sha256)) return false;
            if (strcmp(fields[6], "free") && strcmp(fields[6], "plus")) return false;
            strcpy(e->state, fields[0]); strcpy(e->name, fields[1]); strcpy(e->file, fields[2]); strcpy(e->version, fields[5]);
            e->bytes = (uint32_t)bytes; e->plus = !strcmp(fields[6], "plus"); count++;
        }
        pos = end + 1;
    }
    if (!schema) return false;
    out->count = count;
    return count > 0;
}

bool p25_pack_allowed(const p25_pack_entry_t *e, const char *token)
{ return e && (!e->plus || (token && *token)); }

bool p25_pack_digest_ok(const p25_pack_entry_t *e, size_t bytes, const uint8_t digest[32])
{ return e && digest && bytes == e->bytes && !memcmp(e->sha256, digest, 32); }
