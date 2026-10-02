/* US FAN FSK PHY header and frame counts; nearby utility dependent. */
#include "exp_lorafam.h"
#include <stdlib.h>
extern const ls_experiment_t exp_wisun;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR lf_view_t s_view;
static EXT_RAM_BSS_ATTR uint8_t s_frame[255];
static unsigned s_mode, s_channel, s_fec = 1;
static const char *const MODES[] = {"1b 50 kbps", "2a 100 kbps", "3 150 kbps"};
static const char *const FECS[] = {"none", "NRNSC interleave", "RSC", "RSC interleave"};
static bool start(char *why, size_t n)
{
    unsigned mode, channel, fec;
    portENTER_CRITICAL(&s_lock); mode=s_mode; channel=s_channel; fec=s_fec; portEXIT_CRITICAL(&s_lock);
    const esp_err_t err = lr20xx_exp_begin(LR20XX_ENGINE_WISUN, mode, channel, fec);
    if (err != ESP_OK) { lf_start_fail(why, n, "LR2021 Wi-SUN", err); return false; }
    portENTER_CRITICAL(&s_lock); memset(&s_view, 0, sizeof(s_view)); s_view.preset=mode; s_view.channel=channel; s_view.fec=fec; portEXIT_CRITICAL(&s_lock); return true;
}
static void stop(void) { (void)lr20xx_exp_end(); }
static void poll(void)
{
    lr20xx_engine_packet_t p;
    const int n = lr20xx_exp_poll(s_frame, sizeof(s_frame), &p);
    if (!n) return;
    lorafam_phr_t phr = {0}; uint8_t type=0;
    const bool phr_ok = (n > 0 || n == -2) && lorafam_wisun_phr(p.phr, &phr);
    const bool mac_ok = n > 0 && phr_ok && !phr.mode_switch && lorafam_wisun_mac(s_frame, n, &type);
    portENTER_CRITICAL(&s_lock);
    if (n < 0 && n != -2) s_view.errors++;
    else {
        s_view.phr_count++; s_view.packet=p; s_view.phr=phr; s_view.phr_ok=phr_ok; s_view.mac_type=type; s_view.mac_ok=mac_ok;
        if (!phr_ok) s_view.bad++;
        if (n == -2) s_view.large++; else s_view.frames++;
    }
    portEXIT_CRITICAL(&s_lock);
}
static int lines(char (*out)[LS_EXP_LINE], int max)
{
    lf_view_t v; unsigned mode, channel, fec;
    portENTER_CRITICAL(&s_lock); v=s_view; mode=s_mode; channel=s_channel; fec=s_fec; portEXIT_CRITICAL(&s_lock);
    if (v.phr_count) { mode=v.preset; channel=v.channel; fec=v.fec; }
    const uint32_t hz = mode==2 ? 902600000u+channel*600000u : 902200000u+channel*200000u;
    int n=0;
#define LINE(...) do { if (n < max) snprintf(out[n++], LS_EXP_LINE, __VA_ARGS__); } while (0)
    LINE("US mode %s channel %u %.3f MHz", MODES[mode], channel, hz/1e6);
    LINE("2-FSK FEC %s whitening ON", FECS[fec]);
    LINE("PHR %lu frames %lu errors %lu", (unsigned long)v.phr_count, (unsigned long)v.frames, (unsigned long)v.errors);
    LINE("Bad PHR %lu oversized dropped %lu", (unsigned long)v.bad, (unsigned long)v.large);
    if (v.phr_count) {
        LINE("PHR %04X len %u RSSI %.1f dBm", v.packet.phr, v.packet.length, (double)v.packet.rssi_dbm);
        if (v.phr_ok) LINE("PHR length %u FCS %u DW %u switch %u", v.phr.length, v.phr.crc16 ? 16 : 32, v.phr.whitening, v.phr.mode_switch);
        if (v.mac_ok) LINE("IEEE 802.15.4 MAC frame type %u", v.mac_type);
        else LINE("MAC unavailable / unsupported header");
    } else LINE("Waiting for utility FAN traffic");
    LINE("Nearby meters may use a different PHY");
#undef LINE
    return n;
}
static bool configure(int argc, char **argv, char *why, size_t n)
{
    unsigned mode;
    if (argc!=2 || (strcmp(argv[0],"1b") && strcmp(argv[0],"2a") && strcmp(argv[0],"3"))) goto bad;
    mode=!strcmp(argv[0],"1b") ? 0 : !strcmp(argv[0],"2a") ? 1 : 2;
    char *end; long ch=strtol(argv[1], &end, 10);
    if (!*argv[1] || *end || ch<0 || ch>(mode==2 ? 41 : 128)) goto bad;
    portENTER_CRITICAL(&s_lock); s_mode=mode; s_channel=(unsigned)ch; portEXIT_CRITICAL(&s_lock); return true;
bad:
    snprintf(why,n,"mode 1b/2a channel 0..128; mode 3 0..41"); return false;
}
static int get(const ls_opt_t *o) { portENTER_CRITICAL(&s_lock); int v=o->arg ? s_fec : s_mode; portEXIT_CRITICAL(&s_lock); return v; }
static void restart(void) { if (ls_exp_running()==&exp_wisun) ls_exp_start(&exp_wisun); }
static void set(const ls_opt_t *o,int v)
{
    portENTER_CRITICAL(&s_lock);
    if (o->arg) { if (v>=0 && v<4) s_fec=v; }
    else if (v>=0 && v<3) { s_mode=v; if (v==2 && s_channel>41) s_channel=41; }
    portEXIT_CRITICAL(&s_lock); restart();
}
static double channel(const ls_opt_t *o) { (void)o; portENTER_CRITICAL(&s_lock); unsigned v=s_channel; portEXIT_CRITICAL(&s_lock); return v; }
static void set_channel(const ls_opt_t *o,double v) { (void)o; portENTER_CRITICAL(&s_lock); s_channel=(unsigned)v; if (s_mode==2 && s_channel>41) s_channel=41; portEXIT_CRITICAL(&s_lock); restart(); }
static const ls_opt_t OPTS[] = {
    {.label="MODE",.kind=LS_OPT_CYCLE,.names=MODES,.n=3,.get=get,.set=set},
    {.label="CHANNEL",.kind=LS_OPT_NUMBER,.num=channel,.set_num=set_channel,.lo=0,.hi=128,.unit="Plan 1: 0..128; plan 3: 0..41"},
    {.label="FEC",.kind=LS_OPT_CYCLE,.names=FECS,.n=4,.get=get,.set=set,.arg=1},
};
const ls_experiment_t exp_wisun = {
    .id="wisun",.name="WI-SUN METERS",.sub="US FAN 2-FSK modes 1b/2a/3, PHR recon",.maturity=LS_EXP_TRYING, .lr2021_only=true,
    .needs="A Wi-SUN smart-meter network nearby - depends on the utility",.start=start,.stop=stop,.poll=poll,.lines=lines,
    .configure=configure,.opts=OPTS,.n_opts=3,
};
