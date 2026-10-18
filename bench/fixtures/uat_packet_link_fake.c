/* Legacy PACKET-only bench has no stream source. Keep its existing test intact. */
#include "ls_lora.h"
uint32_t ls_lora_fsk_bw_snap(uint32_t hz) { return hz; }
int ls_lora_fsk_stream_read(uint8_t *b, size_t n, bool *restart)
{ (void)b; (void)n; *restart = false; return 0; }
