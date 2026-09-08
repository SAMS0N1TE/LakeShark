/* LS-689: the device half of the PROGRAM session - SD, PSRAM and the worker.
 *
 * Everything that decides anything lives in p25_program.c and is host-tested.
 * This file supplies the three things the host cannot: a real file, a place to
 * put 16 KiB of staging, and a task to do it on.
 *
 * Not the LVGL task.  Reading a profile off a FAT volume is tens of
 * milliseconds on a good day and a stalled card on a bad one, and the staging
 * area is far too large to be a local anywhere.  So RELOAD hands the work to a
 * short-lived worker and returns; the panel polls p25_program_session() and
 * shows LOADING until the worker publishes a result.  Nothing on the panel
 * claims success before the apply has actually run.
 *
 * Not internal RAM either.  p25_program_staging_t is about 16 KiB - the file
 * text, the parser's line buffer and two profile structures - and internal RAM
 * after the P25 decoder and BLE are up is measured in single-digit kilobytes.
 * It is one MALLOC_CAP_SPIRAM allocation held only while a reload runs, and
 * the session itself (about 2.5 KiB) is a second PSRAM allocation made once.
 * If PSRAM cannot supply either, the reload is refused with a reason rather
 * than falling back onto the internal heap.
 */

#include "p25_program.h"

#include <stdio.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "p25_controls.h"
#include "p25_state.h"
#include "scan_ctrl.h"

static const char *TAG = "p25prog";

/* The worker only reads a FAT volume and copies memory.  It never touches
 * flash, so its stack has no cache-disabled requirement (LS-671) and the
 * profile path is fixed to the SD mount so it cannot be pointed at SPIFFS,
 * where a read would.  4 KiB covers fopen/fread on esp_vfs_fat. */
#define P25_PROGRAM_WORKER_STACK_BYTES 4096
#define P25_PROGRAM_WORKER_PRIORITY    4

static p25_program_t *s_program;
static portMUX_TYPE   s_program_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool  s_worker_live;

/* The endpoint's advertised range.  The P25 session acquires the radio with
 * exactly these limits (see p25_radio_open), so a profile frequency the tuner
 * cannot reach is rejected by the parser rather than by a failed retune. */
static const ls_radio_range_t s_tune_ranges[] = {
    { P25_CONTROL_TUNER_MIN_HZ, P25_CONTROL_TUNER_MAX_HZ },
};
static const p25_profile_parse_config_t s_parse_config = {
    s_tune_ranges, sizeof(s_tune_ranges) / sizeof(s_tune_ranges[0]),
};

/* ------------------------------------------------------------- session --- */

/* Allocate outside the critical section and discard the loser, because
 * heap_caps_malloc cannot be called with interrupts off.  Two tasks can reach
 * here first: the UI pressing RELOAD and p25_on_enter re-applying. */
static p25_program_t *program_session(void)
{
    if (s_program) return s_program;

    p25_program_t *fresh = heap_caps_malloc(sizeof(*fresh), MALLOC_CAP_SPIRAM);
    if (!fresh) {
        ESP_LOGE(TAG, "no PSRAM for the %u-byte PROGRAM session",
                 (unsigned)sizeof(*fresh));
        return NULL;
    }
    p25_program_init(fresh);

    p25_program_t *loser = NULL;
    portENTER_CRITICAL(&s_program_lock);
    if (s_program) loser = fresh;
    else           s_program = fresh;
    portEXIT_CRITICAL(&s_program_lock);

    if (loser) heap_caps_free(loser);
    return s_program;
}

const p25_program_t *p25_program_session(void) { return s_program; }

/* ------------------------------------------------------------------ SD --- */

static p25_program_result_t sd_read(void *ctx, const char *path, char *dst,
                                    size_t cap, size_t *out_len)
{
    (void)ctx;

    struct stat st;
    if (stat(path, &st) != 0) return P25_PROGRAM_ERR_NOT_FOUND;
    if (!S_ISREG(st.st_mode)) return P25_PROGRAM_ERR_NOT_FOUND;
    if (st.st_size < 0) return P25_PROGRAM_ERR_READ;
    /* Refuse rather than truncate: a profile silently missing its last
     * talkgroups reads exactly like a working file and a broken radio. */
    if ((size_t)st.st_size > cap) return P25_PROGRAM_ERR_TOO_LARGE;

    FILE *fp = fopen(path, "rb");
    if (!fp) return P25_PROGRAM_ERR_READ;
    size_t want = (size_t)st.st_size;
    size_t got = want ? fread(dst, 1, want, fp) : 0;
    int bad = ferror(fp);
    fclose(fp);
    if (bad || got != want) return P25_PROGRAM_ERR_READ;

    *out_len = got;
    return P25_PROGRAM_OK;
}

/* ------------------------------------------------------------------ ops --- */

/* Every op forwards to the existing owner of that state.  Nothing here reaches
 * into the grant follower or the radio directly: app_p25.c owns both, and the
 * profile apply must not become a second place that tunes.
 *
 * These run on the worker while the DSD task reads the same follower.  That is
 * the arrangement LS-611 already documents for the console and panel setters:
 * each is a scalar or a small table write, and the worst case is one grant
 * decided under the outgoing policy. */

static void op_release(void *user)
{
    (void)user;
    p25_return_to_control();
}

static void op_auto_follow(void *user, bool enabled)
{
    (void)user;
    (void)p25_set_auto_follow(enabled);
}

static void op_encrypted_policy(void *user, bool skip_enabled, uint32_t skip_ms)
{
    (void)user;
    (void)p25_set_leave_on_encrypted(skip_enabled);
    (void)p25_set_encrypted_skip_ms(skip_ms);
}

static void op_demod_preference(void *user, int preference)
{
    (void)user;
    p25_demod_set_preference(preference);
}

static void op_roster(void *user, const p25_profile_talkgroup_t *talkgroups,
                      size_t count)
{
    (void)user;
    p25_program_apply_roster(&g_p25_scan, talkgroups, count);
    /* The allow list and hold are persisted state; the profile has just
     * replaced them, so write through like every other roster mutation. */
    p25_scan_persist_save_now();
}

static void op_cqpsk_loops(void *user, const p25_cqpsk_config_t *config)
{
    (void)user;
    (void)p25_set_cqpsk_config(config);
}

static void op_restore_roster(void *user,
                              const p25_profile_talkgroup_t *talkgroups,
                              size_t count)
{
    (void)user;
    p25_program_restore_roster(&g_p25_scan, talkgroups, count);
}

static void op_set_control(void *user, uint64_t control_hz)
{
    (void)user;
    p25_set_control_channel(control_hz);
}

static const p25_program_ops_t s_ops = {
    .release              = op_release,
    .set_auto_follow      = op_auto_follow,
    .set_encrypted_policy = op_encrypted_policy,
    .set_cqpsk_loops      = op_cqpsk_loops,
    .set_demod_preference = op_demod_preference,
    .set_roster           = op_roster,
    .restore_roster       = op_restore_roster,
    .set_control          = op_set_control,
    .user                 = NULL,
};

/* --------------------------------------------------------------- worker --- */

static void p25_program_worker(void *arg)
{
    p25_program_t *program = (p25_program_t *)arg;

    p25_program_staging_t *staging =
        heap_caps_malloc(sizeof(*staging), MALLOC_CAP_SPIRAM);
    if (!staging) {
        ESP_LOGE(TAG, "no PSRAM for %u bytes of staging - reload refused",
                 (unsigned)sizeof(*staging));
        (void)p25_program_abandon(program, P25_PROGRAM_ERR_NO_MEMORY);
    } else {
        p25_program_result_t result =
            p25_program_reload(program, sd_read, NULL, staging,
                               &s_parse_config, &s_ops);
        heap_caps_free(staging);

        if (result == P25_PROGRAM_OK) {
            sys_log(1, "Profile: %s / %s  %u control",
                    program->active.system_name, program->active.site_name,
                    (unsigned)program->active.control_count);
        } else {
            char reason[96];
            p25_program_format_status(program, reason, sizeof(reason));
            sys_log(4, "Profile load failed: %s", reason);
        }
    }

    s_worker_live = false;
    vTaskDelete(NULL);
}

bool p25_program_request_reload(void)
{
    p25_program_t *program = program_session();
    if (!program) return false;
    if (s_worker_live) return false;
    /* A reload request starts a profile ownership transaction even when the
     * file later proves invalid.  Stop survey tuning before the SD worker can
     * overlap it; failure still retains the same active profile/control. */
    (void)p25_program_survey_cancel(program,
                                    P25_SURVEY_CANCEL_PROFILE_CHANGE, &s_ops);
    if (!p25_program_claim(program, P25_PROGRAM_DEFAULT_PATH)) return false;

    s_worker_live = true;
    if (xTaskCreate(p25_program_worker, "p25_prog",
                    P25_PROGRAM_WORKER_STACK_BYTES, program,
                    P25_PROGRAM_WORKER_PRIORITY, NULL) != pdPASS) {
        s_worker_live = false;
        (void)p25_program_abandon(program, P25_PROGRAM_ERR_NO_MEMORY);
        ESP_LOGE(TAG, "PROGRAM worker task could not be created");
        return false;
    }
    return true;
}

bool p25_program_step_control_now(int delta)
{
    if (!s_program) return false;
    return p25_program_step_control(s_program, delta, &s_ops);
}

bool p25_program_reapply_now(void)
{
    if (!s_program) return false;
    return p25_program_reapply(s_program, &s_ops);
}

bool p25_program_survey_start_now(void)
{
    if (!s_program) return false;
    return p25_program_survey_start(
        s_program, (uint32_t)(esp_timer_get_time() / 1000LL),
        (uint32_t)P25.dsd_bch_ok_count, P25.p25_tsbk_ok_count, &s_ops);
}

bool p25_program_survey_poll_now(uint32_t now_ms, uint32_t valid_nids,
                                 uint32_t valid_tsbks)
{
    if (!s_program) return false;
    return p25_program_survey_poll(s_program, now_ms, valid_nids, valid_tsbks,
                                   &s_ops);
}

bool p25_program_survey_cancel_now(p25_survey_cancel_t reason)
{
    if (!s_program) return false;
    return p25_program_survey_cancel(s_program, reason, &s_ops);
}

bool p25_program_survey_active_now(void)
{
    return p25_program_survey_active(s_program);
}
