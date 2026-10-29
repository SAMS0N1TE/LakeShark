/* Streaming format bridge. Vendor revisions and read-only table patches
   are recorded in NOTICE.md. No decoder runs on a screen callback. */
#include "ls_decode.h"
#include "../audio_wav.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *codec_malloc(size_t n) {
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void *codec_realloc(void *p, size_t n) {
  return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void codec_free(void *p) { heap_caps_free(p); }
#else
static void *codec_malloc(size_t n) { return malloc(n); }
static void *codec_realloc(void *p, size_t n) { return realloc(p, n); }
static void codec_free(void *p) { free(p); }
#endif
/* Redirect every vendor heap allocation. Vorbis additionally uses a bounded
   arena, including frame scratch, instead of alloca on an internal stack. */
#define malloc codec_malloc
#define realloc codec_realloc
#define free codec_free
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_NO_STDIO
#define MINIMP3_SCAN_LIMIT (256 * 1024)
#include "vendor/minimp3_ex.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_STDIO
#include "vendor/dr_flac.h"
#define STB_VORBIS_NO_PUSHDATA_API
#define get_bits stb_vorbis_get_bits
#include "vendor/stb_vorbis.c"
#undef get_bits
#undef malloc
#undef realloc
#undef free

enum { F_WAV, F_MP3, F_FLAC, F_VORBIS };
struct ls_decode {
  FILE *file;
  unsigned kind, rate, channels;
  uint64_t frames, position;
  bool failed;
  wav_instance wav;
  mp3dec_ex_t *mp3;
  mp3dec_io_t io;
  drflac *flac;
  stb_vorbis *vorbis;
  void *arena;
  char *file_buffer;
};
static size_t mp3_read(void *out, size_t n, void *ctx) {
  return fread(out, 1, n, ctx);
}
static int mp3_seek(uint64_t at, void *ctx) {
  return at > LONG_MAX ? -1 : fseek(ctx, (long)at, SEEK_SET);
}
static size_t flac_read(void *ctx, void *out, size_t n) {
  return fread(out, 1, n, ctx);
}
static drflac_bool32 flac_seek(void *ctx, int offset,
                               drflac_seek_origin origin) {
  int whence = origin == DRFLAC_SEEK_SET   ? SEEK_SET
               : origin == DRFLAC_SEEK_END ? SEEK_END
                                           : SEEK_CUR;
  return fseek(ctx, offset, whence) == 0;
}
static drflac_bool32 flac_tell(void *ctx, drflac_int64 *at) {
  long n = ftell(ctx);
  *at = n;
  return n >= 0;
}
ls_decode *ls_decode_open(FILE *file) {
  if (!file)
    return NULL;
  ls_decode *d = codec_malloc(sizeof(*d));
  if (!d)
    return NULL;
  memset(d, 0, sizeof(*d));
  d->file = file;
  d->file_buffer = codec_malloc(4096);
  if (!d->file_buffer || setvbuf(file, d->file_buffer, _IOFBF, 4096) ||
      fseek(file, 0, SEEK_SET))
    goto fail;
  unsigned char magic[12] = {0};
  size_t n = fread(magic, 1, sizeof(magic), file);
  rewind(file);
  if (n < 12)
    goto fail;
  if (!memcmp(magic, "RIFF", 4)) {
    d->kind = F_WAV;
    if (!is_wav(file, &d->wav))
      goto fail;
    d->rate = d->wav.header.SampleRate;
    d->channels = d->wav.header.NumChannels;
    d->frames = d->wav.data_bytes / d->wav.header.BlockAlign;
  } else if (!memcmp(magic, "fLaC", 4)) {
    d->kind = F_FLAC;
    d->flac = drflac_open(flac_read, flac_seek, flac_tell, file, NULL);
    if (!d->flac)
      goto fail;
    d->rate = d->flac->sampleRate;
    d->channels = d->flac->channels;
    d->frames = d->flac->totalPCMFrameCount;
  } else if (!memcmp(magic, "OggS", 4)) {
    d->kind = F_VORBIS;
    d->arena = codec_malloc(768 * 1024);
    if (!d->arena)
      goto fail;
    stb_vorbis_alloc arena = {d->arena, 768 * 1024};
    int error = 0;
    d->vorbis = stb_vorbis_open_file(file, 0, &error, &arena);
    if (!d->vorbis)
      goto fail;
    stb_vorbis_info info = stb_vorbis_get_info(d->vorbis);
    d->rate = info.sample_rate;
    d->channels = info.channels;
    d->frames = stb_vorbis_stream_length_in_samples(d->vorbis);
  } else if (!memcmp(magic, "ID3", 3) ||
             (magic[0] == 255 && (magic[1] & 224) == 224)) {
    d->kind = F_MP3;
    d->mp3 = codec_malloc(sizeof(*d->mp3));
    if (!d->mp3)
      goto fail;
    memset(d->mp3, 0, sizeof(*d->mp3));
    d->io = (mp3dec_io_t){mp3_read, file, mp3_seek, file};
    if (mp3dec_ex_open_cb(d->mp3, &d->io, MP3D_SEEK_TO_SAMPLE))
      goto fail;
    d->rate = d->mp3->info.hz;
    d->channels = d->mp3->info.channels;
    if (!d->channels)
      goto fail;
    d->frames = d->mp3->samples / d->channels;
  } else
    goto fail;
  if (d->channels < 1 || d->channels > 2 || d->rate < 8000 ||
      d->rate > 192000 || !d->frames)
    goto fail;
  return d;
fail:
  ls_decode_close(d);
  return NULL;
}
void ls_decode_close(ls_decode *d) {
  if (!d)
    return;
  if (d->mp3) {
    mp3dec_ex_close(d->mp3);
    codec_free(d->mp3);
  }
  if (d->flac)
    drflac_close(d->flac);
  if (d->vorbis)
    stb_vorbis_close(d->vorbis);
  codec_free(d->arena);
  if (d->file_buffer) {
    setvbuf(d->file, NULL, _IONBF, 0);
    codec_free(d->file_buffer);
  }
  codec_free(d);
}
unsigned ls_decode_rate(const ls_decode *d) { return d ? d->rate : 0; }
unsigned ls_decode_channels(const ls_decode *d) { return d ? d->channels : 0; }
uint64_t ls_decode_frames(const ls_decode *d) { return d ? d->frames : 0; }
uint64_t ls_decode_position(const ls_decode *d) { return d ? d->position : 0; }
const char *ls_decode_kind(const ls_decode *d) {
  static const char *const names[] = {"WAV", "MP3", "FLAC", "OGG"};
  return d ? names[d->kind] : "";
}
bool ls_decode_failed(const ls_decode *d) { return d && d->failed; }
size_t ls_decode_read(ls_decode *d, int16_t *pcm, size_t count) {
  if (!d || !pcm || !count || count > INT_MAX / 2 || d->failed ||
      d->position >= d->frames)
    return 0;
  if (count > d->frames - d->position)
    count = (size_t)(d->frames - d->position);
  size_t got = 0;
  switch (d->kind) {
  case F_WAV:
    got = fread(pcm, d->channels * sizeof(*pcm), count, d->file);
    d->failed = got < count;
    break;
  case F_MP3:
    got = mp3dec_ex_read(d->mp3, pcm, count * d->channels) / d->channels;
    d->failed = d->mp3->last_error != 0;
    break;
  case F_FLAC:
    got = (size_t)drflac_read_pcm_frames_s16(d->flac, count, pcm);
    break;
  case F_VORBIS:
    got = (size_t)stb_vorbis_get_samples_short_interleaved(
        d->vorbis, d->channels, pcm, (int)(count * d->channels));
    break;
  }
  if (!got && d->position < d->frames)
    d->failed = true;
  d->position += got;
  return got;
}
bool ls_decode_seek(ls_decode *d, uint64_t frame) {
  if (!d)
    return false;
  if (frame > d->frames)
    frame = d->frames;
  bool ok = false;
  if (frame == d->frames) {
    d->position = frame;
    d->failed = false;
    return true;
  }
  switch (d->kind) {
  case F_WAV: {
    uint64_t at = (uint64_t)d->wav.data_offset + frame * d->channels * 2;
    ok = at <= LONG_MAX && fseek(d->file, (long)at, SEEK_SET) == 0;
    break;
  }
  case F_MP3:
    ok = mp3dec_ex_seek(d->mp3, frame * d->channels) == 0;
    break;
  case F_FLAC:
    ok = drflac_seek_to_pcm_frame(d->flac, frame) != 0;
    break;
  case F_VORBIS:
    ok = frame <= UINT_MAX && stb_vorbis_seek(d->vorbis, (unsigned)frame) != 0;
    break;
  }
  if (ok) {
    d->position = frame;
    d->failed = false;
  }
  return ok;
}
