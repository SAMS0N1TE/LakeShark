// Phase II burst/voice handling adapted from OP25 p25p2_tdma.cc and
// tdma/lfsr.py. Copyright 2013, 2014, 2021 Max H. Parke KA1RBI; 2017-2025
// Graham J. Norbury. SPDX-License-Identifier: GPL-3.0-or-later
#include "p25_phase2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include "upstream/ambe.h"
#include "upstream/crc16.h"
#include "upstream/ezpwd/rs"
#include "upstream/p25p2_duid.h"
#include "upstream/p25p2_framer.h"
#include "upstream/p25p2_sync.h"
#include "upstream/p25p2_vf.h"

class decoder {
public:
  p25p2_framer framer;
  p25p2_sync sync;
  p25p2_duid duid;
  p25p2_vf vf;
  ezpwd::RS<63, 35> rs28;
  ls_p2_mbe_parms cur{}, prev{}, enhanced{};
  ls_p2_mbe_errs errors{};
  p25p2_status_t stats{};
  p25p2_audio_fn output;
  void *context;
  std::array<uint8_t, 2160> mask{};
  std::vector<uint8_t> ess_b = std::vector<uint8_t>(16, 0);
  std::vector<uint8_t> ess_a = std::vector<uint8_t>(28, 0);
  int voice_burst = -1;
  unsigned ess_parts = 0;
  uint32_t last_metadata = 0;
  bool configured = false;
  int next_algorithm = -1;
  uint8_t reverse = 0;      /* XORed into every dibit: 2 while polarity is reversed */
  explicit decoder(p25p2_audio_fn fn, void *ctx) : output(fn), context(ctx) {
    reset();
  }
  void reset() {
    unsigned slot = stats.slot;
    stats = {};
    stats.slot = slot;
    /* Polarity belongs to the receive chain, not the call: it survives a
       reset, as it does in OP25. */
    stats.polarity_reversed = reverse != 0;
    stats.algorithm = 0xff;
    framer = p25p2_framer();
    sync = p25p2_sync();
    voice_burst = -1;
    ess_parts = 0;
    last_metadata = 0;
    next_algorithm = -1;
    ls_p2_mbe_initMbeParms(&cur, &prev, &enhanced);
    ls_p2_mbe_initErrParms(&errors);
  }
  int handle_acch_frame(const uint8_t *, bool, bool);
  int process_mac_pdu(const uint8_t *bytes, unsigned length, int) {
    stats.control_ok++;
    unsigned opcode = bytes[0] >> 5;
    if (opcode == 1 && length >= 18) {
      stats.algorithm = bytes[10];
      stats.clear_confirmed = bytes[10] == 0x80;
      stats.source = (bytes[13] << 16) | (bytes[14] << 8) | bytes[15];
      stats.talkgroup = (bytes[16] << 8) | bytes[17];
      last_metadata = stats.symbols;
      voice_burst = -1;
      ess_parts = 0;
      ls_p2_mbe_initMbeParms(&cur, &prev, &enhanced);
      ls_p2_mbe_initErrParms(&errors);
    } else if (opcode == 2 || opcode == 3) {
      stats.clear_confirmed = false;
      stats.algorithm = 0xff;
      ess_parts = 0;
    }
    return (int)opcode;
  }
  void ess(const uint8_t *dibits, int type) {
    voice_burst = type == 6 ? 4 : (voice_burst + 1) % 5;
    if (voice_burst < 4) {
      if (voice_burst == 0)
        ess_parts = 0;
      for (int i = 0; i < 4; i++)
        ess_b[voice_burst * 4 + i] =
            (dibits[i * 3] << 4) | (dibits[i * 3 + 1] << 2) | dibits[i * 3 + 2];
      ess_parts |= 1u << voice_burst;
    } else {
      next_algorithm = -1;
      for (int i = 0, j = 0; i < 28; i++) {
        ess_a[i] = (dibits[j] << 4) | (dibits[j + 1] << 2) | dibits[j + 2];
        j += i == 15 ? 4 : 3;
      }
      if (ess_parts == 15) {
        int rc = rs28.decode(ess_b, ess_a);
        if (rc >= 0 && rc <= 14) {
          next_algorithm = (ess_b[0] << 2) | (ess_b[1] >> 4);
          if (next_algorithm != 0x80)
            stats.clear_confirmed = false;
        } else
          stats.clear_confirmed = false;
      }
      ess_parts = 0;
    }
  }
  void voice(const uint8_t *dibits) {
    stats.voice_frames++;
    int b[9], u[4];
    vf.process_vcw(&errors, dibits, b, u);
#ifdef LS_P25P2_TRACE
    packed_codeword packed;
    vf.pack_cw(packed, u);
    fprintf(stderr, "AMBE ");
    for (uint8_t v : packed)
      fprintf(stderr, "%02X", v);
    fprintf(stderr, "\n");
#endif
    int16_t pcm[160] = {};
    bool clear = stats.clear_confirmed && stats.symbols - last_metadata < 12000;
    int rc =
        clear ? ls_p2_mbe_dequantizeAmbe2250Parms(&cur, &prev, &errors, b) : -1;
    if (rc == 0 && errors.ER <= 0.096f) {
      float samples[160];
      ls_p2_mbe_spectralAmpEnhance(&cur);
      ls_p2_mbe_synthesizeSpeechf(samples, &cur, &prev, 3);
      for (int i = 0; i < 160; i++)
        pcm[i] =
            std::isfinite(samples[i])
                ? (int16_t)std::max(-32767.0f, std::min(32767.0f, samples[i]))
                : 0;
      ls_p2_mbe_moveMbeParms(&cur, &prev);
      stats.audio_frames++;
    } else
      stats.muted_frames++;
    if (output)
      output(pcm, 160, context);
  }
  void packet(uint8_t *dibits) {
    static const unsigned slots[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0};
    stats.bursts++;
    sync.check_confidence(dibits);
    int index = sync.tdma_slotid();
    stats.synchronized = sync.in_sync() && index >= 0 && index < 12;
    if (!stats.synchronized) {
      stats.clear_confirmed = false;
      ess_parts = 0;
      return;
    }
    if (slots[index] != stats.slot)
      return;
    const uint8_t *burst = dibits + 10;
    int type = duid.duid_lookup(duid.extract_duid(burst));
    uint8_t plain[170];
    for (int i = 0; i < 170; i++)
      plain[i] = burst[i] ^ mask[index * 180 + i];
    if (type == 0 || type == 6) {
      ess(plain + 84, type);
      voice(plain + 11);
      voice(plain + 48);
      if (type == 0) {
        voice(plain + 96);
        voice(plain + 133);
      } else if (next_algorithm >= 0) {
        stats.algorithm = (uint8_t)next_algorithm;
        stats.clear_confirmed = next_algorithm == 0x80;
        last_metadata = stats.symbols;
      }
    } else if (type == 3 || type == 9 || type == 12 || type == 13 ||
               type == 15) {
      handle_acch_frame(type == 3 || type == 9 ? plain : burst,
                        type == 9 || type == 15, type == 13);
    }
  }
};
#include "acch.inc"
struct p25p2_decoder {
  decoder core;
  p25p2_decoder(p25p2_audio_fn f, void *c) : core(f, c) {}
};
extern "C" p25p2_decoder_t *p25p2_create(p25p2_audio_fn fn, void *ctx) {
#ifdef ESP_PLATFORM
  void *storage = heap_caps_malloc(sizeof(p25p2_decoder),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return storage ? new (storage) p25p2_decoder(fn, ctx) : nullptr;
#else
  return new (std::nothrow) p25p2_decoder(fn, ctx);
#endif
}
extern "C" void p25p2_destroy(p25p2_decoder_t *d) {
#ifdef ESP_PLATFORM
  if (d) {
    d->~p25p2_decoder();
    heap_caps_free(d);
  }
#else
  delete d;
#endif
}
extern "C" void p25p2_reset(p25p2_decoder_t *d) {
  if (d)
    d->core.reset();
}
extern "C" bool p25p2_configure(p25p2_decoder_t *d, uint32_t wacn,
                                uint16_t system, uint16_t nac, unsigned slot) {
  if (!d || wacn > 0xfffff || system > 0xfff || nac > 0xfff || slot > 1)
    return false;
  static const uint64_t masks[44] = {
#include "lfsr_masks.inc"
  };
  uint64_t seed = ((uint64_t)wacn << 24) | ((uint64_t)system << 12) | nac,
           reg = 0;
  for (uint64_t m : masks)
    reg = (reg << 1) | (__builtin_parityll(seed & m) & 1);
  for (int i = 0; i < 4320; i++) {
    int bit = (reg >> 43) & 1;
    if ((i & 1) == 0)
      d->core.mask[i / 2] = bit << 1;
    else
      d->core.mask[i / 2] |= bit;
    int cy2 = (reg >> 39) & 1, cy3 = (reg >> 34) & 1, cy4 = (reg >> 28) & 1,
        cy5 = (reg >> 23) & 1, cy6 = (reg >> 9) & 1;
    reg = ((reg << 1) & 0xfffffffffffULL) &
          ~((1ULL << 40) | (1ULL << 35) | (1ULL << 29) | (1ULL << 24) |
            (1ULL << 10) | 1ULL);
    reg |= ((uint64_t)(bit ^ cy2) << 40) | ((uint64_t)(bit ^ cy3) << 35) |
           ((uint64_t)(bit ^ cy4) << 29) | ((uint64_t)(bit ^ cy5) << 24) |
           ((uint64_t)(bit ^ cy6) << 10) | bit;
  }
  d->core.reset();
  d->core.stats.slot = slot;
  d->core.configured = true;
  return true;
}
extern "C" void p25p2_push(p25p2_decoder_t *d, const uint8_t *dibits,
                           size_t count) {
  if (!d || !dibits || !d->core.configured)
    return;
  for (size_t i = 0; i < count; i++) {
    if (dibits[i] > 3) {
      d->core.reset();
      continue;
    }
    d->core.stats.symbols++;
    if (d->core.framer.rx_sym(dibits[i] ^ d->core.reverse)) {
      uint8_t packet[180];
      for (int j = 0; j < 180; j++)
        packet[j] = (d->core.framer.d_frame_body[j * 2] << 1) |
                    d->core.framer.d_frame_body[j * 2 + 1];
      const uint64_t fs = d->core.framer.get_fs();
      if (fs == P25P2_FRAME_SYNC_REV_P) {
        /* OP25's rx_sync answers a reversed sync by inverting everything
           that follows. The framer matched the reversed pattern and kept the
           burst as received, and the reversed pattern is the normal one with
           every dibit's high bit flipped - so flipping all 180 gives the
           burst a correctly polarised receiver would have framed. This was
           ignored: an inverted stream framed its bursts and decoded none
           (the public capture inverted: 1263 bursts, 0 voice frames). */
        for (int j = 0; j < 180; j++)
          packet[j] ^= 2;
        d->core.reverse ^= 2;
        d->core.stats.polarity_flips++;
        d->core.stats.polarity_reversed = d->core.reverse != 0;
      } else if (fs != P25P2_FRAME_SYNC_MAGIC) {
        d->core.stats.mistuned_syncs++;
      }
      d->core.packet(packet);
    }
  }
}
extern "C" void p25p2_status(const p25p2_decoder_t *d, p25p2_status_t *out) {
  if (out)
    *out = d ? d->core.stats : p25p2_status_t{};
}
