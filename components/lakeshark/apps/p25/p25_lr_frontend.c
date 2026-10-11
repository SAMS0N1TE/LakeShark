#include "p25_lr_frontend.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846
bool p25_lr_frontend_init(p25_lr_frontend_t *s, float center_hz, bool filtered)
{
    if (!s || !isfinite(center_hz) || fabsf(center_hz) > 50000) return false;
    memset(s, 0, sizeof(*s));
    s->filtered = filtered;
    s->step = 2 * PI * center_hz / 125000;
    double sum = 0, fc = 6500.0 / 125000;
    for (unsigned k = 0; k < P25_LR_FRONTEND_TAPS; ++k) {
        int d = (int)k - 32;
        double sinc = d ? sin(2 * PI * fc * d) / (PI * d) : 2 * fc;
        s->taps[k] = (float)(sinc * (0.54 - 0.46 * cos(2 * PI * k / 64)));
        sum += s->taps[k];
    }
    for (unsigned k = 0; k < P25_LR_FRONTEND_TAPS; ++k) s->taps[k] /= (float)sum;
    return true;
}
void p25_lr_frontend_reset(p25_lr_frontend_t *s)
{
    if (!s) return;
    memset(s->history, 0, sizeof(s->history));
    memset(s->previous, 0, sizeof(s->previous));
    s->angle = 0;
    s->index = s->phase = 0;
    s->have_previous = false;
}
size_t p25_lr_frontend_needed(const p25_lr_frontend_t *s, size_t bytes)
{
    if (!s || bytes % 4 || bytes > 125256) return SIZE_MAX;
    size_t pairs = bytes / 4;
    if (!pairs) return 0;
    size_t first = !s->have_previous;
    size_t units = 48 * (pairs - first);
    unsigned phase = first ? 25 : s->phase;
    return first + (units >= phase ? (units - phase) / 25 + 1 : 0);
}
static float read_sample(const uint8_t *p)
{
    int v = p[0] | (unsigned)p[1] << 8;
    return (v < 32768 ? v : v - 65536) / 32768.0f;
}
size_t p25_lr_frontend_process(p25_lr_frontend_t *s, const uint8_t *iq,
                             size_t bytes, float *out, size_t capacity_pairs)
{
    size_t need = p25_lr_frontend_needed(s, bytes);
    if (need == SIZE_MAX || need > capacity_pairs ||
        (bytes && !iq) || (need && !out)) return SIZE_MAX;
    size_t written = 0;
    for (size_t k = 0; k < bytes; k += 4) {
        float i = read_sample(iq+k), q = read_sample(iq+k+2), v[2];
        if (s->step != 0) {
            float c = (float)cos(s->angle), sn = (float)sin(s->angle);
            float rotated = i*c + q*sn;
            q = q*c - i*sn; i = rotated;
            s->angle += s->step;
            if (s->angle > PI) s->angle -= 2*PI;
            if (s->angle < -PI) s->angle += 2*PI;
        }
        if (s->filtered) {
            s->history[s->index][0] = i; s->history[s->index][1] = q;
            v[0] = v[1] = 0;
            unsigned pos = s->index;
            for (unsigned t = 0; t < P25_LR_FRONTEND_TAPS; ++t) {
                v[0] += s->taps[t]*s->history[pos][0];
                v[1] += s->taps[t]*s->history[pos][1];
                pos = pos ? pos-1 : P25_LR_FRONTEND_TAPS-1;
            }
            s->index = (s->index+1) % P25_LR_FRONTEND_TAPS;
        } else { v[0] = i; v[1] = q; }
        if (!s->have_previous) {
            out[2*written] = v[0]; out[2*written+1] = v[1]; written++;
            s->phase = 25; s->have_previous = true;
        } else {
            while (s->phase <= 48) {
                float f = s->phase / 48.0f;
                for (unsigned c = 0; c < 2; ++c)
                    out[2*written+c] = s->previous[c] + f*(v[c]-s->previous[c]);
                written++; s->phase += 25;
            }
            s->phase -= 48;
        }
        s->previous[0] = v[0]; s->previous[1] = v[1];
    }
    return written;
}
