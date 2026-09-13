#ifndef LS_SCAN_IMPORT_H
#define LS_SCAN_IMPORT_H
#include "scan_channels.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
bool scan_import_line(const char *line, scan_channel_t *out);
bool scan_import_file(const char *path, char *message, size_t capacity);
void scan_import_begin(void);
void scan_import_abort(void);
bool scan_import_add(const char *line);
bool scan_import_commit(char *message, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
