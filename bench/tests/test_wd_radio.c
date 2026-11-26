/* WARDRIVE's radio jobs (wd_radio.c): what the scheduler picks for each
   radio, when it moves on, pins and yields, and how the decoders' messages
   become sightings. The port is a fake: two radios whose presence, abilities
   and holders the test sets. */
#include "wardrive/wd_radio.h"
#include "wardrive/wd_feed.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- what wd_radio.c needs from the rest of WARDRIVE ---- */
static wd_stats_t S;
const wd_stats_t *wd_stats(void) { return &S; }
void wd_lock(void) {}
void wd_unlock(void) {}

/* ---- the fake port ---- */
static wd_rport_t P[WD_RJ_N];
static int job_run[WD_RJ_N] = { -1, -1 }, runs[WD_RJ_N];
static wd_obs_t Q[WD_RJ_N][16];
static int qn[WD_RJ_N];
void wd_rport_probe(int r, wd_rport_t *o) { *o = P[r]; }
bool wd_rport_run(int r, int job, int region, char *why, size_t n)
{
    (void)region; (void)why; (void)n;
    job_run[r] = job;
    runs[r]++;
    return true;
}
int wd_rport_poll(int r, wd_obs_t *out, int cap, uint32_t now)
{
    (void)now;
    int k = qn[r] < cap ? qn[r] : cap;
    memcpy(out, Q[r], sizeof(wd_obs_t) * (size_t)k);
    qn[r] = 0;
    return k;
}
int wd_rport_passive(wd_obs_t *out, int cap, uint32_t now) { (void)out; (void)cap; (void)now; return 0; }
void wd_rport_start(void) {}
void wd_rport_stop(void) {}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void radios(bool sdr)
{
    memset(P, 0, sizeof(P));
    P[WD_RJ_LORA] = (wd_rport_t){ .present = true, .name = "LR2021",
                                  .can = 1 << WD_JOB_TPMS | 1 << WD_JOB_ISM | 1 << WD_JOB_MESH };
    P[WD_RJ_SDR] = (wd_rport_t){ .present = sdr, .name = "RTL-SDR",
                                 .can = 1 << WD_JOB_TPMS | 1 << WD_JOB_ISM | 1 << WD_JOB_AIR };
}

static void place(float speed)
{
    memset(&S, 0, sizeof(S));
    S.fix.fix = true;
    S.fix.lat = 43.44; S.fix.lon = -71.64;      /* New Hampshire */
    S.fix_ms = 1;
    S.speed = speed;
}

static void session(void)
{
    const bool src[WD_KINDS] = { true, true, true, true, true, true, true };
    wd_radio_pin(0, -1);
    wd_radio_pin(1, -1);
    wd_radio_start(src);
}

static wd_obs_t tpms(uint32_t id, int radio)
{
    lr433_msg_t m;
    memset(&m, 0, sizeof(m));
    m.proto = LR433_P_SCHRADER;
    m.channel = -1;
    snprintf(m.id, sizeof(m.id), "%07X", (unsigned)id);
    wd_obs_t o;
    wd_radio_obs_ism(&o, &m, 315000000u, -60, radio);
    return o;
}

/* Poll every 250 ms from t0 for ms; the fix's clock follows. */
static uint32_t run(uint32_t t0, uint32_t ms)
{
    static wd_obs_t out[64];
    uint32_t t = t0;
    for (; t < t0 + ms; t += 250) {
        S.fix_ms = t;
        wd_radio_poll(out, 64, t);
    }
    return t;
}

static void test_tables(void)
{
    CHECK(wd_radio_region_of(43.4, -71.6) == WD_REGION_NA);
    CHECK(wd_radio_region_of(52.5, 13.4) == WD_REGION_EU);
    CHECK(wd_radio_region_of(-34.6, -58.4) == WD_REGION_AU);    /* Buenos Aires */
    CHECK(wd_radio_region_of(-33.9, 151.2) == WD_REGION_AU);
    CHECK(wd_radio_region_of(35.7, 139.7) == WD_REGION_EU);
    CHECK(wd_radio_job_khz(WD_JOB_TPMS, WD_REGION_NA) == 315000);
    CHECK(wd_radio_job_khz(WD_JOB_TPMS, WD_REGION_EU) == 433920);
    CHECK(wd_radio_job_khz(WD_JOB_AIR, WD_REGION_AU) == 1090000);
    uint32_t f[4];
    CHECK(wd_radio_job_freqs(WD_JOB_MESH, WD_REGION_NA, f) == 2 && f[0] == 906875);
    CHECK(wd_radio_motion(-1) == WD_MOVE_UNKNOWN);
    CHECK(wd_radio_motion(0.5f) == WD_MOVE_STILL);
    CHECK(wd_radio_motion(13) == WD_MOVE_DRIVE);
}

static void test_driving_picks(void)
{
    radios(true);
    place(13);
    session();
    run(1000, 500);
    wd_radio_state_t v;
    wd_radio_view(&v);
    /* Driving: the LoRa chip listens for tyres; the SDR, not doubling it, takes the sky. */
    CHECK(v.r[WD_RJ_LORA].job == WD_JOB_TPMS && v.r[WD_RJ_LORA].state == WD_RS_RUN);
    CHECK(v.r[WD_RJ_SDR].job == WD_JOB_AIR);
    CHECK(job_run[WD_RJ_SDR] == WD_JOB_AIR);
    CHECK(v.region == WD_REGION_NA && v.motion == WD_MOVE_DRIVE);
    CHECK(v.r[WD_RJ_LORA].plan_n >= 2 && v.r[WD_RJ_LORA].plan[0].job == WD_JOB_TPMS);
    wd_radio_stop();
    CHECK(job_run[0] == -1 && job_run[1] == -1);
}

static void test_standing_picks(void)
{
    radios(true);
    place(0);
    session();
    run(1000, 500);
    wd_radio_state_t v;
    wd_radio_view(&v);
    CHECK(v.r[WD_RJ_LORA].job == WD_JOB_MESH);
    CHECK(v.r[WD_RJ_SDR].job == WD_JOB_AIR);
    wd_radio_stop();
}

static void test_dry_backs_off(void)
{
    radios(true);
    place(13);
    session();
    uint32_t t = run(1000, 500);
    /* Five minutes of TPMS that hears nothing: the chip goes elsewhere. */
    bool left = false;
    for (int i = 0; i < 20 && !left; i++) {
        t = run(t, 15000);
        wd_radio_state_t v;
        wd_radio_view(&v);
        left = v.r[WD_RJ_LORA].job != WD_JOB_TPMS;
        if (left) CHECK(v.r[WD_RJ_LORA].jobs[WD_JOB_TPMS].dry >= 1);
    }
    CHECK(left);
    wd_radio_stop();
}

static void test_productive_stretch(void)
{
    radios(true);
    place(13);
    session();
    uint32_t t = run(1000, 500);
    wd_radio_state_t v;
    wd_radio_view(&v);
    const uint32_t base = v.r[WD_RJ_LORA].until_ms;
    /* A tyre a few seconds before the dwell ends: it is stretched. */
    t = run(t, base - t - 3000);
    Q[WD_RJ_LORA][0] = tpms(0x1A2B3C, WD_RADIO_LORA);
    Q[WD_RJ_LORA][1] = tpms(0x1A2B3C, WD_RADIO_LORA);    /* the same burst again: one find */
    qn[WD_RJ_LORA] = 2;
    t = run(t, 4000);
    wd_radio_view(&v);
    CHECK(v.r[WD_RJ_LORA].job == WD_JOB_TPMS);
    CHECK(v.r[WD_RJ_LORA].until_ms > base);
    CHECK(v.r[WD_RJ_LORA].hits == 1);
    CHECK(v.r[WD_RJ_LORA].jobs[WD_JOB_TPMS].last_hit_ms != 0);
    int sum = 0;
    for (int i = 0; i < WD_TRACE; i++) sum += v.r[WD_RJ_LORA].trace[i];
    CHECK(sum == 1);
    /* A TPMS find on the SDR, whose job is ADS-B, is someone else's: passive. */
    Q[WD_RJ_SDR][0] = tpms(0x777777, WD_RADIO_SDR);
    qn[WD_RJ_SDR] = 1;
    run(t, 500);
    wd_radio_view(&v);
    CHECK(v.passive == 1 && v.r[WD_RJ_SDR].hits == 0);
    wd_radio_stop();
}

static void test_pin_yield_absent(void)
{
    radios(true);
    place(13);
    session();
    wd_radio_pin(WD_RJ_SDR, WD_JOB_ISM);
    uint32_t t = run(1000, 500);
    wd_radio_state_t v;
    wd_radio_view(&v);
    CHECK(v.r[WD_RJ_SDR].job == WD_JOB_ISM && v.r[WD_RJ_SDR].pin == WD_JOB_ISM);
    CHECK(v.r[WD_RJ_SDR].plan_n == 1 && v.r[WD_RJ_SDR].plan[0].job == WD_JOB_ISM);
    t = run(t, 10 * 60000);
    wd_radio_view(&v);
    CHECK(v.r[WD_RJ_SDR].job == WD_JOB_ISM);              /* the whole drive */
    wd_radio_pin(WD_RJ_SDR, -1);
    t = run(t, 1000);
    wd_radio_view(&v);
    CHECK(v.r[WD_RJ_SDR].pin == -1);
    /* An app takes the SDR: the job waits, its clock stopped. */
    snprintf(P[WD_RJ_SDR].busy, sizeof(P[WD_RJ_SDR].busy), "FM");
    wd_radio_view(&v);
    const uint32_t left = v.r[WD_RJ_SDR].until_ms - t;
    t = run(t, 30000);
    wd_radio_view(&v);
    CHECK(v.r[WD_RJ_SDR].state == WD_RS_YIELD);
    CHECK(strstr(v.r[WD_RJ_SDR].why, "FM") != NULL);
    CHECK(v.r[WD_RJ_SDR].until_ms - t >= left - 500);
    P[WD_RJ_SDR].busy[0] = 0;
    /* Unplugged (no 5 V on battery): the LoRa chip goes on alone. */
    P[WD_RJ_SDR].present = false;
    t = run(t, 1000);
    wd_radio_view(&v);
    CHECK(v.r[WD_RJ_SDR].state == WD_RS_ABSENT && v.r[WD_RJ_SDR].job == -1);
    CHECK(v.r[WD_RJ_LORA].state == WD_RS_RUN);
    CHECK(job_run[WD_RJ_SDR] == -1);
    wd_radio_stop();
}

static void test_no_sdr_has(void)
{
    radios(false);
    CHECK(wd_radio_has(WD_RF));
    CHECK(wd_radio_has(WD_LORA));
    CHECK(!wd_radio_has(WD_AIR));
    radios(true);
    CHECK(wd_radio_has(WD_AIR));
}

static void test_ism_mapping(void)
{
    lr433_msg_t m;
    memset(&m, 0, sizeof(m));
    m.proto = LR433_P_TOYOTA;
    m.channel = -1;
    snprintf(m.id, sizeof(m.id), "0f1e2d3c");
    wd_obs_t o;
    CHECK(wd_radio_obs_ism(&o, &m, 315000000u, -71.4f, WD_RADIO_LORA));
    CHECK(o.kind == WD_RF && (o.tags & WD_T_VEHICLE) && o.radio == WD_RADIO_LORA);
    CHECK(o.addr[2] == 0x0f && o.addr[3] == 0x1e && o.addr[4] == 0x2d && o.addr[5] == 0x3c);
    CHECK(o.freq_khz == 315000 && o.rssi == -71);
    CHECK(strncmp(o.name, "TPMS ", 5) == 0 && strstr(o.name, "0f1e2d3c"));
    CHECK(wd_radio_job_of(&o) == WD_JOB_TPMS);
    /* a weather sensor with a decimal ID */
    m.proto = LR433_P_ACURITE_TOWER;
    m.channel = 2;
    snprintf(m.id, sizeof(m.id), "12053");
    CHECK(wd_radio_obs_ism(&o, &m, 433920000u, NAN, WD_RADIO_SDR));
    CHECK(!(o.tags & WD_T_VEHICLE) && o.rssi == -90);
    CHECK(o.addr[4] == (12053 >> 8) && o.addr[5] == (12053 & 0xFF) && o.addr[1] == 3);
    CHECK(wd_radio_job_of(&o) == WD_JOB_ISM);
    m.id[0] = 0;
    CHECK(!wd_radio_obs_ism(&o, &m, 433920000u, -50, WD_RADIO_SDR));
}

static void test_air_mapping(void)
{
    wd_air_t a = { .icao = 0xA1B2C3, .qual = 0, .cat = 0x43, .pos = true, .lat = 43.5f, .lon = -71.5f };
    snprintf(a.callsign, sizeof(a.callsign), "DAL17   ");
    wd_obs_t o;
    wd_radio_obs_air(&o, &a, WD_RADIO_SDR);
    CHECK(o.kind == WD_AIR && !strcmp(o.name, "DAL17"));
    CHECK(o.addr[3] == 0xA1 && o.addr[4] == 0xB2 && o.addr[5] == 0xC3);
    CHECK(o.own_pos && fabs(o.lat - 43.5) < 1e-4 && o.freq_khz == 1090000);
    a.callsign[0] = 0;
    a.pos = false;
    wd_radio_obs_air(&o, &a, WD_RADIO_SDR);
    CHECK(!strcmp(o.name, "ICAO A1B2C3") && !o.own_pos);
}

static void test_lora_mapping(void)
{
    wd_obs_t o;
    uint8_t p[160];
    memset(p, 0, sizeof(p));
    /* Meshtastic: broadcast from !12345678, hop start 3 limit 2 */
    memset(p, 0xFF, 4);
    p[4] = 0x78; p[5] = 0x56; p[6] = 0x34; p[7] = 0x12;
    p[12] = 3 << 5 | 2;
    CHECK(wd_radio_obs_lora(&o, p, 30, WD_MESH_MESHTASTIC, -95, 906875000u, WD_RADIO_LORA));
    CHECK(o.kind == WD_LORA && !strcmp(o.name, "!12345678") && o.sub == WD_MESH_MESHTASTIC);
    CHECK(o.addr[5] == 0x78 && o.freq_khz == 906875);
    p[12] = 1 << 5 | 5;                                 /* limit above start: not a real header */
    CHECK(!wd_radio_obs_lora(&o, p, 30, WD_MESH_MESHTASTIC, -95, 906875000u, WD_RADIO_LORA));
    /* MeshCore advert, flood, one hop, a place and a name */
    memset(p, 0, sizeof(p));
    p[0] = 1 | 4 << 2;
    p[1] = 1; p[2] = 0xAB;
    for (int i = 0; i < 32; i++) p[3 + i] = (uint8_t)(0x10 + i);
    int j = 3 + 100;
    p[j++] = 0x02 | 0x10 | 0x80;                        /* repeater, place, name */
    const int32_t la = 43444500, lo = -71647300;
    memcpy(p + j, &la, 4); memcpy(p + j + 4, &lo, 4);
    j += 8;
    memcpy(p + j, "Ridge", 5);
    j += 5;
    CHECK(wd_radio_obs_lora(&o, p, j, WD_MESH_MESHCORE, -88, 910525000u, WD_RADIO_LORA));
    CHECK(!strcmp(o.name, "Ridge") && o.own_pos && fabs(o.lat - 43.4445) < 1e-6 && fabs(o.lon + 71.6473) < 1e-6);
    CHECK(o.addr[0] == 0x10 && o.addr[5] == 0x15 && !(o.tags & WD_T_INFRA));   /* a repeater is logged, not a target */
    p[0] = 1 | 2 << 2;                                  /* a text message: no identity */
    CHECK(!wd_radio_obs_lora(&o, p, j, WD_MESH_MESHCORE, -88, 910525000u, WD_RADIO_LORA));
    /* LoRaWAN unconfirmed uplink from 26011BDA */
    memset(p, 0, sizeof(p));
    p[0] = 2 << 5;
    p[1] = 0xDA; p[2] = 0x1B; p[3] = 0x01; p[4] = 0x26;
    CHECK(wd_radio_obs_lora(&o, p, 20, WD_MESH_LORAWAN, -110, 904300000u, WD_RADIO_LORA));
    CHECK(!strcmp(o.name, "LoRaWAN 26011BDA") && o.sub == WD_MESH_LORAWAN);
    p[0] = 3 << 5;                                      /* a downlink */
    CHECK(!wd_radio_obs_lora(&o, p, 20, WD_MESH_LORAWAN, -110, 904300000u, WD_RADIO_LORA));
}

int main(void)
{
    test_tables();
    test_driving_picks();
    test_standing_picks();
    test_dry_backs_off();
    test_productive_stretch();
    test_pin_yield_absent();
    test_no_sdr_has();
    test_ism_mapping();
    test_air_mapping();
    test_lora_mapping();
    if (fails) { printf("test_wd_radio: %d failed\n", fails); return 1; }
    printf("test_wd_radio: all passed\n");
    return 0;
}
