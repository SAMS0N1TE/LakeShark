/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_OBSERVATION_EXPORT_H
#define LS_OBSERVATION_EXPORT_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_OBSERVATION_DETECTOR_LIMIT 4u
#define LS_OBSERVATION_CALL_LIMIT 8u
#define LS_OBSERVATION_LINE_BYTES 768u
/* One bounded JSONL line per callback; false aborts without waiting/retrying.
 * A receipt joins no records and is not an exhaustive SD inventory or RF
 * coverage measurement. Native paths/identifiers are retained; GPS, payload
 * text and stores outside scanner/detector/CALLS are deliberately omitted. */
typedef bool (*ls_observation_write_t)(void *context, const char *line, size_t n);
bool ls_observation_export(ls_observation_write_t write, void *context);
/* Explicit separate schema-2 mission receipt; never expands the v1 receipt. */
bool ls_observation_mission_export(ls_observation_write_t write, void *context);
size_t ls_observation_export_workspace_bytes(void);
int ls_observation_command(int argc, char **argv);
#ifdef __cplusplus
}
#endif
#endif
