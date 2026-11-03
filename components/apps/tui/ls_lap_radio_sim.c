/* The bench's radio behind lap_rx_* (see ls_lap_radio.h), so lssim can run
 * a radio app and its frames can be looked at without a board. One radio,
 * "SIM": a noise floor, a steady carrier 60 kHz below the centre and a
 * carrier 20 kHz above it keyed in Morse ("CQ LAKESHARK", 15 words a
 * minute), so a spectrum shows something and a decoder has something to
 * decode. Each worker pass makes 40 ms of samples (one lssim frame). */
#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
#include "ls_lap_radio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIM_MIN_HZ 24000000ull
#define SIM_MAX_HZ 6000000000ull
#define SIM_DOT_MS 80u
static struct {
    bool open;
    uint64_t center;
    uint32_t rate;
    int gain;
    uint64_t sample;                     /* produced so far */
    uint32_t noise;
    float ph1, ph2;
    int16_t buf[2 * 8192];
} sim;

/* "CQ LAKESHARK" as on/off dot units: one space ends a letter (3 units with
   the element gap), three end a word (7). */
static const char *const MORSE = "-.-. --.-   .-.. .- -.- . ... .... .- .-. -.-";
static int key_units(char c) { return c == '-' ? 3 : 1; }
static bool keyed(uint64_t ms) {
    static int units = -1;
    if (units < 0) {
        units = 0;
        for (const char *p = MORSE; *p; p++) units += *p == ' ' ? 2 : key_units(*p) + 1;
        units += 7;                       /* the gap before it repeats */
    }
    uint64_t u = (ms / SIM_DOT_MS) % (uint64_t)units;
    for (const char *p = MORSE; *p; p++) {
        const int on = *p == ' ' ? 0 : key_units(*p), len = *p == ' ' ? 2 : on + 1;
        if (u < (uint64_t)len) return u < (uint64_t)on;
        u -= (uint64_t)len;
    }
    return false;
}
/* lssim: a board with no radio at all, as a Nano without an SDR is
   (LAPSIM_RADIO=none, or the LAPSIM_SCRIPT step radio=none). */
bool ls_lap_radio_sim_absent;
bool ls_lap_radio_sim_hackrf;
int ls_lap_radio_sim_ook=-1; /* scriptable app-owned A5/payload OOK fixture */
static int sim_list(lap_radio_info_t *out, int max) {
    if (max < 1 || ls_lap_radio_sim_absent) return 0;
    memset(out, 0, sizeof(*out));

    bool hackrf=ls_lap_radio_sim_hackrf || (getenv("LAPSIM_RADIO")&&!strcmp(getenv("LAPSIM_RADIO"),"hackrf"));
    snprintf(out->name,sizeof(out->name),"%s",hackrf?"SIM HackRF":"SIM");
    out->kind = hackrf?LAP_RADIO_KIND_HACKRF:LAP_RADIO_KIND_SIM;
    out->features = LAP_RXF_IQ | LAP_RXF_SPECTRUM | LAP_RXF_GAIN | LAP_RXF_RATE;
    out->present = true;
    out->min_hz = SIM_MIN_HZ;
    out->max_hz = hackrf?6000000000ULL:SIM_MAX_HZ;
    out->min_rate = 48000;
    out->max_rate = hackrf?20000000:2400000;
    out->gain_min = 0;
    out->gain_max = 496;
    return 1;
}
static int sim_valid(uint64_t hz) { return (hz>=SIM_MIN_HZ && hz<=1766000000ull) || (hz>=2300000000ull && hz<=SIM_MAX_HZ); }
static int sim_open(int radio, const lap_rx_config_t *cfg, ls_lap_rx_actual_t *actual) {
    if (radio != 0 || ls_lap_radio_sim_absent) return LAP_RX_ENODEV;
    if (!sim_valid(cfg->center_hz)) return LAP_RX_ERANGE;
    sim.open = true;
    sim.center = cfg->center_hz;
    sim.rate = cfg->rate_hz;
    sim.gain = cfg->gain_tenths_db;
    ls_lap_rx_gain_applied(sim.gain);
    sim.noise = 12345;
    *actual = (ls_lap_rx_actual_t){sim.center, sim.rate, sim.gain};
    ls_lap_rx_mark(sim.center, sim.rate, LAP_IQ_GAP | LAP_IQ_RETUNED | LAP_IQ_SYNTHETIC);
    return 0;
}
static int sim_tune(uint64_t hz, ls_lap_rx_actual_t *actual) {
    if (!sim_valid(hz)) return LAP_RX_ERANGE;
    sim.center = actual->center_hz = hz;
    ls_lap_rx_mark(hz, 0, LAP_IQ_RETUNED);
    return 0;
}
static int sim_gain(int tenths, ls_lap_rx_actual_t *actual) {
    sim.gain = actual->gain_tenths_db = tenths;
    ls_lap_rx_gain_applied(tenths);
    ls_lap_rx_mark(0, 0, LAP_IQ_RETUNED);
    return 0;
}
static int sim_rate(uint32_t rate, ls_lap_rx_actual_t *actual) {
    sim.rate = actual->rate_hz = rate;
    ls_lap_rx_mark(0, rate, LAP_IQ_GAP | LAP_IQ_RETUNED | LAP_IQ_SYNTHETIC);
    return 0;
}
static int sim_poll(ls_lap_rx_actual_t *actual) {
    (void)actual;
    if (ls_lap_radio_sim_absent && sim.open) return LAP_RX_ENODEV;
    if (!sim.open || !sim.rate) return 0;
    uint32_t n = sim.rate / 25u;          /* 40 ms */
    const float gain = sim.gain == LAP_GAIN_AUTO ? 1.0f : 0.25f + sim.gain / 400.0f;
    const float w1 = 6.2831853f * 20000.0f / (float)sim.rate, w2 = 6.2831853f * -60000.0f / (float)sim.rate;
    while (n) {
        const uint32_t k = n < 8192u ? n : 8192u;
        for (uint32_t i = 0; i < k; i++) {
            const uint64_t ms = (sim.sample + i) * 1000u / sim.rate;
            const float a = keyed(ms) ? 9000.0f : 0.0f;
            sim.noise = sim.noise * 1103515245u + 12345u;
            const float ni = (float)((int)(sim.noise >> 16 & 0x3ff) - 512);
            sim.noise = sim.noise * 1103515245u + 12345u;
            const float nq = (float)((int)(sim.noise >> 16 & 0x3ff) - 512);
            sim.ph1 += w1; if (sim.ph1 > 6.2831853f) sim.ph1 -= 6.2831853f;
            sim.ph2 += w2; if (sim.ph2 < -6.2831853f) sim.ph2 += 6.2831853f;
            const float i_ = gain * (ni + a * cosf(sim.ph1) + 1500.0f * cosf(sim.ph2));
            const float q_ = gain * (nq + a * sinf(sim.ph1) + 1500.0f * sinf(sim.ph2));
            sim.buf[2 * i] = (int16_t)(i_ > 32767 ? 32767 : i_ < -32767 ? -32767 : i_);
            sim.buf[2 * i + 1] = (int16_t)(q_ > 32767 ? 32767 : q_ < -32767 ? -32767 : q_);
            if(ls_lap_radio_sim_ook>=0) {
                uint64_t bit=(sim.sample+i)*1000u/sim.rate;
                unsigned word=0xa500u|((unsigned)ls_lap_radio_sim_ook&255);
                sim.buf[2*i]=(word&(1u<<(15-(bit%16))))?12000:0;
                sim.buf[2*i+1]=0;
            }
        }
        ls_lap_rx_push(sim.buf, k);
        sim.sample += k;
        n -= k;
    }
    return 0;
}

/* Radio RX API 2.4 simulator: exact holes, discrete gains, completion delay. */
int ls_lap_rx_sim_delay=2, ls_lap_rx_sim_result;
static int sim_controls[LAP_RX_CTLS], sim_pending;
static int sim_info2(int index, lap_radio_info2_t *out) {
    if (index!=0) return LAP_RX_ENODEV;
    *out=(lap_radio_info2_t){.size=sizeof(*out),.version=1,.present=!ls_lap_radio_sim_absent,.frequency_count=2,.rate_count=2,.gain_count=7};
    snprintf(out->id,sizeof(out->id),"sim.sdr");
    if (sim.open) snprintf(out->owner,sizeof(out->owner),"lap");
    out->frequencies[0]=(lap_radio_range_t){SIM_MIN_HZ,1766000000ull};
    out->frequencies[1]=(lap_radio_range_t){2300000000ull,SIM_MAX_HZ};
    out->rates[0]=(lap_radio_range_t){48000,1024000}; out->rates[1]=(lap_radio_range_t){2000000,2400000};
    static const int16_t gains[]={0,90,140,200,280,370,496}; memcpy(out->gains,gains,sizeof(gains));
    out->controls=((1u<<LAP_RX_CTLS)-1)&~(1u<<LAP_RX_CTL_DIRECT);
    memcpy(out->values,sim_controls,sizeof(sim_controls)); return 0;
}
static int sim_control(int ctl, int64_t value, ls_lap_rx_actual_t *actual) {
    if (!sim_pending) sim_pending=ls_lap_rx_sim_delay+1;
    if (--sim_pending>0) return LAP_RX_EAGAIN;
    if (ls_lap_radio_sim_absent) return LAP_RX_ENODEV;
    if (ls_lap_rx_sim_result) { int rc=ls_lap_rx_sim_result; ls_lap_rx_sim_result=0; return rc; }
    switch (ctl) {
    case LAP_RX_CTL_TUNE: return sim_tune((uint64_t)value,actual);
    case LAP_RX_CTL_GAIN: {
        if (value==LAP_GAIN_AUTO) return sim_gain(LAP_GAIN_AUTO,actual);
        static const int gains[]={0,90,140,200,280,370,496};
        int best=0; for (int i=1;i<7;i++) if (llabs(value-gains[i])<llabs(value-gains[best])) best=i;
        if (value<0 || value>496) return LAP_RX_ERANGE;
        return sim_gain(gains[best],actual);
    }
    case LAP_RX_CTL_RATE: return sim_rate((uint32_t)value,actual);
    case LAP_RX_CTL_ANTENNA: if (value!=0) return LAP_RX_EUNSUPPORTED; break;
    case LAP_RX_CTL_BIAS_T: case LAP_RX_CTL_OFFSET: if (value<0 || value>1) return LAP_RX_ERANGE; break;
    case LAP_RX_CTL_AGC: case LAP_RX_CTL_AMP: if (value<0 || value>2) return LAP_RX_ERANGE; break;
    case LAP_RX_CTL_PPM: if (value< -200 || value>200) return LAP_RX_ERANGE; break;
    case LAP_RX_CTL_LNA_DB: if (value!=-1 && (value<0 || value>40 || value%8)) return LAP_RX_ERANGE; break;
    case LAP_RX_CTL_VGA_DB: if (value!=-1 && (value<0 || value>62 || value%2)) return LAP_RX_ERANGE; break;
    default: return LAP_RX_EUNSUPPORTED;
    }
    sim_controls[ctl]=(int)value; ls_lap_rx_mark(0,0,LAP_IQ_GAP); return 0;
}
static void sim_close(void) { sim.open = false; sim_pending=0; }
static const ls_lap_rx_backend_t SIM = {sim_list, sim_open, sim_tune, sim_gain, sim_rate, sim_poll, sim_close, sim_info2, sim_control};
const ls_lap_rx_backend_t *ls_lap_rx_default_backend(void) { return &SIM; }
#endif
