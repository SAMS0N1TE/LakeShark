/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_COVERAGE_EXPORT_H
#define LS_COVERAGE_EXPORT_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_COVERAGE_LINE_BYTES 3072u
#define LS_COVERAGE_RECEIPT_BYTES 65536u
#define LS_COVERAGE_RECORD_LIMIT 33u
/* Bounded PSRAM metadata copy; no SD, tuner, receiver start, or background task.
 * RECEIVING describes software read/DSP windows, never physical RF coverage. */
typedef bool (*ls_coverage_write_t)(void *context, const char *line, size_t length);
bool ls_coverage_export(ls_coverage_write_t write, void *context);
size_t ls_coverage_export_workspace_bytes(void);
int ls_coverage_command(int argc, char **argv);
#ifdef __cplusplus
}
#endif
#endif
