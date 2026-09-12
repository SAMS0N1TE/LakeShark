#include "ls_dbg.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>

#define LS_DBG_DIR "/ext/apps_data/lakeshark_p25"

void ls_dbg(const char* fmt, ...) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, LS_DBG_DIR);

    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, LS_DBG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FuriString* s = furi_string_alloc();
        va_list ap;
        va_start(ap, fmt);
        furi_string_vprintf(s, fmt, ap);
        va_end(ap);
        furi_string_cat_printf(s, "\n");
        stream_write_string(stream, s);
        furi_string_free(s);
    }

    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}
