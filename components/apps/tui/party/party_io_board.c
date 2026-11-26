/* PARTY on the board (party_io.h): esp_timer, ls_location and the compass,
   the fuel gauge, the haptic driver, the SD card, PSRAM, a FreeRTOS mutex
   and a worker task, and ls_notify for banners. */
#include "party_io.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ls_gauge.h"
#include "ls_haptic.h"
#include "ls_imu.h"
#include "ls_location.h"
#include "ls_sdcard.h"
#include "ls_task_reap.h"
#include "ls_time.h"

static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static int (*s_fn)(void);

uint32_t party_io_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool party_io_fix(party_fix_t *f)
{
    ls_location_t l;
    ls_location_get(&l);
    f->fix = l.fix;
    f->lat = l.lat;
    f->lon = l.lon;
    f->acc_m = l.hdop > 0 ? l.hdop * 5.0f : -1;
    f->speed_mps = -1;
    f->course_deg = -1;
    const float h = ls_imu_heading();
    f->heading_deg = h >= 0 && h <= 360 ? h : -1;
    return f->fix;
}

int party_io_batt(void)
{
    ls_gauge_t g;
    return ls_gauge_get(&g) && g.present ? g.percent : -1;
}

bool party_io_vibe(int ms, int strength) { return ls_haptic_buzz(ms, strength); }
bool party_io_vibe_present(void) { return ls_haptic_present() && ls_haptic_enabled(); }

int party_io_local_hour(void)
{
    if (!ls_time_is_synced()) return -1;
    const time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour;
}

const char *party_io_dir(void) { return "/sdcard/party"; }
bool party_io_card(void) { return ls_sdcard_mounted(); }
bool party_io_mkdir(const char *path) { return mkdir(path, 0775) == 0; }
void *party_io_alloc(size_t bytes) { return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

void party_io_lock(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}
void party_io_unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

static void worker(void *arg)
{
    (void)arg;
    int wait;
    while ((wait = s_fn()) > 0) vTaskDelay(pdMS_TO_TICKS(wait));
    s_task = NULL;
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    ls_task_retire_self();          /* no helper task in internal RAM (ls_task_reap.h) */
#else
    vTaskDelete(NULL);
#endif
}

bool party_io_spawn(int (*fn)(void))
{
    s_fn = fn;
    if (s_task) return true;
    /* The link, the card and the motor: no NVS or internal flash, so the
       stack may be PSRAM, as WARDRIVE's worker is. */
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    const BaseType_t ok = xTaskCreateWithCaps(worker, "party", 6144, NULL, 3, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    const BaseType_t ok = xTaskCreate(worker, "party", 6144, NULL, 3, &s_task);
#endif
    if (ok != pdPASS) s_task = NULL;
    return s_task != NULL;
}

bool party_io_worker_alive(void) { return s_task != NULL; }
const party_link_ops_t *party_io_link(void) { return party_link_board(); }
uint32_t party_io_random(void) { return esp_random(); }
