#include "p25_phase2.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static void audio(const int16_t *pcm, size_t n, void *ctx) {
  fwrite(pcm, sizeof(*pcm), n, (FILE *)ctx);
}
int main(int argc, char **argv) {
  /* --timeline: every MAC PDU and voice run, timestamped */
  bool timeline = argc == 8 && std::strcmp(argv[7], "--timeline") == 0;
  if (argc != 7 && !timeline) {
    fprintf(stderr, "usage: p25p2_replay symbols.bin output.s16 wacn_hex "
                    "sys_hex nac_hex slot [--timeline]\n");
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
  p25p2_status_t was{};
  uint32_t run_start = 0, run_frames = 0;
  while ((n = fread(bytes, 1, sizeof(bytes), in))) {
    if (!timeline) {
      p25p2_push(d, bytes, n);
      continue;
    }
    for (size_t i = 0; i < n; i++) {
      p25p2_push(d, bytes + i, 1);
      p25p2_status_t now;
      p25p2_status(d, &now);
      if (now.voice_frames != was.voice_frames) {
        if (!run_frames || now.symbols - was.last_voice_symbol > 6000) {
          if (run_frames)
            printf("%9.3f s  voice run ends, %u frames from %.3f s\n",
                   was.last_voice_symbol / 6000.0, run_frames,
                   run_start / 6000.0);
          run_start = now.symbols;
          run_frames = 0;
        }
        run_frames += now.voice_frames - was.voice_frames;
      }
      if (now.control_ok != was.control_ok)
        printf("%9.3f s  MAC op=%u mco=%02X tg=%u alg=%02X active=%d\n",
               now.symbols / 6000.0, now.last_mac_opcode, now.last_mac_mco,
               now.talkgroup, now.algorithm, now.call_active ? 1 : 0);
      was = now;
    }
  }
  if (timeline && run_frames)
    printf("%9.3f s  voice run ends, %u frames from %.3f s\n",
           was.last_voice_symbol / 6000.0, run_frames, run_start / 6000.0);
  p25p2_status_t s;
  p25p2_status(d, &s);
  printf("symbols=%u bursts=%u control_ok=%u control_errors=%u voice=%u "
         "audio=%u muted=%u TG=%u SRC=%u ALG=%02x\n",
         s.symbols, s.bursts, s.control_ok, s.control_errors, s.voice_frames,
         s.audio_frames, s.muted_frames, s.talkgroup, s.source, s.algorithm);
  printf("mac SIGNAL=%u PTT=%u END_PTT=%u IDLE=%u ACTIVE=%u op5=%u "
         "HANGTIME=%u op7=%u\n",
         s.mac_opcodes[0], s.mac_opcodes[1], s.mac_opcodes[2],
         s.mac_opcodes[3], s.mac_opcodes[4], s.mac_opcodes[5],
         s.mac_opcodes[6], s.mac_opcodes[7]);
  printf("polarity_flips=%u reversed=%d mistuned_syncs=%u\n",
         s.polarity_flips, s.polarity_reversed ? 1 : 0, s.mistuned_syncs);
  p25p2_destroy(d);
  fclose(in);
  fclose(out);
  return s.audio_frames ? 0 : 1;
}
