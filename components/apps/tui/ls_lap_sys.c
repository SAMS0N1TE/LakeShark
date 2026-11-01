/* See ls_lap_sys.h. */
#include "ls_lap_sys.h"
#include "ls_lap_api.h"
#include "ls_lap_net.h"
#include "ls_lap_radio.h"
#include "ls_lap_kit.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TONES 32
#define SD_JOBS 4
#define SD_MAX (64u * 1024u)
#define SD_LIST_MAX 256

static void (*wake_io)(void);
void ls_lap_sys_set_wake(void (*wake)(void)) { wake_io = wake; ls_lap_net_set_wake(wake); }

/* ---- paths -------------------------------------------------------------- */

#ifndef LS_LAP_SD_ROOT
#define LS_LAP_SD_ROOT "/sdcard" /* the bench points this at a folder of its own */
#endif

/* Under the card, no "..", no backslashes; writes stay out of its apps folder. */
static bool sd_path(const char *p, bool write)
{
    const size_t n = sizeof(LS_LAP_SD_ROOT) - 1;
    if (!p || strncmp(p, LS_LAP_SD_ROOT, n) || strlen(p) > 127 || strchr(p, '\\')) return false;
    if (!p[n] || (p[n] == '/' && !p[n + 1])) return !write;     /* the card itself: list it, never remove it */
    if (p[n] != '/') return false;
    for (const char *q = p; (q = strstr(q, "..")); q += 2)
        if ((q == p || q[-1] == '/') && (q[2] == '/' || !q[2])) return false;
    if (write && (!strcmp(p + n, "/apps") || !strncmp(p + n, "/apps/", 6))) return false;
    return true;
}

/* ---- notes -------------------------------------------------------------- */

typedef struct { uint16_t hz, ms; uint8_t volume, wave; } note_t;
static struct {
    note_t q[TONES];
    unsigned head, tail;
} tones;

bool lap_tone(int hz, int ms, int volume, int wave)
{
    if (!ls_lap_api_inside() || hz < 0 || hz > 12000 || ms <= 0 || ms > 5000 ||
        volume < 0 || volume > 100 || wave < 0 || wave > LAP_WAVE_NOISE)
        return false;
    if (tones.head - tones.tail >= TONES) return false;
    tones.q[tones.head++ % TONES] = (note_t){(uint16_t)hz, (uint16_t)ms, (uint8_t)volume, (uint8_t)wave};
#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
    ls_lap_sys_host()->tones++;
    ls_lap_sys_host()->last_tone_hz = hz;
#endif
    return true;
}
void lap_tone_stop(void)
{
    if (ls_lap_api_inside()) tones.tail = tones.head;
}

/* ---- card jobs ---------------------------------------------------------- */

enum { JOB_FREE, JOB_QUEUED, JOB_RUNNING, JOB_DONE };
enum { OP_READ, OP_WRITE, OP_LIST, OP_SIZE, OP_MKDIR, OP_REMOVE };
typedef struct {
    volatile int state;
    int op, result, serial, max;
    bool orphan;                /* the app closed while it ran */
    char path[128];
    uint32_t offset, len;
    void *out;                  /* the app's buffer, filled on poll */
    void *stage;                /* ours, PSRAM */
} job_t;
static job_t jobs[SD_JOBS];
static int job_serial;

static void *stage_alloc(size_t n);
static void stage_free(void *p);

static int job_start(int op, const char *path, uint32_t offset, void *out, uint32_t len, int max, const void *data)
{
    if (!ls_lap_api_inside() || !sd_path(path, op == OP_WRITE || op == OP_MKDIR || op == OP_REMOVE))
        return LAP_SD_EINVAL;
    if ((op == OP_READ || op == OP_WRITE) && (len > SD_MAX || (len && !(op == OP_READ ? out : data))))
        return LAP_SD_EINVAL;
    if (op == OP_READ && len && !ls_lap_api_owns(out, len)) return LAP_SD_EINVAL;
    if (op == OP_LIST && (max <= 0 || !out || !ls_lap_api_owns(out, (size_t)max * sizeof(lap_sd_entry_t))))
        return LAP_SD_EINVAL;
    int slot = -1;
    for (int i = 0; i < SD_JOBS && slot < 0; i++)
        if (jobs[i].state == JOB_FREE) slot = i;
    if (slot < 0) return LAP_SD_EBUSY;
    job_t *j = &jobs[slot];
    memset(j, 0, sizeof(*j));
    snprintf(j->path, sizeof(j->path), "%s", path);
    j->op = op;
    j->offset = offset;
    j->len = len;
    j->out = out;
    j->max = max > SD_LIST_MAX ? SD_LIST_MAX : max;
    size_t need = op == OP_LIST ? (size_t)j->max * sizeof(lap_sd_entry_t) : (op == OP_READ || op == OP_WRITE) ? len : 0;
    if (need) {
        j->stage = stage_alloc(need);
        if (!j->stage) return LAP_SD_EBUSY;
        if (op == OP_WRITE) memcpy(j->stage, data, len);
    }
    j->serial = ++job_serial & 0xFFFF;
    __atomic_store_n(&j->state, JOB_QUEUED, __ATOMIC_RELEASE);
    if (wake_io) wake_io();
    return slot | j->serial << 3;
}

int lap_sd_read(const char *path, uint32_t offset, void *buf, uint32_t len) { return job_start(OP_READ, path, offset, buf, len, 0, NULL); }
int lap_sd_write(const char *path, uint32_t offset, const void *data, uint32_t len) { return job_start(OP_WRITE, path, offset, NULL, len, 0, data); }
int lap_sd_list(const char *dir, lap_sd_entry_t *out, int max) { return job_start(OP_LIST, dir, 0, out, 0, max, NULL); }
int lap_sd_size(const char *path) { return job_start(OP_SIZE, path, 0, NULL, 0, 0, NULL); }
int lap_sd_mkdir(const char *path) { return job_start(OP_MKDIR, path, 0, NULL, 0, 0, NULL); }
int lap_sd_remove(const char *path) { return job_start(OP_REMOVE, path, 0, NULL, 0, 0, NULL); }

int lap_sd_poll(int id)
{
    if (!ls_lap_api_inside() || id < 0) return LAP_SD_EINVAL;
    if ((id & 7) >= SD_JOBS) return LAP_SD_EINVAL;
    job_t *j = &jobs[id & 7];
    if (j->serial != id >> 3 || j->state == JOB_FREE) return LAP_SD_EINVAL;
    if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) != JOB_DONE) return LAP_SD_PENDING;
    int r = j->result;
    if (r > 0 && j->op == OP_READ) {
        if (ls_lap_api_owns(j->out, (size_t)r)) memcpy(j->out, j->stage, (size_t)r);
        else r = LAP_SD_EINVAL;
    } else if (r > 0 && j->op == OP_LIST) {
        if (ls_lap_api_owns(j->out, (size_t)r * sizeof(lap_sd_entry_t))) memcpy(j->out, j->stage, (size_t)r * sizeof(lap_sd_entry_t));
        else r = LAP_SD_EINVAL;
    }
    stage_free(j->stage);
    j->stage = NULL;
    __atomic_store_n(&j->state, JOB_FREE, __ATOMIC_RELEASE);
    return r;
}

static int err(void) { return errno == ENOENT ? LAP_SD_ENOENT : errno == EACCES || errno == EPERM ? LAP_SD_EACCES : LAP_SD_EIO; }

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#define MAKE_DIR(p) mkdir(p, 0777)
#endif

/* Worker: one job, start to finish. */
static void job_run(job_t *j)
{
    switch (j->op) {
    case OP_READ: {
        FILE *f = fopen(j->path, "rb");
        if (!f) { j->result = err(); break; }
        if (fseek(f, (long)j->offset, SEEK_SET)) { j->result = LAP_SD_EIO; fclose(f); break; }
        j->result = (int)fread(j->stage, 1, j->len, f);
        if (ferror(f)) j->result = LAP_SD_EIO;
        fclose(f);
        break;
    }
    case OP_WRITE: {
        const char *mode = j->offset == 0 ? "wb" : j->offset == LAP_SD_APPEND ? "ab" : "r+b";
        FILE *f = fopen(j->path, mode);
        if (!f) { j->result = err(); break; }
        if (j->offset && j->offset != LAP_SD_APPEND && fseek(f, (long)j->offset, SEEK_SET)) { j->result = LAP_SD_EIO; fclose(f); break; }
        j->result = (int)fwrite(j->stage, 1, j->len, f);
        if (fclose(f) || j->result != (int)j->len) j->result = LAP_SD_EIO;
        break;
    }
    case OP_LIST: {
        DIR *d = opendir(j->path);
        if (!d) { j->result = err(); break; }
        lap_sd_entry_t *e = j->stage;
        int n = 0;
        struct dirent *de;
        while (n < j->max && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            memset(&e[n], 0, sizeof(e[n]));
            snprintf(e[n].name, sizeof(e[n].name), "%s", de->d_name);
            char full[200];
            snprintf(full, sizeof(full), "%s/%s", j->path, de->d_name);
            struct stat st;
            if (!stat(full, &st)) {
                e[n].dir = S_ISDIR(st.st_mode);
                e[n].size = e[n].dir ? 0 : (uint32_t)st.st_size;
            }
            n++;
        }
        closedir(d);
        j->result = n;
        break;
    }
    case OP_SIZE: {
        struct stat st;
        j->result = stat(j->path, &st) ? err() : st.st_size > 0x7FFFFFFF ? 0x7FFFFFFF : (int)st.st_size;
        break;
    }
    case OP_MKDIR:
        j->result = MAKE_DIR(j->path) && errno != EEXIST ? err() : 0;
        break;
    case OP_REMOVE: {
        struct stat st;
        if (stat(j->path, &st)) { j->result = err(); break; }
        j->result = (S_ISDIR(st.st_mode) ? rmdir(j->path) : remove(j->path)) ? err() : 0;
        break;
    }
    }
}

static bool jobs_service(void)
{
    for (int i = 0; i < SD_JOBS; i++) {
        job_t *j = &jobs[i];
        int expect = JOB_QUEUED;
        if (!__atomic_compare_exchange_n(&j->state, &expect, JOB_RUNNING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        job_run(j);
        if (__atomic_load_n(&j->orphan, __ATOMIC_ACQUIRE)) {
            stage_free(j->stage);
            j->stage = NULL;
            __atomic_store_n(&j->state, JOB_FREE, __ATOMIC_RELEASE);
        } else
            __atomic_store_n(&j->state, JOB_DONE, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

/* Screen task, on close: whatever has not started is dropped; a running job
   finishes on the worker, which then frees its stage itself. */
static void jobs_abandon(void)
{
    for (int i = 0; i < SD_JOBS; i++) {
        job_t *j = &jobs[i];
        int expect = JOB_QUEUED;
        if (__atomic_compare_exchange_n(&j->state, &expect, JOB_FREE, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            stage_free(j->stage);
            j->stage = NULL;
            continue;
        }
        __atomic_store_n(&j->orphan, true, __ATOMIC_RELEASE);
        expect = JOB_DONE;
        if (__atomic_compare_exchange_n(&j->state, &expect, JOB_FREE, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            stage_free(j->stage);
            j->stage = NULL;
        }
    }
}

/* ---- touch -------------------------------------------------------------- */

static struct {
    bool seen, down;
    lap_touch_t t;
    int swipe;
    uint32_t drags;
} finger;

bool lap_touch(lap_touch_t *out)
{
    if (!ls_lap_api_inside() || !out || !finger.seen) return false;
    *out = finger.t;
    return true;
}
int lap_swipe(void)
{
    if (!ls_lap_api_inside()) return LAP_SWIPE_NONE;
    const int s = finger.swipe;
    finger.swipe = LAP_SWIPE_NONE;
    return s;
}
static int swipe_of(int dx, int dy, int cw, int ch)
{
    const int ax = abs(dx), ay = abs(dy);
    if (ax < cw * 3 / 2 && ay < ch * 3 / 2) return LAP_SWIPE_NONE;
    if (ax >= ay) return dx > 0 ? LAP_SWIPE_RIGHT : LAP_SWIPE_LEFT;
    return dy > 0 ? LAP_SWIPE_DOWN : LAP_SWIPE_UP;
}

/* ====================================================================== */
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)

#include "audio/audio_out.h"
#include "audio/speech.h"
#include "core/display_ctl.h"
#include "core/ls_time.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ls_ble_heard.h"
#include "ls_compass.h"
#include "ls_compass_live.h"
#include "ls_field.h"
#include "ls_gauge.h"
#include "ls_gps.h"
#include "ls_haptic.h"
#include "ls_imu.h"
#include "ls_mesh.h"
#include "ls_panel.h"
#include "ls_tui_screen.h"
#include "ls_tui_touch.h"
#include "ls_theme.h"
#include "ls_wireless.h"

static void *stage_alloc(size_t n) { return heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void stage_free(void *p) { heap_caps_free(p); }
static int64_t now_us(void) { return esp_timer_get_time(); }

static struct {
    bool open, keep_awake, bright_set, field, ble, wifi;
    tui_rect area;
    int64_t awake_us, audio_until_us;
    float phase;
    uint32_t noise;
    bool immersive;                 /* lap_immersive: no board close button either */
    bool spoke;                     /* lap_say used: hush it when the app closes */
    bool park_held;                 /* lap_tone used: audio clocks kept running until it closes */
} sys;

/* ---- the screen ---------------------------------------------------------- */

void lap_fullscreen(bool on)
{
    if (!ls_lap_api_inside()) return;
    if (sys.immersive) ls_tui_screen_set_full(false);   /* bare is ignored while full */
    sys.immersive = false;
    ls_tui_screen_set_bare(on);
}
void lap_immersive(bool on) { if (ls_lap_api_inside()) { sys.immersive = on; ls_tui_screen_set_full(on); } }
/* The board's voice: queued, spoken in the user's chosen voice at the board's volume. */
bool lap_say(const char *text)
{
    if (!ls_lap_api_inside() || !text || !text[0]) return false;
    sys.spoke = true;
    return speech_say_async(text) == SPEECH_SPOKEN;
}
void lap_say_stop(void) { if (ls_lap_api_inside()) speech_cancel(); }
bool ls_lap_sys_immersive(void) { return sys.open && sys.immersive; }
void lap_hold_rotation(bool on) { if (ls_lap_api_inside()) ls_tui_screen_hold_rotation(on); }
void lap_keep_awake(bool on) { if (ls_lap_api_inside()) sys.keep_awake = on; }
void lap_brightness(int pct)
{
    if (!ls_lap_api_inside()) return;
    if (pct < 0) {
        if (sys.bright_set) display_ctl_reapply();
        sys.bright_set = false;
        return;
    }
    ls_panel_set_brightness((unsigned)(pct < 5 ? 5 : pct > 100 ? 100 : pct));
    sys.bright_set = true;
}

/* ---- motion ------------------------------------------------------------- */

static struct {
    ls_imu_sample_t s;
    int64_t at;
    bool have;
} imu;

bool lap_motion(lap_motion_t *out)
{
    if (!ls_lap_api_inside() || !out || !ls_imu_present()) return false;
    ls_imu_sample_t s;
    if (ls_imu_read(&s)) { imu.s = s; imu.at = now_us(); imu.have = true; }
    if (!imu.have) return false;
    if (!sys.field) {
        sys.field = true;
        ls_field_start();
        ls_field_watch(true);
    }
    /* ls_compass_gravity's screen axes (x right, y top, z out of the glass),
       turned to the edge that is up in the picture now. */
    float a[3], g[3] = { -imu.s.gx, imu.s.gy, -imu.s.gz };
    ls_compass_gravity(&imu.s, a);
    int ux = 0, uy = 1;
    ls_compass_view_up(&ux, &uy);
    memset(out, 0, sizeof(*out));
    out->ax = a[0] * uy - a[1] * ux;  out->ay = a[0] * ux + a[1] * uy;  out->az = a[2];
    out->gx = g[0] * uy - g[1] * ux;  out->gy = g[0] * ux + g[1] * uy;  out->gz = g[2];
    out->mx = imu.s.mx; out->my = imu.s.my; out->mz = imu.s.mz;
    out->mag_valid = imu.s.mag_valid;
    out->temp_c = imu.s.temp_c;
    const float n = sqrtf(out->ax * out->ax + out->ay * out->ay + out->az * out->az);
    if (n > 0.2f) {
        out->pitch = asinf(fmaxf(-1, fminf(1, out->ay / n))) * 57.29578f;
        out->roll = asinf(fmaxf(-1, fminf(1, out->ax / n))) * 57.29578f;
    }
    ls_compass_reading_t r;
    ls_compass_live(&r);
    out->heading = r.valid && !r.low_confidence && isfinite(r.magnetic) ? r.magnetic : -1;
    out->true_heading = r.valid && !r.low_confidence && r.true_valid && isfinite(r.true_deg) ? r.true_deg : -1;
    out->age_ms = (uint32_t)((now_us() - imu.at) / 1000);
    return true;
}

/* ---- feedback ----------------------------------------------------------- */

bool lap_haptic(int effect)
{
    return ls_lap_api_inside() && effect >= 0 && effect < LS_HAPTIC_EFFECT_COUNT && ls_haptic_play((ls_haptic_effect_t)effect);
}
bool lap_vibrate(int ms, int strength)
{
    return ls_lap_api_inside() && ms > 0 && ls_haptic_buzz(ms > 2000 ? 2000 : ms, strength < 0 ? 0 : strength > 100 ? 100 : strength);
}
bool lap_tone_busy(void)
{
    return ls_lap_api_inside() && (tones.head != tones.tail || sys.audio_until_us > now_us());
}

/* Render queued notes into the cue ring, keeping about a quarter second
   ahead of the speaker: notes join without gaps and the ring never fills. */
static void tones_service(void)
{
    static int16_t buf[256];
    const int64_t t = now_us();
    if (sys.audio_until_us < t) sys.audio_until_us = t;
    while (tones.head != tones.tail && sys.audio_until_us - t < 250000) {
        const note_t n = tones.q[tones.tail++ % TONES];
        const int total = AUDIO_RATE_HZ * n.ms / 1000, edge = AUDIO_RATE_HZ * 3 / 1000;
        const float amp = 60.0f * n.volume, step = (float)n.hz / AUDIO_RATE_HZ;
        /* A game's sounds come seconds apart: parked between them, every one
           started on clocks and a rail just woken, and snapped. */
        if (!sys.park_held) { sys.park_held = true; audio_out_park_hold(true); }
        audio_out_play_now();
        for (int i = 0; i < total; i += 256) {
            const int chunk = total - i < 256 ? total - i : 256;
            for (int k = 0; k < chunk; k++) {
                const int at = i + k;
                float v = 0;
                if (n.hz) {
                    sys.phase += step;
                    if (sys.phase >= 1) sys.phase -= 1;
                    const float p = sys.phase;
                    switch (n.wave) {
                    case LAP_WAVE_SQUARE: v = p < 0.5f ? 0.6f : -0.6f; break;
                    case LAP_WAVE_TRIANGLE: v = p < 0.5f ? 4 * p - 1 : 3 - 4 * p; break;
                    case LAP_WAVE_SAW: v = (2 * p - 1) * 0.7f; break;
                    case LAP_WAVE_NOISE: sys.noise = sys.noise * 1664525u + 1013904223u; v = ((int32_t)sys.noise >> 16) / 32768.0f * 0.6f; break;
                    default: v = sinf(6.2831853f * p); break;
                    }
                }
                const float env = at < edge ? (float)at / edge : total - at < edge ? (float)(total - at) / edge : 1;
                buf[k] = (int16_t)(v * amp * env);
            }
            /* Dropped on a busy lock, a chunk cut the note mid-wave: a click. */
            audio_write_cue_wait(buf, chunk, 5);
        }
        sys.audio_until_us += (int64_t)n.ms * 1000;
    }
}

/* ---- where and when ----------------------------------------------------- */

bool lap_gps(lap_gps_t *out)
{
    EXT_RAM_BSS_ATTR static ls_gps_state_t st;
    if (!ls_lap_api_inside() || !out) return false;
    ls_gps_get(&st);
    if (!st.alive) return false;
    *out = (lap_gps_t){ .fix = st.fix, .lat = st.lat_deg, .lon = st.lon_deg, .alt_m = st.alt_m,
                        .speed_kmh = st.speed_kts * 1.852f, .course_deg = st.course_deg, .hdop = st.hdop,
                        .sats_used = st.sats_used, .sats_visible = st.sats_visible, .utc = st.utc_s };
    return true;
}

void lap_time(lap_time_t *out)
{
    if (!ls_lap_api_inside() || !out) return;
    const time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    *out = (lap_time_t){ tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                         tm.tm_wday, (int64_t)t, ls_time_is_synced() };
}

bool lap_battery(lap_battery_t *out)
{
    ls_gauge_t g;
    if (!ls_lap_api_inside() || !out || !ls_gauge_get(&g) || !g.present) return false;
    *out = (lap_battery_t){ g.percent, g.millivolts, g.milliamps, g.temp_c10 / 10.0f, g.charging };
    return true;
}

/* ---- the mesh ------------------------------------------------------------ */

/* The worker fills `fresh` and swaps it in under the lock; the screen reads
   `shown`. The lock is only ever held for a memcpy. */
typedef struct {
    bool valid;
    lap_mesh_self_t self;
    int nodes, msgs;
    lap_mesh_node_t node[LS_MESH_MAX_PEERS];
    lap_mesh_msg_t msg[LS_MESH_MAX_MSGS];
    uint32_t seq;
    uint8_t chan_private;           /* bit n: slot n is keyed, not public, not a hashtag */
    char chan_name[4][16];
} mesh_view_t;
EXT_RAM_BSS_ATTR static ls_mesh_chan_t mesh_chans[4];
EXT_RAM_BSS_ATTR static mesh_view_t mesh_shown, mesh_fresh;
EXT_RAM_BSS_ATTR static ls_mesh_msg_t mesh_raw[LS_MESH_MAX_MSGS];
static SemaphoreHandle_t mesh_lock;
static StaticSemaphore_t mesh_lock_buf;
static volatile int64_t mesh_wanted_us, mesh_refreshed_us;
static struct {
    volatile int state;         /* 1 waiting for the worker, 0 done, <0 refused */
    int kind, chan;             /* 0 channel, 1 direct, 2 advert */
    char id[17], text[151];
} mesh_out;

static void mesh_want(void)
{
    if (!mesh_lock) mesh_lock = xSemaphoreCreateMutexStatic(&mesh_lock_buf);
    const int64_t t = now_us();
    mesh_wanted_us = t;
    if (t - mesh_refreshed_us > 2000000 && wake_io) wake_io();
}
static bool mesh_copy(void *dst, const void *src, size_t n)
{
    if (!mesh_lock || xSemaphoreTake(mesh_lock, pdMS_TO_TICKS(2)) != pdTRUE) return false;
    memcpy(dst, src, n);
    xSemaphoreGive(mesh_lock);
    return true;
}

bool lap_mesh_self(lap_mesh_self_t *out)
{
    if (!ls_lap_api_inside() || !out) return false;
    mesh_want();
    bool valid = false;
    mesh_copy(&valid, &mesh_shown.valid, sizeof(valid));
    return valid && mesh_copy(out, &mesh_shown.self, sizeof(*out));
}
int lap_mesh_nodes(lap_mesh_node_t *out, int max)
{
    if (!ls_lap_api_inside() || !out || max <= 0) return 0;
    mesh_want();
    int n = 0;
    if (!mesh_copy(&n, &mesh_shown.nodes, sizeof(n))) return 0;
    if (n > max) n = max;
    return mesh_copy(out, mesh_shown.node, (size_t)n * sizeof(*out)) ? n : 0;
}
int lap_mesh_messages(lap_mesh_msg_t *out, int max)
{
    if (!ls_lap_api_inside() || !out || max <= 0) return 0;
    mesh_want();
    int n = 0;
    if (!mesh_copy(&n, &mesh_shown.msgs, sizeof(n))) return 0;
    const int skip = n > max ? n - max : 0;     /* keep the newest */
    n -= skip;
    return mesh_copy(out, mesh_shown.msg + skip, (size_t)n * sizeof(*out)) ? n : 0;
}
bool lap_mesh_channel_private(int slot, char *name, size_t cap)
{
    if (!ls_lap_api_inside() || slot < 1 || slot > 3 || (name && (!cap || !ls_lap_api_owns(name, cap)))) return false;
    mesh_want();
    uint8_t bits = 0;
    char shown[16] = "";
    if (!mesh_copy(&bits, &mesh_shown.chan_private, sizeof(bits)) ||
        !mesh_copy(shown, mesh_shown.chan_name[slot], sizeof(shown)))
        return false;
    if (name) snprintf(name, cap, "%.15s", shown);
    return (bits >> slot) & 1;
}
uint32_t lap_mesh_seq(void)
{
    if (!ls_lap_api_inside()) return 0;
    mesh_want();
    return ls_mesh_msg_seq();
}
static bool mesh_queue(int kind, int chan, const char *id, const char *text)
{
    if (!ls_lap_api_inside() || mesh_out.state == 1) return false;
    if (text && strlen(text) > 150) { mesh_out.state = -3; return false; }
    if (kind == 0 && (chan < 0 || chan > 3)) return false;
    if (kind == 1 && (!id || strlen(id) != 16)) return false;
    mesh_out.kind = kind;
    mesh_out.chan = chan;
    snprintf(mesh_out.id, sizeof(mesh_out.id), "%s", id ? id : "");
    snprintf(mesh_out.text, sizeof(mesh_out.text), "%s", text ? text : "");
    __atomic_store_n(&mesh_out.state, 1, __ATOMIC_RELEASE);
    mesh_want();
    if (wake_io) wake_io();
    return true;
}
bool lap_mesh_send(int channel, const char *text) { return text && *text && mesh_queue(0, channel, NULL, text); }
bool lap_mesh_dm(const char *id, const char *text) { return text && *text && mesh_queue(1, 0, id, text); }
bool lap_mesh_advert(void) { return mesh_queue(2, 0, NULL, NULL); }
int lap_mesh_send_state(void) { return ls_lap_api_inside() ? mesh_out.state : 0; }

static void mesh_refresh(void)
{
    mesh_view_t *v = &mesh_fresh;
    memset(v, 0, sizeof(*v));
    if (ls_mesh_running()) {
        ls_mesh_stats_t st;
        ls_mesh_get_stats(&st);
        v->self.running = st.running;
        v->self.tx_armed = st.tx_enabled;
        snprintf(v->self.id, sizeof(v->self.id), "%s", st.self_id);
        const char *name = ls_mesh_name();
        snprintf(v->self.name, sizeof(v->self.name), "%s", name ? name : "");
        v->self.rx_packets = st.rx_packets;
        v->self.tx_packets = st.tx_packets;
        v->self.last_rssi = st.last_rssi;
        v->self.last_snr = st.last_snr;
        const uint32_t clock = ls_mesh_now();
        ls_mesh_peer_t p;
        while (v->nodes < LS_MESH_MAX_PEERS && ls_mesh_peer_at(v->nodes, &p)) {
            lap_mesh_node_t *n = &v->node[v->nodes++];
            snprintf(n->id, sizeof(n->id), "%s", p.id);
            snprintf(n->name, sizeof(n->name), "%s", p.name);
            n->type = p.type;
            n->has_loc = p.has_loc;
            n->lat = p.lat_e6 / 1e6;
            n->lon = p.lon_e6 / 1e6;
            n->rssi = p.rssi;
            n->snr = p.snr;
            n->heard_s = clock > p.last_heard ? clock - p.last_heard : 0;
            n->adverts = p.adverts;
        }
        /* The public key every MeshCore node ships with is no secret. */
        const int c = ls_mesh_channels(mesh_chans, 4);
        for (int i = 1; i < c && i < 4; i++) {
            snprintf(v->chan_name[i], sizeof(v->chan_name[i]), "%s", mesh_chans[i].name);
            if (mesh_chans[i].psk[0] && strcmp(mesh_chans[i].psk, "izOH6cXN6mrJ5e26oRXNcg==") &&
                mesh_chans[i].name[0] != '#')
                v->chan_private |= (uint8_t)(1u << i);
        }
        const int m = ls_mesh_messages(mesh_raw, LS_MESH_MAX_MSGS);
        for (int i = 0; i < m; i++) {
            const ls_mesh_msg_t *r = &mesh_raw[i];
            lap_mesh_msg_t *o = &v->msg[v->msgs++];
            o->id = r->id;
            o->mine = r->mine;
            o->direct = r->direct;
            o->channel = r->chan;
            o->state = (uint8_t)r->state;
            snprintf(o->peer, sizeof(o->peer), "%s", r->peer);
            snprintf(o->sender, sizeof(o->sender), "%s", r->sender);
            snprintf(o->text, sizeof(o->text), "%s", r->text);
            o->age_s = clock > r->t ? clock - r->t : 0;
        }
    }
    v->valid = true;
    v->seq = ls_mesh_msg_seq();
    if (mesh_lock && xSemaphoreTake(mesh_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(&mesh_shown, v, sizeof(*v));
        xSemaphoreGive(mesh_lock);
    }
    mesh_refreshed_us = now_us();
}

static bool mesh_service(void)
{
    bool did = false;
    if (__atomic_load_n(&mesh_out.state, __ATOMIC_ACQUIRE) == 1) {
        esp_err_t e = ESP_FAIL;
        if (!ls_mesh_running()) e = ESP_ERR_NOT_FOUND;
        else if (mesh_out.kind == 0) e = ls_mesh_send_text_on(mesh_out.chan, mesh_out.text);
        else if (mesh_out.kind == 1) e = ls_mesh_send_dm_id(mesh_out.id, mesh_out.text);
        else e = ls_mesh_advertise();
        __atomic_store_n(&mesh_out.state, e == ESP_OK ? 0 : e == ESP_ERR_NOT_ALLOWED ? -1 : e == ESP_ERR_INVALID_STATE ? -2 : e == ESP_ERR_INVALID_SIZE ? -3 : -4, __ATOMIC_RELEASE);
        mesh_refreshed_us = 0;              /* show our own message soon */
        did = true;
    }
    const int64_t t = now_us();
    if (mesh_lock && t - mesh_wanted_us < 5000000 && (t - mesh_refreshed_us > 2000000 || mesh_shown.seq != ls_mesh_msg_seq())) {
        mesh_refresh();
        did = true;
    }
    return did;
}

/* ---- Wi-Fi and Bluetooth ------------------------------------------------- */

EXT_RAM_BSS_ATTR static ls_wireless_snapshot_t wl;
EXT_RAM_BSS_ATTR static ls_ble_heard_t heard[24];

bool lap_wifi_scan(void)
{
    if (!ls_lap_api_inside()) return false;
    if (!sys.wifi) { sys.wifi = true; ls_wireless_set_active(true); }
    return ls_wireless_request(LS_WIRELESS_SCAN, NULL, NULL);
}
int lap_wifi_aps(lap_wifi_ap_t *out, int max)
{
    if (!ls_lap_api_inside() || !out || max <= 0) return 0;
    ls_wireless_get(&wl);
    int n = wl.ap_count < LS_WIRELESS_APS ? wl.ap_count : LS_WIRELESS_APS;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", wl.aps[i].ssid);
        out[i].rssi = (int8_t)wl.aps[i].rssi;
        out[i].channel = (uint8_t)wl.aps[i].channel;
        out[i].secure = wl.aps[i].secure;
    }
    return n;
}
uint32_t lap_wifi_seq(void)
{
    if (!ls_lap_api_inside()) return 0;
    ls_wireless_get(&wl);
    return wl.scan_revision;
}

void lap_ble_scan(bool on)
{
    if (!ls_lap_api_inside() || sys.ble == on) return;
    sys.ble = on;
    ls_wireless_find_ble(on);
}
int lap_ble_devices(lap_ble_dev_t *out, int max)
{
    if (!ls_lap_api_inside() || !out || max <= 0) return 0;
    const int64_t t = now_us();
    int n = ls_ble_heard_list(heard, max < 24 ? max : 24, t, 30000000);
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].addr, heard[i].addr, 6);
        snprintf(out[i].name, sizeof(out[i].name), "%s", heard[i].name);
        out[i].rssi = heard[i].rssi;
        out[i].smooth_rssi = heard[i].smooth;
        out[i].adverts = heard[i].adverts;
        out[i].age_ms = (uint32_t)((t - heard[i].last_us) / 1000);
    }
    return n;
}

/* ---- the host side ------------------------------------------------------- */

void ls_lap_sys_begin(void)
{
    ls_lap_net_begin(false);
    ls_lap_radio_begin(NULL);            /* nothing granted until ls_lap.c says */
    memset(&sys, 0, sizeof(sys));
    memset(&finger, 0, sizeof(finger));
    memset(&imu, 0, sizeof(imu));
    tones.head = tones.tail = 0;
    finger.drags = ls_tui_touch_drags(NULL, NULL, NULL, NULL);
    mesh_out.state = 0;
    sys.open = true;
    ls_lap_kit_begin();
}

void ls_lap_sys_frame(tui_rect area)
{
    sys.area = area;
    int cols, rows, cw = 10, ch = 17;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    int x, y, x0, y0;
    int64_t since;
    if (ls_tui_touch_px(&x, &y, &x0, &y0, &since)) {
        const int ox = area.x * cw, oy = area.y * ch;
        if (!finger.down) {
            /* A finger that went down outside the app (on the tab strip) is not the app's. */
            if (x0 < ox || y0 < oy || x0 >= ox + area.w * cw || y0 >= oy + area.h * ch) goto lifted;
            finger.down = finger.seen = true;
        }
        finger.t = (lap_touch_t){ true, (int16_t)(x - ox), (int16_t)(y - oy), (int16_t)(x0 - ox), (int16_t)(y0 - oy),
                                  (uint32_t)((now_us() - since) / 1000) };
    } else {
lifted:
        finger.down = false;
        finger.t.down = false;
    }
    int dx0, dy0, dx1, dy1;
    const uint32_t drags = ls_tui_touch_drags(&dx0, &dy0, &dx1, &dy1);
    if (drags != finger.drags) {
        finger.drags = drags;
        const int s = swipe_of(dx1 - dx0, dy1 - dy0, cw, ch);
        if (s) finger.swipe = s;
    }
    if (sys.keep_awake && now_us() - sys.awake_us > 1000000) {
        sys.awake_us = now_us();
        display_ctl_activity();
    }
    tones_service();
}

/* The screen bits of ending, for an app whose service keeps it loaded with
   its screen closed: its lists and keypads, the waterfall, the glass and
   the board's brightness and rotation go back; its radio, jobs and scans
   stay. */
void ls_lap_sys_hide(void)
{
    ls_lap_kit_end();
    if (!sys.open) return;
    if (sys.bright_set) display_ctl_reapply();
    sys.bright_set = false;
    sys.keep_awake = false;
    if (sys.spoke) speech_cancel();
    sys.spoke = false;
    if (sys.park_held) audio_out_park_hold(false);
    sys.park_held = false;
    ls_tui_screen_set_full(false);
    ls_tui_screen_set_bare(false);
    ls_tui_screen_hold_rotation(false);
}

void ls_lap_sys_end(void)
{
    ls_lap_kit_end();
    ls_lap_net_end();
    ls_lap_radio_end();                  /* the worker closes the session and frees the radio */
    if (!sys.open) return;
    sys.open = false;
    jobs_abandon();
    tones.head = tones.tail = 0;
    if (sys.bright_set) display_ctl_reapply();
    if (sys.field) ls_field_watch(false);
    if (sys.ble) ls_wireless_find_ble(false);
    if (sys.wifi) ls_wireless_set_active(false);
    if (sys.spoke) speech_cancel();
    if (sys.park_held) audio_out_park_hold(false);
    sys.park_held = false;
    ls_tui_screen_set_full(false);
    ls_tui_screen_set_bare(false);
    ls_tui_screen_hold_rotation(false);
    mesh_wanted_us = 0;
}

bool ls_lap_sys_service(void)
{
    const bool net = ls_lap_net_service();
    const bool a = jobs_service();
    const bool b = mesh_service();
    const bool radio = ls_lap_radio_service();
    return net || a || b || radio;
}

/* ====================================================================== */
#else  /* the bench */

static ls_lap_sys_host_t host;
ls_lap_sys_host_t *ls_lap_sys_host(void) { return &host; }
static void *stage_alloc(size_t n) { return malloc(n ? n : 1); }
static void stage_free(void *p) { free(p); }

void lap_fullscreen(bool on) { (void)on; }
#include "ls_tui_screen.h"
/* lssim draws the full glass only when asked (LSSIM_IMMERSIVE=1), so scripted tests keep their rows. */
void lap_immersive(bool on) { if (getenv("LSSIM_IMMERSIVE")) ls_tui_screen_set_full(on); }
bool lap_say(const char *text) { if (text) printf("lap_say: %s\n", text); return text && text[0]; }
void lap_say_stop(void) {}
bool ls_lap_sys_immersive(void) { return false; }
void lap_hold_rotation(bool on) { (void)on; }
void lap_keep_awake(bool on) { (void)on; }
void lap_brightness(int pct) { (void)pct; }
bool lap_motion(lap_motion_t *out)
{
    if (!ls_lap_api_inside() || !out || !host.motion) return false;
    *out = host.m;
    return true;
}
bool lap_haptic(int effect) { (void)effect; return false; }
bool lap_vibrate(int ms, int strength) { (void)ms; (void)strength; return false; }
bool lap_tone_busy(void) { return ls_lap_api_inside() && tones.head != tones.tail; }
bool lap_gps(lap_gps_t *out)
{
    if (!ls_lap_api_inside() || !out || !host.gps) return false;
    *out = host.g;
    return true;
}
void lap_time(lap_time_t *out) { if (ls_lap_api_inside() && out) *out = host.t; }
bool lap_battery(lap_battery_t *out)
{
    if (!ls_lap_api_inside() || !out || !host.battery) return false;
    *out = host.b;
    return true;
}
bool lap_mesh_self(lap_mesh_self_t *out)
{
    if (!ls_lap_api_inside() || !out) return false;
    *out = host.self;
    return true;
}
int lap_mesh_nodes(lap_mesh_node_t *out, int max)
{
    if (!ls_lap_api_inside() || !out || max <= 0) return 0;
    const int n = host.mesh_nodes < max ? host.mesh_nodes : max;
    memcpy(out, host.nodes, (size_t)n * sizeof(*out));
    return n;
}
int lap_mesh_messages(lap_mesh_msg_t *out, int max) { (void)out; (void)max; return 0; }
uint32_t lap_mesh_seq(void) { return 0; }
bool lap_mesh_send(int channel, const char *text) { return ls_lap_api_inside() && channel >= 0 && channel <= 3 && text && strlen(text) <= 150; }
bool lap_mesh_dm(const char *id, const char *text) { return ls_lap_api_inside() && id && strlen(id) == 16 && text && strlen(text) <= 150; }
bool lap_mesh_advert(void) { return ls_lap_api_inside(); }
int lap_mesh_send_state(void) { return 0; }
bool lap_mesh_channel_private(int slot, char *name, size_t cap)
{
    if (!ls_lap_api_inside() || slot < 1 || slot > 3) return false;
    if (name && cap) snprintf(name, cap, "%s", host.chan_private & (1u << slot) ? "private" : "");
    return (host.chan_private >> slot) & 1;
}
bool lap_wifi_scan(void) { return ls_lap_api_inside(); }
int lap_wifi_aps(lap_wifi_ap_t *out, int max)
{
    if (!ls_lap_api_inside() || !out || max <= 0) return 0;
    const int n = host.wifi_count < max ? host.wifi_count : max;
    memcpy(out, host.aps, (size_t)n * sizeof(*out));
    return n;
}
uint32_t lap_wifi_seq(void) { return (uint32_t)host.wifi_count; }
void lap_ble_scan(bool on) { (void)on; }
int lap_ble_devices(lap_ble_dev_t *out, int max)
{
    if (!ls_lap_api_inside() || !out || max <= 0) return 0;
    const int n = host.ble_count < max ? host.ble_count : max;
    memcpy(out, host.ble, (size_t)n * sizeof(*out));
    return n;
}
void ls_lap_sys_begin(void)
{
    ls_lap_net_begin(false);
    ls_lap_radio_begin(NULL);
    memset(&finger, 0, sizeof(finger));
    tones.head = tones.tail = 0;
    ls_lap_kit_begin();
}
static tui_rect host_area;
void ls_lap_sys_host_finger(int gx, int gy, int cw, int ch, bool down)
{
    const int x = gx - host_area.x * cw, y = gy - host_area.y * ch;
    if (down && !host.down) { host.x0 = x; host.y0 = y; host.held_ms = 0; }
    host.down = down;
    host.x = x;
    host.y = y;
}
void ls_lap_sys_frame(tui_rect area)
{
    host_area = area;
    finger.seen |= host.down;
    finger.t = (lap_touch_t){ host.down, (int16_t)host.x, (int16_t)host.y, (int16_t)host.x0, (int16_t)host.y0, host.held_ms };
    if (host.swipe) { finger.swipe = host.swipe; host.swipe = 0; }
    tones.tail = tones.head;                    /* the bench has no speaker */
    (void)swipe_of;
    ls_lap_radio_service();                     /* no worker here: one pass a frame */
}
void ls_lap_sys_end(void) { ls_lap_kit_end(); ls_lap_net_end(); ls_lap_radio_end(); jobs_abandon(); }
void ls_lap_sys_hide(void) { ls_lap_kit_end(); }
bool ls_lap_sys_service(void)
{
    const bool net = ls_lap_net_service();
    const bool radio = ls_lap_radio_service();
    return jobs_service() || net || radio;
}
#endif
