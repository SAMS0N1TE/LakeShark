/* `bcast`: the FM broadcast extras from the console, for the bench.

     bcast                 what WFM, the channel scanner and SSTV are doing
     bcast air 25          FM to AM, airband scan on 25 kHz (833, cb, cbusb,
                           cblsb; off stops)
     bcast skip|lock       move on / lock this channel out and move on
     bcast hold|free       stay on this channel / let the scan go on
     bcast clear           forget the lockouts
     bcast wx              FM to SAME on the strongest NOAA weather channel
     bcast sstv on|off     the SSTV decoder on LISTEN/AM audio
     bcast rds on|off      the RDS demodulator in WFM
     bcast mono on|off     force mono in WFM */
#include <stdio.h>
#include <string.h>

#include "fm_bcast.h"
#include "fm_state.h"
#include "lakeshark_backend.h"
#include "scan_engine.h"
#include "ls_textpack.h"

static const struct { const char *word; int preset; } AIR_WORDS[] = {
    { "off", AIR_PRESET_OFF }, { "25", AIR_PRESET_AIR25 }, { "833", AIR_PRESET_AIR833 },
    { "cb", AIR_PRESET_CB_AM }, { "cbusb", AIR_PRESET_CB_USB }, { "cblsb", AIR_PRESET_CB_LSB },
};

static void show(void)
{
    static fm_wfm_status_t w;
    static fm_air_status_t a;
    fm_wfm_status(&w);
    fm_air_status(&a);
    LS_PRINTF(t_fm_bcast_cmd_fm_mode_d_freq, "FM mode=%d freq=%lu\n", (int)FM.mode, (unsigned long)FM.freq_hz);
    if (w.running)
        LS_PRINTF(t_fm_bcast_cmd_wfm_s_pilot_d, "WFM %s pilot=%d%% L=%.2f R=%.2f RDS %s PI=%04X PS=[%s] PTY=%u RT=[%s] CT=%s groups=%lu ok=%lu fixed=%lu bad=%lu\n",
               w.stereo ? "STEREO" : "mono", (int)(w.pilot_level * 100.0f + 0.5f), w.lvl_l, w.lvl_r,
               w.rds.synced ? "sync" : "hunt", w.rds.pi, w.rds.ps, w.rds.pty, w.rds.rt,
               w.rds.ct_valid ? "yes" : "no", (unsigned long)w.rds.groups,
               (unsigned long)w.rds.blocks_ok, (unsigned long)w.rds.blocks_fixed,
               (unsigned long)w.rds.blocks_bad);
    else printf("WFM idle\n");
    if (a.preset) {
        const air_preset_t *p = air_preset(a.preset);
        LS_PRINTF(t_fm_bcast_cmd_scan_s_phase_d, "SCAN %s phase=%d lo=%lu listen=%lu carrier=%d held=%d sweeps=%lu (%lu ms) win=%d/%d guard=%s hits=%d locked=%d images=%lu\n",
               p ? p->name : "?", a.phase, (unsigned long)a.lo_hz, (unsigned long)a.listen_hz,
               a.carrier, a.held, (unsigned long)a.sweeps, (unsigned long)a.sweep_ms, a.win + 1,
               a.nwin, a.guard_on ? "on" : "off", a.nhits, a.lockouts, (unsigned long)a.images);
        for (int i = 0; i < a.nhits; ++i)
            LS_PRINTF(t_fm_bcast_cmd_hit_4f_x_u, "  hit %.4f x%u talk %lus peak %ddB%s\n", a.hits[i].hz / 1e6, a.hits[i].count,
                   (unsigned long)(a.hits[i].talk_ms / 1000), a.hits[i].peak_db,
                   a.hits[i].guard ? " GUARD" : "");
    } else printf("SCAN off\n");
    static fm_wx_status_t x;
    fm_wx_status(&x);
    LS_PRINTF(t_fm_bcast_cmd_wx_channel_d_speaker, "WX channel=%d speaker=%s alert=%d finding=%d levels=%u,%u,%u,%u,%u,%u,%u\n",
           x.channel >= 0 ? x.channel + 1 : 0,
           fm_bcast_wx_audio() == 1 ? "on" : fm_bcast_wx_audio() == 2 ? "alerts" : "off",
           x.alert_open, x.finding, x.level[0], x.level[1], x.level[2], x.level[3],
           x.level[4], x.level[5], x.level[6]);
    static fm_sstv_status_t t;
    fm_sstv_status(&t);
    LS_PRINTF(t_fm_bcast_cmd_sstv_s_s_rows, "SSTV %s %s rows=%d/%d slant=%+.0fppm syncs=%d/%d pictures=%lu saved=%s\n",
           t.on ? "on" : "off", t.receiving ? t.mode : "idle", t.rows, t.h,
           (double)t.slant_ppm, t.syncs_found, t.syncs_found + t.syncs_missed,
           (unsigned long)t.images, t.saved[0] ? t.saved : "-");
}

static bool on_word(const char *w) { return w && (!strcmp(w, "on") || !strcmp(w, "1")); }

int fm_bcast_command(int argc, char **argv)
{
    if (argc < 2) { show(); return 0; }
    const char *v = argc > 2 ? argv[2] : NULL;
    if (!strcmp(argv[1], "air")) {
        for (unsigned i = 0; v && i < sizeof(AIR_WORDS) / sizeof(AIR_WORDS[0]); ++i) {
            if (strcmp(v, AIR_WORDS[i].word)) continue;
            if (AIR_WORDS[i].preset) scan_engine_stop();
            if (AIR_WORDS[i].preset && FM.mode != FM_MODE_AM) lakeshark_fm_set_mode(FM_MODE_AM);
            fm_air_request(FM_AIR_REQ_PRESET, AIR_WORDS[i].preset);
            LS_PRINTF(t_fm_bcast_cmd_bcast_air_s_requested, "bcast: air %s requested\n", v);
            return 0;
        }
        printf("bcast air off|25|833|cb|cbusb|cblsb\n");
        return 1;
    }
    if (!strcmp(argv[1], "wx")) {
        if (v && (!strcmp(v, "on") || !strcmp(v, "off") || !strcmp(v, "alerts"))) {
            fm_bcast_set_wx_audio(!strcmp(v, "on") ? 1 : !strcmp(v, "alerts") ? 2 : 0);
            LS_PRINTF(t_fm_bcast_cmd_bcast_weather_speaker_s, "bcast: weather speaker %s\n", v);
            return 0;
        }
        if (FM.mode != FM_MODE_SAME) lakeshark_fm_set_mode(FM_MODE_SAME);
        fm_air_request(FM_AIR_REQ_WX_FIND, 0);
        printf("bcast: looking for the strongest weather channel\n");
        return 0;
    }
    if (!strcmp(argv[1], "sstv")) { fm_bcast_set_opt(FM_BC_SSTV, on_word(v)); return 0; }
    if (!strcmp(argv[1], "skip")) { fm_air_request(FM_AIR_REQ_SKIP, 0); return 0; }
    if (!strcmp(argv[1], "lock")) { fm_air_request(FM_AIR_REQ_LOCKOUT, 0); return 0; }
    if (!strcmp(argv[1], "hold")) { fm_air_request(FM_AIR_REQ_HOLD, 1); return 0; }
    if (!strcmp(argv[1], "free")) { fm_air_request(FM_AIR_REQ_HOLD, 0); return 0; }
    if (!strcmp(argv[1], "clear")) { fm_air_request(FM_AIR_REQ_CLEAR_LOCKOUTS, 0); return 0; }
    if (!strcmp(argv[1], "rds")) { fm_bcast_set_opt(FM_BC_RDS_OFF, !on_word(v)); return 0; }
    if (!strcmp(argv[1], "mono")) { fm_bcast_set_opt(FM_BC_MONO, on_word(v)); return 0; }
    LS_PRINTF(t_fm_bcast_cmd_bcast_unknown_s, "bcast: unknown '%s'\n", argv[1]);
    return 1;
}
