/* MESHTASTIC SCAN (app-sdk/apps/meshtastic/mt_scan.c): the preset table and
   default slots against Meshtastic's firmware (RadioInterface.cpp slot rule,
   MeshRadio.h presets, DisplayFormatters.cpp names), the dwell scheduler,
   pinning, and the stats table fed with real default-channel frames. */
#include <stdio.h>
#include <string.h>
#include "mt_proto.h"
#include "mt_scan.h"

static int fails;
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }

static int region_named(const char *n)
{
    for (int i = 0; i < MT_REGION_COUNT; i++) if (!strcmp(MT_REGIONS[i].name, n)) return i;
    return -1;
}
static int entry_named(const mt_scan_t *s, const char *n)
{
    for (int i = 0; i < s->n; i++) if (!strcmp(MT_SCAN_PRESETS[s->e[i].preset].name, n)) return i;
    return -1;
}

/* A frame as a node sends it on a preset's default channel (key AQ==). */
static int frame(uint8_t *out, uint32_t from, uint32_t id, uint8_t hash, const char *text, bool def_key)
{
    mt_header_t h = { .dest = MT_BROADCAST, .from = from, .id = id, .hop_limit = 3, .hop_start = 3, .channel = hash };
    mt_header_pack(&h, out);
    mt_data_t d = { .portnum = MT_PORT_TEXT, .payload = (const uint8_t *)text, .payload_len = strlen(text) };
    const int n = mt_data_encode(&d, out + MT_HEADER_LEN, 200);
    uint8_t key[32];
    uint8_t psk[16] = { 1 };
    int kl = mt_channel_key(psk, def_key ? 1 : 16, key);
    if (!def_key) { for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i * 7 + 3); kl = 16; }
    mt_crypt(key, kl, from, id, out + MT_HEADER_LEN, (size_t)n);
    return MT_HEADER_LEN + n;
}

int main(void)
{
    const int US = region_named("US"), EU = region_named("EU_868"), L24 = region_named("LORA_24"),
              PH868 = region_named("PH_868");
    mt_scan_t s;

    /* ---- the US plan: ten presets of PRESETS_STD ---- */
    mt_scan_init(&s, US, 10000);
    check(s.n == 10, "US scans the ten PRESETS_STD presets");
    for (int i = 0; i < s.n; i++) {
        const mt_scan_entry_t *e = &s.e[i];
        printf("     US %-11s %9.4f MHz slot %3d/%-3d bw %6u SF%-2u 4/%u hash 0x%02X dwell %5u hang %4u\n",
               MT_SCAN_PRESETS[e->preset].name, (double)e->freq_hz / 1e6, e->slot, e->slots, (unsigned)e->bw_hz,
               e->sf, e->cr, e->def_hash, (unsigned)e->dwell_ms, (unsigned)e->hang_ms);
        check(e->freq_hz - e->bw_hz / 2 >= 902000000ull && e->freq_hz + e->bw_hz / 2 <= 928000000ull, "  inside 902-928 MHz");
    }
    int lf = entry_named(&s, "LongFast");
    check(lf >= 0 && s.e[lf].freq_hz == 906875000ull && s.e[lf].slot == 20 && s.e[lf].slots == 104,
          "US LongFast 906.875 MHz, slot 20 of 104 (published default)");
    int mf = entry_named(&s, "MediumFast");
    check(mf >= 0 && s.e[mf].freq_hz == 913125000ull && s.e[mf].slot == 45, "US MediumFast 913.125 MHz, slot 45");
    int sfst = entry_named(&s, "ShortFast");
    check(sfst >= 0 && s.e[sfst].freq_hz == 918875000ull && s.e[sfst].slot == 68, "US ShortFast 918.875 MHz, slot 68");
    check(lf >= 0 && s.e[lf].def_hash == 0x08, "LongFast default channel hash 0x08 (as the official apps show)");
    check(s.e[lf].preamble == 16 && s.e[lf].sf == 11 && s.e[lf].cr == 5 && s.e[lf].bw_hz == 250000, "LongFast SF11 BW250 4/5, preamble 16");
    {
        const int lt = entry_named(&s, "LongTurbo");
        check(lt >= 0 && s.e[lt].bw_hz == 500000 && s.e[lt].sf == 11 && s.e[lt].cr == 8 && s.e[lt].slots == 52,
              "LongTurbo BW500 SF11 4/8, 52 slots in the US");
        const int mt = entry_named(&s, "MediumTurbo");
        check(mt >= 0 && s.e[mt].bw_hz == 500000 && s.e[mt].sf == 9 && s.e[mt].cr == 5, "MediumTurbo BW500 SF9 4/5");
    }
    /* the app's own slot code agrees where floor and round do (all of the US) */
    for (int p = 0; p < MT_PRESET_COUNT; p++) {
        const int i = entry_named(&s, MT_PRESETS[p].name);
        int sl, sls;
        const uint64_t f = mt_frequency_hz(US, p, "", &sl, &sls);
        char what[64];
        snprintf(what, sizeof(what), "US %s matches mt_frequency_hz", MT_PRESETS[p].name);
        check(i >= 0 && f == s.e[i].freq_hz && sl == s.e[i].slot, what);
    }
    /* long presets dwell longer than the base; turbo ones keep the base */
    {
        const int ls = entry_named(&s, "LongSlow"), st = entry_named(&s, "ShortTurbo");
        const uint32_t a_ls = mt_scan_airtime_ms(125000, 12, 8, 16, 64), a_lf = mt_scan_airtime_ms(250000, 11, 5, 16, 64),
                       a_st = mt_scan_airtime_ms(500000, 7, 5, 16, 64);
        printf("     airtime 64 B: LongSlow %u ms, LongFast %u ms, ShortTurbo %u ms\n", (unsigned)a_ls, (unsigned)a_lf, (unsigned)a_st);
        check(a_ls > a_lf && a_lf > a_st && a_lf > 600 && a_lf < 900, "airtime ordered, LongFast 64 B about 0.7 s");
        check(s.e[ls].dwell_ms == 3 * a_ls && s.e[ls].dwell_ms > 10000, "LongSlow dwell stretched to three long packets");
        check(s.e[st].dwell_ms == 10000 && s.e[st].hang_ms == a_st + 1000, "ShortTurbo dwell = base, hang = one packet + 1 s");
    }

    /* ---- EU_868: PRESETS_EU_868, one 250 kHz slot ---- */
    mt_scan_init(&s, EU, 10000);
    check(s.n == 7, "EU_868 scans the seven PRESETS_EU_868 presets (no turbo)");
    lf = entry_named(&s, "LongFast");
    check(lf >= 0 && s.e[lf].freq_hz == 869525000ull && s.e[lf].slots == 1, "EU_868 LongFast 869.525 MHz (published)");
    {
        const int ls = entry_named(&s, "LongSlow");
        check(ls >= 0 && s.e[ls].slots == 2, "EU_868 LongSlow: two 125 kHz slots");
    }
    /* ---- 2.4 GHz: bandwidth x3.25, preamble 12 ---- */
    mt_scan_init(&s, L24, 10000);
    lf = entry_named(&s, "LongFast");
    check(lf >= 0 && s.e[lf].bw_hz == 812500 && s.e[lf].preamble == 12, "LORA_24 LongFast 812.5 kHz, preamble 12");
    /* ---- round(), not floor: PH_868 fits 2.8 ShortTurbo slots, upstream says 3 ---- */
    check(mt_scan_slots(PH868, 500000) == 3, "PH_868 500 kHz: round(2.8) = 3 slots, as upstream");

    /* ---- the scheduler ---- */
    mt_scan_init(&s, US, 10000);
    lf = entry_named(&s, "LongFast");
    const int lm = 1;                                       /* LongMod comes second */
    check(mt_scan_start(&s, 0) == 0 && s.cur == 0, "start tunes the first preset");
    check(mt_scan_tick(&s, 9999) == -1, "stays for its dwell");
    check(mt_scan_tick(&s, 10000) == lm && s.e[0].listen_ms == 10000, "moves on after 10 s, listen time kept");
    uint8_t b[256];
    int n = frame(b, 0x11223344, 1, s.e[lm].def_hash, "hi", true);
    check(mt_scan_heard(&s, b, n, -900, 55, 19500), "a packet near the end of the dwell");
    check(mt_scan_tick(&s, 20500) == -1, "holds after a packet (hang)");
    check(mt_scan_tick(&s, 19500 + s.e[lm].hang_ms + 1) == 2, "moves on once the hang runs out");
    /* traffic that never stops: out after two dwells */
    {
        const uint32_t t0 = 19500 + s.e[lm].hang_ms + 1, d = s.e[2].dwell_ms;
        int moved = -1;
        for (uint32_t t = t0; t < t0 + 4 * d && moved < 0; t += 500) {
            mt_scan_heard(&s, b, n, -900, 55, t);
            const int r = mt_scan_tick(&s, t);
            if (r >= 0) moved = (int)(t - t0);
        }
        check(moved >= (int)(2 * d) && moved < (int)(2 * d) + 1000, "a busy preset is left after two dwells");
    }
    /* around the cycle: a sweep */
    {
        uint32_t t = 200000;
        int guard = 0;
        while (s.cur != 0 && guard++ < 40) { t += 40000; mt_scan_tick(&s, t); }
        check(s.cur == 0 && s.sweeps == 1, "back to the first preset: one sweep");
    }
    /* pin */
    {
        uint32_t t = 600000;
        check(mt_scan_pin(&s, 5, t) == 5 && s.cur == 5, "pin tunes that preset at once");
        int stayed = 1;
        for (int k = 1; k < 20; k++) stayed &= mt_scan_tick(&s, t + (uint32_t)k * 60000u) == -1;
        check(stayed && s.cur == 5, "a pinned preset is never left");
        t += 20 * 60000u;
        check(mt_scan_pin(&s, -1, t) == -1 && mt_scan_tick(&s, t + 1) == -1, "unpin: a full dwell first");
        check(mt_scan_tick(&s, t + s.e[5].dwell_ms) == 6, "then the cycle goes on");
    }
    /* presets out of the cycle */
    {
        for (int i = 1; i < s.n; i++) check(mt_scan_enable(&s, i, false) || i == 0, "  off");
        check(!mt_scan_enable(&s, 0, false), "the last preset on stays on");
        uint32_t t = 3000000;
        mt_scan_tick(&s, t);                                /* off where it was: to the one left */
        check(s.cur == 0, "only LongFast left: it is tuned");
        check(mt_scan_tick(&s, t + 100000) == -1 && s.cur == 0, "one preset on: it stays");
        for (int i = 0; i < s.n; i++) mt_scan_enable(&s, i, true);
    }

    /* ---- the table: real frames ---- */
    mt_scan_init(&s, US, 10000);
    mt_scan_start(&s, 0);
    lf = 0;
    n = frame(b, 0xA1B2C3D4, 0x100, s.e[lf].def_hash, "hello mesh", true);
    check(mt_scan_heard(&s, b, n, -1012, -35, 1000), "default-channel text heard");
    const mt_scan_pkt_t *p = mt_scan_log_at(&s, 0);
    check(p && p->readable && p->is_default && p->portnum == MT_PORT_TEXT && !strcmp(p->text, "hello mesh"),
          "decoded with AQ==: TEXT \"hello mesh\"");
    check(p && p->from == 0xA1B2C3D4 && p->to == MT_BROADCAST && p->hop_limit == 3 && p->hop_start == 3 && p->id == 0x100,
          "header: from, to, hops, id");
    n = frame(b, 0xA1B2C3D4, 0x101, s.e[lf].def_hash, "again", true);
    mt_scan_heard(&s, b, n, -980, 20, 2000);
    n = frame(b, 0x0BADCAFE, 0x200, 0x3A, "secret", false);
    mt_scan_heard(&s, b, n, -1100, -80, 3000);
    p = mt_scan_log_at(&s, 0);
    check(p && !p->readable && !p->is_default && p->hash == 0x3A, "a private channel: hash 0x3A, not read");
    check(s.e[lf].packets == 3 && s.e[lf].node_n == 2 && s.e[lf].readable == 2, "3 packets, 2 nodes, 2 read");
    check(s.e[lf].hash_n == 2 && mt_scan_top_hash(&s.e[lf]) == s.e[lf].def_hash, "two hashes seen, the default on top");
    check(s.e[lf].rssi == -1100 && s.e[lf].snr == -80 && s.e[lf].last_ms == 3000, "last RSSI, SNR and time");
    check(!mt_scan_heard(&s, (const uint8_t *)"SIM-RX24", 8, -500, 0, 3500) && s.total == 3, "a short frame is no packet");
    mt_scan_tick(&s, 10000 + s.e[lf].hang_ms);              /* to LongMod */
    n = frame(b, 0x55667788, 0x300, s.e[1].def_hash, "on longmod", true);
    mt_scan_heard(&s, b, n, -950, 60, 12000);
    check(s.seen_n == 3, "three distinct senders over all presets");
    {
        mt_scan_tick(&s, 30000);                            /* LongMod heard 18 s, then on to LongSlow */
        const float r0 = mt_scan_rate(&s, 0), r1 = mt_scan_rate(&s, 1);
        const float a = mt_scan_share(&s, 0), c = mt_scan_share(&s, 1);
        printf("     rate LongFast %.2f/min over %u ms, LongMod %.2f/min over %u ms; share %.3f %.3f\n", (double)r0,
               (unsigned)s.e[0].listen_ms, (double)r1, (unsigned)s.e[1].listen_ms, (double)a, (double)c);
        check(r0 > 15.0f && r1 > 3.0f && r1 < 3.5f, "packets a minute of listening");
        check(a > 0.80f && a < 0.85f && a + c > 0.999f && a + c < 1.001f, "share by rate: a longer listen does not inflate it");
    }
    check(mt_scan_log_at(&s, 0)->entry == 1 && mt_scan_log_at(&s, 3)->from == 0xA1B2C3D4, "log newest first, with its preset");
    mt_scan_clear(&s);
    check(s.total == 0 && s.e[0].packets == 0 && s.log_n == 0 && s.n == 10, "CLEAR empties the counts, keeps the plan");

    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
