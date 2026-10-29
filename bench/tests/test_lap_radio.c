/* API 2.0 for native apps (ls_lap_radio.h): permission checks, the session's
 * lifetime and cleanup when the app closes, the I/Q queue when the app falls
 * behind, the spectrum feed, audio, decoded events, actions, notices, grants
 * remembered per install, manifests old and new, and the export table an API
 * 1.3 app links against.
 *
 *   test_lap_radio <api 1.3 exports fixture> <app-sdk/exports.txt> */
#include "ls_lap_api.h"
#include "ls_lap_grants.h"
#include "ls_lap_radio.h"
#include "ls_lap_sys.h"
#include "core/ls_decoded.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_value.h"

/* What ls_lap_api.c draws and reads through; nothing here draws. */
void ls_tui_image(tui_rect r, const uint16_t *p, int w, int h, uint32_t serial) { (void)r; (void)p; (void)w; (void)h; (void)serial; }
void ls_tui_image_ring(tui_rect r, const uint16_t *p, int w, int h, int x0, int y0, int dx, int dy, uint32_t serial) {
    (void)r; (void)p; (void)w; (void)h; (void)x0; (void)y0; (void)dx; (void)dy; (void)serial;
}
bool ls_value_read(const char *path, ls_val_t *out, const char **unit) { (void)path; (void)out; (void)unit; return false; }

static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

/* ---- a backend the test drives ------------------------------------------ */

static struct {
    int opens, closes, tunes, gains, rates, polls;
    int again;                           /* answer EAGAIN this many times */
    int open_error, poll_error;
    uint64_t tuned;
} fake;
static int f_list(lap_radio_info_t *out, int max) {
    if (max < 2) return 0;
    memset(out, 0, 2 * sizeof(*out));
    snprintf(out[0].name, sizeof(out[0].name), "FAKE");
    out[0].kind = LAP_RADIO_KIND_SIM;
    out[0].features = LAP_RXF_IQ | LAP_RXF_SPECTRUM | LAP_RXF_GAIN | LAP_RXF_RATE;
    out[0].present = true;
    out[0].min_hz = 100000000; out[0].max_hz = 200000000;
    out[0].min_rate = 48000; out[0].max_rate = 1024000;
    out[0].gain_max = 400;
    snprintf(out[1].name, sizeof(out[1].name), "PACKETS");
    out[1].kind = LAP_RADIO_KIND_SX1262;
    out[1].present = true;
    return 2;
}
static int f_open(int radio, const lap_rx_config_t *cfg, ls_lap_rx_actual_t *a) {
    fake.opens++;
    if (fake.again) { fake.again--; return LAP_RX_EAGAIN; }
    if (fake.open_error) return fake.open_error;
    (void)radio;
    *a = (ls_lap_rx_actual_t){cfg->center_hz, cfg->rate_hz, cfg->gain_tenths_db};
    ls_lap_rx_mark(cfg->center_hz, cfg->rate_hz, LAP_IQ_GAP | LAP_IQ_RETUNED);
    return 0;
}
static int f_tune(uint64_t hz, ls_lap_rx_actual_t *a) { fake.tunes++; fake.tuned = a->center_hz = hz; ls_lap_rx_mark(hz, 0, LAP_IQ_RETUNED); return 0; }
static int f_gain(int g, ls_lap_rx_actual_t *a) { fake.gains++; a->gain_tenths_db = g; return 0; }
static int f_rate(uint32_t r, ls_lap_rx_actual_t *a) { fake.rates++; a->rate_hz = r; ls_lap_rx_mark(0, r, LAP_IQ_GAP); return 0; }
static int f_poll(ls_lap_rx_actual_t *a) { (void)a; fake.polls++; return fake.poll_error; }
static void f_close(void) { fake.closes++; }
static const ls_lap_rx_backend_t FAKE = {f_list, f_open, f_tune, f_gain, f_rate, f_poll, f_close};

/* ---- app context -------------------------------------------------------- */

static lap_app_t app;
static void enter(uint8_t flags) {
    memset(&app, 0, sizeof(app));
    snprintf(app.id, sizeof(app.id), "radio-test");
    snprintf(app.name, sizeof(app.name), "RADIOTEST");
    app.api_major = 2;
    app.flags = flags;
    ls_lap_api_begin(app.id, 1, 0);
    ls_lap_sys_begin();
    ls_lap_radio_begin(&app);
    ls_lap_api_callback(true);
}
static void leave(void) {
    ls_lap_api_callback(false);
    ls_lap_sys_end();
    for (int i = 0; i < 4; i++) ls_lap_radio_service();
    ls_lap_api_cleanup();
}
static void pump(int n) {
    ls_lap_api_callback(false);
    for (int i = 0; i < n; i++) ls_lap_radio_service();
    ls_lap_api_callback(true);
}
static int16_t *tone(size_t pairs, float cycles_per_sample, int16_t amp) {
    static int16_t buf[2 * 4096];
    assert(pairs <= 4096);
    for (size_t i = 0; i < pairs; i++) {
        buf[2 * i] = (int16_t)(amp * cosf(6.2831853f * cycles_per_sample * (float)i));
        buf[2 * i + 1] = (int16_t)(amp * sinf(6.2831853f * cycles_per_sample * (float)i));
    }
    return buf;
}

/* ---- the cases ---------------------------------------------------------- */

static void manifests(void) {
    lap_app_t m = {.magic = LAP_MAGIC, .api_major = 1, .api_minor = 3, .flags = LAP_CAP_NETWORK};
    snprintf(m.id, sizeof(m.id), "old"); snprintf(m.name, sizeof(m.name), "OLD");
    char why[96];
    CHECK(ls_lap_fields_valid(&m, why, sizeof(why)));             /* a 1.3 app still loads */
    for (int minor = 0; minor <= 3; minor++) {
        m.api_minor = (uint16_t)minor; m.flags = 0;
        CHECK(ls_lap_fields_valid(&m, why, sizeof(why)));
    }
    m.api_minor = 3; m.flags = LAP_CAP_RADIO_RX;                     /* radio needs 2.0 */
    CHECK(!ls_lap_fields_valid(&m, why, sizeof(why)));
    m.api_minor = 4; m.flags = 0;
    CHECK(!ls_lap_fields_valid(&m, why, sizeof(why)));
    m.api_major = 2; m.api_minor = 0;
    m.flags = LAP_CAP_NETWORK | LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX;
    CHECK(ls_lap_fields_valid(&m, why, sizeof(why)));
    m.flags = 8;                                                     /* no such capability */
    CHECK(!ls_lap_fields_valid(&m, why, sizeof(why)));
    m.flags = 0; m.api_minor = 3;                                    /* the next minor, past 2.2 */
    CHECK(!ls_lap_fields_valid(&m, why, sizeof(why)));
    m.api_major = 3; m.api_minor = 0;
    CHECK(!ls_lap_fields_valid(&m, why, sizeof(why)));
}

static int resolve_file(const char *path, int *missing) {
    FILE *f = fopen(path, "r");
    assert(f);
    char name[96];
    int n = 0;
    while (fscanf(f, "%95s", name) == 1) {
        n++;
        if (!ls_lap_resolve(name)) { printf("unresolved: %s\n", name); (*missing)++; }
    }
    fclose(f);
    return n;
}
static void abi(const char *api13, const char *now) {
    /* Every symbol a 1.3 app could import still resolves, by the same name. */
    int missing = 0;
    CHECK(resolve_file(api13, &missing) == 216 && !missing);
    CHECK(resolve_file(now, &missing) >= 239 && !missing);
    size_t n = 0;
    const ls_lap_export_t *e = ls_lap_exports(&n);
    for (size_t i = 1; i < n; i++) CHECK(strcmp(e[i - 1].name, e[i].name) < 0);
    CHECK(!ls_lap_resolve("lap_rx_transmit") && ls_lap_resolve("lap_rx_open") && ls_lap_resolve("lap_tcp_connect"));
}

/* The bench's action table: records what the app was granted. */
static uint32_t granted_bits;
static int hook(const char *n, const char *a, char *o, size_t c, uint32_t g) {
    (void)n; (void)a; (void)o; (void)c;
    granted_bits = g;
    return LAP_ACT_OK;
}
static void permissions(void) {
    enter(0);                                            /* a 2.0 app that asked for nothing */
    lap_radio_info_t *list = ls_lap_malloc(sizeof(lap_radio_info_t) * LAP_RADIOS_MAX);
    lap_rx_config_t *cfg = ls_lap_malloc(sizeof(*cfg));
    *cfg = (lap_rx_config_t){150000000, 48000, LAP_GAIN_AUTO, 0};
    CHECK(lap_radio_list(list, LAP_RADIOS_MAX) == LAP_RX_EACCES);
    CHECK(lap_rx_open(0, cfg) == LAP_RX_EACCES);
    CHECK(lap_audio_listen(true) == LAP_AUDIO_EACCES);
    CHECK(lap_event_subscribe(LAP_EV_PAGE) == LAP_RX_EACCES);
    CHECK(lap_audio_write((int16_t *)cfg, 2) == 2);     /* playing needs no radio */
    /* Actions: read only without RADIO_RX. */
    char *name = ls_lap_malloc(16);
    strcpy(name, "fm.freq");
    ls_lap_host_action = NULL;
    CHECK(lap_action(name, NULL, NULL, 0) == LAP_ACT_UNAVAILABLE);
    ls_lap_host_action = hook;
    lap_action(name, NULL, NULL, 0); CHECK(granted_bits == 1u);
    CHECK(lap_action("fm.freq", NULL, NULL, 0) == LAP_ACT_BADARG);   /* a name not in app memory */
    leave();

    enter(LAP_CAP_RADIO_RX);
    name = ls_lap_malloc(16); strcpy(name, "fm.freq");
    lap_action(name, NULL, NULL, 0); CHECK(granted_bits == 3u);              /* read + tune */
    list = ls_lap_malloc(sizeof(lap_radio_info_t) * LAP_RADIOS_MAX);
    pump(1);
    CHECK(lap_radio_list(list, LAP_RADIOS_MAX) == 1 && !strcmp(list[0].name, "SIM"));   /* the bench radio */
    CHECK(lap_radio_list((lap_radio_info_t *)&granted_bits, 1) == LAP_RX_EINVAL);   /* not the app's memory */
    ls_lap_api_callback(false);
    CHECK(lap_radio_list(list, 1) == LAP_RX_EINVAL);                  /* outside a callback */
    ls_lap_api_callback(true);
    leave();

    enter(LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX);
    name = ls_lap_malloc(16); strcpy(name, "mesh.send");
    lap_action(name, NULL, NULL, 0); CHECK(granted_bits == 35u);             /* read + tune + tx; never ui/store/power */
    leave();
    ls_lap_host_action = NULL;
}

static void lifetime(void) {
    ls_lap_radio_set_backend(&FAKE);
    memset(&fake, 0, sizeof(fake));
    enter(LAP_CAP_RADIO_RX);
    lap_radio_info_t *list = ls_lap_malloc(sizeof(lap_radio_info_t) * 4);
    lap_rx_config_t *cfg = ls_lap_malloc(sizeof(*cfg));
    lap_rx_status_t *st = ls_lap_malloc(sizeof(*st));
    int16_t *iq = ls_lap_malloc(4 * 4096);
    lap_iq_meta_t *meta = ls_lap_malloc(sizeof(*meta));
    pump(1);
    CHECK(lap_radio_list(list, 4) == 2 && !strcmp(list[0].name, "FAKE"));
    *cfg = (lap_rx_config_t){150000000, 0, LAP_GAIN_AUTO, 0};
    CHECK(lap_rx_open(1, cfg) == LAP_RX_EUNSUPPORTED);              /* the SX1262 has no I/Q */
    CHECK(lap_rx_open(7, cfg) == LAP_RX_ENODEV);
    cfg->center_hz = 50000000;
    CHECK(lap_rx_open(0, cfg) == LAP_RX_ERANGE);
    cfg->center_hz = 150000000; cfg->flags = LAP_RX_WIDE;
    CHECK(lap_rx_open(0, cfg) == LAP_RX_EUNSUPPORTED);
    cfg->flags = 0;
    fake.again = 2;                                                  /* the radio is slow to come */
    const int h = lap_rx_open(0, cfg);
    CHECK(h > 0 && lap_rx_state(h) == LAP_RX_OPENING);
    CHECK(lap_rx_open(0, cfg) == LAP_RX_EBUSY);                      /* one session */
    pump(1); CHECK(lap_rx_state(h) == LAP_RX_OPENING);
    pump(2); CHECK(lap_rx_state(h) == LAP_RX_RUNNING && fake.opens == 3);
    CHECK(lap_rx_status(h, st) == 0 && st->rate_hz == 48000 && st->capacity == LS_LAP_RX_SLOTS);
    CHECK(lap_rx_read(h, iq, 4096, meta) == 0);
    ls_lap_rx_push(tone(1000, 0.1f, 1000), 1000);
    CHECK(lap_rx_read(h, iq, 4096, meta) == 0);                      /* a block is handed over whole */
    ls_lap_rx_push(tone(1048, 0.1f, 1000), 1048);
    CHECK(lap_rx_read(h, iq, 1000, meta) == 1000);
    CHECK(meta->first_sample == 0 && meta->flags == (LAP_IQ_GAP | LAP_IQ_RETUNED) && meta->rate_hz == 48000 &&
          meta->center_hz == 150000000);
    CHECK(lap_rx_read(h, iq, 4096, meta) == 1048 && meta->first_sample == 1000 && meta->flags == 0);
    CHECK(lap_rx_read(h, iq, 4096, meta) == 0);
    /* Changes go to the worker; the next block says so. */
    CHECK(lap_rx_tune(h, 160000000) == 0 && lap_rx_tune(h, 260000000) == LAP_RX_ERANGE);
    CHECK(lap_rx_gain(h, 200) == 0 && lap_rx_gain(h, 900) == LAP_RX_ERANGE);
    CHECK(lap_rx_rate(h, 96000) == 0);
    pump(1);
    CHECK(fake.tunes == 1 && fake.gains == 1 && fake.rates == 1 && fake.tuned == 160000000);
    CHECK(lap_rx_status(h, st) == 0 && st->center_hz == 160000000 && st->rate_hz == 96000 && st->gain_tenths_db == 200);
    ls_lap_rx_push(tone(2048, 0.1f, 1000), 2048);
    CHECK(lap_rx_read(h, iq, 4096, meta) == 2048 && (meta->flags & LAP_IQ_RETUNED) && (meta->flags & LAP_IQ_GAP) &&
          meta->center_hz == 160000000 && meta->rate_hz == 96000);
    /* The app closes without closing its session: the board does it. */
    leave();
    CHECK(fake.closes == 1);
    ls_lap_rx_push(tone(2048, 0.1f, 1000), 2048);                    /* a late producer is harmless */
    /* The next app finds the radio free. */
    enter(LAP_CAP_RADIO_RX);
    cfg = ls_lap_malloc(sizeof(*cfg));
    *cfg = (lap_rx_config_t){150000000, 0, LAP_GAIN_AUTO, 0};
    pump(1);
    int h2 = lap_rx_open(0, cfg);
    CHECK(h2 > 0 && h2 != h);
    CHECK(lap_rx_state(h) == LAP_RX_EINVAL);                        /* an old handle means nothing */
    pump(1);
    CHECK(lap_rx_state(h2) == LAP_RX_RUNNING);
    /* A stream that dies keeps the handle (and its error) until closed. */
    fake.poll_error = LAP_RX_EIO;
    pump(1);
    CHECK(lap_rx_state(h2) == LAP_RX_EIO && fake.closes == 2);
    CHECK(lap_rx_open(0, cfg) == LAP_RX_EBUSY);
    lap_rx_close(h2);
    pump(1);
    CHECK(fake.closes == 2 && lap_rx_state(h2) == LAP_RX_EINVAL);   /* closed once, not twice */
    fake.poll_error = 0;
    /* Closing while the radio is still coming: the backend gives it back. */
    fake.again = 100;
    h2 = lap_rx_open(0, cfg);
    pump(3);
    lap_rx_close(h2);
    pump(1);
    fake.again = 0;
    h2 = lap_rx_open(0, cfg);
    CHECK(fake.closes == 3 && h2 > 0);
    pump(1);
    CHECK(lap_rx_state(h2) == LAP_RX_RUNNING);
    leave();
    CHECK(fake.closes == 4);
    /* An app that closes before its radio ever opened leaves nothing to close. */
    enter(LAP_CAP_RADIO_RX);
    cfg = ls_lap_malloc(sizeof(*cfg));
    *cfg = (lap_rx_config_t){150000000, 0, LAP_GAIN_AUTO, 0};
    pump(1);
    CHECK(lap_rx_open(0, cfg) > 0);
    leave();
    CHECK(fake.closes == 4);
}

static void overflow(void) {
    memset(&fake, 0, sizeof(fake));
    enter(LAP_CAP_RADIO_RX);
    lap_rx_config_t *cfg = ls_lap_malloc(sizeof(*cfg));
    lap_rx_status_t *st = ls_lap_malloc(sizeof(*st));
    int16_t *iq = ls_lap_malloc(4 * LS_LAP_RX_SLOT_PAIRS);
    lap_iq_meta_t *meta = ls_lap_malloc(sizeof(*meta));
    pump(1);
    *cfg = (lap_rx_config_t){150000000, 48000, LAP_GAIN_AUTO, 0};
    const int h = lap_rx_open(0, cfg);
    pump(1);
    /* The app reads nothing while 40 blocks arrive: 32 fit, 8 are dropped. */
    for (int i = 0; i < 40; i++)
        for (int k = 0; k < 2; k++) ls_lap_rx_push(tone(1024, 0.05f, 2000), 1024);
    CHECK(lap_rx_status(h, st) == 0 && st->queued == 32 && st->overflows == 1 &&
          st->dropped == 8u * LS_LAP_RX_SLOT_PAIRS);
    uint64_t expect = 0;
    for (int i = 0; i < 32; i++) {
        CHECK(lap_rx_read(h, iq, LS_LAP_RX_SLOT_PAIRS, meta) == (int)LS_LAP_RX_SLOT_PAIRS);
        CHECK(meta->first_sample == expect);
        expect += LS_LAP_RX_SLOT_PAIRS;
    }
    CHECK(lap_rx_read(h, iq, 16, meta) == 0);
    /* The first block after the loss says GAP, and its index shows how much. */
    for (int k = 0; k < 2; k++) ls_lap_rx_push(tone(1024, 0.05f, 2000), 1024);
    CHECK(lap_rx_read(h, iq, LS_LAP_RX_SLOT_PAIRS, meta) == (int)LS_LAP_RX_SLOT_PAIRS);
    CHECK((meta->flags & LAP_IQ_GAP) && meta->first_sample == expect + 8u * LS_LAP_RX_SLOT_PAIRS);
    CHECK(lap_rx_status(h, st) == 0 && st->delivered == 33u * LS_LAP_RX_SLOT_PAIRS && st->overflows == 1);
    /* A second episode counts again. */
    for (int i = 0; i < 33; i++) ls_lap_rx_push(tone(2048, 0.05f, 2000), 2048);
    CHECK(lap_rx_status(h, st) == 0 && st->overflows == 2);
    leave();
}

static void spectrum(void) {
    memset(&fake, 0, sizeof(fake));
    enter(LAP_CAP_RADIO_RX);
    lap_rx_config_t *cfg = ls_lap_malloc(sizeof(*cfg));
    float *db = ls_lap_malloc(sizeof(float) * 64);
    uint32_t *seq = ls_lap_malloc(sizeof(*seq));
    int16_t *iq = ls_lap_malloc(4 * LS_LAP_RX_SLOT_PAIRS);
    pump(1);
    *cfg = (lap_rx_config_t){150000000, 48000, LAP_GAIN_AUTO, 0};
    const int h = lap_rx_open(0, cfg);
    pump(1);
    CHECK(lap_rx_spectrum(h, db, 64, seq) == 0);                     /* asks for the feed */
    CHECK(lap_rx_spectrum(h, db, 4, seq) == LAP_RX_EINVAL);
    /* A tone at +rate/8: bin 64 * (0.5 + 0.125) = 40 of 64. */
    for (int frame = 0; frame < 12; frame++) {
        ls_lap_rx_push(tone(512, 0.125f, 8000), 512);
        pump(1);
        while (lap_rx_read(h, iq, LS_LAP_RX_SLOT_PAIRS, NULL) > 0) {}
    }
    CHECK(lap_rx_spectrum(h, db, 64, seq) == 64 && *seq >= 1);
    int peak = 0;
    for (int i = 1; i < 64; i++) if (db[i] > db[peak]) peak = i;
    CHECK(peak == 40 && db[peak] > -20.0f && db[10] < db[peak] - 30.0f);
    leave();
    ls_lap_radio_set_backend(NULL);
}

static void audio(void) {
    enter(LAP_CAP_RADIO_RX);
    int16_t *pcm = ls_lap_malloc(2 * 20000);
    lap_audio_status_t *st = ls_lap_malloc(sizeof(*st));
    int16_t heard[100];
    for (int i = 0; i < 100; i++) heard[i] = (int16_t)i;
    ls_lap_audio_tap(heard, 100);                                    /* not listening: nothing */
    CHECK(lap_audio_read(pcm, 1000) == 0);
    CHECK(lap_audio_listen(true) == 0);
    ls_lap_audio_tap(heard, 100);
    CHECK(lap_audio_read(pcm, 1000) == 100 && pcm[99] == 99);
    for (int i = 0; i < 20; i++) ls_lap_audio_tap(heard, 100);
    for (int i = 0; i < 170; i++) ls_lap_audio_tap(heard, 100);      /* past the 16384-sample ring */
    CHECK(lap_audio_status(st) == 0 && st->listening && st->heard_dropped == 19000u - 16384u);
    CHECK(lap_audio_read(pcm, 20000) == 16384);
    /* Playing: taken into a 8192-sample queue, then out at the worker. */
    ls_lap_hub_host_t *host = ls_lap_hub_host();
    host->played = 0; host->muted = false; host->live = false;
    CHECK(lap_audio_write(pcm, 10000) == 8192 && lap_audio_write(pcm, 10) == 0);
    pump(1);
    CHECK(host->played == 8192);
    host->muted = true;
    CHECK(lap_audio_write(pcm, 100) == 100);
    pump(1);
    CHECK(host->played == 8192 && lap_audio_status(st) == 0 && st->muted && st->play_dropped == 100);
    host->muted = false;
    leave();
    ls_lap_audio_tap(heard, 100);                                    /* closed: not listening */
    enter(LAP_CAP_RADIO_RX);
    pcm = ls_lap_malloc(200);
    CHECK(lap_audio_read(pcm, 100) == 0);
    leave();
}

static void events(void) {
    enter(LAP_CAP_RADIO_RX);
    lap_event_t *e = ls_lap_malloc(sizeof(*e));
    ls_decoded_t page = {.kind = LS_DECODED_PAGE, .address = 1234567};
    snprintf(page.from, sizeof(page.from), "POCSAG");
    snprintf(page.text, sizeof(page.text), "UNIT 4 RESPOND");
    ls_decoded_publish(&page);                                       /* nobody listening yet */
    CHECK(lap_event_subscribe(LAP_EV_PAGE | LAP_EV_AIS) == 0);
    CHECK(lap_event_next(e) == 0);
    ls_decoded_publish(&page);
    ls_decoded_t aprs = {.kind = LS_DECODED_APRS};
    ls_decoded_publish(&aprs);                                       /* not subscribed */
    ls_decoded_t ais = {.kind = LS_DECODED_AIS, .address = 366000001, .has_pos = true, .lat = 42.5, .lon = -70.9};
    ls_decoded_publish(&ais);
    CHECK(lap_event_next(e) == 1 && e->kind == LAP_EV_PAGE && e->address == 1234567 &&
          !strcmp(e->text, "UNIT 4 RESPOND") && !strcmp(e->from, "POCSAG"));
    CHECK(lap_event_next(e) == 1 && e->kind == LAP_EV_AIS && e->has_pos && e->lat == 42.5);
    CHECK(lap_event_next(e) == 0 && lap_event_dropped() == 0);
    /* Another reader of the same timeline (the radio head) keeps its own place. */
    const int head = ls_decoded_open(LS_DECODED_PAGE);
    CHECK(head >= 0);
    for (int i = 0; i < 300; i++) ls_decoded_publish(&page);
    CHECK(lap_event_next(e) == 1 && lap_event_dropped() == 300 - LS_DECODED_RING);
    int n = 1;
    while (lap_event_next(e) == 1) n++;
    CHECK(n == (int)LS_DECODED_RING && e->seq == 2 + 300);
    ls_decoded_t got;
    uint32_t missed = 0;
    CHECK(ls_decoded_next(head, &got, &missed) && missed == 300 - LS_DECODED_RING && got.kind == LS_DECODED_PAGE);
    ls_decoded_flush(head);
    CHECK(!ls_decoded_next(head, &got, &missed));
    ls_decoded_close(head);
    lap_decoded_t *mine = ls_lap_malloc(sizeof(*mine));
    CHECK(lap_emit(mine) == LAP_RX_EUNSUPPORTED);                    /* reserved for decoder plugins */
    leave();
    enter(LAP_CAP_RADIO_RX);                                         /* a new app starts empty */
    e = ls_lap_malloc(sizeof(*e));
    ls_decoded_publish(&page);
    CHECK(lap_event_next(e) == 0);
    /* Notices: rate limited, and they say what was asked. */
    char *t = ls_lap_malloc(32), *b = ls_lap_malloc(32);
    strcpy(t, "CARRIER"); strcpy(b, "433.92 MHz keyed");
    ls_lap_hub_host_t *host = ls_lap_hub_host();
    const int notices = host->notices, alerts = host->alerts;
    CHECK(lap_notify(t, b) && host->notices == notices + 1 && !strcmp(host->last_body, b));
    CHECK(lap_alert(t, b, LAP_ALERT_ALARM) && host->alerts == alerts + 1 && host->last_level == LAP_ALERT_ALARM);
    CHECK(!lap_alert(t, b, 7) && !lap_notify("x", b));              /* bad level; not app memory */
    leave();
}

static void grants(void) {
    ls_lap_info_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.manifest.id, sizeof(a.manifest.id), "spectre");
    snprintf(a.manifest.version, sizeof(a.manifest.version), "1.0");
    a.size = 4000;
    CHECK(!ls_lap_grant_has(&a, LAP_CAP_RADIO_RX) && ls_lap_grant_has(&a, LAP_CAP_NETWORK) && ls_lap_grant_has(&a, 0));
    ls_lap_grant_set(&a, LAP_CAP_RADIO_RX | LAP_CAP_NETWORK);
    CHECK(ls_lap_grant_has(&a, LAP_CAP_RADIO_RX) && !ls_lap_grant_has(&a, LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX));
    ls_lap_info_t b = a;
    snprintf(b.manifest.version, sizeof(b.manifest.version), "1.1");    /* an update asks again */
    CHECK(!ls_lap_grant_has(&b, LAP_CAP_RADIO_RX));
    b = a; b.size = 4100;                                            /* so does a different file */
    CHECK(!ls_lap_grant_has(&b, LAP_CAP_RADIO_RX));
    const char *path = "./lap-grants-test.txt";   /* replace needs a folder */
    remove(path);
    CHECK(ls_lap_grants_save(path));
    FILE *f = fopen(path, "r");
    char line[96] = {0};
    CHECK(f && fgets(line, sizeof(line), f) && !strcmp(line, "spectre\t1.0\t4000\t2\n"));
    fclose(f);
    remove(path);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_lap_radio <api13 exports> <exports.txt>\n"); return 2; }
    manifests();
    abi(argv[1], argv[2]);
    permissions();
    lifetime();
    overflow();
    spectrum();
    audio();
    events();
    grants();
    printf("test_lap_radio: %u checks passed\n", checks);
    return 0;
}
