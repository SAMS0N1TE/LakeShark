#pragma once

#include <furi.h>

#define LS_DATA_DIR "/ext/apps_data/lakeshark_p25"
#define LS_CFG_PATH LS_DATA_DIR "/settings.txt"
#define LS_PRESET_DIR LS_DATA_DIR "/presets"

#define LS_MEM_LEGACY_PATH LS_DATA_DIR "/channels.txt"

void ls_cfg_mem_path(char* out, size_t len, const char* tag);
void ls_cfg_preset_path(char* out, size_t len, const char* tag);

typedef enum {
    LsBootLauncher,
    LsBootP25,
    LsBootFm,
    LsBootPocsag,
    LsBootAdsb,
    LsBootRec,
    LsBootCount,
} LsBootApp;

typedef enum {
    LsOrientLandscape,
    LsOrientPortraitL,
    LsOrientPortraitR,
    LsOrientCount,
} LsOrient;

typedef struct {
    LsBootApp boot_app;
    LsOrient orient;
    int step_idx;
    int tel_hz;
    bool want_ble;
    bool ble_proven;
    bool rx_wake;
} LsCfg;

extern const char* const LS_BOOT_NAMES[LsBootCount];
extern const char* const LS_ORIENT_NAMES[LsOrientCount];

void ls_cfg_defaults(LsCfg* cfg);
void ls_cfg_load(LsCfg* cfg);
void ls_cfg_save(const LsCfg* cfg);
