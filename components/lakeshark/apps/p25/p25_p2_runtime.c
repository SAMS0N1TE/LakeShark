#include "p25_p2_runtime.h"
#include "p25_p2_runtime_status.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "p25_phase2.h"
#include "p25_p2_slice.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool wanted, configured;
static uint32_t cfg_wacn, generation;
static uint16_t cfg_system, cfg_nac;
static unsigned cfg_slot;
static p25p2_decoder_t *decoder;
static p25p2_status_t status;
static uint32_t last_generation, last_hz;
static uint32_t applied_generation = UINT32_MAX;
static uint32_t max_decode_us;
static EXT_RAM_BSS_ATTR int16_t samples[8192];
static EXT_RAM_BSS_ATTR uint8_t symbols[1024];
extern void audio_write_p25_voice(const int16_t *, int);

bool p25_p2_enabled(void) { return __atomic_load_n(&wanted, __ATOMIC_ACQUIRE); }
void p25_p2_enable(bool on) { __atomic_store_n(&wanted, on, __ATOMIC_RELEASE); }
bool p25_p2_config(uint32_t wacn, uint16_t system, uint16_t nac,
                   unsigned slot) {
  if (wacn > 0xfffff || system > 0xfff || nac > 0xfff || slot > 1)
    return false;
  portENTER_CRITICAL(&lock);
  cfg_wacn = wacn;
  cfg_system = system;
  cfg_nac = nac;
  cfg_slot = slot;
  configured = true;
  generation++;
  portEXIT_CRITICAL(&lock);
  return true;
}
void p25_p2_describe(char *text, unsigned capacity) {
  portENTER_CRITICAL(&lock);
  p25p2_status_t s = status;
  bool ids = configured;
  uint32_t cost = max_decode_us;
  portEXIT_CRITICAL(&lock);
  if (!p25_p2_enabled())
    snprintf(text, capacity, "OFF (experimental)");
  else if (!ids)
    snprintf(text, capacity, "EXP: need WACN/SYS/NAC");
  else
    snprintf(text, capacity, "EXP S%u %s V%lu A%lu %luus", s.slot + 1,
             s.clear_confirmed ? "CLEAR"
             : s.synchronized  ? "MUTED"
                               : "HUNT",
             (unsigned long)s.voice_frames, (unsigned long)s.audio_frames,
             (unsigned long)cost);
}
static void output(const int16_t *pcm, size_t count, void *context) {
  (void)context;
  if (p25_p2_enabled())
    audio_write_p25_voice(pcm, (int)count);
}
uint32_t p25_p2_config_generation(void) {
  portENTER_CRITICAL(&lock);
  uint32_t g = generation;
  portEXIT_CRITICAL(&lock);
  return g;
}
bool p25_p2_status_for(uint32_t wanted_generation, p25p2_status_t *out) {
  portENTER_CRITICAL(&lock);
  bool ok = applied_generation == wanted_generation;
  if (ok && out)
    *out = status;
  portEXIT_CRITICAL(&lock);
  return ok;
}
void p25_p2_stop(void) {
  if (decoder) {
    p25p2_destroy(decoder);
    decoder = NULL;
  }
  portENTER_CRITICAL(&lock);
  memset(&status, 0, sizeof(status));
  max_decode_us = 0;
  applied_generation = UINT32_MAX;
  portEXIT_CRITICAL(&lock);
}
bool p25_p2_rx(dsp_state_t *dsp, const uint8_t *iq, int length, uint32_t hz,
               uint32_t wacn, uint16_t system, uint16_t nac) {
  if (!p25_p2_enabled()) {
    if (decoder)
      p25_p2_stop();
    return false;
  }
  if (!decoder) {
    decoder = p25p2_create(output, NULL);
    if (!decoder) {
      p25_p2_enable(false);
      return false;
    }
    last_generation = UINT32_MAX;
    last_hz = 0;
  }
  portENTER_CRITICAL(&lock);
  if (!configured && wacn && system) {
    cfg_wacn = wacn;
    cfg_system = system;
    cfg_nac = nac;
    configured = true;
    generation++;
  }
  bool ids = configured;
  uint32_t gen = generation, w = cfg_wacn;
  uint16_t s = cfg_system, n = cfg_nac;
  unsigned slot = cfg_slot;
  portEXIT_CRITICAL(&lock);
  if (!ids)
    return true;
  if (last_generation != gen || hz != last_hz || !dsp->phase2) {
    p25p2_configure(decoder, w, s, n, slot);
    dsp->phase2 = true;
    dsp_set_gain(dsp, fabsf(dsp->demod_gain));
    dsp_set_mode(dsp, DEMOD_CQPSK);
    dsp_reset_cqpsk_loops(dsp);
    last_generation = gen;
    last_hz = hz;
  }
  int count = dsp_process_iq(dsp, iq, length, samples, 8192);
  int out = p25_p2_slice(samples, count, dsp->demod_gain, symbols, 1024);
  int64_t start = esp_timer_get_time();
  p25p2_push(decoder, symbols, (size_t)out);
  uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
  p25p2_status_t snapshot;
  p25p2_status(decoder, &snapshot);
  portENTER_CRITICAL(&lock);
  status = snapshot;
  applied_generation = last_generation;
  if (elapsed > max_decode_us)
    max_decode_us = elapsed;
  portEXIT_CRITICAL(&lock);
  return true;
}
