/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cell_session.h"
#include <string.h>
void cell_session_begin(cell_session_t *s, bool multi)
{
    memset(s, 0, sizeof(*s));
    s->running = true;
    s->multi = multi;
}
bool cell_session_keep_raw(const cell_session_t *s, uint32_t bytes)
{
    return bytes && s->raw_bytes <= CELL_SESSION_RAW_LIMIT &&
           bytes <= CELL_SESSION_RAW_LIMIT - s->raw_bytes;
}
void cell_session_finish(cell_session_t *s, bool complete, bool mib,
                         uint32_t saved_bytes)
{
    s->attempts++;
    if (complete && mib) s->decoded++;
    s->failures = complete ? 0 : s->failures + (s->failures < 6);
    s->raw_bytes += saved_bytes;
    s->wait_ms = complete ? 1000 : 1000u << s->failures;
    if (s->wait_ms > 30000) s->wait_ms = 30000;
    if (s->multi) s->channel = (s->channel + 1) % 6;
}
