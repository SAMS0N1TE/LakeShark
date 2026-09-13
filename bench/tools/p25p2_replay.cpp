#include "p25_phase2.h"
#include <cstdio>
#include <cstdlib>
static void audio(const int16_t *pcm, size_t n, void *ctx) {
  fwrite(pcm, sizeof(*pcm), n, (FILE *)ctx);
}
int main(int argc, char **argv) {
  if (argc != 7) {
    fprintf(stderr, "usage: p25p2_replay symbols.bin output.s16 wacn_hex "
                    "sys_hex nac_hex slot\n");
    return 2;
  }
  FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
  if (!in || !out)
    return 2;
  p25p2_decoder_t *d = p25p2_create(audio, out);
  if (!p25p2_configure(
          d, strtoul(argv[3], nullptr, 16), strtoul(argv[4], nullptr, 16),
          strtoul(argv[5], nullptr, 16), strtoul(argv[6], nullptr, 10)))
    return 2;
  uint8_t bytes[4096];
  size_t n;
  while ((n = fread(bytes, 1, sizeof(bytes), in)))
    p25p2_push(d, bytes, n);
  p25p2_status_t s;
  p25p2_status(d, &s);
  printf("symbols=%u bursts=%u control_ok=%u control_errors=%u voice=%u "
         "audio=%u muted=%u TG=%u SRC=%u ALG=%02x\n",
         s.symbols, s.bursts, s.control_ok, s.control_errors, s.voice_frames,
         s.audio_frames, s.muted_frames, s.talkgroup, s.source, s.algorithm);
  p25p2_destroy(d);
  fclose(in);
  fclose(out);
  return s.audio_frames ? 0 : 1;
}
