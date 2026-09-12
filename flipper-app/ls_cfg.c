#include "ls_cfg.h"

#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <stdio.h>

const char* const LS_BOOT_NAMES[LsBootCount] = {
    "Launcher",
    "P25",
    "FM",
    "POCSAG",
    "ADS-B",
    "REC",
};

const char* const LS_ORIENT_NAMES[LsOrientCount] = {
    "Landscape",
    "Portrait L",
    "Portrait R",
};

void ls_cfg_mem_path(char* out, size_t len, const char* tag) {
    snprintf(out, len, LS_DATA_DIR "/ch_%s.txt", tag);
}

void ls_cfg_preset_path(char* out, size_t len, const char* tag) {
    snprintf(out, len, LS_PRESET_DIR "/%s.txt", tag);
}

void ls_cfg_defaults(LsCfg* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->boot_app = LsBootLauncher;
    cfg->orient = LsOrientLandscape;
    cfg->step_idx = 4;
    cfg->tel_hz = 5;
    cfg->want_ble = false;
    cfg->ble_proven = false;

    cfg->rx_wake = true;
}

void ls_cfg_load(LsCfg* cfg) {
    ls_cfg_defaults(cfg);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();

    if(file_stream_open(stream, LS_CFG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            const char* s = furi_string_get_cstr(line);
            int v;
            if(sscanf(s, "orient=%d", &v) == 1 && v >= 0 && v < LsOrientCount) {
                cfg->orient = (LsOrient)v;
            } else if(sscanf(s, "boot=%d", &v) == 1 && v >= 0 && v < LsBootCount) {
                cfg->boot_app = (LsBootApp)v;
            } else if(sscanf(s, "step=%d", &v) == 1 && v >= 0 && v < 16) {
                cfg->step_idx = v;
            } else if(sscanf(s, "tel=%d", &v) == 1 && v >= 1 && v <= 20) {
                cfg->tel_hz = v;
            } else if(sscanf(s, "rxwake=%d", &v) == 1) {
                cfg->rx_wake = (v != 0);
            } else if(sscanf(s, "ble=%d", &v) == 1) {

                cfg->want_ble = (v != 0);
            }
        }
    }

    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

void ls_cfg_save(const LsCfg* cfg) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, LS_DATA_DIR);

    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, LS_CFG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* s = furi_string_alloc_printf(
            "# LakeShark control head settings\n"
            "# boot: 0=launcher 1=P25 2=FM 3=POCSAG 4=ADS-B\n"
            "boot=%d\n"
            "orient=%d\n"
            "step=%d\n"
            "tel=%d\n"
            "rxwake=%d\n"
            "ble=%d\n",
            (int)cfg->boot_app,
            (int)cfg->orient,
            cfg->step_idx,
            cfg->tel_hz,
            cfg->rx_wake ? 1 : 0,
            cfg->want_ble ? 1 : 0);
        stream_write_string(stream, s);
        furi_string_free(s);
    }
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}
