/* imbe_shim.cpp - C wrapper around OP25's imbe_vocoder (decode path). */
#include "imbe.h"           /* defines IMBE_PARAM, used by imbe_vocoder.h */
#include "imbe_vocoder.h"
#include "imbe_shim.h"

/* One decoder instance reused across frames (keeps inter-frame state). */
static imbe_vocoder *g_voc = nullptr;

extern "C" void imbe_shim_init(void)
{
    if (!g_voc) g_voc = new imbe_vocoder();
}

extern "C" void imbe_shim_decode_88(const uint8_t *imbe88, int16_t *snd160)
{
    if (!g_voc) g_voc = new imbe_vocoder();
    /* decode_4400 takes (snd, imbe); imbe is read MSB-first as 88 bits. */
    g_voc->decode_4400(snd160, const_cast<uint8_t *>(imbe88));
}
