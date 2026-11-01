#include "ls_audio_overview.h"
#include "ls_trace.h"
#include "esp_timer.h"
#include "../../chmorgan__esp-audio-player/codecs/ls_decode.h"
#include "ls_audio_analysis.h"
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define OV_ALLOC(n) heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define OV_FREE(p) heap_caps_free(p)
#else
#define OV_ALLOC(n) calloc(1, n)
#define OV_FREE(p) free(p)
#endif
#ifdef _WIN32
#include <direct.h>
#define OV_MKDIR(p) _mkdir(p)
#else
#define OV_MKDIR(p) mkdir(p, 0777)
#endif
typedef struct {
  char magic[8];
  uint64_t hash, size, mtime, frames;
  uint32_t rate, checksum;
} ov_header;
_Static_assert(sizeof(ov_header) == 48, "overview header layout");
typedef struct {
  unsigned generation;
  bool refresh;
  char path[512];
} ov_request;
struct ls_audio_overview {
  _Atomic(ls_ov_service_fn) service;
  _Atomic(void *) service_user;
  _Atomic(ov_request *) request;
  atomic_uint progress, rate, error, generation, active_generation, duration_ms;
  atomic_uint slice_ms, slice_config, slice_at, slice_bins;
  atomic_bool slice_pending, slice_ready;
  atomic_uint slice[2049];
  atomic_bool ready, cached, quit, parked;
  atomic_uint wave[LS_OV_WAVE], spec[LS_OV_COLUMNS * LS_OV_BINS / 4];
  char root[256], path[512], cache[320], io[4096];
  int16_t pcm[4096 * 2];
  bool building;
  FILE *file;
  ls_decode *decoder;
  ls_audio_analysis *analysis;
  ov_header header;
#ifdef ESP_PLATFORM
  TaskHandle_t task;
#endif
};
size_t ls_audio_overview_bytes(void) {
  return sizeof(ls_audio_overview) + ls_audio_analysis_bytes();
}
static void release_source(ls_audio_overview *o) {
  ls_decode_close(o->decoder);
  o->decoder = NULL;
  if (o->file)
    fclose(o->file);
  o->file = NULL;
}
static uint32_t checksum_word(uint32_t hash, uint32_t word) {
  for (int i = 0; i < 4; i++) {
    hash ^= (word >> (8 * i)) & 255;
    hash *= 16777619u;
  }
  return hash;
}
static bool cache_read(ls_audio_overview *o) {
  FILE *f = fopen(o->cache, "rb");
  if (!f)
    return false;
  setvbuf(f, o->io, _IOFBF, sizeof(o->io));
  ov_header h;
  bool ok = fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, "LSOV002", 8) &&
            h.hash == o->header.hash && h.size == o->header.size &&
            h.mtime == o->header.mtime && h.rate >= 8000 && h.rate <= 192000 &&
            h.frames > 0;
  uint32_t hash = 2166136261u;
  for (unsigned i = 0; i < LS_OV_WAVE && ok; i++) {
    uint32_t word;
    ok = fread(&word, 4, 1, f) == 1;
    if (ok) {
      hash = checksum_word(hash, word);
      atomic_store(&o->wave[i], word);
    }
  }
  for (unsigned i = 0; i < LS_OV_COLUMNS * LS_OV_BINS / 4 && ok; i++) {
    uint32_t word;
    ok = fread(&word, 4, 1, f) == 1;
    if (ok) {
      hash = checksum_word(hash, word);
      atomic_store(&o->spec[i], word);
    }
  }
  ok = ok && hash == h.checksum && fgetc(f) == EOF;
  fclose(f);
  if (ok) {
    atomic_store(&o->rate, h.rate);
    atomic_store(&o->duration_ms, h.frames > UINT64_MAX / 1000 ||
                                          h.frames * 1000 / h.rate > UINT32_MAX
                                      ? UINT32_MAX
                                      : (uint32_t)(h.frames * 1000 / h.rate));
    o->header = h;
  }
  return ok;
}
static void cache_write(ls_audio_overview *o) {
  snprintf(o->io, sizeof(o->io), "%s", o->root);
  for (char *p = o->io + 1; *p; p++)
    if (*p == '/' || *p == '\\') {
      char c = *p;
      *p = 0;
      OV_MKDIR(o->io);
      *p = c;
    }
  OV_MKDIR(o->io);
  uint32_t hash = 2166136261u;
  for (unsigned i = 0; i < LS_OV_WAVE; i++)
    hash = checksum_word(hash, atomic_load(&o->wave[i]));
  for (unsigned i = 0; i < LS_OV_COLUMNS * LS_OV_BINS / 4; i++)
    hash = checksum_word(hash, atomic_load(&o->spec[i]));
  o->header.checksum = hash;
  FILE *f = fopen(o->cache, "wb");
  if (!f) {
    atomic_store(&o->error, 3);
    return;
  }
  setvbuf(f, o->io, _IOFBF, sizeof(o->io));
  bool ok = fwrite(&o->header, sizeof(o->header), 1, f) == 1;
  for (unsigned i = 0; i < LS_OV_WAVE && ok; i++) {
    uint32_t word = atomic_load(&o->wave[i]);
    ok = fwrite(&word, 4, 1, f) == 1;
  }
  for (unsigned i = 0; i < LS_OV_COLUMNS * LS_OV_BINS / 4 && ok; i++) {
    uint32_t word = atomic_load(&o->spec[i]);
    ok = fwrite(&word, 4, 1, f) == 1;
  }
  if (fflush(f))
    ok = false;
  if (fclose(f))
    ok = false;
  if (!ok)
    atomic_store(&o->error, 3);
}
static bool start(ls_audio_overview *o, ov_request *request) {
  o->building = false;
  atomic_store(&o->slice_ready, false);
  release_source(o);
  snprintf(o->path, sizeof(o->path), "%s", request->path);
  bool refresh = request->refresh;
  atomic_store(&o->active_generation, request->generation);
  OV_FREE(request);
  atomic_store(&o->ready, false);
  atomic_store(&o->cached, false);
  atomic_store(&o->progress, 0);
  atomic_store(&o->error, 0);
  struct stat st;
  if (stat(o->path, &st)) {
    atomic_store(&o->error, 1);
    return false;
  }
  o->header = (ov_header){"LSOV002",
                          UINT64_C(14695981039346656037),
                          (uint64_t)st.st_size,
                          (uint64_t)st.st_mtime,
                          0,
                          0,
                          0};
  for (const unsigned char *p = (const unsigned char *)o->path; *p; p++) {
    o->header.hash ^= *p;
    o->header.hash *= UINT64_C(1099511628211);
  }
  snprintf(o->cache, sizeof(o->cache), "%s/%016" PRIx64 ".overview", o->root,
           o->header.hash);
  if (!refresh && cache_read(o)) {
    atomic_store(&o->cached, true);
    atomic_store(&o->progress, 100);
    atomic_store(&o->ready, atomic_load(&o->active_generation) ==
                                    atomic_load(&o->generation) &&
                                !atomic_load(&o->quit));
    return false;
  }
  for (unsigned i = 0; i < LS_OV_WAVE; i++)
    atomic_store(&o->wave[i], 0x80007fffu);
  for (unsigned i = 0; i < LS_OV_COLUMNS * LS_OV_BINS / 4; i++)
    atomic_store(&o->spec[i], 0);
  o->file = fopen(o->path, "rb");
  if (o->file)
    o->decoder = ls_decode_open(o->file);
  if (!o->decoder) {
    release_source(o);
    atomic_store(&o->error, 2);
    return false;
  }
  o->header.rate = ls_decode_rate(o->decoder);
  o->header.frames = ls_decode_frames(o->decoder);
  atomic_store(&o->rate, o->header.rate);
  atomic_store(&o->duration_ms,
               o->header.frames > UINT64_MAX / 1000 ||
                       o->header.frames * 1000 / o->header.rate > UINT32_MAX
                   ? UINT32_MAX
                   : (uint32_t)(o->header.frames * 1000 / o->header.rate));
  ls_audio_analysis_reset(o->analysis);
  ls_aa_config config = ls_audio_analysis_defaults();
  config.fft_size = 1024;
  config.hop = 512;
  ls_audio_analysis_configure(o->analysis, &config);
  o->building = true;
  return true;
}
static void slice_step(ls_audio_overview *o) {
  if (!atomic_exchange(&o->slice_pending, false) || !ls_audio_overview_ready(o))
    return;
  unsigned ms = atomic_load(&o->slice_ms),
           config = atomic_load(&o->slice_config);
  unsigned n = config >> 1, window = config & 1,
           generation = atomic_load(&o->active_generation);
  atomic_store(&o->slice_ready, false);
  if (!o->decoder) {
    o->file = fopen(o->path, "rb");
    if (o->file)
      o->decoder = ls_decode_open(o->file);
  }
  if (!o->decoder) {
    release_source(o);
    return;
  }
  unsigned rate = ls_decode_rate(o->decoder),
           channels = ls_decode_channels(o->decoder);
  const int64_t seek_us = LS_TRACE_US();
  if (!ls_decode_seek(o->decoder, (uint64_t)ms * rate / 1000))
    return;
  LS_TRACE("ov slice", (int32_t)((LS_TRACE_US() - seek_us) / 1000), (int32_t)ms);
  size_t got = ls_decode_read(o->decoder, o->pcm, n);
  if (ls_decode_failed(o->decoder))
    return;
  memset(o->pcm + got * channels, 0, (n - got) * channels * sizeof(*o->pcm));
  ls_aa_config c = ls_audio_analysis_defaults();
  c.fft_size = n;
  c.hop = n / 2;
  c.window = (ls_aa_window)window;
  ls_audio_analysis_reset(o->analysis);
  ls_audio_analysis_configure(o->analysis, &c);
  ls_audio_analysis_append(o->analysis, o->pcm, n, channels, rate);
  if (!ls_audio_analysis_process(o->analysis, 1))
    return;
  unsigned bins = ls_audio_analysis_bins(o->analysis);
  for (unsigned i = 0; i < bins; i++) {
    float db = ls_audio_analysis_bin_db(o->analysis, i, false);
    unsigned word;
    memcpy(&word, &db, 4);
    atomic_store(&o->slice[i], word);
  }
  atomic_store(&o->slice_bins, bins);
  atomic_store(&o->slice_at, ms);
  atomic_store(&o->slice_ready, generation == atomic_load(&o->generation) &&
                                    ms == atomic_load(&o->slice_ms));
}
void ls_audio_overview_step(ls_audio_overview *o, unsigned budget) {
  if (!o || atomic_load(&o->quit))
    return;
  ov_request *request = atomic_exchange(&o->request, NULL);
  if (request)
    start(o, request);
  if (!o->building) {
    slice_step(o);
    return;
  }
  if (!o->decoder || !budget)
    return;
  if (budget > 2048)
    budget = 2048;
  unsigned channels = ls_decode_channels(o->decoder);
  uint64_t first = ls_decode_position(o->decoder);
  const int64_t read_us = LS_TRACE_US();
  size_t got = ls_decode_read(o->decoder, o->pcm, budget);
  if (LS_TRACE_US() - read_us > 30000)
    LS_TRACE("ov read slow", (int32_t)((LS_TRACE_US() - read_us) / 1000), (int32_t)got);
  if (!got) {
    if (ls_decode_failed(o->decoder)) {
      atomic_store(&o->error, 2);
      release_source(o);
      return;
    }
    cache_write(o);
    o->building = false;
    atomic_store(&o->progress, 100);
    atomic_store(&o->ready, atomic_load(&o->active_generation) ==
                                    atomic_load(&o->generation) &&
                                !atomic_load(&o->quit));
    return;
  }
  for (size_t i = 0; i < got; i++) {
    unsigned bucket = (unsigned)((first + i) * LS_OV_WAVE / o->header.frames);
    if (bucket >= LS_OV_WAVE)
      bucket = LS_OV_WAVE - 1;
    int l = o->pcm[i * channels],
        r = channels == 1 ? l : o->pcm[i * channels + 1], mono = (l + r) / 2;
    unsigned word = atomic_load(&o->wave[bucket]);
    int lo = (int16_t)(word & 65535), hi = (int16_t)(word >> 16);
    if (mono < lo)
      lo = mono;
    if (mono > hi)
      hi = mono;
    atomic_store(&o->wave[bucket],
                 (uint16_t)lo | ((unsigned)(uint16_t)hi << 16));
  }
  ls_audio_analysis_append(o->analysis, o->pcm, got, channels, o->header.rate);
  if (ls_audio_analysis_process(o->analysis, 4)) {
    unsigned first_column =
        (unsigned)(first * LS_OV_COLUMNS / o->header.frames);
    unsigned last_column =
        (unsigned)((first + got - 1) * LS_OV_COLUMNS / o->header.frames);
    if (last_column >= LS_OV_COLUMNS)
      last_column = LS_OV_COLUMNS - 1;
    unsigned n = ls_audio_analysis_bins(o->analysis), fft = (n - 1) * 2;
    float top = fminf(20000, o->header.rate * .5f);
    for (unsigned k = 0; k < LS_OV_BINS; k++) {
      float hz = 40 * powf(top / 40, (float)k / (LS_OV_BINS - 1));
      unsigned bin = (unsigned)(hz * fft / o->header.rate);
      float db = ls_audio_analysis_bin_db(o->analysis, bin, false);
      int code = (int)((db + 120) * 255 / 120);
      if (code < 0)
        code = 0;
      if (code > 255)
        code = 255;
      for (unsigned column = first_column; column <= last_column; column++) {
        unsigned index = column * LS_OV_BINS + k,
                 word = atomic_load(&o->spec[index / 4]),
                 shift = (index % 4) * 8;
        if ((unsigned)code > ((word >> shift) & 255))
          atomic_store(&o->spec[index / 4],
                       (word & ~(255u << shift)) | ((unsigned)code << shift));
      }
    }
  }
  unsigned progress = (unsigned)((first + got) * 99 / o->header.frames);
  atomic_store(&o->progress, progress);
}
#ifdef ESP_PLATFORM
static void worker(void *ctx) {
  ls_audio_overview *o = ctx;
  while (!atomic_load(&o->quit)) {
    ls_audio_overview_step(o, 2048);
    ls_ov_service_fn service = atomic_load(&o->service);
    if (service)
      service(atomic_load(&o->service_user));
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  release_source(o);
  atomic_store(&o->parked, true);
  vTaskSuspend(NULL);
}
#endif
ls_audio_overview *ls_audio_overview_new(const char *root) {
  if (!root || strlen(root) >= 256)
    return NULL;
  ls_audio_overview *o = OV_ALLOC(sizeof(*o));
  if (!o)
    return NULL;
  snprintf(o->root, sizeof(o->root), "%s", root);
  o->analysis = ls_audio_analysis_new();
  if (!o->analysis) {
    OV_FREE(o);
    return NULL;
  }
  ls_aa_config c = ls_audio_analysis_defaults();
  c.fft_size = 1024;
  c.hop = 512;
  ls_audio_analysis_configure(o->analysis, &c);
#ifdef ESP_PLATFORM
  if (xTaskCreatePinnedToCoreWithCaps(
          worker, "music_overview", 32768, o, 2, &o->task, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ls_audio_analysis_free(o->analysis);
    OV_FREE(o);
    return NULL;
  }
#endif
  return o;
}
void ls_audio_overview_set_service(ls_audio_overview *o, ls_ov_service_fn fn,
                                   void *user) {
  if (o) {
    atomic_store(&o->service_user, user);
    atomic_store(&o->service, fn);
  }
}
bool ls_audio_overview_free(ls_audio_overview *o) {
  if (!o)
    return true;
  atomic_store(&o->quit, true);
#ifdef ESP_PLATFORM
  for (int i = 0; i < 100 && (!atomic_load(&o->parked) ||
                              eTaskGetState(o->task) != eSuspended);
       i++)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (!atomic_load(&o->parked) || eTaskGetState(o->task) != eSuspended)
    return false;
  vTaskDeleteWithCaps(o->task);
#endif
  release_source(o);
  OV_FREE(atomic_exchange(&o->request, NULL));
  ls_audio_analysis_free(o->analysis);
  OV_FREE(o);
  return true;
}
bool ls_audio_overview_request(ls_audio_overview *o, const char *path,
                               bool refresh) {
  if (!o || !path || strlen(path) >= 512 || atomic_load(&o->quit))
    return false;
  ov_request *copy = OV_ALLOC(sizeof(*copy));
  if (!copy)
    return false;
  strcpy(copy->path, path);
  copy->refresh = refresh;
  copy->generation = atomic_fetch_add(&o->generation, 1) + 1;
  atomic_store(&o->ready, false);
  atomic_store(&o->slice_ready, false);
  atomic_store(&o->slice_pending, false);
  atomic_store(&o->progress, 0);
  OV_FREE(atomic_exchange(&o->request, copy));
  return true;
}
unsigned ls_audio_overview_progress(const ls_audio_overview *o) {
  return o ? atomic_load(&o->progress) : 0;
}
bool ls_audio_overview_ready(const ls_audio_overview *o) {
  return o && atomic_load(&o->ready);
}
bool ls_audio_overview_cached(const ls_audio_overview *o) {
  return o && atomic_load(&o->cached);
}
unsigned ls_audio_overview_rate(const ls_audio_overview *o) {
  return o ? atomic_load(&o->rate) : 0;
}
uint32_t ls_audio_overview_duration_ms(const ls_audio_overview *o) {
  if (!o)
    return 0;
  return atomic_load(&o->duration_ms);
}
const char *ls_audio_overview_error(const ls_audio_overview *o) {
  unsigned error = o ? atomic_load(&o->error) : 0;
  return error == 1   ? "File unavailable"
         : error == 2 ? "Overview decode failed"
         : error == 3 ? "Cache save failed"
                      : "";
}
bool ls_audio_overview_wave(const ls_audio_overview *o, unsigned i, int16_t *lo,
                            int16_t *hi) {
  if (!ls_audio_overview_ready(o) || i >= LS_OV_WAVE || !lo || !hi)
    return false;
  unsigned word = atomic_load(&o->wave[i]);
  *lo = (int16_t)(word & 65535);
  *hi = (int16_t)(word >> 16);
  return *lo <= *hi;
}
uint8_t ls_audio_overview_spectro(const ls_audio_overview *o, unsigned column,
                                  unsigned bin) {
  if (!ls_audio_overview_ready(o) || column >= LS_OV_COLUMNS ||
      bin >= LS_OV_BINS)
    return 0;
  unsigned i = column * LS_OV_BINS + bin;
  return (uint8_t)(atomic_load(&o->spec[i / 4]) >> ((i % 4) * 8));
}
bool ls_audio_overview_slice_request(ls_audio_overview *o, uint32_t ms,
                                     unsigned n, unsigned window) {
  if (!o || n < 256 || n > 4096 || (n & (n - 1)) || window > 1)
    return false;
  uint32_t duration = ls_audio_overview_duration_ms(o);
  if (duration && ms > duration)
    ms = duration;
  atomic_store(&o->slice_ms, ms);
  atomic_store(&o->slice_config, (n << 1) | window);
  atomic_store(&o->slice_ready, false);
  atomic_store(&o->slice_pending, true);
  return true;
}
bool ls_audio_overview_slice_ready(const ls_audio_overview *o, uint32_t *ms,
                                   unsigned *bins) {
  if (!o || !atomic_load(&o->slice_ready))
    return false;
  if (ms)
    *ms = atomic_load(&o->slice_at);
  if (bins)
    *bins = atomic_load(&o->slice_bins);
  return true;
}
float ls_audio_overview_slice_db(const ls_audio_overview *o, unsigned bin) {
  if (!o || !atomic_load(&o->slice_ready) || bin >= atomic_load(&o->slice_bins))
    return -120;
  unsigned word = atomic_load(&o->slice[bin]);
  float db;
  memcpy(&db, &word, 4);
  return db;
}
