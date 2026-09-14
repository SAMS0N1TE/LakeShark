#ifndef LS_SCAN_SD_STORE_H
#define LS_SCAN_SD_STORE_H
#include "scan_channels.h"
bool scan_sd_available(void);
bool scan_sd_name(const char *name);
bool scan_sd_write(const char *name, const scan_channel_t *rows, int count);
int scan_sd_read(const char *name, scan_channel_t *rows);
void scan_sd_list(void);
#endif
