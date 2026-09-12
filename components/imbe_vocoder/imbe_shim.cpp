/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original glue around the OP25 IMBE vocoder, which is
   GPL-3.0-or-later. See COPYRIGHT and UPSTREAM.md in this directory. */
/* imbe_shim.cpp - C wrapper around OP25's imbe_vocoder (decode path). */
#include "imbe_vocoder_impl.h"
#include "imbe_shim.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <new>
#include <cstring>

/* One decoder instance reused across frames (keeps inter-frame state). */

static imbe_vocoder_impl *g_voc = nullptr;
static bool allocation_reported = false;

static bool ensure_decoder(void)
{
    if (g_voc) return true;
    void *storage = heap_caps_malloc(sizeof(imbe_vocoder_impl),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) {
        if (!allocation_reported)
            ESP_LOGE("imbe", "decoder PSRAM allocation failed; voice unavailable");
        allocation_reported = true;
        return false;
    }
    /* Constructor has no nested allocations. Reuse state across frames. */
    g_voc = new (storage) imbe_vocoder_impl();
    allocation_reported = false;
    return true;
}

extern "C" void imbe_shim_init(void)
{
    (void)ensure_decoder();
}

extern "C" void imbe_shim_decode_88(const uint8_t *imbe88, int16_t *snd160)
{
    (void)imbe_shim_try_decode_88(imbe88, snd160);
}

extern "C" bool imbe_shim_try_decode_88(const uint8_t *imbe88, int16_t *snd160)
{
    if (!snd160) return false;
    std::memset(snd160, 0, 160 * sizeof(*snd160));
    if (!imbe88 || !ensure_decoder()) return false;
    /* decode_4400 takes (snd, imbe); imbe is read MSB-first as 88 bits. */
    g_voc->decode_4400(snd160, const_cast<uint8_t *>(imbe88));
    return true;
}
