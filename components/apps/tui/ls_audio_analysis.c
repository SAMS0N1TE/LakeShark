/* Real PCM analysis. One producer appends packed atomic stereo frames;
   one consumer owns FFTs, filters and results. All working storage is PSRAM. */
#include "ls_audio_analysis.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "ls_cold.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define AA_ALLOC(n) heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AA_FREE(p) heap_caps_free(p)
#else
#define AA_ALLOC(n) calloc(1, n)
#define AA_FREE(p) free(p)
#endif
#define AA_N 4096u
#define AA_RING 32768u
#define AA_SCOPE 128u
#define AA_TRACK 256u
#define AA_PI 3.14159265358979323846f
typedef struct {
  float b[3], a[2], z[2][2];
} aa_filter;
struct ls_audio_analysis {
  atomic_uint ring[AA_RING], end, rate, epoch, channels, origin_ms;
  ls_aa_config config;
  unsigned seen_epoch, next_fft, next_metric, filter_rate, metric_in_block,
      loud_blocks;
  unsigned track_head, track_count, pitch_at, onset_at;
  uint32_t onset_times[9];
  unsigned onset_count;
  float re[AA_N], im[AA_N], window[AA_N], cosine[AA_N / 2], sine[AA_N / 2];
  float db[AA_N / 2 + 1], hold[AA_N / 2 + 1], wave[AA_N], scope[AA_SCOPE][2];
  float yin[1024], difference[513], chroma_smooth[12], window_sum, previous_rms;
  double loud_energy[30], energy;
  float interpolation[2][4];
  aa_filter shelf, highpass;
  ls_aa_track track[AA_TRACK];
  ls_aa_track pitch_track[AA_TRACK];
  ls_aa_result result;
};
static float db_power(double p) {
  return p > 1e-12 ? 10 * log10f((float)p) : -120;
}
static float bound(float v, float lo, float hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
static void note(float hz, int *n, float *cents) {
  *n = -1;
  *cents = 0;
  if (!(hz > 0))
    return;
  float m = 69 + 12 * log2f(hz / 440);
  int nearest = (int)floorf(m + .5f);
  if (nearest >= 0 && nearest <= 127) {
    *n = nearest;
    *cents = (m - nearest) * 100;
  }
}
ls_aa_config ls_audio_analysis_defaults(void) {
  return (ls_aa_config){2048, 512, LS_AA_HANN, false, 40, 20000, -100, 80, 0};
}
size_t ls_audio_analysis_bytes(void) { return sizeof(ls_audio_analysis); }
LS_COLD bool ls_audio_analysis_configure(ls_audio_analysis *a, const ls_aa_config *c) {
  if (!a || !c || c->fft_size < 256 || c->fft_size > AA_N ||
      (c->fft_size & (c->fft_size - 1)) ||
      (c->hop != c->fft_size / 2 && c->hop != c->fft_size / 4 &&
       c->hop != c->fft_size / 8) ||
      c->window > LS_AA_BLACKMAN_HARRIS || !isfinite(c->lo_hz) ||
      !isfinite(c->hi_hz) || c->lo_hz < 0 || c->hi_hz <= c->lo_hz ||
      !isfinite(c->floor_db) || !isfinite(c->range_db) || c->range_db < 10 ||
      c->range_db > 160 || !isfinite(c->gain_db) || fabsf(c->gain_db) > 60)
    return false;
  a->config = *c;
  a->window_sum = 0;
  for (unsigned i = 0; i < c->fft_size; i++) {
    float t = 2 * AA_PI * i / c->fft_size;
    a->window[i] = c->window == LS_AA_HANN
                       ? .5f - .5f * cosf(t)
                       : .35875f - .48829f * cosf(t) + .14128f * cosf(2 * t) -
                             .01168f * cosf(3 * t);
    a->window_sum += a->window[i];
  }
  for (unsigned i = 0; i <= AA_N / 2; i++)
    a->hold[i] = -120;
  unsigned end = atomic_load(&a->end);
  a->next_fft = end >= c->fft_size ? end - c->fft_size : 0;
  return true;
}
LS_COLD ls_aa_config ls_audio_analysis_config(const ls_audio_analysis *a) {
  return a ? a->config : ls_audio_analysis_defaults();
}
LS_COLD ls_audio_analysis *ls_audio_analysis_new(void) {
  ls_audio_analysis *a = AA_ALLOC(sizeof(*a));
  if (!a)
    return NULL;
  for (unsigned i = 0; i < AA_RING; i++)
    atomic_init(&a->ring[i], 0);
  atomic_init(&a->end, 0);
  atomic_init(&a->rate, 0);
  atomic_init(&a->channels, 0);
  atomic_init(&a->epoch, 1);
  for (unsigned i = 0; i < AA_N / 2; i++) {
    a->cosine[i] = cosf(2 * AA_PI * i / AA_N);
    a->sine[i] = -sinf(2 * AA_PI * i / AA_N);
  }
  ls_aa_config c = ls_audio_analysis_defaults();
  ls_audio_analysis_configure(a, &c);
  a->result.note = a->result.key_root = -1;
  return a;
}
void ls_audio_analysis_free(ls_audio_analysis *a) { AA_FREE(a); }
void ls_audio_analysis_reset(ls_audio_analysis *a) {
  ls_audio_analysis_reset_at(a,0);
}
uint32_t ls_audio_analysis_origin_ms(const ls_audio_analysis *a){return a?atomic_load(&a->origin_ms):0;}
void ls_audio_analysis_reset_at(ls_audio_analysis *a,uint32_t source_ms) {
  if (a) {
    atomic_store(&a->end, 0);
    atomic_store(&a->origin_ms,source_ms);
    atomic_fetch_add(&a->epoch, 1);
  }
}
void ls_audio_analysis_append(ls_audio_analysis *a, const int16_t *pcm,
                              size_t frames, unsigned channels, unsigned rate) {
  if (!a || !pcm || (channels != 1 && channels != 2) || rate < 8000 ||
      rate > 192000)
    return;
  if (rate != atomic_load(&a->rate) || channels != atomic_load(&a->channels)) {
    ls_audio_analysis_reset_at(a,atomic_load(&a->origin_ms));
    atomic_store(&a->rate, rate);
    atomic_store(&a->channels, channels);
  }
  unsigned end = atomic_load_explicit(&a->end, memory_order_relaxed);
  for (size_t i = 0; i < frames; i++) {
    uint16_t l = (uint16_t)pcm[i * channels],
             r = channels == 1 ? l : (uint16_t)pcm[i * channels + 1];
    atomic_store_explicit(&a->ring[(end + (unsigned)i) & (AA_RING - 1)],
                          (unsigned)l | ((unsigned)r << 16),
                          memory_order_relaxed);
  }
  atomic_store_explicit(&a->end, end + (unsigned)frames, memory_order_release);
}
static void read_frame(const ls_audio_analysis *a, unsigned at, float *l,
                       float *r) {
  unsigned packed =
      atomic_load_explicit(&a->ring[at & (AA_RING - 1)], memory_order_relaxed);
  *l = (int16_t)(packed & 65535) * (1.0f / 32768);
  *r = (int16_t)(packed >> 16) * (1.0f / 32768);
}
static void filters(ls_audio_analysis *a, unsigned rate) {
  /* BS.1770 K weighting, rate-adapted pre-filter and RLB high pass.
     Constants follow the ITU design also used by libebur128. */
  float k = tanf(AA_PI * 1681.974450955533f / rate), q = .7071752369554196f;
  float vh = 1.58486470113f, vb = 1.25872093023f, d = 1 + k / q + k * k;
  a->shelf = (aa_filter){{(vh + vb * k / q + k * k) / d, 2 * (k * k - vh) / d,
                          (vh - vb * k / q + k * k) / d},
                         {2 * (k * k - 1) / d, (1 - k / q + k * k) / d},
                         {{0}}};
  k = tanf(AA_PI * 38.13547087602444f / rate);
  q = .5003270373238773f;
  d = 1 + k / q + k * k;
  a->highpass = (aa_filter){
      {1, -2, 1}, {2 * (k * k - 1) / d, (1 - k / q + k * k) / d}, {{0}}};
  a->filter_rate = rate;
}
static float filter(aa_filter *f, int ch, float x) {
  float y = f->b[0] * x + f->z[ch][0];
  f->z[ch][0] = f->b[1] * x - f->a[0] * y + f->z[ch][1];
  f->z[ch][1] = f->b[2] * x - f->a[1] * y;
  return y;
}
static void consumer_reset(ls_audio_analysis *a, unsigned epoch,
                           unsigned rate) {
  memset(&a->result, 0, sizeof(a->result));
  a->result.note = a->result.key_root = -1;
  a->result.rms_db = a->result.peak_db = a->result.estimated_peak_db = -120;
  a->result.momentary_lufs = a->result.short_lufs = a->result.spectral_db =
      -120;
  a->next_fft = a->next_metric = a->metric_in_block = a->loud_blocks =
      a->pitch_at = a->onset_at = 0;
  a->track_count = a->track_head = a->onset_count = 0;
  a->energy = 0;
  a->previous_rms = 0;
  memset(a->chroma_smooth, 0, sizeof(a->chroma_smooth));
  memset(a->interpolation, 0, sizeof(a->interpolation));
  memset(a->loud_energy, 0, sizeof(a->loud_energy));
  for (unsigned k = 0; k <= AA_N / 2; k++)
    a->hold[k] = -120;
  filters(a, rate);
  a->seen_epoch = epoch;
}
static void metrics(ls_audio_analysis *a, unsigned end, unsigned rate) {
  unsigned available = end - a->next_metric;
  if (available > AA_RING / 2) {
    a->next_metric = end - AA_RING / 2;
    available = AA_RING / 2;
  }
  unsigned take = available > 8192 ? 8192 : available,
           channels = atomic_load(&a->channels);
  float batch_peak = 0;
  for (unsigned i = 0; i < take; i++) {
    float l, r;
    read_frame(a, a->next_metric++, &l, &r);
    float x[2] = {l, r};
    for (unsigned ch = 0; ch < channels; ch++) {
      float y = filter(&a->highpass, ch, filter(&a->shelf, ch, x[ch]));
      a->energy += (double)y * y;
      float *p = a->interpolation[ch];
      p[0] = p[1];
      p[1] = p[2];
      p[2] = p[3];
      p[3] = x[ch];
      float peak = fabsf(x[ch]);
      for (int q = 1; q < 4; q++) {
        float t = q * .25f;
        float y2 = .5f * ((2 * p[1]) + (-p[0] + p[2]) * t +
                          (2 * p[0] - 5 * p[1] + 4 * p[2] - p[3]) * t * t +
                          (-p[0] + 3 * p[1] - 3 * p[2] + p[3]) * t * t * t);
        if (fabsf(y2) > peak)
          peak = fabsf(y2);
      }
      if (peak > batch_peak)
        batch_peak = peak;
    }
    if (++a->metric_in_block >= rate / 10) {
      a->loud_energy[a->loud_blocks % 30] = a->energy / a->metric_in_block;
      a->loud_blocks++;
      a->metric_in_block = 0;
      a->energy = 0;
      double m = 0, s = 0;
      unsigned count = a->loud_blocks < 30 ? a->loud_blocks : 30;
      for (unsigned b = 0; b < count; b++) {
        double v = a->loud_energy[(a->loud_blocks - 1 - b) % 30];
        s += v;
        if (b < 4)
          m += v;
      }
      if (a->loud_blocks >= 4)
        a->result.momentary_lufs = -.691f + db_power(m / 4);
      if (a->loud_blocks >= 30)
        a->result.short_lufs = -.691f + db_power(s / 30);
    }
  }
  /* Live MUSIC analysis measured 47-51 ms draws on T-Display-P4. Peak hold
     needs one logarithm of the maximum, not one for every PCM channel sample:
     log is monotonic. Keep every interpolated candidate and the same dB floor. */
  if (take) {
    float db = db_power((double)batch_peak * batch_peak);
    if (db > a->result.estimated_peak_db)
      a->result.estimated_peak_db = db;
  }
}
static void fft(ls_audio_analysis *a, unsigned n) {
  for (unsigned i = 1, j = 0; i < n; i++) {
    unsigned bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = a->re[i];
      a->re[i] = a->re[j];
      a->re[j] = t;
      t = a->im[i];
      a->im[i] = a->im[j];
      a->im[j] = t;
    }
  }
  for (unsigned len = 2; len <= n; len <<= 1)
    for (unsigned i = 0; i < n; i += len)
      for (unsigned k = 0; k < len / 2; k++) {
        unsigned b = i + k + len / 2, j = i + k, t = k * (AA_N / len);
        float re = a->re[b] * a->cosine[t] - a->im[b] * a->sine[t];
        float im = a->re[b] * a->sine[t] + a->im[b] * a->cosine[t];
        a->re[b] = a->re[j] - re;
        a->im[b] = a->im[j] - im;
        a->re[j] += re;
        a->im[j] += im;
      }
}
static float yin_difference(const ls_audio_analysis *a,unsigned count,unsigned lag){
  float total=0;
  for(unsigned i=0;i<count;i++){float d=a->yin[i]-a->yin[i+lag];total+=d*d;}
  return total;
}
static void pitch(ls_audio_analysis *a, unsigned end, unsigned rate) {
  if (end - a->pitch_at < rate / 10)
    return;
  a->pitch_at = end;
  unsigned stride = (rate + 15999) / 16000, n = end / stride;
  if (n > 1024)
    n = 1024;
  a->result.pitch_hz = a->result.pitch_confidence = 0;
  if (n < 64)
    return;
  unsigned first = end - n * stride;
  float energy = 0;
  for (unsigned i = 0; i < n; i++) {
    float l, r;
    read_frame(a, first + i * stride, &l, &r);
    a->yin[i] = (l + r) * .5f;
    energy += a->yin[i] * a->yin[i];
  }
  if (energy / n < 1e-7f)
    return;
  float fs = (float)rate / stride;
  unsigned max = (unsigned)(fs / 40), min = (unsigned)(fs / 1600);
  if (max > 512)
    max = 512;
  if (max > n / 2)
    max = n / 2;
  if (min < 2)
    min = 2;
  float sum = 0;
  for (unsigned lag = 1; lag <= max; lag++) {
    float v = 0;
    for (unsigned i = 0; i < n - max; i++) {
      float d = a->yin[i] - a->yin[i + lag];
      v += d * d;
    }
    sum += v;
    a->difference[lag] = sum > 1e-15f ? v * lag / sum : 1;
  }
  for (unsigned lag = min; lag < max; lag++)
    if (a->difference[lag] < .15f) {
      while (lag + 1 < max && a->difference[lag + 1] < a->difference[lag])
        lag++;
      float delta = 0;
      if (lag > 1 && lag < max) {
        /* Interpolate the raw difference. Normalization slopes bias a tuner
           by tens of cents at shorter periods. */
        float left = yin_difference(a,n-max,lag-1), mid = yin_difference(a,n-max,lag),
              right = yin_difference(a,n-max,lag+1);
        float d = left - 2 * mid + right;
        if (fabsf(d) > 1e-12f)
          delta = .5f * (left - right) / d;
      }
      a->result.pitch_hz = fs / (lag + bound(delta, -.5f, .5f));
      a->result.pitch_confidence = bound(1 - a->difference[lag], 0, 1);
      break;
    }
}
static void key_estimate(ls_audio_analysis *a) {
  static const float profile[2][12] = {
      {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f,
       2.29f, 2.88f},
      {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f,
       3.34f, 3.17f}};
  float sum = 0, max = 0;
  for (int i = 0; i < 12; i++) {
    sum += a->chroma_smooth[i];
    if (a->chroma_smooth[i] > max)
      max = a->chroma_smooth[i];
  }
  a->result.key_root = -1;
  a->result.key_confidence = 0;
  if (sum < 1e-9f || max / sum < .17f)
    return;
  float best = -2, second = -2;
  for (int minor = 0; minor < 2; minor++)
    for (int root = 0; root < 12; root++) {
      float mean = 0;
      for (int i = 0; i < 12; i++)
        mean += profile[minor][i] / 12;
      float xy = 0, xx = 0, yy = 0;
      for (int i = 0; i < 12; i++) {
        float x = a->chroma_smooth[(i + root) % 12] / sum - 1.0f / 12,
              y = profile[minor][i] - mean;
        xy += x * y;
        xx += x * x;
        yy += y * y;
      }
      float score = xx * yy > 1e-15f ? xy / sqrtf(xx * yy) : 0;
      if (score > best) {
        second = best;
        best = score;
        a->result.key_root = root;
        a->result.key_minor = minor != 0;
      } else if (score > second)
        second = score;
    }
  a->result.key_confidence = bound((best - second) * 3, 0, 1);
  if (best < .5f)
    a->result.key_root = -1;
}
static void onset(ls_audio_analysis *a, float rms, unsigned end,
                  unsigned rate) {
  float strength = rms - a->previous_rms * 1.8f;
  a->result.onset_strength = fmaxf(0, strength);
  a->previous_rms = rms;
  uint32_t ms = (uint32_t)((uint64_t)end * 1000 / rate);
  if (strength < .02f || rms < .035f || ms - a->onset_at < 180)
    return;
  a->onset_at = ms;
  a->result.onsets++;
  a->onset_times[a->onset_count % 9] = ms;
  a->onset_count++;
  if (a->onset_count >= 3) {
    unsigned count = a->onset_count < 9 ? a->onset_count : 9, intervals = 0;
    float total = 0;
    for (unsigned i = 1; i < count; i++) {
      uint32_t now = a->onset_times[(a->onset_count - i) % 9],
               before = a->onset_times[(a->onset_count - i - 1) % 9];
      unsigned d = now - before;
      if (d >= 250 && d <= 2000) {
        total += d;
        intervals++;
      }
    }
    if (intervals) {
      float bpm = 60000 * intervals / total;
      while (bpm < 60)
        bpm *= 2;
      while (bpm > 200)
        bpm *= .5f;
      a->result.bpm = bpm;
    }
  }
}
unsigned ls_audio_analysis_process(ls_audio_analysis *a, unsigned max_windows) {
  if (!a)
    return 0;
  unsigned rate = atomic_load(&a->rate), epoch = atomic_load(&a->epoch);
  if (!rate)
    return 0;
  if (epoch != a->seen_epoch)
    consumer_reset(a, epoch, rate);
  unsigned end = atomic_load_explicit(&a->end, memory_order_acquire),
           n = a->config.fft_size;
  metrics(a, end, rate);
  if (end < n)
    return 0;
  if (end - a->next_fft > AA_RING / 2)
    a->next_fft = end - n;
  if (max_windows > 4)
    max_windows = 4;
  if (max_windows && end - a->next_fft > n + max_windows * a->config.hop * 4)
    a->next_fft = end - n - (max_windows - 1) * a->config.hop;
  pitch(a, end, rate);
  unsigned done = 0;
  while (done < max_windows && end - a->next_fft >= n) {
    int64_t ll_pcm = 0, rr_pcm = 0, lr_pcm = 0;
    float peak = 0;
    for (unsigned i = 0; i < n; i++) {
      unsigned packed = atomic_load_explicit(
          &a->ring[(a->next_fft + i) & (AA_RING - 1)], memory_order_relaxed);
      int32_t left = (int16_t)(packed & 65535), right = (int16_t)(packed >> 16);
      float l = left * (1.0f / 32768), r = right * (1.0f / 32768);
      ll_pcm += (int64_t)left * left;
      rr_pcm += (int64_t)right * right;
      lr_pcm += (int64_t)left * right;
      float p = fmaxf(fabsf(l), fabsf(r));
      if (p > peak)
        peak = p;
      a->wave[i] = (l + r) * .5f;
      a->re[i] = a->wave[i] * a->window[i];
      a->im[i] = 0;
    }
    /* Int16/32768 products are binary-exact. At 4096 samples these integer
       sums are bounded by 2^42, so one exact scale preserves the original
       doubles while removing software-double arithmetic from the P4 hot loop.
       Raw products and waveform floats share the same packed atomic read. */
    double ll = (double)ll_pcm * (1.0 / 1073741824.0),
           rr = (double)rr_pcm * (1.0 / 1073741824.0),
           lr = (double)lr_pcm * (1.0 / 1073741824.0);
    for (unsigned i = 0; i < AA_SCOPE; i++)
      read_frame(a, a->next_fft + i * (n - 1) / (AA_SCOPE - 1), &a->scope[i][0],
                 &a->scope[i][1]);
    a->result.rms_db = db_power((ll + rr) / (2 * n));
    a->result.peak_db = db_power((double)peak * peak);
    a->result.crest_db = a->result.peak_db - a->result.rms_db;
    a->result.correlation =
        ll * rr > 1e-15 ? bound((float)(lr / sqrt(ll * rr)), -1, 1) : 0;
    fft(a, n);
    float scale = 4 / (a->window_sum * a->window_sum), maximum = 0;
    unsigned peak_bin = 0;
    for (unsigned i = 0; i < 12; i++)
      a->result.chroma[i] = 0;
    for (unsigned k = 0; k <= n / 2; k++) {
      float power = (a->re[k] * a->re[k] + a->im[k] * a->im[k]) * scale;
      a->db[k] = db_power(power);
      if (a->db[k] > a->hold[k])
        a->hold[k] = a->db[k];
      if (k && power > maximum) {
        maximum = power;
        peak_bin = k;
      }
      float hz = (float)k * rate / n;
      if (hz >= 40 && hz <= 5000 && power > 1e-10f) {
        int midi = (int)floorf(69 + 12 * log2f(hz / 440) + .5f);
        int chroma = (midi % 12 + 12) % 12;
        a->result.chroma[chroma] += power;
      }
    }
    a->result.peak_hz = 0;
    a->result.spectral_db = -120;
    if (maximum > 1e-10f) {
      float delta = 0;
      if (peak_bin > 0 && peak_bin < n / 2) {
        float l = a->db[peak_bin - 1], m = a->db[peak_bin],
              r = a->db[peak_bin + 1], d = l - 2 * m + r;
        if (fabsf(d) > 1e-8f)
          delta = bound(.5f * (l - r) / d, -.5f, .5f);
      }
      a->result.peak_hz = (peak_bin + delta) * rate / n;
      a->result.spectral_db = a->db[peak_bin];
    }
    note(a->result.peak_hz, &a->result.note, &a->result.cents);
    for (unsigned i = 0; i < 12; i++)
      a->chroma_smooth[i] =
          a->chroma_smooth[i] * .85f + a->result.chroma[i] * .15f;
    key_estimate(a);
    unsigned at = a->next_fft + n;
    a->result.time_ms = atomic_load(&a->origin_ms)+(uint32_t)((uint64_t)at * 1000 / rate);
    a->result.frames++;
    onset(a, sqrtf((float)((ll + rr) / (2 * n))), at, rate);
    a->track[a->track_head] =
        (ls_aa_track){a->result.time_ms, a->result.peak_hz};
    a->pitch_track[a->track_head] =
        (ls_aa_track){a->result.time_ms, a->result.pitch_hz};
    a->track_head = (a->track_head + 1) % AA_TRACK;
    if (a->track_count < AA_TRACK)
      a->track_count++;
    a->next_fft += a->config.hop;
    done++;
  }
  return done;
}
const ls_aa_result *ls_audio_analysis_result(const ls_audio_analysis *a) {
  return a ? &a->result : NULL;
}
unsigned ls_audio_analysis_bins(const ls_audio_analysis *a) {
  return a ? a->config.fft_size / 2 + 1 : 0;
}
float ls_audio_analysis_bin_db(const ls_audio_analysis *a, unsigned bin,
                               bool hold) {
  return a && bin < ls_audio_analysis_bins(a)
             ? (hold ? a->hold[bin] : a->db[bin]) + a->config.gain_db
             : -120;
}
float ls_audio_analysis_axis(const ls_audio_analysis *a, unsigned pixel,
                             unsigned pixels, bool melodic) {
  if (!a || pixels < 2)
    return 0;
  float t = (float)pixel / (pixels - 1), nyquist = atomic_load(&a->rate) * .5f;
  float lo = melodic ? 40 : a->config.lo_hz,
        hi = fminf(melodic ? 1600 : a->config.hi_hz, nyquist);
  if (hi <= lo)
    return lo;
  return melodic || a->config.log_axis
             ? fmaxf(lo, 1) * powf(hi / fmaxf(lo, 1), t)
             : lo + (hi - lo) * t;
}
LS_COLD bool ls_audio_analysis_cursor(const ls_audio_analysis *a, uint32_t ms, float hz,
                              ls_aa_cursor *out) {
  if (!a || !out || !a->result.frames || !isfinite(hz) || hz < 0)
    return false;
  unsigned rate = atomic_load(&a->rate),
           bin = (unsigned)(hz * a->config.fft_size / rate + .5f);
  if (bin >= ls_audio_analysis_bins(a))
    return false;
  *out = (ls_aa_cursor){ms, hz, a->db[bin], -1, 0};
  note(hz, &out->note, &out->cents);
  return true;
}
unsigned ls_audio_analysis_track(const ls_audio_analysis *a, ls_aa_track *out,
                                 unsigned capacity) {
  if (!a || !out)
    return 0;
  unsigned count = a->track_count < capacity ? a->track_count : capacity;
  for (unsigned i = 0; i < count; i++)
    out[i] = a->track[(a->track_head + AA_TRACK - count + i) % AA_TRACK];
  return count;
}
bool ls_audio_analysis_scope(const ls_audio_analysis *a, unsigned i,
                             float *left, float *right) {
  if (!a || i >= AA_SCOPE || !left || !right || !a->result.frames)
    return false;
  *left = a->scope[i][0];
  *right = a->scope[i][1];
  return true;
}
unsigned ls_audio_analysis_pitch_track(const ls_audio_analysis *a,
                                       ls_aa_track *out, unsigned capacity) {
  if (!a || !out)
    return 0;
  unsigned count = a->track_count < capacity ? a->track_count : capacity;
  for (unsigned i = 0; i < count; i++)
    out[i] = a->pitch_track[(a->track_head + AA_TRACK - count + i) % AA_TRACK];
  return count;
}
size_t ls_audio_analysis_read(const ls_audio_analysis *a, uint32_t *next,
                              uint32_t *epoch, int16_t *stereo, size_t frames,
                              unsigned *rate) {
  if (!a || !next || !epoch || !stereo || !rate || !frames)
    return 0;
  unsigned current_epoch = atomic_load(&a->epoch),
           end = atomic_load_explicit(&a->end, memory_order_acquire);
  *rate = atomic_load(&a->rate);
  if (!*rate)
    return 0;
  if (*epoch != current_epoch || end - *next > AA_RING / 2) {
    *epoch = current_epoch;
    *next = end > AA_RING/2 ? end-AA_RING/2 : 0;
  }
  size_t available = end - *next;
  if (frames > available)
    frames = available;
  for (size_t i = 0; i < frames; i++) {
    unsigned packed = atomic_load_explicit(
        &a->ring[(*next + (unsigned)i) & (AA_RING - 1)], memory_order_relaxed);
    stereo[2 * i] = (int16_t)(packed & 65535);
    stereo[2 * i + 1] = (int16_t)(packed >> 16);
  }
  *next += (uint32_t)frames;
  return frames;
}
float ls_audio_analysis_wave(const ls_audio_analysis *a, unsigned pixel,
                             unsigned pixels) {
  if (!a || !pixels || !a->result.frames)
    return 0;
  unsigned at = (unsigned)((uint64_t)pixel * a->config.fft_size / pixels);
  return at < a->config.fft_size ? a->wave[at] : 0;
}
