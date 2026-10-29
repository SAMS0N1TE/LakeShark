/* DIGI's audio sink for host targets that build the FM app without the DIGI
   engine: never wanted, so app_fm.c's HF path pushes nothing. */
#include "digi_engine.h"

bool digi_sink_wanted(void) { return false; }
void digi_sink_push(const int16_t *pcm, int n, int rate_hz, uint64_t dial_hz)
{
    (void)pcm; (void)n; (void)rate_hz; (void)dial_hz;
}
