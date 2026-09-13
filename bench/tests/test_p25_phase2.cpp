extern "C" {
#include "ls_test.h"
}
#include "p25_phase2.h"
#include "fast_math.h"
#include <cmath>
#include <cstdio>
#include <array>

static void collect(const int16_t *, size_t count, void *context) {
  *(size_t *)context += count;
}

LS_CASE(synthesis_cosine_matches_libm_over_its_phase_range) {
  double largest = 0;
  for (int i = -600000; i <= 600000; ++i) {
    float angle = i * 0.001f;
    double error = std::fabs(p25p2_cos(angle) - std::cos((double)angle));
    if (error > largest) largest = error;
    float phase = p25p2_phase(angle);
    LS_CHECK(phase >= 0 && phase <= 6.283186f);
  }
  std::printf("Synthesis cosine max error %.8f\n", largest);
  LS_CHECK(largest < 0.00007);
}

LS_CASE(unconfigured_input_and_invalid_configuration_cannot_decode) {
  size_t samples = 0;
  auto *d = p25p2_create(collect, &samples);
  LS_CHECK(d != nullptr);
  std::array<uint8_t, 8192> bytes{};
  p25p2_push(d, bytes.data(), bytes.size());
  p25p2_status_t s;
  p25p2_status(d, &s);
  LS_EQ_UINT(s.symbols, 0);
  LS_CHECK(!p25p2_configure(d, 0x100000, 1, 1, 0));
  LS_CHECK(!p25p2_configure(d, 1, 0x1000, 1, 0));
  LS_CHECK(!p25p2_configure(d, 1, 1, 0x1000, 0));
  LS_CHECK(!p25p2_configure(d, 1, 1, 1, 2));
  LS_EQ_UINT(samples, 0);
  p25p2_destroy(d);
}

LS_CASE(noise_reset_and_broken_symbol_stream_stay_muted) {
  size_t samples = 0;
  auto *d = p25p2_create(collect, &samples);
  LS_CHECK(p25p2_configure(d, 0x92715, 0x1f6, 0x01a, 1));
  std::array<uint8_t, 8192> bytes;
  uint32_t random = 117;
  for (auto &b : bytes) {
    random = random * 1664525u + 1013904223u;
    b = random >> 30;
  }
  for (int i = 0; i < 16; ++i)
    p25p2_push(d, bytes.data(), bytes.size());
  p25p2_status_t s;
  p25p2_status(d, &s);
  LS_EQ_UINT(s.audio_frames, 0);
  LS_CHECK(!s.clear_confirmed);
  uint8_t broken = 255;
  p25p2_push(d, &broken, 1);
  p25p2_status(d, &s);
  LS_EQ_UINT(s.symbols, 0);
  LS_EQ_UINT(s.slot, 1);
  LS_EQ_UINT(s.algorithm, 255);
  LS_CHECK(!s.synchronized);
  p25p2_push(d, bytes.data(), 100);
  p25p2_reset(d);
  p25p2_status(d, &s);
  LS_EQ_UINT(s.symbols, 0);
  LS_CHECK(!s.clear_confirmed);
  p25p2_destroy(d);
}
