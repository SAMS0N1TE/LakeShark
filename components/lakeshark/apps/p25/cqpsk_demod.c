
#include "cqpsk_demod.h"
#include <math.h>
#include <string.h>

#define LPF1_TAPS 31
#define LPF1_DECIM 5
static const float lpf1[LPF1_TAPS] = {
     0.00000000f, -0.00005528f, -0.00012878f,  0.00000000f,  0.00069996f,  0.00250890f,
     0.00608852f,  0.01209581f,  0.02098580f,  0.03279500f,  0.04697689f,  0.06235878f,
     0.07726076f,  0.08977067f,  0.09811769f,  0.10105053f,  0.09811769f,  0.08977067f,
     0.07726076f,  0.06235878f,  0.04697689f,  0.03279500f,  0.02098580f,  0.01209581f,
     0.00608852f,  0.00250890f,  0.00069996f,  0.00000000f, -0.00012878f, -0.00005528f,
     0.00000000f
};

static const float rrc_taps[CQPSK_RRC_TAPS] = {
     0.012241651f,  0.003160650f, -0.010366737f, -0.022293508f, -0.025852009f, -0.016883213f,
     0.003408411f,  0.027753598f,  0.044930713f,  0.044257123f,  0.020845543f, -0.020607126f,
    -0.065436025f, -0.092778978f, -0.082589337f, -0.023512301f,  0.081381961f,  0.213468339f,
     0.342914962f,  0.437340900f,  0.471912394f,  0.437340900f,  0.342914962f,  0.213468339f,
     0.081381961f, -0.023512301f, -0.082589337f, -0.092778978f, -0.065436025f, -0.020607126f,
     0.020845543f,  0.044257123f,  0.044930713f,  0.027753598f,  0.003408411f, -0.016883213f,
    -0.025852009f, -0.022293508f, -0.010366737f,  0.003160650f,  0.012241651f,
};

#define SYM_OUTER   13000.0f
#define SYM_INNER   4500.0f

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

void cqpsk_init(cqpsk_state_t *s)
{
    memset(s, 0, sizeof(cqpsk_state_t));

    s->costas_alpha = 0.030f;
    s->costas_beta  = 0.030f * 0.030f * 0.25f;
    s->tracking_feedback  = 0.75f;
    s->tracking_threshold = 120.0f;
    s->sym_period = (float)CQPSK_SPS;
    s->sym_clock  = 0.0f;
    s->sym_gain   = 0.01f;
    s->last_sym_i = 1.0f;
    s->last_sym_q = 0.0f;
    s->constellation_rot = 0;
}

void cqpsk_set_tracking(cqpsk_state_t *s, float feedback, float threshold)
{
    s->tracking_feedback = feedback;
    s->tracking_threshold = threshold;
    s->costas_alpha = 0.04f * feedback;
    s->costas_beta  = s->costas_alpha * s->costas_alpha * 0.25f;
}

static inline float phase_error_qpsk(float i, float q)
{
    float si = (i > 0) ? 1.0f : -1.0f;
    float sq = (q > 0) ? 1.0f : -1.0f;
    return sq * i - si * q;
}

static int16_t diff_decode(float di, float dq, float *li, float *lq, int rot)
{

    float ri = di * (*li) + dq * (*lq);
    float rq = dq * (*li) - di * (*lq);
    *li = di;
    *lq = dq;

    float a = ri, b = rq;
    switch (rot) {
        case 1:  a = -rq; b =  ri; break;
        case 2:  a = -ri; b = -rq; break;
        case 3:  a =  rq; b = -ri; break;
        default: break;
    }

    if (a > 0 && b > 0)       return (int16_t)( SYM_INNER);
    else if (a < 0 && b > 0)  return (int16_t)( SYM_OUTER);
    else if (a < 0 && b < 0)  return (int16_t)(-SYM_INNER);
    else                       return (int16_t)(-SYM_OUTER);
}

int cqpsk_process_iq(cqpsk_state_t *s, const uint8_t *iq_data, int iq_len,
                     int16_t *audio_out, int audio_max)
{
    int n_out = 0;
    int n_iq = iq_len / 2;

    for (int k = 0; k < n_iq; k++) {
        float fi = ((float)iq_data[k * 2]     - 127.5f) / 127.5f;
        float fq = ((float)iq_data[k * 2 + 1] - 127.5f) / 127.5f;

        s->lpf1_buf_i[s->lpf1_idx] = fi;
        s->lpf1_buf_q[s->lpf1_idx] = fq;

        s->decim1_count++;
        if (s->decim1_count < LPF1_DECIM) {
            s->lpf1_idx = (s->lpf1_idx + 1) % LPF1_TAPS;
            continue;
        }
        s->decim1_count = 0;

        float s1_i = 0, s1_q = 0;
        int idx = s->lpf1_idx;
        for (int t = 0; t < LPF1_TAPS; t++) {
            s1_i += lpf1[t] * s->lpf1_buf_i[idx];
            s1_q += lpf1[t] * s->lpf1_buf_q[idx];
            idx--;
            if (idx < 0) idx = LPF1_TAPS - 1;
        }
        s->lpf1_idx = (s->lpf1_idx + 1) % LPF1_TAPS;

        s->decim2_count++;
        if (s->decim2_count < 2) continue;
        s->decim2_count = 0;

        float cos_p = cosf(s->costas_phase);
        float sin_p = sinf(s->costas_phase);
        float mi = s1_i * cos_p + s1_q * sin_p;
        float mq = -s1_i * sin_p + s1_q * cos_p;

        s->rrc_buf_i[s->rrc_idx] = mi;
        s->rrc_buf_q[s->rrc_idx] = mq;

        float fi2 = 0, fq2 = 0;
        idx = s->rrc_idx;
        for (int t = 0; t < CQPSK_RRC_TAPS; t++) {
            fi2 += rrc_taps[t] * s->rrc_buf_i[idx];
            fq2 += rrc_taps[t] * s->rrc_buf_q[idx];
            idx--;
            if (idx < 0) idx = CQPSK_RRC_TAPS - 1;
        }
        s->rrc_idx = (s->rrc_idx + 1) % CQPSK_RRC_TAPS;

        float pe = phase_error_qpsk(fi2, fq2);
        s->costas_freq  += s->costas_beta * pe;
        s->costas_phase += s->costas_freq + s->costas_alpha * pe;

        while (s->costas_phase > M_PI)  s->costas_phase -= 2.0f * M_PI;
        while (s->costas_phase < -M_PI) s->costas_phase += 2.0f * M_PI;
        if (s->costas_freq > 0.3f) s->costas_freq = 0.3f;
        if (s->costas_freq < -0.3f) s->costas_freq = -0.3f;

        s->sym_clock += 1.0f;
        s->sym_count++;

        int half = (int)(s->sym_period * 0.5f);
        if (s->sym_count == half) {
            s->mid_i = fi2;
            s->mid_q = fq2;
        }

        if (s->sym_clock >= s->sym_period) {
            s->sym_clock -= s->sym_period;
            s->sym_count = 0;

            float te = s->mid_i * (s->prev_i - fi2) + s->mid_q * (s->prev_q - fq2);
            s->sym_period -= s->sym_gain * te;
            if (s->sym_period < (float)CQPSK_SPS - 0.5f) s->sym_period = (float)CQPSK_SPS - 0.5f;
            if (s->sym_period > (float)CQPSK_SPS + 0.5f) s->sym_period = (float)CQPSK_SPS + 0.5f;

            int16_t sym = diff_decode(fi2, fq2, &s->last_sym_i, &s->last_sym_q,
                                       s->constellation_rot);

            for (int r = 0; r < 10 && n_out < audio_max; r++)
                audio_out[n_out++] = sym;

            s->prev_i = fi2;
            s->prev_q = fq2;
        }
    }

    return n_out;
}
