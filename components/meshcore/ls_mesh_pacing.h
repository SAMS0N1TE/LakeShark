/* Mesh listener polling budget. No hardware access or scheduler dependency. */
#pragma once
#include <stdint.h>

/* Leave eight programmed preamble symbols for reacquisition after draining
 * RX_DONE (SX126x reissues SetRx). Do not count the 4.25-symbol LoRa tail.
 * Both parts listen continuously; this bounds servicing, not RF sampling.
 * The 100 ms ceiling also bounds maintenance/recovery latency. */
static inline uint32_t ls_mesh_poll_ms(uint8_t sf, uint32_t bw, uint16_t preamble)
{
    if (sf < 5 || sf > 12 || !bw || preamble <= 8) return 1;
    uint64_t us = ((uint64_t)(preamble - 8) * (1u << sf) * 1000000u) / bw;
    /* Reserve two ms for bus service; scheduling contention is additional. */
    uint32_t ms = us > 2000 ? (uint32_t)((us - 2000) / 1000) : 1;
    return ms < 1 ? 1 : ms > 100 ? 100 : ms;
}
