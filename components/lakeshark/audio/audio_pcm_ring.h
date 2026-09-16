#ifndef AUDIO_PCM_RING_H
#define AUDIO_PCM_RING_H
#include <stddef.h>

/* Static FreeRTOS stream buffers reserve one byte inside the supplied
 * storage length. Keep their usable capacity a whole number of PCM16 samples. */
static inline size_t audio_pcm_storage_bytes(size_t payload_bytes)
{
    return (payload_bytes & ~(size_t)1) + 1;
}

static inline size_t audio_pcm_write_bytes(size_t requested, size_t available)
{
    return (requested < available ? requested : available) & ~(size_t)1;
}
#endif
