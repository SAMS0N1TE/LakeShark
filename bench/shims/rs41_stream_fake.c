/* Bench experiments without a stream source receive no RS41 bytes. */
#include "ls_lora.h"
int ls_lora_fsk_stream_read(uint8_t *buf, size_t n, bool *restarted)
{
    (void)buf; (void)n; if (restarted) *restarted = false; return 0;
}
