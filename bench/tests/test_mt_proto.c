/* MESHTASTIC (app-sdk/apps/meshtastic): the protocol messages 1.3 added,
   checked byte for byte against the field numbers in Meshtastic's protobufs
   (mesh.proto Data/User/Waypoint/NeighborInfo, storeforward.proto,
   apponly.proto ChannelSet, config.proto LoRaConfig), the published default
   channel link and the firmware's slot rule. */
#include <stdio.h>
#include <string.h>
#include "mt_proto.h"
#include "mt_pki.h"

static int fails;
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }
static int has(const uint8_t *b, int n, const uint8_t *pat, int m)
{
    for (int i = 0; i + m <= n; i++) if (!memcmp(b + i, pat, (size_t)m)) return 1;
    return 0;
}

int main(void)
{
    uint8_t b[256];
    int slot = 0, slots = 0;

    /* slot rule and preset names (DisplayFormatters: LONG_MODERATE is "LongMod") */
    uint64_t f = mt_frequency_hz(0, 0, "", &slot, &slots);
    check(f == 906875000ull && slot == 20, "US LongFast 906.875 MHz slot 20");
    check(!strcmp(MT_PRESETS[3].name, "LongMod") && MT_PRESETS[3].code == 7, "LONG_MODERATE named LongMod, code 7");
    for (int p = 0; p < MT_PRESET_COUNT; p++) {
        f = mt_frequency_hz(0, p, "", &slot, &slots);
        printf("     US %-10s %llu Hz slot %d/%d\n", MT_PRESETS[p].name, (unsigned long long)f, slot, slots);
    }
    mt_lora_t l = { 0, 0, 0, 0, 0, 0, 0, 0 };
    l.slot = 1;
    f = mt_lora_frequency(&l, "", &slot, &slots);
    check(f == 902125000ull && slot == 1, "channel_num 1 = first slot (902.125 MHz)");
    l.slot = 0; l.offset_hz = -2500;
    check(mt_lora_frequency(&l, "", &slot, &slots) == 906872500ull, "frequency_offset added to the slot centre");
    l.offset_hz = 0; l.custom = 1; l.bw_code = MT_BW_125; l.sf = 10; l.cr = 7;
    f = mt_lora_frequency(&l, "", &slot, &slots);
    check(!strcmp(mt_lora_name(&l), "Custom") && slots == 208 && mt_lora_bw_hz(&l) == 125000, "custom: 125 kHz, 208 slots, named Custom");
    check(mt_region_by_code(3) == 2 && mt_region_by_code(26) == 24 && mt_preset_by_code(4) == 1, "RegionCode / ModemPreset values map");

    /* Data: reply_id = field 7 fixed32 (0x3d), emoji = field 8 fixed32 (0x45) */
    const char *heart = "\xe2\x9d\xa4\xef\xb8\x8f";
    mt_data_t d = { MT_PORT_TEXT, (const uint8_t *)heart, strlen(heart), false, 0, 0xA1B2C3D4u, 1 };
    int n = mt_data_encode(&d, b, sizeof(b));
    const uint8_t reply_tag[] = { 0x3d, 0xd4, 0xc3, 0xb2, 0xa1 }, emoji_tag[] = { 0x45, 1, 0, 0, 0 };
    check(n > 0 && has(b, n, reply_tag, 5) && has(b, n, emoji_tag, 5), "Data reply_id (0x3d) and emoji (0x45) on the wire");
    mt_data_t back;
    check(mt_data_decode(b, (size_t)n, &back) && back.reply_id == 0xA1B2C3D4u && back.emoji == 1 &&
          back.payload_len == strlen(heart), "Data tapback round trip");
    check(mt_text_room(false, false, 0) == 233 && mt_text_room(false, false, MT_PKI_OVERHEAD) == 222 &&
          mt_text_room(true, false, 0) == 229, "text room: 233 channel, 222 private DM, 229 reply");
    /* the longest text fits */
    char longtext[234];
    memset(longtext, 'x', 233); longtext[233] = 0;
    mt_data_t big = { MT_PORT_TEXT, (const uint8_t *)longtext, 233, false, 0, 0, 0 };
    check(mt_data_encode(&big, b, MT_MAX_PAYLOAD) > 0, "233 text bytes fit one frame");

    /* User.is_licensed = field 6 varint (0x30) */
    mt_user_t u, u2;
    memset(&u, 0, sizeof(u));
    strcpy(u.id, "!11223344"); strcpy(u.long_name, "N0CALL"); strcpy(u.short_name, "N0C");
    u.hw_model = 255; u.role = MT_ROLE_ROUTER_LATE; u.licensed = true;
    const uint8_t mac[6] = { 0 };
    n = mt_user_encode(&u, mac, b, sizeof(b));
    const uint8_t lic[] = { 0x30, 1, 0x38, 11 };
    check(n > 0 && has(b, n, lic, 4) && mt_user_decode(b, (size_t)n, &u2) && u2.licensed && u2.role == 11,
          "User is_licensed (0x30) and role ROUTER_LATE (0x38 0x0b)");

    /* Waypoint: id 0x08, lat 0x15, lon 0x1d, expire 0x20, name 0x32, desc 0x3a, icon 0x45 */
    mt_waypoint_t w, w2;
    memset(&w, 0, sizeof(w));
    w.id = 1234; w.lat = -34.6037; w.lon = -58.3816; w.has_pos = true; w.expire = 1800000000u;
    w.icon = 0x1F4CD; strcpy(w.name, "Plaza"); strcpy(w.desc, "meet here");
    n = mt_waypoint_encode(&w, b, sizeof(b));
    const uint8_t wid[] = { 0x08, 0xd2, 0x09 }, wname[] = { 0x32, 5, 'P', 'l', 'a', 'z', 'a' }, wicon[] = { 0x45, 0xcd, 0xf4, 0x01, 0 };
    check(n > 0 && has(b, n, wid, 3) && has(b, n, wname, 7) && has(b, n, wicon, 5) && b[3] == 0x15,
          "Waypoint fields on the wire");
    check(mt_waypoint_decode(b, (size_t)n, &w2) && w2.id == 1234 && w2.has_pos && w2.lat < -34.6036 &&
          w2.lat > -34.6038 && w2.expire == 1800000000u && !strcmp(w2.desc, "meet here") && w2.icon == 0x1F4CD,
          "Waypoint round trip");

    /* NeighborInfo: node 0x08, last_sent_by 0x10, interval 0x18, neighbors 0x22 {node 0x08, snr 0x15} */
    mt_neighbors_t nb, nb2;
    memset(&nb, 0, sizeof(nb));
    nb.node = 0x11223344u; nb.last_sent_by = 0x11223344u; nb.interval_s = 21600;
    nb.id[0] = 0xA1B2C3D4u; nb.snr[0] = 6.25f; nb.id[1] = 0x5E11F00Du; nb.snr[1] = -7.5f; nb.n = 2;
    n = mt_neighbors_encode(&nb, b, sizeof(b));
    check(n > 0 && b[0] == 0x08 && has(b, n, (const uint8_t[]){ 0x18, 0xe0, 0xa8, 0x01 }, 4) &&
          has(b, n, (const uint8_t[]){ 0x22, 0x0b, 0x08 }, 3), "NeighborInfo fields on the wire");
    check(mt_neighbors_decode(b, (size_t)n, &nb2) && nb2.n == 2 && nb2.id[1] == 0x5E11F00Du && nb2.snr[1] == -7.5f &&
          nb2.interval_s == 21600, "NeighborInfo round trip");

    /* StoreAndForward: CLIENT_HISTORY (65) with history.window = 120 min */
    n = mt_sf_history_request(120, b, sizeof(b));
    const uint8_t sfreq[] = { 0x08, 0x41, 0x1a, 0x02, 0x10, 0x78 };
    check(n == 6 && !memcmp(b, sfreq, 6), "S&F history request bytes 08 41 1a 02 10 78");
    const uint8_t sftext[] = { 0x08, 0x09, 0x2a, 0x02, 'h', 'i' };
    mt_sf_t sf;
    check(mt_sf_decode(sftext, sizeof(sftext), &sf) && sf.rr == MT_SF_ROUTER_TEXT_BROADCAST && sf.text_len == 2 &&
          !memcmp(sf.text, "hi", 2), "S&F ROUTER_TEXT_BROADCAST decodes");
    const uint8_t sfhb[] = { 0x08, 0x02, 0x22, 0x02, 0x08, 0x3c };
    check(mt_sf_decode(sfhb, sizeof(sfhb), &sf) && sf.rr == MT_SF_ROUTER_HEARTBEAT && sf.period == 60, "S&F heartbeat period");

    /* ChannelSet: the default US LongFast link as the official apps print it */
    const char *def = "https://meshtastic.org/e/#CgMSAQESCAgBOAFAA0gB";
    mt_chset_t s;
    bool add = true;
    check(mt_chset_parse(def, &s, &add) && !add && s.n == 1 && s.ch[0].psk_len == 1 && s.ch[0].psk[0] == 1 &&
          !s.ch[0].name[0] && s.has_lora && !s.lora.custom && s.region_code == 1 && s.lora.region == 0 &&
          s.hop_limit == 3, "default link: one channel, AQ==, US, preset, hop limit 3");
    char url[1100];
    n = mt_chset_url(&s, url, sizeof(url));
    printf("     %s\n", url);
    check(n > 0 && !strcmp(url, def), "default link encodes back byte for byte");
    /* a private channel set with a custom LoRa config survives the trip */
    memset(&s, 0, sizeof(s));
    s.n = 2;
    strcpy(s.ch[0].name, "friends"); s.ch[0].psk_len = 32; for (int i = 0; i < 32; i++) s.ch[0].psk[i] = (uint8_t)(i * 7);
    s.ch[0].precision = 13; s.ch[1].psk_len = 1; s.ch[1].psk[0] = 1; strcpy(s.ch[1].name, "LongFast"); s.ch[1].muted = 1;
    s.has_lora = true; s.region_code = 3; s.hop_limit = 5;
    s.lora.region = 2; s.lora.custom = 1; s.lora.bw_code = MT_BW_62; s.lora.sf = 12; s.lora.cr = 8; s.lora.slot = 2;
    n = mt_chset_url(&s, url, sizeof(url));
    mt_chset_t s2;
    check(n > 0 && mt_chset_parse(url, &s2, &add) && s2.n == 2 && !strcmp(s2.ch[0].name, "friends") &&
          s2.ch[0].psk_len == 32 && !memcmp(s2.ch[0].psk, s.ch[0].psk, 32) && s2.ch[0].precision == 13 &&
          s2.ch[1].muted && s2.lora.custom && s2.lora.bw_code == MT_BW_62 && s2.lora.sf == 12 && s2.lora.cr == 8 &&
          s2.lora.slot == 2 && s2.lora.region == 2 && s2.hop_limit == 5, "private set + custom LoRa round trip");
    check(mt_chset_parse("https://meshtastic.org/e/?add=true#CgMSAQESCAgBOAFAA0gB", &s2, &add) && add, "?add=true read");

    check(!strcmp(mt_role_name(MT_ROLE_CLIENT_HIDDEN), "CLIENT_HIDDEN") && !strcmp(mt_hw_name(43), "HELTEC_V3") &&
          !strcmp(mt_hw_name(110), "HELTEC_V4") && !strcmp(mt_hw_name(9), "RAK4631") && !strcmp(mt_hw_name(255), "PRIVATE_HW"),
          "role and hardware names by value");
    printf("%s\n", fails ? "SOME FAILED" : "ALL PASS");
    return fails;
}
