/* TIME MACHINE replay: a clip played back through the same pager chains the
   receivers run live, so a page missed on the air is decoded afterwards.

     RTL-SDR / HackRF clips (CU8 at FM_RTL_RATE): fm_dsp's NFM discriminator
       and pocsag, as FM's POCSAG mode runs them (and bench fm_iq_replay).
     LR2021 clips (CS16 at 125k): pager_iq's discriminator and pocsag, as the
       LR pager path runs them.

   Each runs 512, 1200 and 2400 baud in both polarities. Pages go onto the
   decoded bus stamped with the moment they were on the air, so the timeline
   shows them where they happened, linked to the clip. Runs on the TIME
   MACHINE worker; nothing here touches the screen. */
#include "ls_tmach_decode.h"
#include "ls_tmach.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/fm/fm_dsp.h"
#include "apps/fm/fm_mode_label.h"
#include "apps/fm/fm_state.h"
#include "apps/fm/pocsag.h"
#include "experiments/pager_iq.h"
#include "ls_decoded.h"
#include "ls_radio_select.h"
#include "p25_state.h"
#include "ls_textpack.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BIG_FREE(p)  heap_caps_free(p)
#else
#define BIG_ALLOC(n) calloc(1, (n))
#define BIG_FREE(p)  free(p)
#endif

#define DECODERS 6
#define DEMOD_MAX 8192

typedef struct {
    fm_dsp_t dsp;
    pgr_iq_t pgr;
    fm_state_t out[DECODERS];
    pocsag_ctx_t *p[DECODERS];
    uint32_t seen[DECODERS];
    float demod[DEMOD_MAX], neg[DEMOD_MAX];
    bool lr;
    uint32_t rate, found;
    uint8_t radio;
    uint32_t hz;
    char first[48];
} replay_t;

static replay_t *s_r;

static void release(void)
{
    if (!s_r) return;
    for (int i = 0; i < DECODERS; i++) if (s_r->p[i]) pocsag_destroy(s_r->p[i]);
    BIG_FREE(s_r);
    s_r = NULL;
}

static bool open_clip(const ls_tm_clip_meta_t *m, char *why, size_t n)
{
    release();
    const bool cu8 = m->src_fmt == LS_TM_FMT_CU8 && m->src_rate == FM_RTL_RATE;
    const bool lr = m->src_fmt == LS_TM_FMT_CS16 && m->src_rate == PGR_IQ_INPUT_RATE;
    if (!cu8 && !lr) {
        LS_SNPRINTF(t_ls_tmach_decode_no_pager_chain_at, why, n, "No pager chain at %lu S/s %s", (unsigned long)m->src_rate, ls_tm_fmt_name(m->src_fmt));
        return false;
    }
    s_r = BIG_ALLOC(sizeof(*s_r));
    if (!s_r) { LS_SNPUTS(t_ls_tmach_decode_no_memory_for_the, why, n, "No memory for the decoders"); return false; }
    static const int BAUD[3] = { 512, 1200, 2400 };
    s_r->lr = lr;
    s_r->radio = (uint8_t)ls_tm_radio_parse(m->rec.radio);
    s_r->hz = (uint32_t)m->center_hz;
    fm_dsp_init(&s_r->dsp);
    pgr_iq_init(&s_r->pgr);
    for (int i = 0; i < DECODERS; i++) {
        s_r->p[i] = pocsag_create(&s_r->out[i], BAUD[i % 3]);
        if (!s_r->p[i]) { release(); LS_SNPUTS(t_ls_tmach_decode_no_memory_for_the, why, n, "No memory for the decoders"); return false; }
        if (lr) pocsag_sample_rate(s_r->p[i], PGR_IQ_OUTPUT_RATE);
        pocsag_quiet(s_r->p[i], true);   /* publish() stamps them with the air time */
    }
    return true;
}

/* New pages since the last look, onto the bus at the time they were sent. */
static void publish(int i, int64_t at_us)
{
    const uint32_t pages = pocsag_n_pages(s_r->p[i]);
    while (s_r->seen[i] < pages) {
        const fm_state_t *o = &s_r->out[i];
        const uint32_t back = pages - 1 - s_r->seen[i];
        s_r->seen[i]++;
        if ((int)back >= o->page_count) continue;
        const int at = ((o->page_head - 1 - (int)back) % FM_PAGE_LOG_MAX + FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
        const fm_page_t *p = &o->pages[at];
        ls_decoded_t d = { .kind = LS_DECODED_PAGE, .address = p->address, .at_us = at_us,
                           .freq_hz = s_r->hz, .radio = s_r->radio };
        snprintf(d.from, sizeof(d.from), "POCSAG %u replay", (unsigned)p->baud);
        snprintf(d.text, sizeof(d.text), "%s", p->text);
        ls_decoded_publish(&d);
        if (!s_r->found++) snprintf(s_r->first, sizeof(s_r->first), "%lu: %.30s", (unsigned long)p->address, p->text);
    }
}

static void feed(const uint8_t *iq, size_t bytes, int64_t at_us)
{
    if (!s_r) return;
    for (size_t off = 0; off < bytes;) {
        size_t n = bytes - off;
        int nd = 0;
        if (s_r->lr) {
            if (n > (DEMOD_MAX - 1) * 20) n = (DEMOD_MAX - 1) * 20;
            size_t got = 0;
            if (!pgr_iq_process(&s_r->pgr, iq + off, n, s_r->demod, DEMOD_MAX, &got)) return;
            nd = (int)got;
        } else {
            if (n > 16384) n = 16384;
            nd = fm_demod_iq(&s_r->dsp, iq + off, (int)n, s_r->demod, DEMOD_MAX);
        }
        for (int k = 0; k < nd; k++) s_r->neg[k] = -s_r->demod[k];
        for (int i = 0; i < DECODERS; i++) {
            pocsag_process(s_r->p[i], i < 3 ? s_r->demod : s_r->neg, nd);
            publish(i, at_us);
        }
        off += n;
    }
}

static uint32_t close_clip(char *line, size_t n)
{
    if (!s_r) { snprintf(line, n, "Replay stopped"); return 0; }
    const uint32_t found = s_r->found;
    if (found == 1) snprintf(line, n, "Replay: 1 page, %s", s_r->first);
    else if (found) LS_SNPRINTF(t_ls_tmach_decode_replay_lu_pages_first, line, n, "Replay: %lu pages, first %s", (unsigned long)found, s_r->first);
    else LS_SNPUTS(t_ls_tmach_decode_replay_no_pages_in, line, n, "Replay: no pages in this clip");
    release();
    return found;
}

static const ls_tm_decoder_t DECODER = { .open = open_clip, .feed = feed, .close = close_clip };

/* What a receiver is decoding, named the way its own screen names it, for a
   clip's sidecar. Reads state the receivers publish; never waits. */
static const char *decoder_name(unsigned radio)
{
    if (radio == LS_TM_RADIO_LR2021) {
        if (FM.lr_iq_active) return fm_mode_label(FM.mode);
        if (P25.lr_iq_active) return "P25";
        return "";
    }
    const ls_rsel_radio_t r = radio == LS_TM_RADIO_HACKRF ? LS_RSEL_SDR_HACKRF : LS_RSEL_SDR_RTL;
    if (ls_rsel_sdr_held_by("p25") == r) return "P25";
    if (ls_rsel_sdr_held_by("fm") == r) return fm_mode_label(FM.mode);
    if (ls_rsel_sdr_held_by("adsb") == r) return "ADS-B";
    if (ls_rsel_sdr_held_by("rec") == r) return "REC";
    return "";
}

void ls_tmach_decode_register(void)
{
    ls_tm_set_decoder(&DECODER);
    ls_tm_set_decoder_name(decoder_name);
}
