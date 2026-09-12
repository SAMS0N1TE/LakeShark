#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define LS_DBG_PATH "/ext/apps_data/lakeshark_p25/debug.txt"

void ls_dbg(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
