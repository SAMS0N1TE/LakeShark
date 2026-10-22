#include "ls_test.h"
#include "p25_pack.h"
#include "p25_geo.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static const ls_radio_range_t ranges[] = {{24000000, 1766000000}};
static const p25_profile_parse_config_t cfg = {ranges, 1};
static p25_pack_db_t db;
static p25_profile_parse_scratch_t scratch;
static const char a[] = "version=2\nsystem=Fictional A\nsite=Hill A\nsysid=291\nwacn=781824\nnac=659\ncontrol=851012500|42|-71|10000\ntg=123|Test dispatch|true|0\n";
static const char b[] = "version=2\nsystem=Fictional B\nsite=Hill B\ncontrol=852012500|42.2|-71|10000\n";
static bool parse(const char *text) { return p25_pack_parse(&db, &scratch, text, strlen(text), &cfg, NULL); }
LS_CASE(pack_concatenates_profiles_and_keeps_optional_ids_and_rosters)
{
    memset(&db, 0, sizeof(db));
    char text[1024]; snprintf(text, sizeof(text), "%s\n%s", a, b);
    LS_CHECK(parse(text)); LS_EQ_INT(2, db.count); LS_EQ_INT(1, db.packs); LS_EQ_INT(2, db.sites);
    LS_EQ_STR("Fictional B", db.records[1].system_name);
    LS_EQ_INT(291, db.records[0].sysid); LS_CHECK(db.records[0].wacn_set);
    LS_EQ_STR("Test dispatch", db.records[0].talkgroups[0].alias);
}
LS_CASE(invalid_pack_does_not_commit_partial_records)
{
    memset(&db, 0, sizeof(db)); LS_CHECK(parse(a));
    char text[1024]; snprintf(text, sizeof(text), "%sversion=2\nsystem=Bad\nsite=Bad\ncontrol=851000000|91|0|1000\n", b);
    LS_CHECK(!parse(text)); LS_EQ_INT(1, db.count); LS_EQ_INT(1, db.packs); LS_EQ_INT(1, db.sites);
    LS_EQ_STR("Fictional A", db.records[0].system_name);
    LS_CHECK(!parse("version=1\nsystem=A\nsite=A\ncontrol=851000000\n"));
    LS_CHECK(!parse("version=2\nsystem=A\nsite=A\ncontrol=851000000\n"));
    LS_CHECK(!parse("version=2\nsystem=A\nsite=A\nnac=4096\ncontrol=851000000|0|0|1000\n"));
    LS_CHECK(!parse("version=2\nsystem=A\nsite=A\ncontrol=851000000|0|0|499\n"));
    LS_CHECK(!parse("version=2\nsystem=A\nsite=A\ncontrol=851000000|0|0|200001\n"));
    LS_CHECK(!p25_pack_parse(&db, &scratch, a, P25_PACK_MAX_BYTES + 1, &cfg, NULL));
}
LS_CASE(pack_capacity_is_bounded)
{
    memset(&db, 0, sizeof(db)); db.count = P25_PACK_RECORD_MAX;
    LS_CHECK(!parse(a)); LS_EQ_INT(P25_PACK_RECORD_MAX, db.count);
    LS_CHECK(p25_pack_state_valid("MA")); LS_CHECK(p25_pack_state_valid("DC"));
    LS_CHECK(!p25_pack_state_valid("ZZ")); LS_CHECK(!p25_pack_state_valid("../MA")); LS_CHECK(!p25_pack_state_valid(NULL));
}
LS_CASE(nearest_across_packs_and_two_km_hysteresis)
{
    memset(&db, 0, sizeof(db)); LS_CHECK(parse(a)); LS_CHECK(parse(b)); LS_EQ_INT(2, db.packs);
    p25_geo_t g = {.chosen = -1}; int64_t t = 1000000;
    LS_EQ_INT(16, p25_geo_select_many(&g, db.records, db.count, true, 42.19, -71, t, t));
    LS_EQ_INT(16, g.chosen);
    // At 42.1 both sites are 11.12 km away, outside nominal radius. Keep held.
    t += 1000000;
    LS_EQ_INT(-1, p25_geo_select_many(&g, db.records, db.count, true, 42.1, -71, t, t));
    LS_EQ_INT(16, g.chosen);
    // Expand site A so it competes while B is inside its 2 km exit margin.
    db.records[0].control_radius_m[0] = 30000;
    t += 1000000;
    LS_EQ_INT(-1, p25_geo_select_many(&g, db.records, db.count, true, 42.095, -71, t, t));
    LS_EQ_INT(16, g.chosen);
    t += 1000000;
    LS_EQ_INT(0, p25_geo_select_many(&g, db.records, db.count, true, 42.08, -71, t, t));
    t += 1000000;
    LS_EQ_INT(-1, p25_geo_select_many(&g, db.records, db.count, false, 42.2, -71, t, t));
    LS_EQ_INT(0, g.chosen);
}
static bool accept(const char *text, const ls_ota_manifest_t *m) { return text && m->sig_len == 8; }
static bool reject(const char *text, const ls_ota_manifest_t *m) { (void)text; (void)m; return false; }
static void catalog_body(char *out, size_t cap, const char *sha, const char *tier)
{
    snprintf(out, cap, "LAKESHARK-OTA 1\nboard=P25-sites\nchannel=catalog\nschema=p25-sites-1\nversion=1.0.0\nbuild=packs-1\nsize=1\nsha256=%s\nurl=catalog.txt\nentry=MA|Massachusetts|MA.txt|12|%s|1.0.0|%s\nsig=3006020101020101\n", sha, sha, tier);
}
LS_CASE(catalog_requires_verifier_and_refuses_bad_sha_tier_and_unsigned_tail)
{
    const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char text[2048]; p25_pack_catalog_t c;
    catalog_body(text, sizeof(text), sha, "plus");
    LS_CHECK(p25_pack_catalog_parse(&c, text, strlen(text), accept)); LS_EQ_INT(1, c.count);
    LS_CHECK(c.entries[0].plus); LS_EQ_STR("MA", c.entries[0].state);
    LS_CHECK(!p25_pack_catalog_parse(&c, text, strlen(text), reject)); LS_EQ_INT(0, c.count);
    LS_CHECK(!p25_pack_catalog_parse(&c, text, strlen(text), NULL));
    strcat(text, "entry=NH|unsigned\n"); LS_CHECK(!p25_pack_catalog_parse(&c, text, strlen(text), accept));
    catalog_body(text, sizeof(text), "bad-sha", "free"); LS_CHECK(!p25_pack_catalog_parse(&c, text, strlen(text), accept));
    catalog_body(text, sizeof(text), sha, "premium"); LS_CHECK(!p25_pack_catalog_parse(&c, text, strlen(text), accept));
    catalog_body(text, sizeof(text), sha, "free");
    char *file = strstr(text, "MA.txt"); memcpy(file, "../bad", 6);
    LS_CHECK(!p25_pack_catalog_parse(&c, text, strlen(text), accept));
}
LS_CASE(tiers_and_download_hash_are_enforced)
{
    p25_pack_entry_t e = {.bytes = 12, .plus = true}; uint8_t digest[32] = {0};
    LS_CHECK(!p25_pack_allowed(&e, NULL)); LS_CHECK(!p25_pack_allowed(&e, ""));
    LS_CHECK(p25_pack_allowed(&e, "test-entitlement")); e.plus = false;
    LS_CHECK(p25_pack_allowed(&e, NULL));
    LS_CHECK(p25_pack_digest_ok(&e, 12, digest)); LS_CHECK(!p25_pack_digest_ok(&e, 11, digest));
    digest[7] = 1; LS_CHECK(!p25_pack_digest_ok(&e, 12, digest));
}

LS_CASE(all_states_fit_signed_data_envelope_and_oversized_input_is_refused)
{
    static char text[LS_OTA_DATA_MAX_TEXT];
    const char *sha = "0000000000000000000000000000000000000000000000000000000000000000";
    size_t n = (size_t)snprintf(text, sizeof(text), "LAKESHARK-OTA 1\nboard=P25-sites\nchannel=catalog\nschema=p25-sites-1\nversion=1.0.0\nbuild=states-1\nsize=1\nsha256=%s\nurl=catalog.txt\n", sha);
    unsigned entries = 0;
    for (char a = 'A'; a <= 'Z'; a++) for (char b = 'A'; b <= 'Z'; b++) {
        char st[] = {a, b, 0}; if (!p25_pack_state_valid(st)) continue;
        n += (size_t)snprintf(text + n, sizeof(text) - n, "entry=%s|%s|%s.txt|12|%s|1.0.0|free\n", st, st, st, sha); entries++;
    }
    n += (size_t)snprintf(text + n, sizeof(text) - n, "sig=3006020101020101\n");
    p25_pack_catalog_t c;
    LS_EQ_INT(51, entries); LS_CHECK(n > LS_OTA_MAX_TEXT);
    LS_CHECK(p25_pack_catalog_parse(&c, text, n, accept)); LS_EQ_INT(51, c.count);
    LS_CHECK(!p25_pack_catalog_parse(&c, "small", LS_OTA_DATA_MAX_TEXT + 1, accept));
}
