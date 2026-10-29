/* WFM stereo + RDS glue for the FM receive task. See fm_wfm.h. */
#include "fm_wfm.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fm_bcast.h"
#include "ls_time.h"
#include "wfm_mpx.h"

static const char *TAG = "fm_wfm";

static wfm_mpx_t *s_mpx;          /* PSRAM, made on first use              */
static EXT_RAM_BSS_ATTR int16_t s_disc[FM_DSP_CHUNK / 2];
static EXT_RAM_BSS_ATTR fm_wfm_status_t s_pub;
static fm_ct_trust_t s_ct;
static uint32_t s_ct_seen;
static int s_publish_tick;

static bool ensure(void)
{
    if (s_mpx) return true;
    s_mpx = heap_caps_calloc(1, sizeof(*s_mpx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mpx) { ESP_LOGE(TAG, "no PSRAM for the multiplex decoder"); return false; }
    wfm_mpx_init(s_mpx);
    return true;
}

void fm_wfm_reset(void)
{
    if (!ensure()) return;
    wfm_mpx_reset(s_mpx);
    memset(&s_ct, 0, sizeof(s_ct));
    s_ct_seen = 0;
    s_publish_tick = 0;
}

void fm_wfm_idle(void)
{
    memset(&s_pub, 0, sizeof(s_pub));
    fm_wfm_publish(&s_pub);
}

/* RDS clock time sets the wall clock only when nothing else has (no SNTP,
   no RTC, no GPS) and two CT groups in a row agree. CT is sent on the
   minute, so the time it carries is the time it arrives. */
static void offer_clock(const rds_info_t *rds)
{
    if (!rds->ct_valid || rds->ct_seq == s_ct_seen) return;
    s_ct_seen = rds->ct_seq;
    const bool trusted = fm_ct_trust(&s_ct, rds->ct_unix, esp_timer_get_time());
    if (!trusted || ls_time_is_synced() || fm_bcast_opt(FM_BC_CLOCK_OFF)) return;
    struct timeval tv = { .tv_sec = (time_t)rds->ct_unix, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ls_time_note_set();
    ESP_LOGI(TAG, "wall clock set from RDS CT (PI %04X)", rds->pi);
}

int fm_wfm_block(fm_dsp_t *dsp, const uint8_t *iq, int iq_len,
                 int16_t *pcm, int max)
{
    if (!ensure()) return fm_demod_wide(dsp, iq, iq_len, pcm, max);
    wfm_mpx_set_rds(s_mpx, !fm_bcast_opt(FM_BC_RDS_OFF));
    wfm_mpx_set_mono(s_mpx, fm_bcast_opt(FM_BC_MONO));
    int out = 0;
    for (int off = 0; off < iq_len; off += FM_DSP_CHUNK) {
        const int n = iq_len - off < FM_DSP_CHUNK ? iq_len - off : FM_DSP_CHUNK;
        const int nd = fm_demod_disc(dsp, iq + off, n, s_disc, FM_DSP_CHUNK / 2);
        out += wfm_mpx_process(s_mpx, s_disc, nd, pcm + out, NULL, max - out);
    }
    offer_clock(&s_mpx->rds.info);
    /* The card when it changes; the meters about eight times a second. */
    if (s_mpx->rds.info.seq != s_pub.rds.seq || s_mpx->stereo != s_pub.stereo ||
        !s_pub.running || ++s_publish_tick >= 4) {
        s_publish_tick = 0;
        s_pub.running = true;
        s_pub.pilot = s_mpx->pilot;
        s_pub.stereo = s_mpx->stereo;
        s_pub.pilot_level = s_mpx->pilot_level;
        s_pub.lvl_l = s_mpx->lvl_l;
        s_pub.lvl_r = s_mpx->lvl_r;
        s_pub.rds_level = s_mpx->rds_level;
        s_pub.rds = s_mpx->rds.info;
        fm_wfm_publish(&s_pub);
    }
    return out;
}

const char *fm_wfm_summary(char *out, int n)
{
    if (!s_mpx) { snprintf(out, (size_t)n, "-"); return out; }
    const rds_info_t *r = &s_mpx->rds.info;
    snprintf(out, (size_t)n, "%s pilot=%d%% rds=%s pi=%04X ps=[%s] grp=%lu ok=%lu fix=%lu bad=%lu",
             s_mpx->stereo ? "STEREO" : "mono", (int)(s_mpx->pilot_level * 100.0f + 0.5f),
             r->synced ? "sync" : "hunt", r->pi, r->ps, (unsigned long)r->groups,
             (unsigned long)r->blocks_ok, (unsigned long)r->blocks_fixed,
             (unsigned long)r->blocks_bad);
    return out;
}
