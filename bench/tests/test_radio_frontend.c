/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/radio_frontend.c ${FW}/components/lakeshark/radio/radio_endpoint.c */
#include "ls_test.h"
#include "radio_endpoint.h"
#include "radio_frontend.h"

#include <string.h>

/* SDR front ends: the stored forms, the bias-T's rules (never saved on,
   refused while absent, a warning when it goes on, pinned only by choice),
   PPM per dongle, and every control reaching the right driver through the
   registry. Fake drivers stand where the RTL-SDR and HackRF register. */

/* ------------------------------------------------ the settings it keeps -- */
static uint32_t g_rffe;
static uint64_t g_ppm_table;
static int g_hackrf_ppm, g_rffe_writes;
uint32_t settings_get_rffe(void) { return g_rffe; }
bool settings_set_rffe(uint32_t v) { g_rffe = v; g_rffe_writes++; return true; }
uint64_t settings_get_rtl_ppm_table(void) { return g_ppm_table; }
bool settings_set_rtl_ppm_table(uint64_t v) { g_ppm_table = v; return true; }
void settings_hackrf_ppm_set(int ppm) { g_hackrf_ppm = ppm; }

/* ------------------------------------------------------- fake drivers -- */
typedef struct { int value[LS_RADIO_FE_CTLS]; int writes[LS_RADIO_FE_CTLS]; int reads; ls_radio_err_t fail; } fake_t;
static fake_t rtl, hackrf;

static ls_radio_err_t f_configure(void *c, const ls_radio_iq_config_t *r, ls_radio_iq_config_t *a) {(void)c;*a=*r;return LS_RADIO_OK;}
static ls_radio_err_t f_gain(void *c, ls_radio_gain_mode_t m, int g, int *a) {(void)c;(void)m;*a=g;return LS_RADIO_OK;}
static ls_radio_err_t f_start(void *c) {(void)c;return LS_RADIO_OK;}
static ls_radio_err_t f_stop(void *c) {(void)c;return LS_RADIO_OK;}
static void f_cancel(void *c) {(void)c;}
static ls_radio_err_t f_read(void *c, void *d, size_t b, uint32_t t, size_t *got) {(void)c;(void)t;memset(d,0x80,b);*got=b;return LS_RADIO_OK;}
static ls_radio_err_t f_retune(void *c, uint64_t hz, bool fast, uint64_t *a) {(void)c;(void)fast;*a=hz;return LS_RADIO_OK;}
static ls_radio_err_t f_fe(void *ctx, ls_radio_fe_ctl_t ctl, bool write, int *value)
{
    fake_t *f = ctx;
    if (f->fail) return f->fail;
    if (write) { f->value[ctl] = *value; f->writes[ctl]++; }
    else { *value = f->value[ctl]; f->reads++; }
    return LS_RADIO_OK;
}
static int f_gains(void *ctx, int *t, int max)
{
    (void)ctx;
    static const int G[] = {0, 9, 14, 27, 37};
    int n = max < 5 ? max : 5;
    memcpy(t, G, (size_t)n * sizeof(int));
    return 5;
}
static const ls_radio_driver_ops_t OPS = {
    .iq_configure=f_configure,.iq_set_gain=f_gain,.iq_start=f_start,.iq_read=f_read,
    .iq_retune=f_retune,.iq_stop=f_stop,.cancel_read=f_cancel,.fe_ctl=f_fe,.fe_gains=f_gains,
};
static const ls_radio_range_t FREQ[] = {{24000000, 1766000000}};
static const ls_radio_range_t RATE[] = {{225001, 3200000}};

#define RTL_CTLS (LS_RADIO_FE_BIT(LS_RADIO_FE_BIAS_T) | LS_RADIO_FE_BIT(LS_RADIO_FE_PPM) | \
                  LS_RADIO_FE_BIT(LS_RADIO_FE_DIGITAL_AGC))
#define HRF_CTLS (LS_RADIO_FE_BIT(LS_RADIO_FE_BIAS_T) | LS_RADIO_FE_BIT(LS_RADIO_FE_PPM) | \
                  LS_RADIO_FE_BIT(LS_RADIO_FE_AMP) | LS_RADIO_FE_BIT(LS_RADIO_FE_LNA_DB) | \
                  LS_RADIO_FE_BIT(LS_RADIO_FE_VGA_DB))

static void attach(const char *id, fake_t *f, uint32_t ctls)
{
    memset(f, 0, sizeof(*f));
    const ls_radio_endpoint_t e = {
        .endpoint_id=id,.name=id,.capabilities=LS_RADIO_RX_IQ_U8,
        .iq_formats=LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,.frequency_ranges=FREQ,.frequency_range_count=1,
        .sample_rate_ranges=RATE,.sample_rate_range_count=1,.ops=&OPS,.driver_ctx=f,.fe_ctls=ctls,
    };
    LS_EQ_INT(ls_radio_endpoint_register(&e), LS_RADIO_OK);
}
static void detach(const char *id) { LS_EQ_INT(ls_radio_endpoint_unregister(id), LS_RADIO_OK); }
static void drain_notice(void) { char t[20], b[128]; while (ls_rffe_take_notice(t, sizeof t, b, sizeof b)) {} }

/* --------------------------------------------------------------- cases -- */

LS_CASE(stored_word_round_trips_and_defaults)
{
    ls_rffe_cfg_t c, d;
    ls_rffe_cfg_default(&c);
    LS_EQ_INT(c.lna_db, -1);
    LS_EQ_INT(c.vga_db, -1);
    LS_CHECK(!ls_rffe_unpack(0, &d));
    LS_CHECK(!memcmp(&c, &d, sizeof c));
    c.dagc = 2; c.offset = true; c.pin[LS_RFFE_HACKRF] = true; c.amp = 1; c.lna_db = 40; c.vga_db = 62;
    LS_CHECK(ls_rffe_unpack(ls_rffe_pack(&c), &d));
    LS_CHECK(!memcmp(&c, &d, sizeof c));
    c.lna_db = 0; c.vga_db = 0; c.pin[LS_RFFE_RTL] = true;
    LS_CHECK(ls_rffe_unpack(ls_rffe_pack(&c), &d));
    LS_CHECK(!memcmp(&c, &d, sizeof c));
    /* Another version is all defaults; a bad field falls back alone. */
    LS_CHECK(!ls_rffe_unpack(ls_rffe_pack(&c) ^ (3u << 28), &d));
    LS_EQ_INT(d.amp, 0);
    LS_CHECK(!ls_rffe_unpack((1u << 28) | 3u | (1u << 2), &d));
    LS_EQ_INT(d.dagc, 0);
    LS_CHECK(d.offset);
}

LS_CASE(ppm_table_keeps_four_dongles_newest_first)
{
    uint64_t t = 0;
    int ppm = 99;
    LS_CHECK(!ls_rffe_ppm_get(t, 5, &ppm));
    t = ls_rffe_ppm_put(t, 5, -3);
    LS_CHECK(ls_rffe_ppm_get(t, 5, &ppm)); LS_EQ_INT(ppm, -3);
    for (int k = 6; k <= 8; ++k) t = ls_rffe_ppm_put(t, (uint32_t)k, k);
    LS_CHECK(ls_rffe_ppm_get(t, 5, &ppm)); LS_EQ_INT(ppm, -3);
    t = ls_rffe_ppm_put(t, 9, 250);       /* fifth: the oldest (5) goes; clamped */
    LS_CHECK(!ls_rffe_ppm_get(t, 5, NULL));
    LS_CHECK(ls_rffe_ppm_get(t, 9, &ppm)); LS_EQ_INT(ppm, LS_RFFE_RTL_PPM_MAX);
    t = ls_rffe_ppm_put(t, 6, -100);      /* an update moves it to the front */
    LS_EQ_UINT(t & 0xffff, (6u << 8) | (uint8_t)-100);
    LS_CHECK(ls_rffe_ppm_get(t, 7, &ppm)); LS_EQ_INT(ppm, 7);
    LS_EQ_UINT(ls_rffe_ppm_put(t, 0, 1), t);
    const uint32_t a = ls_rffe_dongle_key("RTLSDRBlog", "Blog V4", "00000001");
    LS_CHECK(a >= 1 && a <= 255);
    LS_EQ_UINT(a, ls_rffe_dongle_key("RTLSDRBlog", "Blog V4", "00000001"));
    LS_CHECK(a != ls_rffe_dongle_key("RTLSDRBlog", "Blog V4", "00000002"));
    LS_EQ_UINT(ls_rffe_dongle_key(NULL, NULL, NULL), ls_rffe_dongle_key("", "", ""));
}

LS_CASE(values_are_checked)
{
    LS_CHECK(!ls_rffe_check(LS_RADIO_FE_BIAS_T, 1));
    LS_CHECK(ls_rffe_check(LS_RADIO_FE_BIAS_T, 2));
    LS_CHECK(ls_rffe_check(LS_RADIO_FE_PPM, 101));
    LS_CHECK(!ls_rffe_check(LS_RADIO_FE_PPM, -100));
    LS_CHECK(ls_rffe_check(LS_RADIO_FE_LNA_DB, 12));
    LS_CHECK(!ls_rffe_check(LS_RADIO_FE_LNA_DB, 32));
    LS_CHECK(!ls_rffe_check(LS_RADIO_FE_LNA_DB, -1));
    LS_CHECK(ls_rffe_check(LS_RADIO_FE_VGA_DB, 63));
    LS_CHECK(!ls_rffe_check(LS_RADIO_FE_VGA_DB, 62));
    LS_CHECK(ls_rffe_check(LS_RADIO_FE_AMP, 3));
    LS_CHECK(ls_rffe_has(LS_RFFE_RTL, LS_RADIO_FE_DIGITAL_AGC));
    LS_CHECK(!ls_rffe_has(LS_RFFE_RTL, LS_RADIO_FE_AMP));
    LS_CHECK(!ls_rffe_has(LS_RFFE_HACKRF, LS_RADIO_FE_OFFSET_TUNING));
    ls_rffe_dev_t d;
    LS_CHECK(ls_rffe_from_word("hackrf", &d)); LS_EQ_INT(d, LS_RFFE_HACKRF);
    LS_CHECK(ls_rffe_from_endpoint(LS_RADIO_ENDPOINT_RTL_USB, &d)); LS_EQ_INT(d, LS_RFFE_RTL);
    LS_CHECK(!ls_rffe_from_word("lr", &d));
}

LS_CASE(bias_t_is_refused_while_absent_and_warns_when_switched_on)
{
    drain_notice();
    char t[20], b[128];
    LS_CHECK(!ls_radio_bias_t_supported(LS_RADIO_ENDPOINT_RTL_USB));
    LS_EQ_INT(ls_radio_bias_t_set(LS_RADIO_ENDPOINT_RTL_USB, true), LS_RADIO_ERR_UNAVAILABLE);
    LS_CHECK(!ls_rffe_request(LS_RFFE_RTL, LS_RADIO_FE_BIAS_T, 1));
    LS_CHECK(!ls_rffe_take_notice(t, sizeof t, b, sizeof b));

    attach(LS_RADIO_ENDPOINT_RTL_USB, &rtl, RTL_CTLS);
    LS_CHECK(ls_radio_bias_t_supported(LS_RADIO_ENDPOINT_RTL_USB));
    const uint32_t saved_before = g_rffe;
    LS_EQ_INT(ls_radio_bias_t_set(LS_RADIO_ENDPOINT_RTL_USB, true), LS_RADIO_OK);
    LS_EQ_INT(rtl.value[LS_RADIO_FE_BIAS_T], 1);
    LS_EQ_INT(rtl.writes[LS_RADIO_FE_BIAS_T], 1);
    LS_CHECK(ls_radio_bias_t_state(LS_RADIO_ENDPOINT_RTL_USB));
    LS_EQ_UINT(g_rffe, saved_before);     /* never saved on */
    LS_CHECK(ls_rffe_take_notice(t, sizeof t, b, sizeof b));
    LS_EQ_STR(t, "BIAS-T ON");
    LS_CHECK(strstr(b, LS_RFFE_BIAS_WARNING) != NULL);
    LS_CHECK(strstr(b, "RTL-SDR") != NULL);
    LS_CHECK(!ls_rffe_take_notice(t, sizeof t, b, sizeof b));

    /* Read back from the device, not the cache. */
    rtl.value[LS_RADIO_FE_BIAS_T] = 0;
    bool on = true;
    LS_EQ_INT(ls_radio_bias_t_get(LS_RADIO_ENDPOINT_RTL_USB, &on), LS_RADIO_OK);
    LS_CHECK(!on);
    LS_CHECK(!ls_radio_bias_t_state(LS_RADIO_ENDPOINT_RTL_USB));

    /* Switching off leaves no warning. */
    LS_EQ_INT(ls_radio_bias_t_set(LS_RADIO_ENDPOINT_RTL_USB, false), LS_RADIO_OK);
    LS_CHECK(!ls_rffe_take_notice(t, sizeof t, b, sizeof b));

    /* A device that fails the write is left recorded off. */
    rtl.fail = LS_RADIO_ERR_IO;
    LS_EQ_INT(ls_radio_bias_t_set(LS_RADIO_ENDPOINT_RTL_USB, true), LS_RADIO_ERR_IO);
    LS_CHECK(!ls_radio_bias_t_state(LS_RADIO_ENDPOINT_RTL_USB));
    LS_CHECK(!ls_rffe_take_notice(t, sizeof t, b, sizeof b));
    rtl.fail = LS_RADIO_OK;

    /* The UI's way: saved nowhere, applied, warned. */
    LS_CHECK(ls_rffe_request(LS_RFFE_RTL, LS_RADIO_FE_BIAS_T, 1));
    LS_EQ_INT(rtl.value[LS_RADIO_FE_BIAS_T], 1);
    LS_CHECK(ls_rffe_take_notice(t, sizeof t, b, sizeof b));

    /* The driver drops it with the device. */
    ls_rffe_note(LS_RFFE_RTL, LS_RADIO_FE_BIAS_T, 0, false);
    detach(LS_RADIO_ENDPOINT_RTL_USB);
    LS_CHECK(!ls_radio_bias_t_state(LS_RADIO_ENDPOINT_RTL_USB));
    LS_CHECK(!ls_radio_bias_t_supported(LS_RADIO_ENDPOINT_RTL_USB));
}

LS_CASE(pinned_bias_is_the_only_saved_on_and_announces_itself)
{
    drain_notice();
    char t[20], b[128];
    LS_CHECK(!ls_radio_bias_t_pinned(LS_RADIO_ENDPOINT_HACKRF_USB));
    LS_EQ_INT(ls_rffe_saved(LS_RFFE_HACKRF, LS_RADIO_FE_BIAS_T), 0);
    ls_radio_bias_t_pin(LS_RADIO_ENDPOINT_HACKRF_USB, true);
    LS_CHECK(ls_radio_bias_t_pinned(LS_RADIO_ENDPOINT_HACKRF_USB));
    LS_EQ_INT(ls_rffe_saved(LS_RFFE_HACKRF, LS_RADIO_FE_BIAS_T), 1);
    ls_rffe_cfg_t c;
    LS_CHECK(ls_rffe_unpack(g_rffe, &c));
    LS_CHECK(c.pin[LS_RFFE_HACKRF] && !c.pin[LS_RFFE_RTL]);
    /* What the driver does at attach when it finds the pin. */
    ls_rffe_note(LS_RFFE_HACKRF, LS_RADIO_FE_BIAS_T, 1, true);
    LS_CHECK(ls_rffe_take_notice(t, sizeof t, b, sizeof b));
    LS_CHECK(strstr(b, "pinned") != NULL && strstr(b, "HackRF") != NULL);
    ls_rffe_note(LS_RFFE_HACKRF, LS_RADIO_FE_BIAS_T, 0, false);
    ls_radio_bias_t_pin(LS_RADIO_ENDPOINT_HACKRF_USB, false);
    LS_EQ_INT(ls_rffe_saved(LS_RFFE_HACKRF, LS_RADIO_FE_BIAS_T), 0);
}

LS_CASE(rtl_ppm_is_kept_per_dongle)
{
    LS_EQ_INT(ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_PPM, 7), LS_RADIO_ERR_UNAVAILABLE);
    LS_EQ_UINT(g_ppm_table, 0);           /* no dongle seen: nothing to key it by */
    const uint32_t k1 = ls_rffe_dongle_key("A", "B", "1"), k2 = ls_rffe_dongle_key("A", "B", "2");
    ls_rffe_rtl_attach(k1);
    attach(LS_RADIO_ENDPOINT_RTL_USB, &rtl, RTL_CTLS);
    LS_EQ_INT(ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_PPM, 7), LS_RADIO_OK);
    LS_EQ_INT(rtl.value[LS_RADIO_FE_PPM], 7);
    LS_EQ_INT(ls_rffe_value(LS_RFFE_RTL, LS_RADIO_FE_PPM), 7);
    detach(LS_RADIO_ENDPOINT_RTL_USB);
    ls_rffe_rtl_attach(k2);               /* another dongle starts at 0 */
    LS_EQ_INT(ls_rffe_saved(LS_RFFE_RTL, LS_RADIO_FE_PPM), 0);
    LS_EQ_INT(ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_PPM, -4), LS_RADIO_ERR_UNAVAILABLE);  /* kept for it */
    LS_EQ_INT(ls_rffe_saved(LS_RFFE_RTL, LS_RADIO_FE_PPM), -4);
    ls_rffe_rtl_attach(k1);
    LS_EQ_INT(ls_rffe_saved(LS_RFFE_RTL, LS_RADIO_FE_PPM), 7);
    LS_EQ_INT(ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_PPM, 101), LS_RADIO_ERR_INVALID);
}

LS_CASE(rtl_agc_saved_and_applied_unsupported_ctl_refused)
{
    attach(LS_RADIO_ENDPOINT_RTL_USB, &rtl, RTL_CTLS);
    LS_EQ_INT(ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_DIGITAL_AGC, 2), LS_RADIO_OK);
    LS_EQ_INT(rtl.value[LS_RADIO_FE_DIGITAL_AGC], 2);
    LS_EQ_INT(ls_rffe_saved(LS_RFFE_RTL, LS_RADIO_FE_DIGITAL_AGC), 2);
    ls_rffe_cfg_t c;
    LS_CHECK(ls_rffe_unpack(g_rffe, &c));
    LS_EQ_INT(c.dagc, 2);
    /* This tuner has no offset tuning: the registry says so, nothing sent. */
    LS_CHECK(!ls_rffe_supported(LS_RFFE_RTL, LS_RADIO_FE_OFFSET_TUNING));
    LS_EQ_INT(ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_OFFSET_TUNING, 1), LS_RADIO_ERR_UNSUPPORTED);
    LS_EQ_INT(rtl.writes[LS_RADIO_FE_OFFSET_TUNING], 0);
    /* Direct sampling is the HF work's; not in this driver, never saved. */
    LS_EQ_INT(ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_DIRECT_SAMPLING, 1), LS_RADIO_ERR_UNSUPPORTED);
    LS_CHECK(!ls_rffe_request(LS_RFFE_RTL, LS_RADIO_FE_DIRECT_SAMPLING, 1));
    /* No amp on an RTL-SDR at all. */
    LS_EQ_INT(ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_AMP, 2), LS_RADIO_ERR_INVALID);
    int g[8];
    LS_EQ_INT(ls_rffe_gains(LS_RFFE_RTL, g, 8), 5);
    LS_EQ_INT(g[4], 37);
    LS_EQ_INT(ls_rffe_gains(LS_RFFE_RTL, g, 3), 3);
    ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_DIGITAL_AGC, 0);
    detach(LS_RADIO_ENDPOINT_RTL_USB);
    LS_EQ_INT(ls_rffe_gains(LS_RFFE_RTL, g, 8), 0);
}

LS_CASE(hackrf_amp_warns_and_stages_are_saved)
{
    drain_notice();
    char t[20], b[128];
    attach(LS_RADIO_ENDPOINT_HACKRF_USB, &hackrf, HRF_CTLS);
    LS_EQ_INT(ls_rffe_set(LS_RFFE_HACKRF, LS_RADIO_FE_AMP, 2), LS_RADIO_OK);
    LS_EQ_INT(hackrf.value[LS_RADIO_FE_AMP], 2);
    LS_CHECK(ls_rffe_take_notice(t, sizeof t, b, sizeof b));
    LS_EQ_STR(t, "RF AMP ON");
    LS_EQ_INT(ls_rffe_set(LS_RFFE_HACKRF, LS_RADIO_FE_AMP, 1), LS_RADIO_OK);
    LS_CHECK(!ls_rffe_take_notice(t, sizeof t, b, sizeof b));
    LS_EQ_INT(ls_rffe_set(LS_RFFE_HACKRF, LS_RADIO_FE_LNA_DB, 12), LS_RADIO_ERR_INVALID);
    LS_EQ_INT(ls_rffe_set(LS_RFFE_HACKRF, LS_RADIO_FE_LNA_DB, 16), LS_RADIO_OK);
    LS_EQ_INT(ls_rffe_set(LS_RFFE_HACKRF, LS_RADIO_FE_VGA_DB, 30), LS_RADIO_OK);
    LS_EQ_INT(hackrf.value[LS_RADIO_FE_LNA_DB], 16);
    LS_EQ_INT(hackrf.value[LS_RADIO_FE_VGA_DB], 30);
    LS_EQ_INT(ls_rffe_set(LS_RFFE_HACKRF, LS_RADIO_FE_PPM, -12), LS_RADIO_OK);
    LS_EQ_INT(g_hackrf_ppm, -12);
    LS_EQ_INT(ls_rffe_value(LS_RFFE_HACKRF, LS_RADIO_FE_PPM), -12);
    ls_rffe_cfg_t c;
    LS_CHECK(ls_rffe_unpack(g_rffe, &c));
    LS_EQ_INT(c.amp, 1); LS_EQ_INT(c.lna_db, 16); LS_EQ_INT(c.vga_db, 30);
    detach(LS_RADIO_ENDPOINT_HACKRF_USB);
    /* Away: still saved, for the next attach, and answered as such. */
    LS_EQ_INT(ls_rffe_set(LS_RFFE_HACKRF, LS_RADIO_FE_LNA_DB, -1), LS_RADIO_ERR_UNAVAILABLE);
    LS_EQ_INT(ls_rffe_saved(LS_RFFE_HACKRF, LS_RADIO_FE_LNA_DB), -1);
    LS_CHECK(ls_rffe_request(LS_RFFE_HACKRF, LS_RADIO_FE_VGA_DB, -1));
    LS_EQ_INT(ls_rffe_saved(LS_RFFE_HACKRF, LS_RADIO_FE_VGA_DB), -1);
    LS_EQ_INT(hackrf.writes[LS_RADIO_FE_VGA_DB], 1);
    ls_rffe_set(LS_RFFE_HACKRF, LS_RADIO_FE_AMP, 0);
}

LS_CASE(console_switches_and_reports)
{
    drain_notice();
    attach(LS_RADIO_ENDPOINT_RTL_USB, &rtl, RTL_CTLS);
    char *on[] = {"bias", "rtl", "on"}, *off[] = {"bias", "off"}, *st[] = {"bias"};
    char *bad[] = {"bias", "rtl", "sideways"};
    LS_EQ_INT(ls_bias_command(3, on), 0);
    LS_EQ_INT(rtl.value[LS_RADIO_FE_BIAS_T], 1);
    LS_EQ_INT(ls_bias_command(2, off), 0);      /* the attached one */
    LS_EQ_INT(rtl.value[LS_RADIO_FE_BIAS_T], 0);
    LS_EQ_INT(ls_bias_command(1, st), 0);
    LS_EQ_INT(ls_bias_command(3, bad), 1);
    char *hon[] = {"bias", "hackrf", "on"};
    LS_EQ_INT(ls_bias_command(3, hon), 1);     /* not attached */
    char *dagc[] = {"rffe", "rtl", "dagc", "on"}, *lna[] = {"rffe", "rtl", "lna", "8"};
    LS_EQ_INT(ls_rffe_command(4, dagc), 0);
    LS_EQ_INT(rtl.value[LS_RADIO_FE_DIGITAL_AGC], 1);
    LS_EQ_INT(ls_rffe_command(4, lna), 1);      /* an RTL-SDR has no LNA control */
    char *show[] = {"rffe"};
    LS_EQ_INT(ls_rffe_command(1, show), 0);
    ls_rffe_set(LS_RFFE_RTL, LS_RADIO_FE_DIGITAL_AGC, 0);
    detach(LS_RADIO_ENDPOINT_RTL_USB);
    drain_notice();
}
