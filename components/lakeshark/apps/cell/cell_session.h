/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_CELL_SESSION_H
#define LS_CELL_SESSION_H
#include <stdbool.h>
#include <stdint.h>
/* Capture policy is independent of the RTOS and receiver for host replay. */
#define CELL_SESSION_RAW_LIMIT (UINT64_C(256) * 1024 * 1024)
typedef struct {
    bool running, multi;
    unsigned channel, attempts, decoded, failures;
    uint64_t raw_bytes;
    uint32_t wait_ms;
} cell_session_t;
void cell_session_begin(cell_session_t *s, bool multi);
bool cell_session_keep_raw(const cell_session_t *s, uint32_t bytes);
void cell_session_finish(cell_session_t *s, bool complete, bool mib,
                         uint32_t saved_bytes);
#endif
