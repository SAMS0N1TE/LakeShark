#include "dsp_pipeline.h"
#include "ls_test.h"
#include "lsm_gen.h"
#include <math.h>
#include <stdlib.h>

void sys_log(unsigned char color, const char *fmt, ...) {
  (void)color;
  (void)fmt;
}

LS_CASE(clean_6000_baud_iq_recovers_symbols_only_at_phase2_rate) {
  enum {
    N = 3000,
    IQ_BYTES = N * 2 * (DSP_SAMPLE_RATE * DSP_PRE_DECIM / 6000)
  };
  int *tx = malloc(N * sizeof(int));
  uint8_t *iq = malloc(IQ_BYTES);
  LS_CHECK(tx && iq);
  if (!tx || !iq) {
    free(tx);
    free(iq);
    return;
  }
  uint32_t random = 271;
  static const int levels[] = {1, 3, -1, -3};
  for (int i = 0; i < N; ++i) {
    random = random * 1664525u + 1013904223u;
    tx[i] = levels[random >> 30];
  }
  lsm_channel_t channel;
  lsm_channel_clean(&channel);
  for (int variant = 0; variant < 3; ++variant) {
    channel.freq_off_hz = (variant - 1) * 200.0f;
    channel.noise_sd = variant == 1 ? 0.0f : 0.03f;
    ls_rng_t rng;
    ls_rng_seed(&rng, 981);
    LS_EQ_INT(lsm_render_iq(tx, N, &channel, &rng, iq, IQ_BYTES), IQ_BYTES);
    for (int phase2 = 0; phase2 < 2; ++phase2) {
      dsp_state_t dsp;
      dsp_init(&dsp);
      dsp.phase2 = phase2;
      dsp_set_mode(&dsp, DEMOD_CQPSK);
      dsp_reset_cqpsk_loops(&dsp);
      int16_t audio[8192];
      int rx[N + 100], count = 0;
      int repeat = phase2 ? 8 : 10;
      for (int pos = 0; pos < IQ_BYTES; pos += 16384) {
        int len = IQ_BYTES - pos;
        if (len > 16384)
          len = 16384;
        int n = dsp_process_iq(&dsp, iq + pos, len, audio, 8192);
        for (int j = 0; j + repeat <= n && count < N + 100; j += repeat) {
          float v = audio[j] / fabsf(dsp.demod_gain);
          rx[count++] = v >= 0 ? (v > 2 ? 3 : 1) : (v < -2 ? -3 : -1);
        }
      }
      if (!phase2) {
        LS_CHECK(count < N * 9 / 10);
        continue;
      }
      LS_CHECK(abs(count - N) < 20);
      int best = 0, total = N - 300;
      for (int lag = -30; lag <= 30; ++lag) {
        int same = 0;
        for (int i = 150; i < N - 150 && i + lag < count; ++i)
          if (rx[i + lag] == tx[i])
            ++same;
        if (same > best)
          best = same;
      }
      printf("6000 baud offset=%.0f: recovered=%d matched=%d/%d\n",
             channel.freq_off_hz, count, best, total);
      LS_CHECK(best > total * 98 / 100);
    }
  }
  free(tx);
  free(iq);
}
