

#include "ls_haptic.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ls_i2c.h"

static const char *TAG = "ls_haptic";

/* ------------------------------------------------------ the arithmetic --- */
/* Outside the board gate on purpose: none of it depends on this board having
   the part, and the bench checks it on every build. */

uint8_t ls_haptic_f_pre_code(int f0_hz)
{

    if (f0_hz < 94) f0_hz = 94;
    if (f0_hz > 24000) f0_hz = 24000;
    int code = (24000 + f0_hz / 2) / f0_hz;
    if (code < 1) code = 1;
    if (code > 255) code = 255;
    return (uint8_t)code;
}

uint8_t ls_haptic_drv_width_code(int f0_hz, int track_margin, int brk_gain)
{
    /* The datasheet's recommendation verbatim:
         DRV_WIDTH = (24000/F0) - 8 - TRACK_MARGIN - BRK_GAIN
       and its constraint: it must stay under the half-cycle time of F0,
       which in the part's 1/48 kHz units is 24000/F0. Both matter. Too wide
       and the brake fires into the drive, which feels mushy and reads as a
       weak motor - and nothing anywhere raises an error about it. */
    if (f0_hz < 94) f0_hz = 94;
    if (f0_hz > 24000) f0_hz = 24000;
    const int half_cycle = (24000 + f0_hz / 2) / f0_hz;
    int code = half_cycle - 8 - track_margin - brk_gain;
    if (code < 1) code = 1;
    if (code >= half_cycle) code = half_cycle - 1;
    if (code < 1) code = 1;
    if (code > 255) code = 255;
    return (uint8_t)code;
}

uint8_t ls_haptic_level_code(int strength)
{
    /* Seven bits: CONTCFG6 keeps TRACK_EN in bit 7 and CONTCFG7 reserves it.
       So the top of the scale is 127, not 128 or 255, and a level written
       with bit 7 set into CONTCFG6 would silently turn resonance tracking
       into part of the amplitude. */
    if (strength <= 0) return 0;
    if (strength >= 100) return 127;
    return (uint8_t)((strength * 127 + 50) / 100);
}

int ls_haptic_clamp_ms(int ms)
{
    if (ms < 0) return 0;
    return ms > LS_HAPTIC_MAX_MS ? LS_HAPTIC_MAX_MS : ms;
}

float ls_haptic_rl_ohms(uint8_t rl, uint8_t rl_lo, int d2s_gain)
{
    /* RL = 678 * (RL*4 + RL_LO) / (1024 * D2S_GAIN). The low two bits arrive
       in a different register's bits 1:0, so the code is ten bits split
       across two reads - which is exactly the shape that gets assembled
       wrong and still produces a plausible number. */
    if (d2s_gain <= 0) return 0.0f;
    const int code = ((int)rl * 4) + (rl_lo & 0x03);
    return (678.0f * (float)code) / (1024.0f * (float)d2s_gain);
}

float ls_haptic_vbat_volts(uint8_t vbat, uint8_t vbat_lo)
{
    /* VDD = 6.1 * (VBAT*4 + VBAT_LO) / 1024. VBAT_LO is bits 5:4 of DET_LO,
       not bits 1:0 - those are RL's. */
    const int code = ((int)vbat * 4) + ((vbat_lo >> 4) & 0x03);
    return (6.1f * (float)code) / 1024.0f;
}

float ls_haptic_f0_hz(uint8_t f0_h, uint8_t f0_l)
{
    const int code = ((int)f0_h << 8) | f0_l;
    if (code <= 0) return 0.0f;
    return 384000.0f / (float)code;
}

#if LS_HAS_HAPTIC

/* ---------------------------------------------------------- registers --- */

#define R_SRST       0x00   /* write 0xAA                                   */
#define R_SYSST      0x01   /* UVLS FF_AES FF_AFS OCDS OTS DONES            */
#define R_SYSST2     0x04   /* LDO_OK in bit 3                              */
#define R_PLAYCFG3   0x08   /* STOP_MODE BRK_EN PLAY_MODE                   */
#define R_PLAYCFG4   0x09   /* STOP GO                                      */
#define R_CONTCFG1   0x18   /* EDGE_FRE EN_F0_DET SIN_MODE                  */
#define R_CONTCFG2   0x19   /* F_PRE                                        */
#define R_CONTCFG3   0x1A   /* DRV_WIDTH                                    */
#define R_CONTCFG5   0x1C   /* BRK_GAIN                                     */
#define R_CONTCFG6   0x1D   /* TRACK_EN + DRV1_LVL                          */
#define R_CONTCFG7   0x1E   /* DRV2_LVL                                     */
#define R_CONTCFG8   0x1F   /* DRV1_TIME                                    */
#define R_CONTCFG9   0x20   /* DRV2_TIME                                    */
#define R_CONTCFG10  0x21   /* BRK_TIME                                     */
#define R_CONTCFG11  0x22   /* TRACK_MARGIN                                 */
#define R_CONTRD16   0x27   /* CONT_F0_H                                    */
#define R_CONTRD17   0x28   /* CONT_F0_L                                    */
#define R_SYSCTRL1   0x43   /* VBAT_MODE EN_RAMINIT EN_FIR                  */
#define R_SYSCTRL2   0x44   /* WAKE STANDBY INTN_PIN WAVDAT_MODE            */
#define R_SYSCTRL7   0x49   /* GAIN_BYPASS D2S_GAIN                         */
#define R_DETCFG1    0x51   /* RL_OS CLK_ADC                                */
#define R_DETCFG2    0x52   /* VBAT_GO DIAG_GO                              */
#define R_DET_RL     0x53   /* measured coil resistance, high 8             */
#define R_DET_VBAT   0x55   /* measured supply, high 8                      */
#define R_DET_LO     0x57   /* VBAT_LO in 5:4, RL_LO in 1:0                 */
#define R_CHIPID     0x64

/* The datasheet's own register list has 0x53 twice - once as DET_RL
   and once as TRIMCFG3 - and the detailed description that follows puts
   TRIMCFG3 at 0x5A. The detail section wins; the summary table is wrong.
   Nothing here writes TRIMCFG3, but the next person to reach for LRA trim
   should know the table cannot be trusted for it. */

#define SRST_MAGIC        0xAA

/* PLAYCFG3 */
#define PLAY_BRK_EN       0x04
#define PLAY_MODE_CONT    0x02

/* PLAYCFG4 */
#define PLAY_STOP         0x02
#define PLAY_GO           0x01

/* SYSCTRL1 */
#define SYS1_VBAT_MODE    0x80
#define SYS1_EN_RAMINIT   0x08
#define SYS1_EN_FIR       0x04

/* SYSCTRL2, reset default: INTN_PIN set, reserved bits 5:4 = 0b10. Neither
   WAKE nor STANDBY forced - the part enters active on a play request by
   itself and falls back to standby when the waveform is over. */
#define SYS2_IDLE         0x28

/* SYSST */
#define ST_UVLS           0x20
#define ST_OCDS           0x04
#define ST_OTS            0x02

/* DETCFG1/2 */
#define DET_RL_OS         0x10
#define DET_VBAT_GO       0x02
#define DET_DIAG_GO       0x01

/* CONTCFG1 reset default: EDGE_FRE 0b1110 (700 Hz), F0 detect off, cosine
   edges. The vendor's shape for a filtered square wave; nothing here has a
   reason to prefer another. */
#define CONTCFG1_DEFAULT  0xE1

#define BRK_GAIN          8
#define TRACK_MARGIN      15
#define BRK_TIME          8

#define DRV1_TIME         4
#define DRV2_TIME         6

/* CONTCFG6 */
#define CONT_TRACK_EN     0x80

/* D2S_GAIN codes, SYSCTRL7 bits 2:0. The datasheet's table maps coil
   resistance to the gain that measures it: 2-30 ohms wants 20, 31-60 wants
   10. An LRA of this size is at the bottom of that, so the diagnostic uses
   20 and restores whatever was there afterwards. */
#define D2S_CODE_20       0x06
#define D2S_GAIN_20       20

/* ------------------------------------------------------------- state ---- */

static i2c_master_dev_handle_t s_dev;
static bool s_present;

#define STACK_WORDS  (2560 / sizeof(StackType_t))

static QueueHandle_t     s_q;
static StaticQueue_t     s_q_ctrl;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_ctrl;
static StackType_t       s_stack[STACK_WORDS];
static StaticTask_t      s_tcb;

/* Set by any task, cleared by the worker. Not a mutex-protected
   field: a single aligned bool written by one side and read by the other is
   the one case where that is honest, and taking a lock to ask "should I stop"
   inside the delay loop would mean holding it against the very caller trying
   to stop the motor. */
static volatile bool s_abort;

static uint8_t s_f0_h, s_f0_l;

typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
    uint8_t  strength;   /* percent */
    uint8_t  repeat;
} ls_haptic_pulse_t;

typedef struct {
    ls_haptic_pulse_t pulse[3];
    uint8_t           pulses;
} ls_haptic_pattern_t;

static const ls_haptic_pattern_t s_pattern[LS_HAPTIC_EFFECT_COUNT] = {
    [LS_HAPTIC_TICK]    = { .pulses = 1, .pulse = { { 20,   0,  40, 1 } } },
    [LS_HAPTIC_CLICK]   = { .pulses = 1, .pulse = { { 25,   0,  75, 1 } } },
    [LS_HAPTIC_CONFIRM] = { .pulses = 2, .pulse = { { 30,  60,  70, 1 },
                                                    { 70,   0, 100, 1 } } },
    [LS_HAPTIC_ALERT]   = { .pulses = 1, .pulse = { { 90, 110, 100, 3 } } },
    [LS_HAPTIC_WARN]    = { .pulses = 1, .pulse = { { 220, 180, 100, 2 } } },
};

/* A queued request is either a named pattern or one raw buzz. */
typedef struct {
    bool     raw;
    uint16_t ms;
    uint8_t  strength;
    uint8_t  effect;
} ls_haptic_req_t;

#define QUEUE_DEPTH  4
static uint8_t s_q_store[QUEUE_DEPTH * sizeof(ls_haptic_req_t)];

/* ---------------------------------------------------------- bus access -- */

static bool wr(uint8_t reg, uint8_t val)
{
    if (!s_dev) return false;
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100) == ESP_OK;
}

static bool rd(uint8_t reg, uint8_t *out)
{
    if (!s_dev) return false;
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, 1, 100) == ESP_OK;
}

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* ------------------------------------------------------------- worker --- */

static void drive_start(int strength)
{
    const uint8_t lvl = ls_haptic_level_code(strength);
    lock();
    /* DRV1 is the kick and DRV2 the sustain, so DRV1 is never below DRV2 -
       a kick quieter than what follows it is a fade-in, which is the thing
       DRV1 exists to prevent. */
    wr(R_CONTCFG6, (uint8_t)(CONT_TRACK_EN | lvl));
    wr(R_CONTCFG7, lvl);
    wr(R_PLAYCFG3, PLAY_BRK_EN | PLAY_MODE_CONT);
    wr(R_PLAYCFG4, PLAY_GO);
    unlock();
}

static void drive_stop(void)
{
    lock();
    wr(R_PLAYCFG4, PLAY_STOP);
    /* CONTRD16/17 hold what the tracking loop locked onto during the
       playback that just ended, and they are overwritten by the next one.
       Read them here or never. */
    rd(R_CONTRD16, &s_f0_h);
    rd(R_CONTRD17, &s_f0_l);
    unlock();
}

static void sleep_ms(int ms)
{
    const int slice = 20;
    while (ms > 0 && !s_abort) {
        const int step = ms < slice ? ms : slice;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
}

static void run_pulse(const ls_haptic_pulse_t *p)
{
    for (int i = 0; i < (p->repeat ? p->repeat : 1) && !s_abort; i++) {
        const int on = ls_haptic_clamp_ms(p->on_ms);
        if (on <= 0) continue;
        drive_start(p->strength);
        sleep_ms(on);
        drive_stop();
        if (p->off_ms && i + 1 < (p->repeat ? p->repeat : 1)) sleep_ms(p->off_ms);
    }
}

bool ls_haptic_service(int wait_ms)
{
    if (!s_q) return false;

    ls_haptic_req_t req;
    const TickType_t wait = wait_ms < 0 ? portMAX_DELAY
                                        : pdMS_TO_TICKS((TickType_t)wait_ms);
    if (xQueueReceive(s_q, &req, wait) != pdTRUE) return false;

    /* An abort raised while this request was still in the queue applies to
       the queue, not to this one request: the drain happens in
       ls_haptic_stop and the flag is cleared here so the next thing asked
       for still plays. */
    s_abort = false;

    if (req.raw) {
        const ls_haptic_pulse_t one = { req.ms, 0, req.strength, 1 };
        run_pulse(&one);
    } else if (req.effect < LS_HAPTIC_EFFECT_COUNT) {
        const ls_haptic_pattern_t *pat = &s_pattern[req.effect];
        for (int i = 0; i < pat->pulses && !s_abort; i++) {
            run_pulse(&pat->pulse[i]);
            if (pat->pulse[i].off_ms && i + 1 < pat->pulses)
                sleep_ms(pat->pulse[i].off_ms);
        }
    }
    s_abort = false;
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) ls_haptic_service(-1);
}

/* --------------------------------------------------------------- init --- */

static bool configure(void)
{
    const int f0 = LS_BOARD_HAPTIC_F0_HZ;

    if (!wr(R_SRST, SRST_MAGIC)) return false;
    vTaskDelay(pdMS_TO_TICKS(3));

    /* Power-supply feedback, so a buzz feels the same at 3.4 V as at 4.1 V.
       The datasheet is explicit that this only works in CONT mode, which is
       the mode this driver uses and the reason it was chosen. */
    if (!wr(R_SYSCTRL1, SYS1_VBAT_MODE | SYS1_EN_FIR)) return false;
    wr(R_SYSCTRL2, SYS2_IDLE);

    wr(R_CONTCFG1,  CONTCFG1_DEFAULT);
    wr(R_CONTCFG2,  ls_haptic_f_pre_code(f0));
    wr(R_CONTCFG3,  ls_haptic_drv_width_code(f0, TRACK_MARGIN, BRK_GAIN));
    wr(R_CONTCFG5,  BRK_GAIN);
    wr(R_CONTCFG8,  DRV1_TIME);
    wr(R_CONTCFG9,  DRV2_TIME);
    wr(R_CONTCFG10, BRK_TIME);
    wr(R_CONTCFG11, TRACK_MARGIN);
    wr(R_PLAYCFG3,  PLAY_BRK_EN | PLAY_MODE_CONT);
    return true;
}

esp_err_t ls_haptic_start(void)
{
    if (s_present) return ESP_OK;

    esp_err_t err = ls_i2c_device(LS_I2C_SECONDARY, LS_BOARD_HAPTIC_I2C_ADDR,
                                  100 * 1000, &s_dev);
    if (err != ESP_OK) return err;

    if (ls_i2c_probe(LS_I2C_SECONDARY, LS_BOARD_HAPTIC_I2C_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "no ACK at 0x%02X", LS_BOARD_HAPTIC_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t id = 0;
    if (!rd(R_CHIPID, &id)) {
        ESP_LOGW(TAG, "0x%02X ACKed but would not read", LS_BOARD_HAPTIC_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    if ((id & 0xC1) != 0x80) {
        ESP_LOGW(TAG, "0x%02X is not an AW86224-class part (CHIPID 0x%02X)",
                 LS_BOARD_HAPTIC_I2C_ADDR, id);
        return ESP_ERR_NOT_FOUND;
    }

    if (!configure()) {
        ESP_LOGW(TAG, "configuration writes failed");
        return ESP_FAIL;
    }

    uint8_t st2 = 0;
    if (rd(R_SYSST2, &st2) && !(st2 & 0x08))
        ESP_LOGW(TAG, "LDO_OK is low (SYSST2 0x%02X) - driving anyway", st2);

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    s_q    = xQueueCreateStatic(QUEUE_DEPTH, sizeof(ls_haptic_req_t),
                                s_q_store, &s_q_ctrl);
    if (!s_lock || !s_q) {
        ESP_LOGE(TAG, "could not create the worker's queue or lock");
        return ESP_ERR_NO_MEM;
    }

    /* No affinity: all this task does is write two registers and sleep, so
       pinning it would constrain the scheduler for nothing. */
    if (!xTaskCreateStaticPinnedToCore(worker, "haptic", STACK_WORDS, NULL, 3,
                                       s_stack, &s_tcb, tskNO_AFFINITY)) {
        ESP_LOGE(TAG, "could not start the worker");
        return ESP_ERR_NO_MEM;
    }

    s_present = true;
    ESP_LOGI(TAG, "AW86224 up at 0x%02X, LRA %d Hz (F_PRE %u, DRV_WIDTH %u)",
             LS_BOARD_HAPTIC_I2C_ADDR, LS_BOARD_HAPTIC_F0_HZ,
             (unsigned)ls_haptic_f_pre_code(LS_BOARD_HAPTIC_F0_HZ),
             (unsigned)ls_haptic_drv_width_code(LS_BOARD_HAPTIC_F0_HZ,
                                                TRACK_MARGIN, BRK_GAIN));
    return ESP_OK;
}

bool ls_haptic_present(void) { return s_present; }

/* --------------------------------------------------------------- play --- */

static bool submit(const ls_haptic_req_t *req)
{
    if (!s_present || !s_q) return false;
    /* Never block. This is reachable from the draw path, where a full queue
       has to mean "drop it" - the motor is already busy saying the same
       thing. */
    return xQueueSend(s_q, req, 0) == pdTRUE;
}

bool ls_haptic_play(ls_haptic_effect_t effect)
{
    if (effect < 0 || effect >= LS_HAPTIC_EFFECT_COUNT) return false;
    ls_haptic_req_t req = { .raw = false, .effect = (uint8_t)effect };
    return submit(&req);
}

bool ls_haptic_buzz(int ms, int strength)
{
    ms = ls_haptic_clamp_ms(ms);
    if (ms <= 0) return false;
    if (strength < 0) strength = 0;
    if (strength > 100) strength = 100;
    ls_haptic_req_t req = { .raw = true, .ms = (uint16_t)ms,
                            .strength = (uint8_t)strength };
    return submit(&req);
}

void ls_haptic_stop(void)
{
    if (!s_present) return;
    s_abort = true;
    if (s_q) xQueueReset(s_q);
    /* The STOP register write is left to the worker. Two tasks writing the
       same device handle is the kind of race that shows up once a week on a
       bus this busy, and the worker is never more than one slice away from
       noticing the flag. */
}

/* --------------------------------------------------------------- diag --- */

bool ls_haptic_diag(ls_haptic_diag_t *out)
{
    if (!s_present || !out) return false;
    memset(out, 0, sizeof(*out));

    lock();

    uint8_t st = 0;
    if (rd(R_SYSST, &st)) {
        out->over_current  = (st & ST_OCDS) != 0;
        out->over_temp     = (st & ST_OTS) != 0;
        out->under_voltage = (st & ST_UVLS) != 0;
    }

    /* Supply, per the datasheet's Battery Voltage Detect steps. EN_RAMINIT
       is the digital clock enable and both measurements need it running. */
    uint8_t lo = 0, hi = 0;
    wr(R_SYSCTRL1, SYS1_VBAT_MODE | SYS1_EN_FIR | SYS1_EN_RAMINIT);
    wr(R_DETCFG2, DET_VBAT_GO);
    vTaskDelay(pdMS_TO_TICKS(3));
    wr(R_SYSCTRL1, SYS1_VBAT_MODE | SYS1_EN_FIR);
    if (rd(R_DET_VBAT, &hi) && rd(R_DET_LO, &lo))
        out->vdd_v = ls_haptic_vbat_volts(hi, lo);

    /* Coil resistance, per LRA Resistance Detect. D2S_GAIN has to suit the
       coil being measured and then be put back - the datasheet's own steps
       say save and restore, and leaving it at the diagnostic value would
       change what every later measurement means. */
    uint8_t d2s_pre = 0;
    rd(R_SYSCTRL7, &d2s_pre);
    wr(R_SYSCTRL7, (uint8_t)((d2s_pre & ~0x07) | D2S_CODE_20));
    wr(R_SYSCTRL1, SYS1_VBAT_MODE | SYS1_EN_FIR | SYS1_EN_RAMINIT);
    wr(R_DETCFG1, DET_RL_OS | 0x02);
    wr(R_DETCFG2, DET_DIAG_GO);
    vTaskDelay(pdMS_TO_TICKS(3));
    wr(R_SYSCTRL1, SYS1_VBAT_MODE | SYS1_EN_FIR);
    hi = lo = 0;
    if (rd(R_DET_RL, &hi) && rd(R_DET_LO, &lo))
        out->lra_ohms = ls_haptic_rl_ohms(hi, lo, D2S_GAIN_20);
    wr(R_SYSCTRL7, d2s_pre);
    wr(R_DETCFG1, 0x02);
    wr(R_DETCFG2, 0x00);

    unlock();

    out->f0_hz = ls_haptic_f0_hz(s_f0_h, s_f0_l);
    return true;
}

#else  /* board declares no haptic driver */

esp_err_t ls_haptic_start(void)   { return ESP_ERR_NOT_SUPPORTED; }
bool ls_haptic_present(void)      { return false; }
bool ls_haptic_play(ls_haptic_effect_t e) { (void)e; return false; }
bool ls_haptic_buzz(int ms, int s)        { (void)ms; (void)s; return false; }
void ls_haptic_stop(void)         { }
bool ls_haptic_service(int w)     { (void)w; return false; }
bool ls_haptic_diag(ls_haptic_diag_t *o)  { (void)o; return false; }

#endif
