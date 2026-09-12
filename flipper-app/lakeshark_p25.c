#include <furi.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <furi_hal_bt.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_link.h"
#include "ls_ui.h"
#include "ls_cfg.h"
#include "ls_dbg.h"

#define TAG "LakeShark"

#define REDRAW_MS 100

#define MEM_MAX 32
#define MEM_NAME_MAX 14

#define PRESET_MAX 24

#define RX_FLASH_MS 420

#define HIST_N 128

#define ECHO_MS 1200

static const uint32_t STEPS[] = {1250, 3125, 5000, 6250, 12500, 25000, 100000, 1000000};
static const char* const STEP_NAMES[] =
    {"1.25k", "3.125k", "5k", "6.25k", "12.5k", "25k", "100k", "1M"};
#define N_STEPS ((int)(sizeof(STEPS) / sizeof(STEPS[0])))

static const int GAINS[] = {0, 90, 200, 280, 340, 370, 400, 437, 463, 496};
#define N_GAINS ((int)(sizeof(GAINS) / sizeof(GAINS[0])))

static const char* const EQ_PRESET_NAMES[] = {"flat", "voice", "punch", "full", "custom"};
#define EQ_PRESET_COUNT ((int)(sizeof(EQ_PRESET_NAMES) / sizeof(EQ_PRESET_NAMES[0])))

static const char* const EQ_LOUD_NAMES[] = {"off", "low", "med", "high"};
#define EQ_LOUD_COUNT ((int)(sizeof(EQ_LOUD_NAMES) / sizeof(EQ_LOUD_NAMES[0])))

static const int EQ_HP_HZ[] = {0, 80, 120, 150, 180, 220, 260, 300, 400};
#define EQ_HP_COUNT ((int)(sizeof(EQ_HP_HZ) / sizeof(EQ_HP_HZ[0])))

static const char* const EQ_TEST_NAMES[] = {"sweep", "bass", "noise", "tone", "chirp", "moto"};
static const char* const EQ_TEST_LABELS[] = {
    "Sweep 70-4k",
    "Bass ladder",
    "Noise",
    "1 kHz tone",
    "P25 chirp",
    "Moto alert",
};
#define EQ_TEST_COUNT ((int)(sizeof(EQ_TEST_NAMES) / sizeof(EQ_TEST_NAMES[0])))

static const char* const DEMOD_NAMES[] = {"C4FM", "CQPSK", "DIF4FSK", "FSK4TRK"};

static const ViewPortOrientation ORIENT_VP[LsOrientCount] = {
    ViewPortOrientationHorizontal,
    ViewPortOrientationVertical,
    ViewPortOrientationVerticalFlip,
};

typedef enum {
    LsRadioP25,
    LsRadioFm,
    LsRadioPocsag,
    LsRadioAdsb,
    LsRadioRec,
    LsRadioCount,
} LsRadioApp;

typedef enum {
    PG_VFO,
    PG_SIGNAL,
    PG_CALL,
    PG_MEM,
    PG_DIAG,
    PG_FM_SCAN,
    PG_POC,
    PG_POC_LOG,
    PG_TRAFFIC,
    PG_AIRCRAFT,
    PG_ADSB_STAT,
    /**/
    PG_ADSB_MAP,
    PG_REC,
    PG_REC_SIG,
    PG_REC_CAP,
} LsPage;

typedef struct {
    const char* name;
    const char* tag;
    const char* mode_cmd;
    const LsPage* pages;
    int n_pages;
} LsAppDef;

static const LsPage P25_PAGES[] = {PG_VFO, PG_SIGNAL, PG_CALL, PG_MEM, PG_DIAG};
static const LsPage FM_PAGES[] = {PG_VFO, PG_SIGNAL, PG_FM_SCAN, PG_MEM, PG_DIAG};

static const LsPage POC_PAGES[] = {PG_POC, PG_VFO, PG_POC_LOG, PG_MEM, PG_SIGNAL, PG_DIAG};
/**/
static const LsPage ADSB_PAGES[] =
    {PG_TRAFFIC, PG_AIRCRAFT, PG_ADSB_MAP, PG_ADSB_STAT, PG_SIGNAL, PG_DIAG};
static const LsPage REC_PAGES[] = {PG_REC, PG_REC_SIG, PG_REC_CAP, PG_MEM, PG_DIAG};

static const LsAppDef APPS[LsRadioCount] = {
    {"P25", "p25", "MODE p25", P25_PAGES, (int)(sizeof(P25_PAGES) / sizeof(LsPage))},
    {"FM", "fm", "FM listen", FM_PAGES, (int)(sizeof(FM_PAGES) / sizeof(LsPage))},
    {"POCSAG", "pocsag", "FM pocsag", POC_PAGES, (int)(sizeof(POC_PAGES) / sizeof(LsPage))},
    {"ADS-B", "adsb", "MODE adsb", ADSB_PAGES, (int)(sizeof(ADSB_PAGES) / sizeof(LsPage))},
    {"REC", "rec", "MODE rec", REC_PAGES, (int)(sizeof(REC_PAGES) / sizeof(LsPage))},
};

typedef enum {
    SET_LEVELS,
    SET_AUDIO,
    SET_LINK,
    SET_DEVICE,
    SET_DISPLAY,
    SET_ABOUT,
    SET_COUNT,
} LsSetPage;

static const char* const SET_TITLES[SET_COUNT] =
    {"LEVELS", "AUDIO", "LINK", "DEVICE", "DISPLAY", "ABOUT"};

typedef enum {
    LsScreenLauncher,
    LsScreenApp,
    LsScreenSettings,
    LsScreenEdit,
} LsScreen;

typedef struct {
    uint8_t sig[HIST_N];
    uint8_t err[HIST_N];
    uint8_t bps[HIST_N];
    uint8_t fill[HIST_N];
    uint8_t voice[HIST_N];
    uint8_t mag[HIST_N];
    /**/
    uint8_t audio[HIST_N];
    uint8_t msgs[HIST_N];
    uint8_t tracked[HIST_N];
    uint16_t head;
    uint16_t count;
} LsHistory;

typedef struct {
    char name[MEM_NAME_MAX + 1];
    uint32_t freq_hz;
} LsMem;

#define POC_LOG_MAX 16

#define POC_TEXT_MAX 80
typedef struct {
    uint32_t addr;
    char text[POC_TEXT_MAX];
    char type[4];
    int baud;
    uint32_t tick;
} LsPocEntry;

typedef struct {
    int32_t value;
    uint32_t until;
    bool active;
} LsEcho;

typedef enum {
    ECHO_VOL,
    ECHO_GAIN,
    ECHO_SQL,
    ECHO_VGATE,
    ECHO_EQ_PRESET,
    ECHO_EQ_HP,
    ECHO_EQ_BASS,
    ECHO_EQ_TREB,
    ECHO_EQ_PUNCH,
    ECHO_EQ_LOUD,
    ECHO_REC_THRESH,
    ECHO_REC_GAP,
    ECHO_REC_BW,
    ECHO_REC_MINP,
    ECHO_REC_MAXSPAN,
    ECHO_REC_MINEDGES,
    /**/
    ECHO_TTSVOL,
    ECHO_COUNT,
} LsEchoId;

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* queue;
    FuriMutex* lock;
    NotificationApp* notif;
    LsLink* link;

    LsCfg cfg;

    LsScreen screen;
    LsRadioApp app;
    int page;
    LsSetPage set_page;

    LsTelemetry tel;
    bool have_tel;
    bool link_up;

    int focus;
    int list_top;

    bool editing;

    char edit[12];
    int edit_pos;

    LsHistory hist;
    uint32_t last_frames;
    int32_t prev_bch_ok;
    int32_t prev_bch_fail;

    LsEcho echo[ECHO_COUNT];

    LsMem mem[MEM_MAX];
    int mem_count;
    LsMem preset[PRESET_MAX];
    int preset_count;

    LsPocEntry poc[POC_LOG_MAX];
    int poc_count;
    uint32_t poc_last_addr;
    char poc_last_text[POC_TEXT_MAX];

    int pending_transport;

    char pending_mode[24];
    uint32_t pending_mode_until;
    uint32_t pending_mode_retry;

    char modal_title[24];
    char modal_l1[32];
    char modal_l2[32];
    uint32_t modal_until;

    char toast[40];
    uint32_t toast_until;

    uint32_t flash_until;

    int32_t* rec_buf;
    int rec_have;
    int rec_total;
    uint32_t rec_freq_hz;
    int rec_xfer;
    uint32_t rec_deadline;
    int rec_retries;
    int rec_view;
    char rec_file[40];

    bool prev_voice;
    bool prev_sdr_bad;
    bool running;
} LsApp;

typedef enum {
    RecXferIdle,
    RecXferActive,
} LsRecXfer;

static void toast(LsApp* app, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->toast, sizeof(app->toast), fmt, ap);
    va_end(ap);
    app->toast_until = furi_get_tick() + furi_ms_to_ticks(1400);
}

static void modal_set(LsApp* app, const char* title, const char* l1, const char* l2, uint32_t ms) {
    snprintf(app->modal_title, sizeof(app->modal_title), "%s", title ? title : "");
    snprintf(app->modal_l1, sizeof(app->modal_l1), "%s", l1 ? l1 : "");
    snprintf(app->modal_l2, sizeof(app->modal_l2), "%s", l2 ? l2 : "");
    app->modal_until = ms ? furi_get_tick() + furi_ms_to_ticks(ms) : 0;
}

static void modal_clear(LsApp* app) {
    app->modal_title[0] = '\0';
    app->modal_until = 0;
}

static bool modal_active(LsApp* app) {
    if(!app->modal_title[0]) return false;
    if(app->modal_until && furi_get_tick() >= app->modal_until) {
        modal_clear(app);
        return false;
    }
    return true;
}

static void echo_set(LsApp* app, LsEchoId id, int32_t value) {
    app->echo[id].value = value;
    app->echo[id].until = furi_get_tick() + furi_ms_to_ticks(ECHO_MS);
    app->echo[id].active = true;
}

static int32_t echo_get(LsApp* app, LsEchoId id, int32_t from_radio) {
    LsEcho* e = &app->echo[id];
    if(!e->active) return from_radio;
    if(e->value == from_radio || furi_get_tick() >= e->until) {
        e->active = false;
        return from_radio;
    }
    return e->value;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static LsPage cur_page(LsApp* app) {
    const LsAppDef* d = &APPS[app->app];
    return d->pages[clampi(app->page, 0, d->n_pages - 1)];
}

static void apply_orientation(LsApp* app) {
    if(app->vp && app->gui) view_port_set_orientation(app->vp, ORIENT_VP[app->cfg.orient]);
}

static uint8_t signal_pct(int32_t iq_x1000) {

    static const uint8_t curve[17] = {

             0,    40,    55,    64,    70,    75,    79,    82,    85,

            88,    90,    92,    94,    96,    97,    99,   100,
    };
    if(iq_x1000 <= 8) return 0;
    if(iq_x1000 >= 1000) return 100;

    int32_t scaled = iq_x1000 * 16;
    int idx = scaled / 1000;
    int frac = scaled % 1000;
    if(idx >= 16) return 100;

    int lo = curve[idx];
    int hi = curve[idx + 1];
    return (uint8_t)(lo + ((hi - lo) * frac) / 1000);
}

static uint8_t pct_of(int32_t v, int32_t full) {
    if(full <= 0 || v <= 0) return 0;
    int64_t p = ((int64_t)v * 100) / full;
    return (uint8_t)(p > 100 ? 100 : p);
}

/**/

static int32_t iq_rate_full(LsApp* app) {
    switch(app->app) {
    case LsRadioAdsb:
        return 4000000;
    case LsRadioRec:
    case LsRadioFm:
    case LsRadioPocsag:
        return 512000;
    default:
        return 480000;
    }
}

static void hist_push(LsApp* app) {
    LsTelemetry* t = &app->tel;
    LsHistory* h = &app->hist;

    int32_t dok = t->bch_ok - app->prev_bch_ok;
    int32_t dfail = t->bch_fail - app->prev_bch_fail;
    if(dok < 0 || dfail < 0) dok = dfail = 0;
    app->prev_bch_ok = t->bch_ok;
    app->prev_bch_fail = t->bch_fail;

    h->sig[h->head] = signal_pct(t->iq_level);
    h->err[h->head] = (dok + dfail) > 0 ? pct_of(dfail, dok + dfail) : 0;
    /**/
    h->bps[h->head] = pct_of((int32_t)t->iq_bytes_sec, iq_rate_full(app));
    h->fill[h->head] = pct_of(t->ring_fill, t->ring_size);
    h->voice[h->head] = t->voice_active ? 2 : (t->has_sync ? 1 : 0);
    h->mag[h->head] = pct_of(t->rec_mag ? t->rec_mag : t->mag_peak, 254);
    h->audio[h->head] = pct_of(t->audio_level, 100);
    h->msgs[h->head] = pct_of(t->msgs_sec, 50);
    h->tracked[h->head] = pct_of(t->ac_tracked, 24);

    h->head = (h->head + 1) % HIST_N;
    if(h->count < HIST_N) h->count++;
}

static uint8_t hist_at(const LsHistory* h, const uint8_t* series, int i) {
    int start = (h->head - h->count + HIST_N) % HIST_N;
    return series[(start + i) % HIST_N];
}

static const LsMem P25_PRESETS[] = {
    {"8CALL90", 851012500},
    {"8TAC91", 851512500},
    {"8TAC92", 852012500},
    {"8TAC93", 852512500},
    {"VCALL10", 155752500},
    {"UCALL40", 453212500},
};

static const LsMem FM_PRESETS[] = {
    {"FM 88.5", 88500000},
    {"FM 96.9", 96900000},
    {"FM 101.1", 101100000},
    {"NOAA WX1", 162400000},
    {"NOAA WX2", 162425000},
    {"NOAA WX3", 162450000},
    {"NOAA WX4", 162475000},
    {"NOAA WX5", 162500000},
    {"NOAA WX6", 162525000},
    {"NOAA WX7", 162550000},
};

static const LsMem POC_PRESETS[] = {
    {"Pager 929.6", 929612500},
    {"Pager 931.9", 931937500},
    {"Pager 152.0", 152007500},
};

static const LsMem ADSB_PRESETS[] = {
    {"ADS-B 1090", 1090000000},
};

static const LsMem REC_PRESETS[] = {
    {"OOK 433.92", 433920000},
    {"FSK 432.80", 432800000},
    {"OOK 315.00", 315000000},
    {"OOK 345.00", 345000000},
    {"OOK 390.00", 390000000},
    {"OOK 868.35", 868350000},
    {"OOK 915.00", 915000000},
};

#define PRESET_TABLE(t) t, (int)(sizeof(t) / sizeof(LsMem))
static const struct {
    const LsMem* list;
    int count;
} PRESETS[LsRadioCount] = {
    {PRESET_TABLE(P25_PRESETS)},
    {PRESET_TABLE(FM_PRESETS)},
    {PRESET_TABLE(POC_PRESETS)},
    {PRESET_TABLE(ADSB_PRESETS)},
    {PRESET_TABLE(REC_PRESETS)},
};
#undef PRESET_TABLE

static int list_load(const char* path, LsMem* out, int max) {
    int n = -1;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();

    if(file_stream_open(stream, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        n = 0;
        while(n < max && stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(furi_string_size(line) == 0) continue;
            if(furi_string_get_char(line, 0) == '#') continue;

            size_t comma = furi_string_search_char(line, ',');
            if(comma == FURI_STRING_FAILURE) continue;

            FuriString* fs = furi_string_alloc_set(line);
            furi_string_left(fs, comma);
            uint32_t hz = (uint32_t)strtoul(furi_string_get_cstr(fs), NULL, 10);
            furi_string_free(fs);
            if(hz < 1000000) continue;

            LsMem* m = &out[n];
            m->freq_hz = hz;
            const char* nm = furi_string_get_cstr(line) + comma + 1;
            strncpy(m->name, nm, MEM_NAME_MAX);
            m->name[MEM_NAME_MAX] = '\0';
            n++;
        }
    }

    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
    return n;
}

static void list_save(const char* path, const LsMem* list, int n) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, LS_DATA_DIR);
    storage_common_mkdir(storage, LS_PRESET_DIR);

    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        stream_write_cstring(stream, "# LakeShark channel list: <freq_hz>,<name>\n");
        for(int i = 0; i < n; i++) {
            FuriString* s =
                furi_string_alloc_printf("%lu,%s\n", (unsigned long)list[i].freq_hz, list[i].name);
            stream_write_string(stream, s);
            furi_string_free(s);
        }
    }
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

static void mem_load(LsApp* app) {
    char path[64];
    ls_cfg_mem_path(path, sizeof(path), APPS[app->app].tag);
    int n = list_load(path, app->mem, MEM_MAX);
    app->mem_count = n > 0 ? n : 0;
}

static void mem_save(LsApp* app) {
    char path[64];
    ls_cfg_mem_path(path, sizeof(path), APPS[app->app].tag);
    list_save(path, app->mem, app->mem_count);
}

static void preset_load(LsApp* app) {
    char path[64];
    ls_cfg_preset_path(path, sizeof(path), APPS[app->app].tag);

    int n = list_load(path, app->preset, PRESET_MAX);
    if(n >= 0) {

        app->preset_count = n;
        return;
    }

    n = PRESETS[app->app].count;
    if(n > PRESET_MAX) n = PRESET_MAX;
    for(int i = 0; i < n; i++) app->preset[i] = PRESETS[app->app].list[i];
    app->preset_count = n;

    list_save(path, app->preset, app->preset_count);
}

static void mem_migrate_legacy(LsApp* app) {
    char path[64];
    ls_cfg_mem_path(path, sizeof(path), APPS[LsRadioP25].tag);
    if(list_load(path, app->mem, MEM_MAX) >= 0) return;

    int n = list_load(LS_MEM_LEGACY_PATH, app->mem, MEM_MAX);
    if(n > 0) list_save(path, app->mem, n);
    app->mem_count = 0;
}

static bool mem_add(LsApp* app, uint32_t hz, const char* name) {
    if(app->mem_count >= MEM_MAX) {
        toast(app, "Memories full");
        return false;
    }
    if(hz < 1000000) {
        toast(app, "No frequency yet");
        return false;
    }
    for(int i = 0; i < app->mem_count; i++) {
        if(app->mem[i].freq_hz == hz) {
            toast(app, "Already saved");
            return false;
        }
    }
    LsMem* m = &app->mem[app->mem_count++];
    m->freq_hz = hz;
    if(name && name[0]) {
        snprintf(m->name, sizeof(m->name), "%s", name);
    } else {
        snprintf(m->name, sizeof(m->name), "CH%d", app->mem_count);
    }
    mem_save(app);
    toast(app, "Saved %s", m->name);
    return true;
}

static void mem_add_current(LsApp* app) {
    mem_add(app, app->tel.freq_hz, NULL);
}

static void mem_delete(LsApp* app, int idx) {
    if(idx < 0 || idx >= app->mem_count) return;
    for(int i = idx; i < app->mem_count - 1; i++) app->mem[i] = app->mem[i + 1];
    app->mem_count--;
    mem_save(app);
    toast(app, "Deleted");
}

static void poc_ingest(LsApp* app) {
    LsTelemetry* t = &app->tel;
    if(!t->pocsag_last_text[0] && !t->pocsag_last_addr) return;
    if(t->pocsag_last_addr == app->poc_last_addr &&
       !strcmp(t->pocsag_last_text, app->poc_last_text))
        return;

    app->poc_last_addr = t->pocsag_last_addr;
    snprintf(app->poc_last_text, sizeof(app->poc_last_text), "%s", t->pocsag_last_text);

    for(int i = POC_LOG_MAX - 1; i > 0; i--) app->poc[i] = app->poc[i - 1];
    LsPocEntry* e = &app->poc[0];
    e->addr = t->pocsag_last_addr;
    snprintf(e->text, sizeof(e->text), "%s", t->pocsag_last_text);
    snprintf(e->type, sizeof(e->type), "%s", t->pocsag_last_type);
    e->baud = t->pocsag_last_baud;
    e->tick = furi_get_tick();
    if(app->poc_count < POC_LOG_MAX) app->poc_count++;

    notification_message(app->notif, &sequence_blink_blue_10);
}

#define REC_SUB_ROOT "/ext/subghz"
#define REC_SUB_DIR REC_SUB_ROOT "/lakeshark"
#define REC_XFER_TIMEOUT_MS 900
#define REC_XFER_RETRIES 6
#define REC_LINE_VALUES 40

static const char* const REC_PHASE_NAMES[] = {"idle", "armed", "capturing", "done"};

static const char* rec_phase_name(LsApp* app) {
    return REC_PHASE_NAMES[clampi((int)app->tel.rec_phase, 0, 3)];
}

static void rec_preview_clear(LsApp* app) {
    if(app->rec_buf) {
        free(app->rec_buf);
        app->rec_buf = NULL;
    }
    app->rec_have = 0;
    app->rec_total = 0;
    app->rec_file[0] = '\0';
}

static bool rec_write_sub(LsApp* app, char* name_out, size_t name_len) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, REC_SUB_ROOT);
    storage_common_mkdir(storage, REC_SUB_DIR);

    char path[96];
    unsigned long khz = (unsigned long)(app->rec_freq_hz / 1000);
    FileInfo info;
    int idx = 1;
    for(; idx < 1000; idx++) {
        snprintf(path, sizeof(path), REC_SUB_DIR "/LS_%lu_%03d.sub", khz, idx);
        if(storage_common_stat(storage, path, &info) != FSE_OK) break;
    }

    bool ok = false;
    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* s = furi_string_alloc();
        furi_string_printf(
            s,
            "Filetype: Flipper SubGhz RAW File\n"
            "Version: 1\n"
            "Frequency: %lu\n"
            "Preset: FuriHalSubGhzPresetOok650Async\n"
            "Protocol: RAW\n",
            (unsigned long)app->rec_freq_hz);
        stream_write_string(stream, s);

        int i = 0;
        while(i < app->rec_have) {
            furi_string_set_str(s, "RAW_Data:");
            for(int k = 0; k < REC_LINE_VALUES && i < app->rec_have; k++, i++) {
                furi_string_cat_printf(s, " %ld", (long)app->rec_buf[i]);
            }
            furi_string_cat_str(s, "\n");
            stream_write_string(stream, s);
        }
        furi_string_free(s);
        ok = true;
    }

    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);

    if(ok && name_out && name_len) {
        const char* base = strrchr(path, '/');
        if(!base) base = path;
        else base++;

        size_t n = strlen(base);
        if(n >= name_len) n = name_len - 1;
        memcpy(name_out, base, n);
        name_out[n] = '\0';
    }
    return ok;
}

static void rec_xfer_request(LsApp* app) {
    ls_link_send(app->link, "REC GET %d", app->rec_have);
    app->rec_deadline = furi_get_tick() + furi_ms_to_ticks(REC_XFER_TIMEOUT_MS);
}

static void rec_xfer_start(LsApp* app) {
    if(app->rec_xfer != RecXferIdle) return;

    if(!app->link_up) {
        toast(app, "No radio");
        return;
    }
    if(app->tel.rec_phase == LsRecCapturing) {
        toast(app, "Still capturing");
        return;
    }

    int total = (int)app->tel.rec_edges;
    if(total <= 0) {
        toast(app, "Nothing captured");
        return;
    }
    if(total > LS_REC_MAX_EDGES) total = LS_REC_MAX_EDGES;

    rec_preview_clear(app);
    app->rec_buf = malloc(sizeof(int32_t) * (size_t)total);
    if(!app->rec_buf) {
        toast(app, "Out of memory");
        return;
    }

    app->rec_total = total;
    app->rec_have = 0;
    app->rec_retries = 0;
    app->rec_view = 0;
    app->rec_freq_hz = app->tel.freq_hz;
    app->rec_xfer = RecXferActive;

    ls_link_rec_reset(app->link);
    modal_set(app, "SAVING", "reading capture", NULL, 0);
    rec_xfer_request(app);
}

static void rec_xfer_cancel(LsApp* app, const char* why) {
    if(app->rec_xfer == RecXferIdle) return;
    app->rec_xfer = RecXferIdle;
    rec_preview_clear(app);
    modal_clear(app);
    if(why) toast(app, "%s", why);
}

static void rec_xfer_finish(LsApp* app) {
    app->rec_xfer = RecXferIdle;
    modal_clear(app);

    char name[40];
    if(app->rec_have > 0 && rec_write_sub(app, name, sizeof(name))) {
        snprintf(app->rec_file, sizeof(app->rec_file), "%s", name);
        modal_set(app, "SAVED", name, "subghz/lakeshark", 2500);
        notification_message(app->notif, &sequence_success);
    } else {
        rec_preview_clear(app);
        toast(app, "Save failed");
    }
}

static void rec_xfer_tick(LsApp* app) {
    if(app->rec_xfer != RecXferActive) return;

    uint32_t off = 0;
    int count = 0;
    int32_t chunk[LS_REC_CHUNK];

    while(ls_link_rec_take(app->link, &off, &count, chunk, LS_REC_CHUNK)) {
        if((int)off != app->rec_have) continue;

        if(count == 0) {
            rec_xfer_finish(app);
            return;
        }
        for(int i = 0; i < count && app->rec_have < app->rec_total; i++) {
            app->rec_buf[app->rec_have++] = chunk[i];
        }
        app->rec_retries = 0;
        if(app->rec_have >= app->rec_total) {
            rec_xfer_finish(app);
            return;
        }

        char prog[24];
        snprintf(
            prog,
            sizeof(prog),
            "%d / %d",
            clampi(app->rec_have, 0, LS_REC_MAX_EDGES),
            clampi(app->rec_total, 0, LS_REC_MAX_EDGES));
        modal_set(app, "SAVING", "reading capture", prog, 0);

        rec_xfer_request(app);
        return;
    }

    if(furi_get_tick() >= app->rec_deadline) {
        if(++app->rec_retries > REC_XFER_RETRIES) {
            rec_xfer_cancel(app, "Transfer failed");
            return;
        }
        rec_xfer_request(app);
    }
}

static void request_mode(LsApp* app, LsRadioApp which) {
    snprintf(app->pending_mode, sizeof(app->pending_mode), "%s", APPS[which].mode_cmd);
    app->pending_mode_until = furi_get_tick() + furi_ms_to_ticks(8000);
    app->pending_mode_retry = 0;
}

static void enter_app(LsApp* app, LsRadioApp which) {
    app->app = which;
    app->page = 0;
    app->focus = 0;
    app->list_top = 0;
    app->editing = false;
    app->screen = LsScreenApp;
    request_mode(app, which);

    mem_load(app);
    preset_load(app);

    if(!app->link_up) {
        modal_set(app, APPS[which].name, "waiting for radio", "Back to cancel", 0);
    }
}

static const char* transport_badge(LsApp* app) {
    return ls_link_transport(app->link) == LsTransportBle ? "BT" : "US";
}

static void draw_status_line(Canvas* c, const char* text) {
    if(!text || !text[0]) return;
    const int h = canvas_height(c);
    canvas_set_font(c, FontSecondary);
    canvas_draw_line(c, 0, h - 10, canvas_width(c) - 1, h - 10);
    canvas_draw_str(c, 2, h - 1, text);
}

static int body_top(void) {
    return LS_HDR_H + 1;
}
static int body_bottom(Canvas* c) {
    return canvas_height(c) - 11;
}
static int body_full(Canvas* c) {
    return canvas_height(c);
}

static void draw_focus_str(
    Canvas* c,
    int cx,
    int base,
    Align valign,
    int top,
    int h,
    const char* s) {
    const int cw = canvas_width(c);
    int sw = canvas_string_width(c, s);
    int x = cx - sw / 2 - 3;
    int bw = sw + 6;
    if(x < 0) x = 0;
    if(x + bw > cw) bw = cw - x;

    elements_slightly_rounded_box(c, x, top, (size_t)bw, (size_t)h);
    canvas_set_color(c, ColorWhite);
    canvas_draw_str_aligned(c, cx, base, AlignCenter, valign, s);
    canvas_set_color(c, ColorBlack);
}

typedef enum {
    VFO_FREQ,
    VFO_STEP,
    VFO_VOL,
    VFO_GAIN,
    VFO_SQL,
    VFO_VGATE,
    VFO_EQ,
    VFO_MUTE,
} LsVfoRow;

#define VFO_ROW_MAX 8

static int vfo_rows(LsApp* app, uint8_t* out) {
    int n = 0;
    out[n++] = VFO_FREQ;
    out[n++] = VFO_STEP;
    out[n++] = VFO_VOL;
    out[n++] = VFO_GAIN;
    if(app->app == LsRadioFm || app->app == LsRadioPocsag) out[n++] = VFO_SQL;
    if(app->app == LsRadioP25) out[n++] = VFO_VGATE;
    out[n++] = VFO_EQ;
    out[n++] = VFO_MUTE;
    return n;
}

static void draw_vfo_row(Canvas* c, LsApp* app, int kind, int y, bool sel, bool edit) {
    LsTelemetry* t = &app->tel;
    char v[24];

    switch(kind) {
    case VFO_STEP:
        ls_ui_row_edit(c, y, "Step", STEP_NAMES[app->cfg.step_idx], sel, edit);
        break;
    case VFO_VOL: {
        int32_t vol = echo_get(app, ECHO_VOL, t->volume);
        snprintf(v, sizeof(v), "%ld%s", (long)vol, t->muted ? " M" : "");
        ls_ui_level_edit(c, y, "Vol", v, clampi((int)vol, 0, 100), sel, edit);
        break;
    }
    case VFO_GAIN: {
        int32_t g = echo_get(app, ECHO_GAIN, t->gain_tenths);
        snprintf(v, sizeof(v), "%ld.%ld", (long)(g / 10), (long)(g % 10));
        ls_ui_level_edit(c, y, "Gain", v, clampi((int)(g * 100 / 496), 0, 100), sel, edit);
        break;
    }
    case VFO_SQL: {
        int32_t sq = echo_get(app, ECHO_SQL, t->squelch_tenths);
        snprintf(v, sizeof(v), "%ld %s", (long)sq, t->squelch_open ? "OPEN" : "mute");
        ls_ui_level_edit(c, y, "Sql", v, clampi((int)sq, 0, 100), sel, edit);
        break;
    }
    case VFO_VGATE: {
        int32_t vg = echo_get(app, ECHO_VGATE, t->voice_gate);
        snprintf(v, sizeof(v), "%ld", (long)vg);
        ls_ui_level_edit(c, y, "Vgate", v, clampi(((int)vg - 6) * 100 / 93, 0, 100), sel, edit);
        break;
    }
    case VFO_EQ: {
        int32_t p = echo_get(app, ECHO_EQ_PRESET, t->eq_preset);
        ls_ui_row_edit(
            c, y, "Audio", EQ_PRESET_NAMES[clampi((int)p, 0, EQ_PRESET_COUNT - 1)], sel, edit);
        break;
    }
    case VFO_MUTE:
        ls_ui_row_edit(c, y, "Mute", t->muted ? "ON" : "off", sel, false);
        break;
    default:
        break;
    }
}

static void draw_vfo(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    const int w = canvas_width(c);
    const bool port = ls_ui_portrait(c);

    uint8_t rows[VFO_ROW_MAX];
    const int nrows = vfo_rows(app, rows);
    if(app->focus < 0 || app->focus >= nrows) app->focus = 0;
    const bool freq_sel = (app->focus == 0);

    char f[16];
    ls_ui_mhz(f, sizeof(f), t->freq_hz);

    int y = body_top();

    if(port) {
        const char* dot = strchr(f, '.');
        char whole[16] = {0};
        if(dot) {
            size_t n = (size_t)(dot - f);
            if(n >= sizeof(whole)) n = sizeof(whole) - 1;
            memcpy(whole, f, n);
        } else {
            snprintf(whole, sizeof(whole), "%s", f);
        }
        canvas_set_font(c, FontBigNumbers);
        if(freq_sel) {
            draw_focus_str(c, w / 2, y + 12, AlignCenter, y + 1, 21, whole);
        } else {
            canvas_draw_str_aligned(c, w / 2, y + 12, AlignCenter, AlignCenter, whole);
        }
        canvas_set_font(c, FontPrimary);
        const char* frac = dot ? dot : "";
        if(freq_sel && frac[0]) {
            draw_focus_str(c, w / 2, y + 32, AlignBottom, y + 22, 11, frac);
        } else {
            canvas_draw_str_aligned(c, w / 2, y + 32, AlignCenter, AlignBottom, frac);
        }
        y += 36;
    } else {
        canvas_set_font(c, FontBigNumbers);
        if(freq_sel) {
            draw_focus_str(c, w / 2, y + 10, AlignCenter, y, 21, f);
        } else {
            canvas_draw_str_aligned(c, w / 2, y + 10, AlignCenter, AlignCenter, f);
        }
        y += 22;
    }

    canvas_set_font(c, FontSecondary);
    if(t->voice_active) {
        canvas_draw_box(c, 0, body_top() + 1, 5, 5);
    } else if(t->has_sync) {
        canvas_draw_frame(c, 0, body_top() + 1, 5, 5);
    }

    canvas_draw_line(c, 0, y, w - 1, y);
    y += 2;

    canvas_draw_str(c, 0, y + 6, "S");
    ls_ui_bar(c, 9, y + 1, w - 10, 6, signal_pct(t->iq_level));
    y += 9;

    int bottom = body_full(c);
    if(t->sdr_stall_s > 0 || !t->rtl_ready) {
        char l[32];
        if(!t->rtl_ready) {
            snprintf(l, sizeof(l), "NO SDR");
        } else {
            snprintf(l, sizeof(l), "SDR STALLED %lds", (long)t->sdr_stall_s);
        }
        bottom -= LS_ROW_H;
        canvas_draw_box(c, 0, bottom, w, LS_ROW_H);
        canvas_set_color(c, ColorWhite);
        canvas_draw_str(c, 3, bottom + LS_ROW_H - 3, l);
        canvas_set_color(c, ColorBlack);
    }

    const int list_n = nrows - 1;
    int visible = (bottom - y) / LS_ROW_H;
    if(visible < 1) visible = 1;
    if(visible > list_n) visible = list_n;

    int sel = freq_sel ? 0 : app->focus - 1;
    ls_ui_scroll(&app->list_top, sel, list_n, visible);

    for(int r = 0; r < visible; r++) {
        int i = app->list_top + r;
        if(i >= list_n) break;
        const bool rsel = !freq_sel && (i == sel);
        draw_vfo_row(c, app, rows[i + 1], y + r * LS_ROW_H, rsel, rsel && app->editing);
    }
}

typedef struct {
    const char* title;
    const char* unit;
    size_t offset;
    bool filled;
} LsScopeSeries;

#define SERIES(field) offsetof(LsHistory, field)

/**/

static const LsScopeSeries SCOPE_P25[] = {
    {"SIGNAL", "%", SERIES(sig), true},
    {"BCH ERR", "%fail", SERIES(err), true},
    {"IQ RATE", "%rate", SERIES(bps), false},
    {"RING", "%full", SERIES(fill), false},
};

static const LsScopeSeries SCOPE_FM[] = {
    {"SIGNAL", "%", SERIES(sig), true},
    {"AUDIO", "%", SERIES(audio), true},
    {"IQ RATE", "%rate", SERIES(bps), false},
};

static const LsScopeSeries SCOPE_ADSB[] = {
    {"MESSAGES", "%of50/s", SERIES(msgs), true},
    {"TRACKED", "%of24", SERIES(tracked), true},
    {"CRC ERR", "%fail", SERIES(err), true},
    {"MAG PEAK", "%", SERIES(mag), false},
    {"IQ RATE", "%rate", SERIES(bps), false},
};

static const LsScopeSeries SCOPE_REC[] = {
    {"MAGNITUDE", "%", SERIES(mag), true},
    {"IQ RATE", "%rate", SERIES(bps), false},
};

static const LsScopeSeries* scope_table(LsApp* app, int* n) {
    switch(app->app) {
    case LsRadioFm:
    case LsRadioPocsag:
        *n = (int)(sizeof(SCOPE_FM) / sizeof(SCOPE_FM[0]));
        return SCOPE_FM;
    case LsRadioAdsb:
        *n = (int)(sizeof(SCOPE_ADSB) / sizeof(SCOPE_ADSB[0]));
        return SCOPE_ADSB;
    case LsRadioRec:
        *n = (int)(sizeof(SCOPE_REC) / sizeof(SCOPE_REC[0]));
        return SCOPE_REC;
    default:
        *n = (int)(sizeof(SCOPE_P25) / sizeof(SCOPE_P25[0]));
        return SCOPE_P25;
    }
}

static void draw_trace(
    Canvas* c,
    const LsHistory* h,
    const uint8_t* series,
    int x0,
    int y0,
    int w,
    int hgt,
    bool filled) {
    canvas_draw_frame(c, x0, y0, w, hgt);

    int inner_w = w - 2;
    int inner_h = hgt - 2;
    if(inner_w < 1 || inner_h < 1) return;

    int n = h->count < inner_w ? h->count : inner_w;
    int first = h->count - n;

    for(int i = 0; i < n; i++) {
        uint8_t v = hist_at(h, series, first + i);
        int col_h = (v * inner_h) / 100;
        int x = x0 + 1 + (inner_w - n) + i;
        if(filled) {
            if(col_h > 0) canvas_draw_line(c, x, y0 + hgt - 1 - col_h, x, y0 + hgt - 2);
        } else {
            canvas_draw_dot(c, x, y0 + hgt - 2 - col_h);
        }
    }
}

static void draw_activity(Canvas* c, const LsHistory* h, int x0, int y0, int w) {
    int inner_w = w - 2;
    int n = h->count < inner_w ? h->count : inner_w;
    int first = h->count - n;
    for(int i = 0; i < n; i++) {
        uint8_t v = hist_at(h, h->voice, first + i);
        int x = x0 + 1 + (inner_w - n) + i;
        if(v == 2) {
            canvas_draw_line(c, x, y0, x, y0 + 4);
        } else if(v == 1) {
            canvas_draw_dot(c, x, y0 + 4);
        }
    }
}

static void draw_signal(Canvas* c, LsApp* app) {
    const int w = canvas_width(c);
    /**/
    int n_scope = 0;
    const LsScopeSeries* tbl = scope_table(app, &n_scope);
    const LsScopeSeries* p = &tbl[app->focus % n_scope];
    const uint8_t* series = (const uint8_t*)&app->hist + p->offset;

    canvas_set_font(c, FontSecondary);

    char hdr[32];
    uint8_t now = app->hist.count ? hist_at(&app->hist, series, app->hist.count - 1) : 0;
    snprintf(hdr, sizeof(hdr), "%s %d%s", p->title, now, p->unit);
    canvas_draw_str(c, 2, body_top() + 8, hdr);

    char pg[12];
    snprintf(pg, sizeof(pg), "%d/%d", (app->focus % n_scope) + 1, n_scope);
    canvas_draw_str_aligned(c, w - 2, body_top() + 8, AlignRight, AlignBottom, pg);

    int top = body_top() + 11;
    int bottom = body_bottom(c);
    int plot_h = bottom - top - 6;
    if(plot_h < 8) plot_h = 8;

    draw_trace(c, &app->hist, series, 0, top, w, plot_h, p->filled);
    draw_activity(c, &app->hist, 0, top + plot_h + 1, w);

    char ft[40];
    if(!app->link_up) {
        snprintf(ft, sizeof(ft), "no link");
    } else if(ls_ui_portrait(c)) {
        snprintf(ft, sizeof(ft), "%d smp @%dHz", app->hist.count, app->cfg.tel_hz);
    } else {
        snprintf(ft, sizeof(ft), "%d samples @%dHz telemetry", app->hist.count, app->cfg.tel_hz);
    }
    draw_status_line(c, ft);
}

static void draw_call(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    int y = body_top();
    char v[24], age[8];

    if(t->nac_age_ms >= 0) {
        ls_ui_age(age, sizeof(age), t->nac_age_ms);
        snprintf(v, sizeof(v), "%03X  %s", (unsigned)t->nac, age);
    } else {
        snprintf(v, sizeof(v), "---");
    }
    ls_ui_row(c, y, "NAC", v, false);
    y += LS_ROW_H;

    if(t->tg_age_ms >= 0) {
        ls_ui_age(age, sizeof(age), t->tg_age_ms);
        snprintf(v, sizeof(v), "%ld  %s", (long)t->tg, age);
    } else {
        snprintf(v, sizeof(v), "---");
    }
    ls_ui_row(c, y, "TG", v, false);
    y += LS_ROW_H;

    if(t->src_age_ms >= 0) {
        ls_ui_age(age, sizeof(age), t->src_age_ms);
        snprintf(v, sizeof(v), "%ld  %s", (long)t->src, age);
    } else {
        snprintf(v, sizeof(v), "---");
    }
    ls_ui_row(c, y, "SRC", v, false);
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%ld/%ld", (long)t->bch_ok, (long)t->bch_fail);
    ls_ui_row(c, y, "BCH ok/fail", v, false);
    y += LS_ROW_H;

    if(y + LS_ROW_H <= body_bottom(c)) {

        const char* dmn = t->demod_name[0] ? t->demod_name :
                          (t->demod_mode >= 0 && t->demod_mode < 4) ?
                                              DEMOD_NAMES[t->demod_mode] :
                                              "-";
        snprintf(v, sizeof(v), "%s %s", dmn, t->polarity_inverted ? "INV" : "NRM");
        ls_ui_row(c, y, "Demod", v, false);
        y += LS_ROW_H;
    }
    if(y + LS_ROW_H <= body_bottom(c)) {
        snprintf(v, sizeof(v), "%s", t->ftype[0] ? t->ftype : "-");
        ls_ui_row(c, y, "Frame", v, false);
        y += LS_ROW_H;
    }
    if(y + LS_ROW_H <= body_bottom(c)) {
        snprintf(v, sizeof(v), "%ld/%ld", (long)t->sync_count, (long)t->voice_count);
        ls_ui_row(c, y, "Sync/Voice", v, false);
    }

    draw_status_line(c, t->err);
}

typedef enum {
    MEM_ROW_SAVE,
    MEM_ROW_MEM,
    MEM_ROW_DIV,
    MEM_ROW_PRESET,
} LsMemRowKind;

static int mem_rows(LsApp* app) {
    return 1 + app->mem_count + (app->preset_count ? 1 + app->preset_count : 0);
}

static LsMemRowKind mem_row_kind(LsApp* app, int row, int* idx) {
    *idx = 0;
    if(row <= 0) return MEM_ROW_SAVE;
    if(row <= app->mem_count) {
        *idx = row - 1;
        return MEM_ROW_MEM;
    }
    if(row == app->mem_count + 1) return MEM_ROW_DIV;
    *idx = row - app->mem_count - 2;
    return MEM_ROW_PRESET;
}

static void draw_mem(Canvas* c, LsApp* app) {
    canvas_set_font(c, FontSecondary);

    const int n = mem_rows(app);
    const int rows = (body_full(c) - body_top()) / LS_ROW_H;
    ls_ui_scroll(&app->list_top, app->focus, n, rows);

    for(int r = 0; r < rows; r++) {
        int i = app->list_top + r;
        if(i >= n) break;

        int y = body_top() + r * LS_ROW_H;
        int idx = 0;
        char f[16];

        switch(mem_row_kind(app, i, &idx)) {
        case MEM_ROW_SAVE:
            ls_ui_mhz(f, sizeof(f), app->tel.freq_hz);
            ls_ui_row(c, y, "+ Save current", f, i == app->focus);
            break;
        case MEM_ROW_MEM:
            ls_ui_mhz(f, sizeof(f), app->mem[idx].freq_hz);
            ls_ui_row(c, y, app->mem[idx].name, f, i == app->focus);
            break;
        case MEM_ROW_DIV:

            ls_ui_divider_row(c, y, "PRESETS");
            break;
        case MEM_ROW_PRESET:
            ls_ui_mhz(f, sizeof(f), app->preset[idx].freq_hz);
            ls_ui_row(c, y, app->preset[idx].name, f, i == app->focus);
            break;
        }
    }
    elements_scrollbar(c, app->focus, n);
}

static void draw_diag(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    uint32_t frames, replies, bad;
    ls_link_stats(app->link, &frames, &replies, &bad);

    const int rows = (body_bottom(c) - body_top()) / LS_ROW_H;
    char v[24];
    int y = body_top();
    int r = 0;

#define DIAG_ROW(label, ...)                        \
    if(r < rows) {                                  \
        snprintf(v, sizeof(v), __VA_ARGS__);        \
        ls_ui_row(c, y, label, v, false);           \
        y += LS_ROW_H;                              \
        r++;                                        \
    }

    DIAG_ROW("Link", "%s", ls_link_state_str(app->link));
    DIAG_ROW("Port", "%s", ls_link_port_name(app->link));
    DIAG_ROW("Frames", "%lu", (unsigned long)frames);
    DIAG_ROW("Replies/bad", "%lu/%lu", (unsigned long)replies, (unsigned long)bad);
    DIAG_ROW("IQ B/s", "%lu", (unsigned long)t->iq_bytes_sec);
    DIAG_ROW("Ring", "%ld/%ld", (long)t->ring_fill, (long)t->ring_size);
    DIAG_ROW("Rd err/drops", "%ld/%lu", (long)t->read_errors, (unsigned long)t->audio_drops);
    DIAG_ROW("Decode", "%ldus", (long)t->decode_us);
#undef DIAG_ROW

    char rep[64];
    ls_link_last_reply(app->link, rep, sizeof(rep));
    draw_status_line(c, rep);
}

static void draw_fm_scan(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    char v[24];
    int y = body_top();

    ls_ui_mhz(v, sizeof(v), t->scan_start_hz);
    ls_ui_row(c, y, "From", v, false);
    y += LS_ROW_H;

    ls_ui_mhz(v, sizeof(v), t->scan_stop_hz);
    ls_ui_row(c, y, "To", v, false);
    y += LS_ROW_H;

    if(t->scan_peak_hz) {
        ls_ui_mhz(v, sizeof(v), t->scan_peak_hz);
    } else {
        snprintf(v, sizeof(v), "---");
    }
    ls_ui_row(c, y, "Peak", v, false);
    y += LS_ROW_H;

    snprintf(
        v,
        sizeof(v),
        "%ld.%ld dB",
        (long)(t->scan_peak_db / 10),
        (long)labs(t->scan_peak_db % 10));
    ls_ui_row(c, y, "Peak level", v, false);
    y += LS_ROW_H;

    if(y + LS_ROW_H <= body_full(c)) {
        snprintf(v, sizeof(v), "%lu", (unsigned long)t->scan_sweeps);
        ls_ui_row(c, y, "Sweeps", v, false);
    }
}

static void draw_poc(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    char v[24];
    int y = body_top();

    if(t->pocsag_auto) {
        snprintf(v, sizeof(v), "auto (%d)", (int)t->pocsag_baud);
    } else {
        snprintf(v, sizeof(v), "%d", (int)t->pocsag_baud);
    }
    ls_ui_row(c, y, "Baud", v, app->focus == 0);
    y += LS_ROW_H;

    int32_t sql = echo_get(app, ECHO_SQL, t->squelch_tenths);
    snprintf(v, sizeof(v), "%ld%s", (long)sql, t->squelch_open ? " open" : "");
    ls_ui_level(c, y, "Squelch", v, clampi((int)sql, 0, 100), app->focus == 1);
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%s", t->pocsag_sync ? "LOCKED" : "no sync");
    ls_ui_row(c, y, "Sync", v, false);
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%lu", (unsigned long)t->pocsag_pages);
    ls_ui_row(c, y, "Pages", v, false);
    y += LS_ROW_H;

    if(y + LS_ROW_H <= body_full(c)) {
        snprintf(v, sizeof(v), "%lu", (unsigned long)t->pocsag_frames);
        ls_ui_row(c, y, "Frames", v, false);
    }
}

static void draw_poc_log(Canvas* c, LsApp* app) {
    canvas_set_font(c, FontSecondary);

    if(app->poc_count == 0) {
        ls_ui_empty(c, "No pages yet", "listening...");
        return;
    }

    const int per = 18;
    const int rows = (body_full(c) - body_top()) / per;
    ls_ui_scroll(&app->list_top, app->focus, app->poc_count, rows);

    const int w = canvas_width(c);
    for(int r = 0; r < rows; r++) {
        int i = app->list_top + r;
        if(i >= app->poc_count) break;
        LsPocEntry* e = &app->poc[i];
        int y = body_top() + r * per;
        bool sel = (i == app->focus);

        if(sel) {
            canvas_draw_box(c, 0, y, w, per - 1);
            canvas_set_color(c, ColorWhite);
        }

        char head[28];
        snprintf(head, sizeof(head), "%lu%s%s", (unsigned long)e->addr,
                 e->type[0] ? " " : "", e->type);
        canvas_draw_str(c, 3, y + 8, head);

        char baud[12];
        snprintf(baud, sizeof(baud), "%d", e->baud);
        canvas_draw_str_aligned(c, w - 3, y + 8, AlignRight, AlignBottom, baud);

        canvas_draw_str(c, 3, y + 16, e->text[0] ? e->text : "(no text)");

        if(sel) canvas_set_color(c, ColorBlack);
    }
    elements_scrollbar(c, app->focus, app->poc_count);
}

static int adsb_visible(LsApp* app) {
    int n = 0;
    for(int i = 0; i < LS_AC_MAX; i++)
        if(app->tel.ac[i].seen && app->tel.ac[i].icao) n++;
    return n;
}

static const LsAircraft* adsb_at(LsApp* app, int idx) {
    int n = 0;
    for(int i = 0; i < LS_AC_MAX; i++) {
        const LsAircraft* a = &app->tel.ac[i];
        if(!a->seen || !a->icao) continue;
        if(n == idx) return a;
        n++;
    }
    return NULL;
}

static void draw_traffic(Canvas* c, LsApp* app) {
    canvas_set_font(c, FontSecondary);

    int n = adsb_visible(app);
    if(n == 0) {
        ls_ui_empty(c, app->link_up ? "No aircraft" : "No link", "1090 MHz");
        return;
    }

    if(app->focus >= n) app->focus = n - 1;
    const int rows = (body_bottom(c) - body_top()) / LS_ROW_H;
    ls_ui_scroll(&app->list_top, app->focus, n, rows);

    for(int r = 0; r < rows; r++) {
        int i = app->list_top + r;
        if(i >= n) break;
        const LsAircraft* a = adsb_at(app, i);
        if(!a) break;

        char label[20], value[20];

        if(a->call[0]) {
            snprintf(label, sizeof(label), "%s", a->call);
        } else {
            snprintf(label, sizeof(label), "%06lX", (unsigned long)a->icao);
        }
        snprintf(value, sizeof(value), "%ldft", (long)a->altitude);
        ls_ui_row(c, body_top() + r * LS_ROW_H, label, value, i == app->focus);
    }
    elements_scrollbar(c, app->focus, n);

    char ft[32];
    snprintf(ft, sizeof(ft), "%d tracked", n);
    draw_status_line(c, ft);
}

static void draw_aircraft(Canvas* c, LsApp* app) {
    canvas_set_font(c, FontSecondary);

    const LsAircraft* a = adsb_at(app, app->focus);
    if(!a) {
        ls_ui_empty(c, "No aircraft", NULL);
        return;
    }

    char v[24];
    int y = body_top();

    snprintf(v, sizeof(v), "%06lX", (unsigned long)a->icao);
    ls_ui_row(c, y, a->call[0] ? a->call : "(no callsign)", v, false);
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%ld ft", (long)a->altitude);
    ls_ui_row(c, y, "Altitude", v, false);
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%ld kt", (long)a->velocity);
    ls_ui_row(c, y, "Speed", v, false);
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%ld deg", (long)a->heading);
    ls_ui_row(c, y, "Heading", v, false);
    y += LS_ROW_H;

    if(y + LS_ROW_H <= body_full(c)) {
        snprintf(v, sizeof(v), "%+ld fpm", (long)a->vert_rate);
        ls_ui_row(c, y, "Climb", v, false);
        y += LS_ROW_H;
    }
    if(y + LS_ROW_H <= body_full(c)) {
        char age[8];
        ls_ui_age(age, sizeof(age), a->age_ms);
        snprintf(v, sizeof(v), "%ld msg  %s", (long)a->msg_count, age);
        ls_ui_row(c, y, "Seen", v, false);
    }
}

/**/

static const uint16_t COS_256[10] = {256, 252, 241, 222, 196, 165, 128, 88, 44, 0};

static uint16_t cos256_for_lat_e4(int32_t lat_e4) {
    int32_t d = lat_e4 < 0 ? -lat_e4 : lat_e4;
    int idx = (int)(d / 100000);
    if(idx > 9) idx = 9;
    return COS_256[idx];
}

static int adsb_positioned(LsApp* app) {
    int n = 0;
    for(int i = 0; i < LS_AC_MAX; i++) {
        const LsAircraft* a = &app->tel.ac[i];
        if(a->seen && a->icao && a->pos_valid) n++;
    }
    return n;
}

static const LsAircraft* adsb_positioned_at(LsApp* app, int want) {
    int n = 0;
    for(int i = 0; i < LS_AC_MAX; i++) {
        const LsAircraft* a = &app->tel.ac[i];
        if(!(a->seen && a->icao && a->pos_valid)) continue;
        if(n++ == want) return a;
    }
    return NULL;
}

static void draw_adsb_map(Canvas* c, LsApp* app) {
    const int w = canvas_width(c);
    int count = adsb_positioned(app);

    if(count == 0) {
        ls_ui_empty(c, "No positions yet", "needs airborne CPR pairs");
        draw_status_line(c, "position needs 2 frames per plane");
        return;
    }

    int32_t lat_lo = 0, lat_hi = 0, lon_lo = 0, lon_hi = 0;
    for(int i = 0; i < count; i++) {
        const LsAircraft* a = adsb_positioned_at(app, i);
        if(!a) continue;
        if(i == 0) {
            lat_lo = lat_hi = a->lat_e4;
            lon_lo = lon_hi = a->lon_e4;
            continue;
        }
        if(a->lat_e4 < lat_lo) lat_lo = a->lat_e4;
        if(a->lat_e4 > lat_hi) lat_hi = a->lat_e4;
        if(a->lon_e4 < lon_lo) lon_lo = a->lon_e4;
        if(a->lon_e4 > lon_hi) lon_hi = a->lon_e4;
    }

    int32_t lat_span = lat_hi - lat_lo;
    int32_t lon_span = lon_hi - lon_lo;
    if(lat_span < 10) lat_span = 10;
    if(lon_span < 10) lon_span = 10;

    const int px = 4;
    const int py = body_top() + 2;
    const int pw = w - 8;
    const int ph = body_full(c) - 10 - py - 2;
    if(ph < 8) return;

    canvas_draw_frame(c, px, py, pw, ph);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, px + 2, py + 8, "N");

    int sel = count > 0 ? app->focus % count : 0;

    for(int i = 0; i < count; i++) {
        const LsAircraft* a = adsb_positioned_at(app, i);
        if(!a) continue;

        int x = px + 1 + (int)(((int64_t)(a->lon_e4 - lon_lo) * (pw - 3)) / lon_span);
        int y = py + 1 + (int)(((int64_t)(lat_hi - a->lat_e4) * (ph - 3)) / lat_span);

        if(i == sel) {
            canvas_draw_circle(c, x, y, 3);
            canvas_draw_dot(c, x, y);
        } else {
            canvas_draw_disc(c, x, y, 1);
        }
    }

    const LsAircraft* s = adsb_positioned_at(app, sel);
    char line[40];
    if(s) {
        char id[10];
        if(s->call[0] && s->call[0] != '-') {
            snprintf(id, sizeof(id), "%s", s->call);
        } else {
            snprintf(id, sizeof(id), "%06lX", (unsigned long)s->icao);
        }
        snprintf(line, sizeof(line), "%s %ldft %ldkt", id, (long)s->altitude,
                 (long)s->velocity);
    } else {
        snprintf(line, sizeof(line), "%d shown", count);
    }
    draw_status_line(c, line);

    int32_t ns_m = (lat_hi - lat_lo) * 1113 / 100;
    int32_t ew_m = (lon_hi - lon_lo) * 1113 / 100;
    ew_m = (int32_t)(((int64_t)ew_m * cos256_for_lat_e4((lat_hi + lat_lo) / 2)) >> 8);

    char span[24];
    snprintf(span, sizeof(span), "%ldx%ldkm", (long)(ns_m / 1000), (long)(ew_m / 1000));
    canvas_draw_str_aligned(c, px + pw - 2, py + 8, AlignRight, AlignBottom, span);
}

/**/
#define ADSB_STAT_ROWS 9
/**/
#define ADSB_ROW_TTSVOL 8

static void adsb_stat_row(LsApp* app, int idx, char* label, size_t ll, char* val, size_t vl) {
    LsTelemetry* t = &app->tel;
    switch(idx) {
    case 0:
        snprintf(label, ll, "Tracked");
        snprintf(val, vl, "%ld", (long)t->ac_tracked);
        break;
    case 1:
        snprintf(label, ll, "Messages");
        snprintf(val, vl, "%ld/s", (long)t->msgs_sec);
        break;
    case 2:
        snprintf(label, ll, "Total msgs");
        snprintf(val, vl, "%ld", (long)t->msgs_total);
        break;
    case 3:
        snprintf(label, ll, "CRC ok/err");
        snprintf(val, vl, "%ld/%ld", (long)t->crc_good, (long)t->crc_err);
        break;
    case 4:
        snprintf(label, ll, "Preambles");
        snprintf(val, vl, "%ld/s", (long)t->bursts_sec);
        break;
    case 5:
        snprintf(label, ll, "Mag avg/peak");
        snprintf(val, vl, "%ld/%ld", (long)t->mag_avg, (long)t->mag_peak);
        break;
    case 6:
        snprintf(label, ll, "Last message");
        ls_ui_age(val, vl, t->last_msg_ms);
        break;
    case 7:
        snprintf(label, ll, "Gain");
        if(t->gain_tenths <= 0) {
            snprintf(val, vl, "auto");
        } else {
            snprintf(val, vl, "%ld.%ld dB", (long)(t->gain_tenths / 10),
                     (long)(t->gain_tenths % 10));
        }
        break;
    /**/
    default:
        snprintf(label, ll, "TTS volume");
        snprintf(val, vl, "%ld%%", (long)echo_get(app, ECHO_TTSVOL, t->tts_vol));
        break;
    }
}

/**/
static void adsb_ttsvol_adjust(LsApp* app, int delta) {
    int32_t v = echo_get(app, ECHO_TTSVOL, app->tel.tts_vol) + delta;
    if(v < 0) v = 0;
    if(v > 200) v = 200;
    ls_link_send(app->link, "TTSVOL %ld", (long)v);
    echo_set(app, ECHO_TTSVOL, v);
}

static void draw_adsb_stat(Canvas* c, LsApp* app) {
    canvas_set_font(c, FontSecondary);

    int rows = (body_full(c) - body_top()) / LS_ROW_H;
    if(rows < 1) rows = 1;
    if(rows > ADSB_STAT_ROWS) rows = ADSB_STAT_ROWS;

    ls_ui_scroll(&app->list_top, app->focus, ADSB_STAT_ROWS, rows);

    int y = body_top();
    for(int i = 0; i < rows; i++) {
        int idx = app->list_top + i;
        if(idx >= ADSB_STAT_ROWS) break;
        char label[16], val[24];
        adsb_stat_row(app, idx, label, sizeof(label), val, sizeof(val));
        /**/
        ls_ui_row_edit(c, y, label, val, idx == app->focus,
                       app->editing && idx == app->focus);
        y += LS_ROW_H;
    }
}

typedef enum {
    REC_ROW_ARM,
    REC_ROW_FREQ,
    REC_ROW_GAIN,
    REC_ROW_THRESH,
    REC_ROW_GAP,
    REC_ROW_BW,
    REC_ROW_MINP,
    REC_ROW_MAXSPAN,
    REC_ROW_MINEDGES,
    REC_ROW_COUNT,
} LsRecRow;

static void rec_row_value(LsApp* app, int row, char* out, size_t len) {
    LsTelemetry* t = &app->tel;

    switch(row) {
    case REC_ROW_ARM:
        snprintf(out, len, "%s", app->link_up ? rec_phase_name(app) : "---");
        break;
    case REC_ROW_FREQ:
        ls_ui_mhz(out, len, t->freq_hz);
        break;
    case REC_ROW_GAIN: {
        int32_t g = echo_get(app, ECHO_GAIN, t->gain_tenths);
        if(g <= 0) {
            snprintf(out, len, "auto");
        } else {
            snprintf(out, len, "%ld.%ld", (long)(g / 10), (long)(g % 10));
        }
        break;
    }
    case REC_ROW_THRESH: {
        int32_t th = echo_get(app, ECHO_REC_THRESH, t->rec_thresh_fixed);
        if(th <= 0) {
            snprintf(out, len, "auto (%ld)", (long)t->rec_thresh);
        } else {
            snprintf(out, len, "%ld", (long)th);
        }
        break;
    }
    case REC_ROW_GAP: {
        int32_t gp = echo_get(app, ECHO_REC_GAP, t->rec_gap_ms);
        snprintf(out, len, "%ld ms", (long)gp);
        break;
    }
    case REC_ROW_BW: {
        int32_t bw = echo_get(app, ECHO_REC_BW, (int32_t)(t->rec_bw_hz / 1000));
        if(bw <= 0) {
            snprintf(out, len, "auto");
        } else {
            snprintf(out, len, "%ld kHz", (long)bw);
        }
        break;
    }
    case REC_ROW_MINP: {
        int32_t mp = echo_get(app, ECHO_REC_MINP, (int32_t)t->rec_min_pulse_us);
        snprintf(out, len, "%ld us", (long)mp);
        break;
    }
    case REC_ROW_MAXSPAN: {
        int32_t ms = echo_get(app, ECHO_REC_MAXSPAN, (int32_t)t->rec_max_span_ms);
        snprintf(out, len, "%ld ms", (long)ms);
        break;
    }
    case REC_ROW_MINEDGES: {
        int32_t me = echo_get(app, ECHO_REC_MINEDGES, t->rec_min_edges);
        snprintf(out, len, "%ld", (long)me);
        break;
    }
    default:
        out[0] = '\0';
        break;
    }
}

static const char* const REC_END_NAMES[] = {"-", "gap", "span cap", "edge cap"};

static void draw_rec_sig(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    const int w = canvas_width(c);
    const int y0 = body_top();
    const int hgt = 28;

    draw_trace(c, &app->hist, app->hist.mag, 0, y0, w, hgt, true);

    if(t->rec_thresh > 0) {
        int pct = clampi((int)t->rec_thresh * 100 / 254, 0, 100);
        int ty = y0 + hgt - 2 - ((hgt - 2) * pct) / 100;
        for(int x = 1; x < w - 1; x += 3) canvas_draw_dot(c, x, ty);
    }

    char v[36];
    snprintf(
        v,
        sizeof(v),
        "mag %ld  fl %ld  th %ld",
        (long)t->rec_mag,
        (long)t->rec_floor,
        (long)t->rec_thresh);
    canvas_draw_str(c, 0, y0 + hgt + 9, v);

    if(t->rec_edges > 0) {
        snprintf(
            v,
            sizeof(v),
            "%s %lu-%lu us ~%lub",
            REC_END_NAMES[clampi((int)t->rec_end_reason, 0, 3)],
            (unsigned long)t->rec_min_mark_us,
            (unsigned long)t->rec_max_mark_us,
            (unsigned long)t->rec_baud_est);
    } else {
        snprintf(v, sizeof(v), "%s", app->link_up ? "waiting for a burst" : "no link");
    }
    draw_status_line(c, v);
}

static void draw_rec(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    static const char* const LABELS[REC_ROW_COUNT] = {
        "Record",
        "Freq",
        "Gain",
        "Thresh",
        "Gap",
        "Bandwidth",
        "Min pulse",
        "Max span",
        "Min edges"};

    const int rows = (body_bottom(c) - body_top()) / LS_ROW_H;
    ls_ui_scroll(&app->list_top, app->focus, REC_ROW_COUNT, rows);

    char v[24];
    for(int r = 0; r < rows; r++) {
        int i = app->list_top + r;
        if(i >= REC_ROW_COUNT) break;

        rec_row_value(app, i, v, sizeof(v));
        int y = body_top() + r * LS_ROW_H;

        if(i == REC_ROW_ARM) {
            ls_ui_row(c, y, LABELS[i], v, i == app->focus);
        } else {
            ls_ui_row_edit(
                c, y, LABELS[i], v, i == app->focus, app->editing && app->focus == i);
        }
    }
    elements_scrollbar(c, app->focus, REC_ROW_COUNT);

    static const char* const SHORT[] = {"idle", "arm", "cap", "done"};

    int on = t->rec_thresh > 0 ? t->rec_thresh : 1;
    snprintf(
        v,
        sizeof(v),
        "%s e%ld %ld/%ld",
        app->link_up ? SHORT[clampi((int)t->rec_phase, 0, 3)] : "no link",
        (long)t->rec_edges,
        (long)t->rec_mag,
        (long)t->rec_thresh);
    draw_status_line(c, v);

    const int bw = 26;
    const int bx = canvas_width(c) - bw - 2;
    ls_ui_bar(c, bx, canvas_height(c) - 8, bw, 6, clampi((int)(t->rec_mag * 100 / (on * 2)), 0, 100));
}

static void draw_rec_wave(Canvas* c, LsApp* app, int x0, int y0, int w, int h) {
    uint32_t total = 0;
    for(int i = 0; i < app->rec_have; i++) total += (uint32_t)labs(app->rec_buf[i]);
    if(!total) return;

    uint32_t acc = 0;
    for(int i = 0; i < app->rec_have; i++) {
        uint32_t d = (uint32_t)labs(app->rec_buf[i]);
        int xa = x0 + (int)(((uint64_t)acc * w) / total);
        acc += d;
        int xb = x0 + (int)(((uint64_t)acc * w) / total);

        if(app->rec_buf[i] > 0) {
            int ww = xb - xa;
            if(ww < 1) ww = 1;
            canvas_draw_box(c, xa, y0, ww, h);
        }
    }
    canvas_draw_line(c, x0, y0 + h, x0 + w - 1, y0 + h);
}

static void draw_rec_cap(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    char v[32];
    int y = body_top();

    snprintf(v, sizeof(v), "%ld", (long)t->rec_edges);
    ls_ui_row(c, y, "Edges", v, false);
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%lu ms", (unsigned long)(t->rec_span_us / 1000));
    ls_ui_row(c, y, "Span", v, false);
    y += LS_ROW_H;

    if(app->rec_buf && app->rec_have > 0) {
        draw_rec_wave(c, app, 2, y + 1, canvas_width(c) - 4, 10);
        draw_status_line(c, app->rec_file[0] ? app->rec_file : "OK saves .sub");
    } else if(t->rec_phase == LsRecDone && t->rec_edges > 0) {
        canvas_draw_str(c, 2, y + 9, "OK to save to SubGHz");
        draw_status_line(c, "OK saves .sub");
    } else {
        canvas_draw_str(c, 2, y + 9, "Arm and transmit");
        draw_status_line(c, app->link_up ? rec_phase_name(app) : "no link");
    }
}

static void draw_launcher(Canvas* c, LsApp* app) {
    canvas_set_font(c, FontSecondary);

    const int n = LsRadioCount + 1;
    const int rows = (body_full(c) - body_top()) / LS_ROW_H;
    ls_ui_scroll(&app->list_top, app->focus, n, rows);

    for(int r = 0; r < rows; r++) {
        int i = app->list_top + r;
        if(i >= n) break;

        const char* label;
        const char* value = "";
        if(i < LsRadioCount) {
            label = APPS[i].name;

            if(app->link_up) {
                bool live = false;
                switch(i) {
                case LsRadioP25:
                    live = (app->tel.mode == LsModeP25);
                    break;
                case LsRadioFm:
                    live = (app->tel.mode == LsModeFm && app->tel.fm_submode != LsFmPocsag);
                    break;
                case LsRadioPocsag:
                    live = (app->tel.mode == LsModeFm && app->tel.fm_submode == LsFmPocsag);
                    break;
                case LsRadioAdsb:
                    live = (app->tel.mode == LsModeAdsb);
                    break;
                case LsRadioRec:
                    live = (app->tel.mode == LsModeRec);
                    break;
                default:
                    break;
                }
                value = live ? "live" : "";
            }
        } else {
            label = "Settings";
            value = ls_link_state_str(app->link);
        }
        ls_ui_row(c, body_top() + r * LS_ROW_H, label, value, i == app->focus);
    }
    elements_scrollbar(c, app->focus, n);
}

typedef enum {
    LVL_VOL,
    LVL_GAIN,
    LVL_SQL,
    LVL_VGATE,
    LVL_COUNT,
} LsLevel;

static void draw_set_levels(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    int32_t vol = echo_get(app, ECHO_VOL, t->volume);
    int32_t gain = echo_get(app, ECHO_GAIN, t->gain_tenths);
    int32_t sql = echo_get(app, ECHO_SQL, t->squelch_tenths);
    int32_t vg = echo_get(app, ECHO_VGATE, t->voice_gate);

    int y = body_top();
    char v[20];

#define LVL_EDIT(row) (app->editing && app->focus == (row))

    snprintf(v, sizeof(v), "%ld%s", (long)vol, t->muted ? " M" : "");
    ls_ui_level_edit(
        c, y, "Vol", v, clampi((int)vol, 0, 100), app->focus == LVL_VOL, LVL_EDIT(LVL_VOL));
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%ld.%ld", (long)(gain / 10), (long)(gain % 10));
    ls_ui_level_edit(
        c,
        y,
        "Gain",
        v,
        clampi((int)(gain * 100 / 496), 0, 100),
        app->focus == LVL_GAIN,
        LVL_EDIT(LVL_GAIN));
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%ld", (long)sql);
    ls_ui_level_edit(
        c, y, "Sql", v, clampi((int)sql, 0, 100), app->focus == LVL_SQL, LVL_EDIT(LVL_SQL));
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%ld", (long)vg);
    ls_ui_level_edit(
        c,
        y,
        "Vgate",
        v,
        clampi(((int)vg - 6) * 100 / 93, 0, 100),
        app->focus == LVL_VGATE,
        LVL_EDIT(LVL_VGATE));
    y += LS_ROW_H;

#undef LVL_EDIT

    if(y + LS_ROW_H <= body_full(c)) {
        snprintf(v, sizeof(v), "%s", t->muted ? "ON" : "off");
        ls_ui_row(c, y, "Mute", v, false);
    }
}

typedef enum {
    AUD_PRESET,
    AUD_BASS,
    AUD_TREB,
    AUD_PUNCH,
    AUD_RUMBLE,
    AUD_LOUD,
    AUD_GR,
    AUD_TESTS,
    AUD_TEST0,
    AUD_COUNT = AUD_TEST0 + EQ_TEST_COUNT,
} LsAudioRow;

static int eq_hp_index(int hz) {
    int best = 0;
    int bestd = 100000;
    for(int i = 0; i < EQ_HP_COUNT; i++) {
        int d = EQ_HP_HZ[i] > hz ? EQ_HP_HZ[i] - hz : hz - EQ_HP_HZ[i];
        if(d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

static void draw_set_audio(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    const int32_t preset = echo_get(app, ECHO_EQ_PRESET, t->eq_preset);
    const int32_t bass = echo_get(app, ECHO_EQ_BASS, t->eq_bass_db);
    const int32_t treb = echo_get(app, ECHO_EQ_TREB, t->eq_treb_db);
    const int32_t punch = echo_get(app, ECHO_EQ_PUNCH, t->eq_punch);
    const int32_t hp = echo_get(app, ECHO_EQ_HP, t->eq_hp_hz);
    const int32_t loud = echo_get(app, ECHO_EQ_LOUD, t->eq_loud);

    const int rows = (body_full(c) - body_top()) / LS_ROW_H;
    ls_ui_scroll(&app->list_top, app->focus, AUD_COUNT, rows);

    char v[20];
    for(int r = 0; r < rows; r++) {
        int i = app->list_top + r;
        if(i >= AUD_COUNT) break;

        const int y = body_top() + r * LS_ROW_H;
        const bool sel = (app->focus == i);
        const bool edit = app->editing && sel;
        v[0] = '\0';

        switch(i) {
        case AUD_PRESET:
            ls_ui_row_edit(
                c,
                y,
                "Profile",
                EQ_PRESET_NAMES[clampi((int)preset, 0, EQ_PRESET_COUNT - 1)],
                sel,
                edit);
            break;
        case AUD_BASS:
            snprintf(v, sizeof(v), "%+ld dB", (long)bass);
            ls_ui_level_edit(
                c, y, "Bass", v, clampi(((int)bass + 6) * 100 / 18, 0, 100), sel, edit);
            break;
        case AUD_TREB:
            snprintf(v, sizeof(v), "%+ld dB", (long)treb);
            ls_ui_level_edit(
                c, y, "Treble", v, clampi(((int)treb + 8) * 100 / 16, 0, 100), sel, edit);
            break;
        case AUD_PUNCH:
            snprintf(v, sizeof(v), "%ld", (long)punch);
            ls_ui_level_edit(c, y, "Punch", v, clampi((int)punch, 0, 100), sel, edit);
            break;
        case AUD_RUMBLE:
            if(hp <= 0)
                snprintf(v, sizeof(v), "off");
            else
                snprintf(v, sizeof(v), "%ld Hz", (long)hp);
            ls_ui_row_edit(c, y, "Rumble cut", v, sel, edit);
            break;
        case AUD_LOUD:
            ls_ui_row_edit(
                c,
                y,
                "Loudness",
                EQ_LOUD_NAMES[clampi((int)loud, 0, EQ_LOUD_COUNT - 1)],
                sel,
                edit);
            break;
        case AUD_GR: {
            int gr = t->eq_gr_db10;
            if(gr > 0)
                snprintf(v, sizeof(v), "-%d.%d dB", gr / 10, gr % 10);
            else
                snprintf(v, sizeof(v), "0.0 dB");
            ls_ui_row(c, y, "Limiter", v, sel);
            break;
        }
        case AUD_TESTS:
            ls_ui_divider_row(c, y, "TESTS");
            break;
        default:
            if(i >= AUD_TEST0 && i < AUD_TEST0 + EQ_TEST_COUNT) {
                ls_ui_row(c, y, EQ_TEST_LABELS[i - AUD_TEST0], "play", sel);
            }
            break;
        }
    }

    elements_scrollbar(c, app->focus, AUD_COUNT);
}

typedef enum {
    LNK_TRANSPORT,
    LNK_BT_RADIO,
    LNK_PAIR,
    LNK_TEL,
    LNK_PING,
    LNK_COUNT,
} LsLinkRow;

static void draw_set_link(Canvas* c, LsApp* app) {
    canvas_set_font(c, FontSecondary);

    const int rows = (body_bottom(c) - body_top()) / LS_ROW_H;
    ls_ui_scroll(&app->list_top, app->focus, LNK_COUNT, rows);

    char v[24];
    for(int r = 0; r < rows; r++) {
        int i = app->list_top + r;
        if(i >= LNK_COUNT) break;
        const char* label = "";
        v[0] = '\0';

        switch(i) {
        case LNK_TRANSPORT:
            label = "Transport";
            snprintf(
                v,
                sizeof(v),
                "%s",
                ls_link_transport(app->link) == LsTransportBle ? "Bluetooth" : "UART");
            break;
        case LNK_BT_RADIO:
            label = "BT radio";
            snprintf(v, sizeof(v), "%s", furi_hal_bt_is_active() ? "on" : "OFF");
            break;
        case LNK_PAIR:
            label = "Status";
            snprintf(v, sizeof(v), "%s", ls_link_state_str(app->link));
            break;
        case LNK_TEL:
            label = "Rate";
            snprintf(v, sizeof(v), "%d Hz", app->cfg.tel_hz);
            break;
        case LNK_PING:
            label = "Send PING";
            break;
        default:
            break;
        }
        ls_ui_row_edit(
            c,
            body_top() + r * LS_ROW_H,
            label,
            v,
            i == app->focus,
            app->editing && i == app->focus);
    }
    elements_scrollbar(c, app->focus, LNK_COUNT);

    const char* hint;
    switch(ls_link_state(app->link)) {
    case LsStateBleFailed:
        hint = "Turn BT on: Settings>Bluetooth";
        break;
    case LsStateBleAdvertising:
        hint = "On the P4 run:  ble on";
        break;
    case LsStateBleConnected:
        hint = "connected, waiting for data";
        break;
    case LsStateUartWaiting:
        hint = "Check pins 13/14 + GND";
        break;
    case LsStateBleStarting:
        hint = "starting BLE core...";
        break;
    default:

        hint = "";
        break;
    }
    draw_status_line(c, hint);
}

typedef enum {
    DEV_UPTIME,
    DEV_MEM,
    DEV_SDR,
    DEV_SYS,
    DEV_TESTSND,
    DEV_SDR_RESET,
    DEV_SDR_RECOVER,
    DEV_SDR_POWER,
    DEV_C6_RESET,
    DEV_C6_UP,
    DEV_BLE_OFF,
    DEV_REBOOT,
    DEV_COUNT,
} LsDevRow;

static void draw_set_device(Canvas* c, LsApp* app) {
    LsTelemetry* t = &app->tel;
    canvas_set_font(c, FontSecondary);

    const int rows = (body_full(c) - body_top()) / LS_ROW_H;
    ls_ui_scroll(&app->list_top, app->focus, DEV_COUNT, rows);

    char v[24];
    for(int r = 0; r < rows; r++) {
        int i = app->list_top + r;
        if(i >= DEV_COUNT) break;
        const char* label = "";
        v[0] = '\0';

        switch(i) {
        case DEV_UPTIME: {
            label = "Uptime";
            uint32_t s = t->uptime_s;
            if(s >= 3600)
                snprintf(v, sizeof(v), "%luh%lum", (unsigned long)(s / 3600),
                         (unsigned long)((s % 3600) / 60));
            else
                snprintf(v, sizeof(v), "%lum%lus", (unsigned long)(s / 60),
                         (unsigned long)(s % 60));
            break;
        }
        case DEV_MEM:
            label = "Free int/dma";
            snprintf(
                v,
                sizeof(v),
                "%lu/%lu",
                (unsigned long)t->free_internal,
                (unsigned long)t->free_dma);
            break;
        case DEV_SDR:
            label = "SDR";

            if(!t->rtl_ready) {
                snprintf(v, sizeof(v), "DOWN");
            } else if(t->sdr_stall_s > 0) {
                snprintf(v, sizeof(v), "STALL %lds", (long)t->sdr_stall_s);
            } else if(t->rtl_health[0] && strcmp(t->rtl_health, "ok") != 0) {
                snprintf(v, sizeof(v), "%s", t->rtl_health);
            } else {
                snprintf(v, sizeof(v), "ready");
            }
            break;
        case DEV_SYS:
            label = "Read SYS info";
            break;
        case DEV_TESTSND:
            label = "Test sound";
            break;
        case DEV_SDR_RESET:
            label = "Restart SDR";
            break;
        case DEV_SDR_RECOVER:
            label = "Recover SDR (USB)";
            break;
        case DEV_SDR_POWER:
            label = "Power-cycle SDR";
            break;
        case DEV_C6_RESET:
            label = "Reset ESP32-C6";
            break;
        case DEV_C6_UP:
            label = "C6 handshake";
            break;
        case DEV_BLE_OFF:
            label = "Radio BLE off";
            break;
        case DEV_REBOOT:
            label = "Reboot radio";
            break;
        default:
            break;
        }
        ls_ui_row(c, body_top() + r * LS_ROW_H, label, v, i == app->focus);
    }
    elements_scrollbar(c, app->focus, DEV_COUNT);
}

typedef enum {
    DSP_BOOT,
    DSP_ORIENT,
    DSP_STEP,
    DSP_RXWAKE,
    DSP_COUNT,
} LsDispRow;

static void draw_set_display(Canvas* c, LsApp* app) {
    canvas_set_font(c, FontSecondary);

    char v[20];
    int y = body_top();

#define DSP_EDIT(row) (app->editing && app->focus == (row))

    ls_ui_row_edit(
        c,
        y,
        "Boot into",
        LS_BOOT_NAMES[app->cfg.boot_app],
        app->focus == DSP_BOOT,
        DSP_EDIT(DSP_BOOT));
    y += LS_ROW_H;
    ls_ui_row_edit(
        c,
        y,
        "Screen",
        LS_ORIENT_NAMES[app->cfg.orient],
        app->focus == DSP_ORIENT,
        DSP_EDIT(DSP_ORIENT));
    y += LS_ROW_H;
    snprintf(v, sizeof(v), "%s", STEP_NAMES[app->cfg.step_idx]);
    ls_ui_row_edit(c, y, "Tune step", v, app->focus == DSP_STEP, DSP_EDIT(DSP_STEP));
    y += LS_ROW_H;
    ls_ui_row_edit(
        c,
        y,
        "RX wakes screen",
        app->cfg.rx_wake ? "on" : "off",
        app->focus == DSP_RXWAKE,
        DSP_EDIT(DSP_RXWAKE));
    y += LS_ROW_H;

#undef DSP_EDIT

    if(app->cfg.boot_app != LsBootLauncher && y + LS_ROW_H * 2 <= body_full(c)) {
        canvas_draw_str(c, 3, y + 8, "Opens that app at start;");
        canvas_draw_str(c, 3, y + 18, "waits for the link first.");
    }
}

static void draw_set_about(Canvas* c, LsApp* app) {
    canvas_set_font(c, FontSecondary);

    char v[24];
    int y = body_top();

    ls_ui_row(c, y, "Radio", app->tel.mode_name[0] ? app->tel.mode_name : "-", false);
    y += LS_ROW_H;
    ls_ui_row(c, y, "Transport", ls_link_port_name(app->link), false);
    y += LS_ROW_H;
    uint32_t frames = 0;
    ls_link_stats(app->link, &frames, NULL, NULL);
    snprintf(v, sizeof(v), "%lu", (unsigned long)frames);
    ls_ui_row(c, y, "Frames", v, false);
    y += LS_ROW_H;

    snprintf(v, sizeof(v), "%d Hz", app->cfg.tel_hz);
    ls_ui_row(c, y, "Telemetry", v, false);
    y += LS_ROW_H;
    if(y + LS_ROW_H <= body_full(c)) {
        snprintf(v, sizeof(v), "%d", app->mem_count);
        ls_ui_row(c, y, "Memories", v, false);
    }
}

static void edit_begin(LsApp* app) {
    uint32_t hz = app->tel.freq_hz;
    if(hz < 1000000) hz = 851012500;
    if(hz > 999999999u) hz = 999999999u;
    snprintf(app->edit, sizeof(app->edit), "%09lu", (unsigned long)hz);
    app->edit_pos = 0;
    app->screen = LsScreenEdit;
}

static void draw_edit(Canvas* c, LsApp* app) {
    const int w = canvas_width(c);
    const bool port = ls_ui_portrait(c);

    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(
        c, w / 2, body_top() + 8, AlignCenter, AlignBottom, port ? "FREQ MHz" : "SET FREQUENCY (MHz)");

    const int cw = port ? 6 : 11;
    const int dot_gap = port ? 3 : 5;
    const int base = body_top() + (port ? 30 : 26);
    canvas_set_font(c, port ? FontPrimary : FontBigNumbers);

    int total = 9 * cw + dot_gap + 2;
    int x = (w - total) / 2;
    if(x < 1) x = 1;

    for(int i = 0; i < 9; i++) {
        char ch[2] = {app->edit[i], 0};
        canvas_draw_str(c, x, base, ch);
        if(i == app->edit_pos) canvas_draw_box(c, x, base + 3, cw - 2, 2);
        x += cw;
        if(i == 2) {
            canvas_draw_box(c, x, base - 3, 2, 3);
            x += dot_gap;
        }
    }
}

static uint32_t edit_value(LsApp* app) {
    return (uint32_t)strtoul(app->edit, NULL, 10);
}

static const char* page_title(LsApp* app) {
    switch(cur_page(app)) {
    case PG_VFO:
        return APPS[app->app].name;
    case PG_SIGNAL:
        return "SIGNAL";
    case PG_CALL:
        return "CALL";
    case PG_MEM:
        return "MEMORY";
    case PG_DIAG:
        return "DIAG";
    case PG_FM_SCAN:
        return "SCAN";
    case PG_POC:
        return "POCSAG";
    case PG_POC_LOG:
        return "PAGES";
    case PG_TRAFFIC:
        return "TRAFFIC";
    case PG_AIRCRAFT:
        return "AIRCRAFT";
    case PG_ADSB_STAT:
        return "STATS";
    /**/
    case PG_ADSB_MAP:
        return "MAP";
    case PG_REC:
        return "RECORD";
    case PG_REC_SIG:
        return "SIGNAL";
    case PG_REC_CAP:
        return "CAPTURE";
    }
    return "?";
}

static void draw_app_page(Canvas* c, LsApp* app) {
    switch(cur_page(app)) {
    case PG_VFO:
        draw_vfo(c, app);
        break;
    case PG_SIGNAL:
        draw_signal(c, app);
        break;
    case PG_CALL:
        draw_call(c, app);
        break;
    case PG_MEM:
        draw_mem(c, app);
        break;
    case PG_DIAG:
        draw_diag(c, app);
        break;
    case PG_FM_SCAN:
        draw_fm_scan(c, app);
        break;
    case PG_POC:
        draw_poc(c, app);
        break;
    case PG_POC_LOG:
        draw_poc_log(c, app);
        break;
    case PG_TRAFFIC:
        draw_traffic(c, app);
        break;
    case PG_AIRCRAFT:
        draw_aircraft(c, app);
        break;
    case PG_ADSB_STAT:
        draw_adsb_stat(c, app);
        break;
    /**/
    case PG_ADSB_MAP:
        draw_adsb_map(c, app);
        break;
    case PG_REC:
        draw_rec(c, app);
        break;
    case PG_REC_SIG:
        draw_rec_sig(c, app);
        break;
    case PG_REC_CAP:
        draw_rec_cap(c, app);
        break;
    }
}

static void draw_settings_page(Canvas* c, LsApp* app) {
    switch(app->set_page) {
    case SET_LEVELS:
        draw_set_levels(c, app);
        break;
    case SET_AUDIO:
        draw_set_audio(c, app);
        break;
    case SET_LINK:
        draw_set_link(c, app);
        break;
    case SET_DEVICE:
        draw_set_device(c, app);
        break;
    case SET_DISPLAY:
        draw_set_display(c, app);
        break;
    case SET_ABOUT:
        draw_set_about(c, app);
        break;
    default:
        break;
    }
}

static void draw_cb(Canvas* c, void* ctx) {
    LsApp* app = ctx;
    furi_mutex_acquire(app->lock, FuriWaitForever);

    canvas_clear(c);

    switch(app->screen) {
    case LsScreenLauncher:
        ls_ui_header(c, "LAKESHARK", 0, 1, app->link_up, transport_badge(app));
        draw_launcher(c, app);
        break;
    case LsScreenApp:
        ls_ui_header(
            c,
            page_title(app),
            app->page,
            APPS[app->app].n_pages,
            app->link_up,
            transport_badge(app));
        draw_app_page(c, app);
        break;
    case LsScreenSettings:
        ls_ui_header(
            c,
            SET_TITLES[app->set_page],
            app->set_page,
            SET_COUNT,
            app->link_up,
            transport_badge(app));
        draw_settings_page(c, app);
        break;
    case LsScreenEdit:
        ls_ui_header(c, "FREQ", 0, 1, app->link_up, transport_badge(app));
        draw_edit(c, app);
        break;
    }

    if(app->flash_until && furi_get_tick() < app->flash_until) {
        canvas_set_color(c, ColorXOR);
        canvas_draw_box(c, 0, 0, canvas_width(c), canvas_height(c));
        canvas_set_color(c, ColorBlack);
    }

    if(modal_active(app)) {
        ls_ui_modal(c, app->modal_title, app->modal_l1, app->modal_l2);
    } else if(app->toast[0] && furi_get_tick() < app->toast_until) {
        ls_ui_toast(c, app->toast);
    }

    furi_mutex_release(app->lock);
}

static void input_cb(InputEvent* event, void* ctx) {
    LsApp* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

static int gain_index_of(int tenths) {
    for(int i = 0; i < N_GAINS; i++)
        if(GAINS[i] == tenths) return i;
    return -1;
}

static void adjust_gain(LsApp* app, int dir) {
    int cur = (int)echo_get(app, ECHO_GAIN, app->tel.gain_tenths);
    int idx = gain_index_of(cur);
    if(idx < 0) {

        idx = 0;
        for(int i = 0; i < N_GAINS; i++)
            if(abs(GAINS[i] - cur) < abs(GAINS[idx] - cur)) idx = i;
    }
    idx = clampi(idx + dir, 0, N_GAINS - 1);
    ls_link_send(app->link, "GAIN %d", GAINS[idx]);
    echo_set(app, ECHO_GAIN, GAINS[idx]);
}

static void adjust_level(LsApp* app, LsLevel which, int dir) {
    LsTelemetry* t = &app->tel;
    switch(which) {
    case LVL_VOL: {
        int v = clampi((int)echo_get(app, ECHO_VOL, t->volume) + 5 * dir, 0, 100);
        ls_link_send(app->link, "VOL %d", v);
        echo_set(app, ECHO_VOL, v);
        break;
    }
    case LVL_GAIN:
        adjust_gain(app, dir);
        break;
    case LVL_SQL: {
        int v = clampi((int)echo_get(app, ECHO_SQL, t->squelch_tenths) + 5 * dir, 0, 100);
        ls_link_send(app->link, "SQL %d", v);
        echo_set(app, ECHO_SQL, v);
        break;
    }
    case LVL_VGATE: {
        int v = clampi((int)echo_get(app, ECHO_VGATE, t->voice_gate) + dir, 6, 99);
        ls_link_send(app->link, "VGATE %d", v);
        echo_set(app, ECHO_VGATE, v);
        break;
    }
    default:
        break;
    }
}

static void eq_cycle_preset(LsApp* app, int dir) {
    int p = (int)echo_get(app, ECHO_EQ_PRESET, app->tel.eq_preset);
    p = (p + dir + EQ_PRESET_COUNT) % EQ_PRESET_COUNT;
    ls_link_send(app->link, "EQ %s", EQ_PRESET_NAMES[p]);
    echo_set(app, ECHO_EQ_PRESET, p);
    for(int i = ECHO_EQ_HP; i <= ECHO_EQ_LOUD; i++) app->echo[i].active = false;
}

static void adjust_audio(LsApp* app, int dir) {
    LsTelemetry* t = &app->tel;

    switch(app->focus) {
    case AUD_PRESET:
        eq_cycle_preset(app, dir);
        break;
    case AUD_BASS: {
        int v = clampi((int)echo_get(app, ECHO_EQ_BASS, t->eq_bass_db) + dir, -6, 12);
        ls_link_send(app->link, "EQ BASS %d", v);
        echo_set(app, ECHO_EQ_BASS, v);
        echo_set(app, ECHO_EQ_PRESET, EQ_PRESET_COUNT - 1);
        break;
    }
    case AUD_TREB: {
        int v = clampi((int)echo_get(app, ECHO_EQ_TREB, t->eq_treb_db) + dir, -8, 8);
        ls_link_send(app->link, "EQ TREB %d", v);
        echo_set(app, ECHO_EQ_TREB, v);
        echo_set(app, ECHO_EQ_PRESET, EQ_PRESET_COUNT - 1);
        break;
    }
    case AUD_PUNCH: {
        int v = clampi((int)echo_get(app, ECHO_EQ_PUNCH, t->eq_punch) + 5 * dir, 0, 100);
        ls_link_send(app->link, "EQ PUNCH %d", v);
        echo_set(app, ECHO_EQ_PUNCH, v);
        echo_set(app, ECHO_EQ_PRESET, EQ_PRESET_COUNT - 1);
        break;
    }
    case AUD_RUMBLE: {
        int idx = eq_hp_index((int)echo_get(app, ECHO_EQ_HP, t->eq_hp_hz));
        idx = clampi(idx + dir, 0, EQ_HP_COUNT - 1);
        ls_link_send(app->link, "EQ HP %d", EQ_HP_HZ[idx]);
        echo_set(app, ECHO_EQ_HP, EQ_HP_HZ[idx]);
        echo_set(app, ECHO_EQ_PRESET, EQ_PRESET_COUNT - 1);
        break;
    }
    case AUD_LOUD: {
        int v = clampi((int)echo_get(app, ECHO_EQ_LOUD, t->eq_loud) + dir, 0, EQ_LOUD_COUNT - 1);
        ls_link_send(app->link, "EQ LOUD %d", v);
        echo_set(app, ECHO_EQ_LOUD, v);
        echo_set(app, ECHO_EQ_PRESET, EQ_PRESET_COUNT - 1);
        break;
    }
    default:
        break;
    }
}

static void vfo_tune(LsApp* app, int dir) {
    ls_link_send(app->link, "TUNE %ld", (long)((int32_t)STEPS[app->cfg.step_idx] * dir));
}

static bool vfo_row_is_value(int kind) {
    return kind != VFO_MUTE;
}

static void vfo_adjust(LsApp* app, int kind, int dir) {
    switch(kind) {
    case VFO_FREQ:
        vfo_tune(app, dir);
        break;
    case VFO_STEP:
        app->cfg.step_idx = (app->cfg.step_idx + dir + N_STEPS) % N_STEPS;
        ls_cfg_save(&app->cfg);
        break;
    case VFO_VOL:
        adjust_level(app, LVL_VOL, dir);
        break;
    case VFO_GAIN:
        adjust_gain(app, dir);
        break;
    case VFO_SQL:
        adjust_level(app, LVL_SQL, dir);
        break;
    case VFO_VGATE:
        adjust_level(app, LVL_VGATE, dir);
        break;
    case VFO_EQ:
        eq_cycle_preset(app, dir);
        break;
    default:
        break;
    }
}

static void rec_adjust(LsApp* app, int row, int dir) {
    LsTelemetry* t = &app->tel;

    switch(row) {
    case REC_ROW_FREQ:
        vfo_tune(app, dir);
        break;
    case REC_ROW_GAIN:
        adjust_gain(app, dir);
        break;
    case REC_ROW_THRESH: {
        int v = clampi((int)echo_get(app, ECHO_REC_THRESH, t->rec_thresh_fixed) + 2 * dir, 0, 254);
        ls_link_send(app->link, "REC THRESH %d", v);
        echo_set(app, ECHO_REC_THRESH, v);
        break;
    }
    case REC_ROW_GAP: {
        int cur = (int)echo_get(app, ECHO_REC_GAP, t->rec_gap_ms);
        int step = cur >= 100 ? 20 : 5;
        int v = clampi(cur + step * dir, 2, 2000);
        ls_link_send(app->link, "REC GAP %d", v);
        echo_set(app, ECHO_REC_GAP, v);
        break;
    }
    case REC_ROW_BW: {
        int cur = (int)echo_get(app, ECHO_REC_BW, (int32_t)(t->rec_bw_hz / 1000));
        int v = clampi(cur + 50 * dir, 0, 2000);
        ls_link_send(app->link, "REC BW %d", v * 1000);
        echo_set(app, ECHO_REC_BW, v);
        break;
    }
    case REC_ROW_MINP: {
        int cur = (int)echo_get(app, ECHO_REC_MINP, (int32_t)t->rec_min_pulse_us);
        int step = cur >= 100 ? 20 : 4;
        int v = clampi(cur + step * dir, 4, 10000);
        ls_link_send(app->link, "REC MINP %d", v);
        echo_set(app, ECHO_REC_MINP, v);
        break;
    }
    case REC_ROW_MAXSPAN: {
        int cur = (int)echo_get(app, ECHO_REC_MAXSPAN, (int32_t)t->rec_max_span_ms);
        int v = clampi(cur + 500 * dir, 10, 30000);
        ls_link_send(app->link, "REC MAXSPAN %d", v);
        echo_set(app, ECHO_REC_MAXSPAN, v);
        break;
    }
    case REC_ROW_MINEDGES: {
        int cur = (int)echo_get(app, ECHO_REC_MINEDGES, t->rec_min_edges);
        int v = clampi(cur + dir, 2, 64);
        ls_link_send(app->link, "REC MINEDGES %d", v);
        echo_set(app, ECHO_REC_MINEDGES, v);
        break;
    }
    default:
        break;
    }
}

static void rec_toggle_arm(LsApp* app) {
    if(!app->link_up) {
        toast(app, "No radio");
        return;
    }
    if(app->tel.rec_phase == LsRecArmed || app->tel.rec_phase == LsRecCapturing) {
        ls_link_send(app->link, "REC STOP");
        toast(app, "Disarmed");
    } else {
        rec_preview_clear(app);
        ls_link_send(app->link, "REC ARM");
        toast(app, "Armed - transmit now");
    }
}

static void vfo_action(LsApp* app, int kind) {
    if(kind == VFO_MUTE) {
        ls_link_send(app->link, "MUTE");
        toast(app, app->tel.muted ? "Unmute" : "Mute");
    }
}

static void device_action(LsApp* app, int row) {
    switch(row) {
    case DEV_SYS:
        ls_link_send(app->link, "SYS");
        toast(app, "SYS requested");
        break;
    case DEV_TESTSND:
        ls_link_send(app->link, "BEEP NOW");
        toast(app, "Test sound sent");
        break;
    case DEV_SDR_RESET:
        ls_link_send(app->link, "SDR reset");
        modal_set(app, "SDR", "restarting stream", NULL, 2500);
        break;
    case DEV_SDR_RECOVER:

        ls_link_send(app->link, "SDR recover");
        modal_set(app, "SDR", "reopening USB", NULL, 4000);
        break;
    case DEV_SDR_POWER:

        ls_link_send(app->link, "SDR power");
        modal_set(app, "SDR POWER", "cycling + reboot", "~15s", 18000);
        break;
    case DEV_C6_RESET:
        ls_link_send(app->link, "C6 reset");
        modal_set(app, "ESP32-C6", "resetting", "BLE will drop", 3000);
        break;
    case DEV_C6_UP:
        ls_link_send(app->link, "C6 up");
        modal_set(app, "ESP32-C6", "handshaking", NULL, 3000);
        break;
    case DEV_BLE_OFF:

        ls_link_send(app->link, "BLE off");
        toast(app, "Radio BLE off");
        break;
    case DEV_REBOOT:
        ls_link_send(app->link, "REBOOT");
        modal_set(app, "REBOOTING", "radio restarting", "~10s", 12000);
        break;
    default:

        toast(app, "Live from telemetry");
        break;
    }
}

static bool settings_row_is_value(LsApp* app) {
    switch(app->set_page) {
    case SET_LEVELS:
        return app->focus >= 0 && app->focus < LVL_COUNT;
    case SET_AUDIO:
        return app->focus >= 0 && app->focus < AUD_GR;
    case SET_LINK:
        return app->focus == LNK_TEL;
    case SET_DISPLAY:
        return app->focus >= 0 && app->focus < DSP_COUNT;
    default:

        return false;
    }
}

static void settings_adjust(LsApp* app, int dir) {
    switch(app->set_page) {
    case SET_LEVELS:
        adjust_level(app, (LsLevel)app->focus, dir);
        break;

    case SET_AUDIO:
        adjust_audio(app, dir);
        break;

    case SET_LINK:
        if(app->focus == LNK_TEL) {

            int hz = app->cfg.tel_hz + dir;
            if(hz > 20) hz = 1;
            if(hz < 1) hz = 20;
            app->cfg.tel_hz = hz;
            ls_link_send(app->link, "TEL %d", app->cfg.tel_hz);
            ls_cfg_save(&app->cfg);
        }
        break;

    case SET_DISPLAY:
        switch(app->focus) {
        case DSP_BOOT:
            app->cfg.boot_app =
                (LsBootApp)(((int)app->cfg.boot_app + dir + LsBootCount) % LsBootCount);
            break;
        case DSP_ORIENT:
            app->cfg.orient =
                (LsOrient)(((int)app->cfg.orient + dir + LsOrientCount) % LsOrientCount);
            apply_orientation(app);
            break;
        case DSP_STEP:
            app->cfg.step_idx = (app->cfg.step_idx + dir + N_STEPS) % N_STEPS;
            break;
        case DSP_RXWAKE:

            app->cfg.rx_wake = !app->cfg.rx_wake;
            break;
        default:
            return;
        }
        ls_cfg_save(&app->cfg);
        break;

    default:
        break;
    }
}

static void handle_launcher(LsApp* app, InputEvent* ev, bool press) {
    const int n = LsRadioCount + 1;

    if(press && ev->key == InputKeyUp) {
        app->focus = (app->focus + n - 1) % n;
    } else if(press && ev->key == InputKeyDown) {
        app->focus = (app->focus + 1) % n;
    } else if(ev->type == InputTypeShort && ev->key == InputKeyOk) {
        if(app->focus < LsRadioCount) {
            enter_app(app, (LsRadioApp)app->focus);
        } else {
            app->screen = LsScreenSettings;
            app->set_page = SET_LEVELS;
            app->focus = 0;
            app->list_top = 0;
            app->editing = false;
        }
    } else if(ev->type == InputTypeShort && ev->key == InputKeyBack) {

        app->running = false;
    }
}

static void handle_app(LsApp* app, InputEvent* ev, bool press) {
    const LsAppDef* d = &APPS[app->app];
    LsPage pg = cur_page(app);

    if(pg == PG_VFO && app->editing) {
        uint8_t rows[VFO_ROW_MAX];
        const int nrows = vfo_rows(app, rows);
        const int kind = rows[clampi(app->focus, 0, nrows - 1)];

        bool handled = true;
        if(press && (ev->key == InputKeyUp || ev->key == InputKeyRight)) {
            vfo_adjust(app, kind, +1);
        } else if(press && (ev->key == InputKeyDown || ev->key == InputKeyLeft)) {
            vfo_adjust(app, kind, -1);
        } else if(
            ev->type == InputTypeShort && (ev->key == InputKeyOk || ev->key == InputKeyBack)) {
            app->editing = false;
        } else if(ev->type == InputTypeLong && ev->key == InputKeyOk) {
            app->editing = false;
            if(kind == VFO_FREQ) edit_begin(app);
        } else {
            handled = false;
        }
        if(handled) return;
    }

    /**/
    if(pg == PG_ADSB_STAT && app->editing) {
        bool handled = true;
        if(press && (ev->key == InputKeyUp || ev->key == InputKeyRight)) {
            adsb_ttsvol_adjust(app, +10);
        } else if(press && (ev->key == InputKeyDown || ev->key == InputKeyLeft)) {
            adsb_ttsvol_adjust(app, -10);
        } else if(
            ev->type == InputTypeShort && (ev->key == InputKeyOk || ev->key == InputKeyBack)) {
            app->editing = false;
        } else {
            handled = false;
        }
        if(handled) return;
    }

    if(pg == PG_REC && app->editing) {
        bool handled = true;
        if(press && (ev->key == InputKeyUp || ev->key == InputKeyRight)) {
            rec_adjust(app, app->focus, +1);
        } else if(press && (ev->key == InputKeyDown || ev->key == InputKeyLeft)) {
            rec_adjust(app, app->focus, -1);
        } else if(
            ev->type == InputTypeShort && (ev->key == InputKeyOk || ev->key == InputKeyBack)) {
            app->editing = false;
        } else if(ev->type == InputTypeLong && ev->key == InputKeyOk) {
            app->editing = false;
            if(app->focus == REC_ROW_FREQ) edit_begin(app);
        } else {
            handled = false;
        }
        if(handled) return;
    }

    if(press && ev->key == InputKeyRight) {
        app->page = (app->page + 1) % d->n_pages;
        app->focus = 0;
        app->list_top = 0;
        app->editing = false;
        return;
    }
    if(press && ev->key == InputKeyLeft) {
        app->page = (app->page + d->n_pages - 1) % d->n_pages;
        app->focus = 0;
        app->list_top = 0;
        app->editing = false;
        return;
    }
    if(ev->type == InputTypeShort && ev->key == InputKeyBack) {
        app->screen = LsScreenLauncher;
        app->focus = (int)app->app;
        app->list_top = 0;
        app->editing = false;
        return;
    }

    const bool up = press && ev->key == InputKeyUp;
    const bool down = press && ev->key == InputKeyDown;
    const bool ok = ev->type == InputTypeShort && ev->key == InputKeyOk;
    const bool ok_long = ev->type == InputTypeLong && ev->key == InputKeyOk;

    switch(pg) {
    case PG_VFO: {
        uint8_t rows[VFO_ROW_MAX];
        const int nrows = vfo_rows(app, rows);
        if(up) app->focus = (app->focus + nrows - 1) % nrows;
        if(down) app->focus = (app->focus + 1) % nrows;

        const int kind = rows[clampi(app->focus, 0, nrows - 1)];
        if(ok) {
            if(vfo_row_is_value(kind)) {
                app->editing = true;
            } else {
                vfo_action(app, kind);
            }
        }
        if(ok_long && kind == VFO_FREQ) edit_begin(app);
        break;
    }

    case PG_SIGNAL:

        if(up) ls_link_send(app->link, "TUNE %ld", (long)STEPS[app->cfg.step_idx]);
        if(down) ls_link_send(app->link, "TUNE -%ld", (long)STEPS[app->cfg.step_idx]);
        /**/
        if(ok) {
            int n_scope = 0;
            scope_table(app, &n_scope);
            app->focus = (app->focus + 1) % n_scope;
        }
        if(ok_long) {
            memset(&app->hist, 0, sizeof(app->hist));
            toast(app, "History cleared");
        }
        break;

    case PG_CALL:
        if(ok) ls_link_send(app->link, "DEMOD CYCLE");
        if(up) ls_link_send(app->link, "POL");
        if(down) ls_link_send(app->link, "POL");
        if(ok_long) {
            ls_link_send(app->link, "RESET");
            toast(app, "Stats reset");
        }
        break;

    case PG_MEM: {
        int n = mem_rows(app);
        int idx = 0;

        if(up || down) {
            int dir = up ? -1 : +1;
            app->focus = (app->focus + n + dir) % n;

            if(mem_row_kind(app, app->focus, &idx) == MEM_ROW_DIV) {
                app->focus = (app->focus + n + dir) % n;
            }
        }

        if(ok) {
            const LsMem* m = NULL;
            switch(mem_row_kind(app, app->focus, &idx)) {
            case MEM_ROW_SAVE:
                mem_add_current(app);
                break;
            case MEM_ROW_MEM:
                m = &app->mem[idx];
                break;
            case MEM_ROW_PRESET:
                m = &app->preset[idx];
                break;
            case MEM_ROW_DIV:
                break;
            }
            if(m) {
                ls_link_send(app->link, "FREQ %lu", (unsigned long)m->freq_hz);
                toast(app, "Tuned %s", m->name);
            }
        }

        if(ok_long) {
            switch(mem_row_kind(app, app->focus, &idx)) {
            case MEM_ROW_MEM:
                mem_delete(app, idx);

                if(app->focus >= mem_rows(app)) app->focus = mem_rows(app) - 1;
                if(mem_row_kind(app, app->focus, &idx) == MEM_ROW_DIV) app->focus++;
                break;
            case MEM_ROW_PRESET:

                if(mem_add(app, app->preset[idx].freq_hz, app->preset[idx].name)) {
                    app->focus++;
                }
                break;
            default:
                break;
            }
        }
        break;
    }

    case PG_DIAG:
        if(ok) ls_link_send(app->link, "PING");
        if(ok_long) ls_link_selftest(app->link);
        break;

    case PG_FM_SCAN:
        if(ok) {
            ls_link_send(app->link, "SCAN");
            toast(app, "Scan restarted");
        }
        if(ok_long) {
            ls_link_send(app->link, "PEAK");
            toast(app, "Tuned to peak");
        }
        break;

    case PG_POC:
        if(ok) app->focus = (app->focus + 1) % 2;
        if(up || down) {
            int dir = up ? +1 : -1;
            if(app->focus == 0) {

                static const int BAUDS[] = {0, 512, 1200, 2400};
                int cur = app->tel.pocsag_auto ? 0 : app->tel.pocsag_baud;
                int idx = 0;
                for(int i = 0; i < 4; i++)
                    if(BAUDS[i] == cur) idx = i;
                idx = clampi(idx + dir, 0, 3);
                ls_link_send(app->link, "BAUD %d", BAUDS[idx]);
            } else {
                adjust_level(app, LVL_SQL, dir);
            }
        }
        break;

    case PG_POC_LOG:
        if(up && app->poc_count) app->focus = (app->focus + app->poc_count - 1) % app->poc_count;
        if(down && app->poc_count) app->focus = (app->focus + 1) % app->poc_count;
        if(ok_long) {
            app->poc_count = 0;
            app->focus = 0;
            app->poc_last_addr = 0;
            app->poc_last_text[0] = '\0';
            toast(app, "Log cleared");
        }
        break;

    case PG_TRAFFIC:
    case PG_AIRCRAFT: {
        int n = adsb_visible(app);
        if(n > 0) {
            if(up) app->focus = (app->focus + n - 1) % n;
            if(down) app->focus = (app->focus + 1) % n;
        }
        if(ok && pg == PG_TRAFFIC) {

            for(int i = 0; i < d->n_pages; i++) {
                if(d->pages[i] == PG_AIRCRAFT) app->page = i;
            }
        }
        break;
    }

    /**/
    case PG_ADSB_MAP: {
        int n = adsb_positioned(app);
        if(n > 0) {
            if(up) app->focus = (app->focus + n - 1) % n;
            if(down) app->focus = (app->focus + 1) % n;
        }
        break;
    }

    /**/
    case PG_ADSB_STAT:
        if(up) app->focus = (app->focus + ADSB_STAT_ROWS - 1) % ADSB_STAT_ROWS;
        if(down) app->focus = (app->focus + 1) % ADSB_STAT_ROWS;
        /**/
        if(ok && app->focus == ADSB_ROW_TTSVOL) app->editing = true;
        if(ok_long) {
            if(app->focus == ADSB_ROW_TTSVOL) {
                ls_link_send(app->link, "TTSVOL 100");
                echo_set(app, ECHO_TTSVOL, 100);
                toast(app, "TTS volume 100%");
            } else {
                ls_link_send(app->link, "GAIN AUTO");
                toast(app, "Gain -> auto");
            }
        }
        break;

    case PG_REC:
        if(up) app->focus = (app->focus + REC_ROW_COUNT - 1) % REC_ROW_COUNT;
        if(down) app->focus = (app->focus + 1) % REC_ROW_COUNT;
        if(ok) {
            if(app->focus == REC_ROW_ARM) {
                rec_toggle_arm(app);
            } else {
                app->editing = true;
            }
        }
        if(ok_long) {
            if(app->focus == REC_ROW_FREQ) {
                edit_begin(app);
            } else if(app->focus == REC_ROW_THRESH) {
                ls_link_send(app->link, "REC THRESH 0");
                echo_set(app, ECHO_REC_THRESH, 0);
                toast(app, "Threshold auto");
            } else if(app->focus == REC_ROW_GAIN) {
                ls_link_send(app->link, "GAIN AUTO");
                echo_set(app, ECHO_GAIN, 0);
                toast(app, "Gain -> auto");
            } else if(app->focus == REC_ROW_BW) {
                ls_link_send(app->link, "REC BW 0");
                echo_set(app, ECHO_REC_BW, 0);
                toast(app, "Bandwidth auto");
            }
        }
        break;

    case PG_REC_SIG:
        if(up) ls_link_send(app->link, "TUNE %ld", (long)STEPS[app->cfg.step_idx]);
        if(down) ls_link_send(app->link, "TUNE -%ld", (long)STEPS[app->cfg.step_idx]);
        if(ok) rec_toggle_arm(app);
        if(ok_long) {
            memset(&app->hist, 0, sizeof(app->hist));
            toast(app, "Trace cleared");
        }
        break;

    case PG_REC_CAP:
        if(ok) rec_xfer_start(app);
        if(ok_long) {
            rec_preview_clear(app);
            toast(app, "Preview cleared");
        }
        break;
    }

    if(ev->type == InputTypeLong && ev->key == InputKeyBack) {
        ls_link_send(app->link, "MUTE");
        toast(app, app->tel.muted ? "Unmute" : "Mute");
    }
}

static void handle_settings(LsApp* app, InputEvent* ev, bool press) {
    const bool up = press && ev->key == InputKeyUp;
    const bool down = press && ev->key == InputKeyDown;
    const bool left = press && ev->key == InputKeyLeft;
    const bool right = press && ev->key == InputKeyRight;
    const bool ok = ev->type == InputTypeShort && ev->key == InputKeyOk;
    const bool ok_long = ev->type == InputTypeLong && ev->key == InputKeyOk;
    const bool back = ev->type == InputTypeShort && ev->key == InputKeyBack;

    if(app->editing) {
        if(ok || back) {
            app->editing = false;
            return;
        }
        if(left) settings_adjust(app, -1);
        if(right) settings_adjust(app, +1);

        return;
    }

    if(right) {
        app->set_page = (LsSetPage)((app->set_page + 1) % SET_COUNT);
        app->focus = 0;
        app->list_top = 0;
        app->editing = false;
        return;
    }
    if(left) {
        app->set_page = (LsSetPage)((app->set_page + SET_COUNT - 1) % SET_COUNT);
        app->focus = 0;
        app->list_top = 0;
        app->editing = false;
        return;
    }
    if(back) {
        ls_cfg_save(&app->cfg);
        app->screen = LsScreenLauncher;
        app->focus = LsRadioCount;
        app->list_top = 0;
        app->editing = false;
        return;
    }

    switch(app->set_page) {
    case SET_LEVELS:
        if(up) app->focus = (app->focus + LVL_COUNT - 1) % LVL_COUNT;
        if(down) app->focus = (app->focus + 1) % LVL_COUNT;
        break;
    case SET_AUDIO:
        if(up || down) {
            int dir = up ? -1 : +1;
            app->focus = (app->focus + AUD_COUNT + dir) % AUD_COUNT;
            if(app->focus == AUD_TESTS) app->focus = (app->focus + AUD_COUNT + dir) % AUD_COUNT;
        }
        break;
    case SET_LINK:
        if(up) app->focus = (app->focus + LNK_COUNT - 1) % LNK_COUNT;
        if(down) app->focus = (app->focus + 1) % LNK_COUNT;
        break;
    case SET_DEVICE:
        if(up) app->focus = (app->focus + DEV_COUNT - 1) % DEV_COUNT;
        if(down) app->focus = (app->focus + 1) % DEV_COUNT;
        break;
    case SET_DISPLAY:
        if(up) app->focus = (app->focus + DSP_COUNT - 1) % DSP_COUNT;
        if(down) app->focus = (app->focus + 1) % DSP_COUNT;
        break;
    case SET_ABOUT:
    default:
        break;
    }

    if(ok) {
        if(settings_row_is_value(app)) {
            app->editing = true;
        } else if(app->set_page == SET_LINK) {
            switch(app->focus) {
            case LNK_TRANSPORT: {
                LsTransport want = (ls_link_transport(app->link) == LsTransportBle) ?
                                       LsTransportUart :
                                       LsTransportBle;
                app->pending_transport = (int)want;
                break;
            }
            case LNK_BT_RADIO:

                toast(app, "Settings > Bluetooth");
                break;
            case LNK_PAIR:
            case LNK_PING:
                ls_link_send(app->link, "PING");
                toast(app, "PING sent");
                break;
            default:
                break;
            }
        } else if(app->set_page == SET_DEVICE) {
            device_action(app, app->focus);
        } else if(
            app->set_page == SET_AUDIO && app->focus >= AUD_TEST0 &&
            app->focus < AUD_TEST0 + EQ_TEST_COUNT) {
            int k = app->focus - AUD_TEST0;
            ls_link_send(app->link, "TEST %s", EQ_TEST_NAMES[k]);
            toast(app, "%s", EQ_TEST_LABELS[k]);
        }
    }

    if(ok_long && app->set_page == SET_LEVELS) {
        ls_link_send(app->link, "MUTE");
        toast(app, app->tel.muted ? "Unmute" : "Mute");
    }
}

static void handle_edit(LsApp* app, InputEvent* ev, bool press) {
    if(press && ev->key == InputKeyUp) {
        app->edit[app->edit_pos] = (char)('0' + ((app->edit[app->edit_pos] - '0' + 1) % 10));
    } else if(press && ev->key == InputKeyDown) {
        app->edit[app->edit_pos] = (char)('0' + ((app->edit[app->edit_pos] - '0' + 9) % 10));
    } else if(press && ev->key == InputKeyRight) {
        if(app->edit_pos < 8) app->edit_pos++;
    } else if(press && ev->key == InputKeyLeft) {
        if(app->edit_pos > 0) app->edit_pos--;
    } else if(ev->type == InputTypeShort && ev->key == InputKeyOk) {
        uint32_t hz = edit_value(app);
        if(hz < 1000000u) {
            toast(app, "Too low");
        } else {
            ls_link_send(app->link, "FREQ %lu", (unsigned long)hz);
            app->screen = LsScreenApp;
        }
    } else if(ev->type == InputTypeShort && ev->key == InputKeyBack) {
        app->screen = LsScreenApp;
    }
}

static void handle_input(LsApp* app, InputEvent* ev) {
    bool press = (ev->type == InputTypeShort) || (ev->type == InputTypeRepeat);

    if(modal_active(app)) {
        if(ev->type == InputTypeShort &&
           (ev->key == InputKeyBack || ev->key == InputKeyOk)) {
            if(app->rec_xfer == RecXferActive) rec_xfer_cancel(app, "Transfer cancelled");
            modal_clear(app);
            app->pending_mode[0] = '\0';
        }
        return;
    }

    switch(app->screen) {
    case LsScreenLauncher:
        handle_launcher(app, ev, press);
        break;
    case LsScreenApp:
        handle_app(app, ev, press);
        break;
    case LsScreenSettings:
        handle_settings(app, ev, press);
        break;
    case LsScreenEdit:
        handle_edit(app, ev, press);
        break;
    }
}

int32_t lakeshark_p25_app(void* p) {
    UNUSED(p);

    LsApp* app = malloc(sizeof(LsApp));
    memset(app, 0, sizeof(LsApp));
    app->running = true;
    app->screen = LsScreenLauncher;
    app->pending_transport = -1;

    app->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->notif = furi_record_open(RECORD_NOTIFICATION);

    ls_cfg_load(&app->cfg);

    if(app->cfg.boot_app != LsBootLauncher) {
        app->app = (LsRadioApp)(app->cfg.boot_app - 1);
    }
    mem_migrate_legacy(app);
    mem_load(app);
    preset_load(app);

    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, draw_cb, app);
    view_port_input_callback_set(app->vp, input_cb, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->vp, GuiLayerFullscreen);

    apply_orientation(app);

    app->link = ls_link_alloc(LS_LINK_BAUD_DEFAULT);
    ls_link_send(app->link, "PING");
    ls_link_send(app->link, "TEL %d", app->cfg.tel_hz);

    ls_dbg(
        "--- app start: boot=%d want_ble=%d bt_active=%d ---",
        (int)app->cfg.boot_app,
        app->cfg.want_ble ? 1 : 0,
        furi_hal_bt_is_active() ? 1 : 0);

    if(app->cfg.want_ble) {
        app->pending_transport = (int)LsTransportBle;
        modal_set(app, "BLUETOOTH", "starting radio...", NULL, 0);
    }

    if(app->cfg.boot_app != LsBootLauncher) {
        LsRadioApp which = app->app;
        app->page = 0;
        app->screen = LsScreenApp;
        request_mode(app, which);
        if(!app->cfg.want_ble) {
            modal_set(app, APPS[which].name, "waiting for radio", "Back to cancel", 0);
        }
    }

    uint32_t next_hello = 0;
    uint32_t hello_interval_ms = 0;
    uint32_t next_probe = furi_get_tick() + furi_ms_to_ticks(LS_LINK_PROBE_MS);

    uint32_t selftest_at = furi_get_tick() + furi_ms_to_ticks(5000);
    bool selftested = false;

    InputEvent ev;
    while(app->running) {

        uint32_t wait = app->rec_xfer == RecXferActive ? 10 : REDRAW_MS;
        if(app->flash_until) {
            uint32_t now = furi_get_tick();
            uint32_t left = app->flash_until - now;
            if(now < app->flash_until && left < wait) wait = left;
        }

        FuriStatus st = furi_message_queue_get(app->queue, &ev, wait);

        furi_mutex_acquire(app->lock, FuriWaitForever);
        if(app->flash_until && furi_get_tick() >= app->flash_until) app->flash_until = 0;
        if(st == FuriStatusOk) handle_input(app, &ev);

        if(!selftested && furi_get_tick() >= selftest_at) {
            selftested = true;
            ls_link_selftest(app->link);
            FURI_LOG_I(
                "LsApp",
                "startup: boot=%d transport=%s pending=%d",
                (int)app->cfg.boot_app,
                ls_link_transport(app->link) == LsTransportBle ? "BLE" : "UART",
                app->pending_transport);
        }

        bool had = app->have_tel;
        app->have_tel = ls_link_get(app->link, &app->tel);
        app->link_up = ls_link_is_up(app->link);
        if(!had && app->have_tel) toast(app, "Linked to LakeShark");

        if(app->have_tel) poc_ingest(app);

        rec_xfer_tick(app);

        uint32_t frames = 0;
        ls_link_stats(app->link, &frames, NULL, NULL);
        if(app->have_tel && frames != app->last_frames) {
            app->last_frames = frames;
            hist_push(app);

            if(!app->cfg.ble_proven && ls_link_transport(app->link) == LsTransportBle) {
                app->cfg.ble_proven = true;
                if(!app->cfg.want_ble) {
                    app->cfg.want_ble = true;
                    ls_cfg_save(&app->cfg);
                    toast(app, "BLE link saved");
                }
            }
        }

        if(app->pending_mode[0]) {
            if(app->link_up) {
                if(furi_get_tick() >= app->pending_mode_retry) {
                    ls_link_send(app->link, "%s", app->pending_mode);
                    ls_link_send(app->link, "TEL %d", app->cfg.tel_hz);
                    app->pending_mode[0] = '\0';
                    modal_clear(app);
                }
            } else if(furi_get_tick() >= app->pending_mode_until) {

                app->pending_mode[0] = '\0';
                modal_clear(app);
                toast(app, "No radio - check Link");
            }
        }

        if(!app->link_up && furi_get_tick() >= next_hello) {
            hello_interval_ms = hello_interval_ms ? hello_interval_ms * 2 : 500;
            if(hello_interval_ms > 5000) hello_interval_ms = 5000;
            next_hello = furi_get_tick() + furi_ms_to_ticks(hello_interval_ms);

            ls_link_send(app->link, "PING");
            ls_link_send(app->link, "TEL %d", app->cfg.tel_hz);

            if(furi_get_tick() >= next_probe) {
                next_probe = furi_get_tick() + furi_ms_to_ticks(LS_LINK_PROBE_MS);
                ls_link_toggle_port(app->link);
            }
        } else if(app->link_up) {
            next_probe = furi_get_tick() + furi_ms_to_ticks(LS_LINK_PROBE_MS);

            hello_interval_ms = 0;
            next_hello = 0;
        }

        bool voice = app->link_up && app->tel.voice_active;
        if(voice && !app->prev_voice) {
            notification_message(app->notif, &sequence_blink_blue_10);
            if(app->cfg.rx_wake) {

                notification_message(app->notif, &sequence_display_backlight_on);
                app->flash_until = furi_get_tick() + furi_ms_to_ticks(RX_FLASH_MS);
            }
        }
        app->prev_voice = voice;

        bool health_bad = app->tel.rtl_health[0] && strcmp(app->tel.rtl_health, "ok") != 0 &&
                          strcmp(app->tel.rtl_health, "settling") != 0;
        bool sdr_bad = app->link_up && app->have_tel &&
                       (!app->tel.rtl_ready || app->tel.sdr_stall_s > 0 || health_bad);
        if(sdr_bad && !app->prev_sdr_bad) {
            notification_message(app->notif, &sequence_error);
            toast(app, app->tel.rtl_ready ? "SDR stalled - recovering" : "SDR lost - recovering");
        } else if(!sdr_bad && app->prev_sdr_bad) {
            notification_message(app->notif, &sequence_success);
            toast(app, "SDR recovered");
        }
        app->prev_sdr_bad = sdr_bad;

        furi_mutex_release(app->lock);

        if(app->pending_transport >= 0) {
            LsTransport want = (LsTransport)app->pending_transport;
            app->pending_transport = -1;

            furi_mutex_acquire(app->lock, FuriWaitForever);
            modal_set(
                app,
                want == LsTransportBle ? "BLUETOOTH" : "UART",
                want == LsTransportBle ? "starting radio..." : "switching...",
                NULL,
                0);
            furi_mutex_release(app->lock);
            view_port_update(app->vp);

            ls_dbg("apply transport -> %s", want == LsTransportBle ? "BLE" : "UART");
            bool ok = ls_link_set_transport(app->link, want);
            ls_dbg(
                "  set_transport ok=%d now=%s bt_active=%d",
                ok ? 1 : 0,
                ls_link_transport(app->link) == LsTransportBle ? "BLE" : "UART",
                furi_hal_bt_is_active() ? 1 : 0);

            furi_mutex_acquire(app->lock, FuriWaitForever);
            if(!ok) {
                modal_set(app, "BLUETOOTH", "could not start.", "Turn BT on in Settings", 3500);
            } else if(want == LsTransportBle) {

                modal_set(app, "BLUETOOTH", "advertising.", "On the P4:  ble on", 4000);
            } else {
                modal_clear(app);

                app->cfg.want_ble = false;
                app->cfg.ble_proven = false;
                ls_cfg_save(&app->cfg);
            }
            furi_mutex_release(app->lock);
        }

        view_port_update(app->vp);
    }

    ls_cfg_save(&app->cfg);
    ls_link_free(app->link);
    rec_preview_clear(app);

    gui_remove_view_port(app->gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->lock);
    free(app);
    return 0;
}
