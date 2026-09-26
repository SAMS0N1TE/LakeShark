#include "ls_ble_heard.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

EXT_RAM_BSS_ATTR static ls_ble_heard_t s_heard[LS_BLE_HEARD_MAX];
static int s_count;
static StaticSemaphore_t s_lock_memory;
static SemaphoreHandle_t s_lock;

static bool lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_memory);
    return s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}
static void unlock(void) { xSemaphoreGive(s_lock); }

void ls_ble_heard_note(const uint8_t addr[6], const char *name, int name_len,
                       int rssi, int64_t now_us)
{
    if (!addr || !lock()) return;
    int slot = -1, oldest = 0;
    for (int i = 0; i < s_count; i++) {
        if (!memcmp(s_heard[i].addr, addr, 6)) { slot = i; break; }
        if (s_heard[i].last_us < s_heard[oldest].last_us) oldest = i;
    }
    if (slot < 0) {
        /* Full: the device heard longest ago makes room. */
        slot = s_count < LS_BLE_HEARD_MAX ? s_count++ : oldest;
        memset(&s_heard[slot], 0, sizeof(s_heard[slot]));
        memcpy(s_heard[slot].addr, addr, 6);
        s_heard[slot].smooth = (float)rssi;
    }
    ls_ble_heard_t *d = &s_heard[slot];
    /* Adverts come and go; a device keeps the name it once gave. */
    if (name && name_len > 0) {
        const int n = name_len < (int)sizeof(d->name) - 1 ? name_len : (int)sizeof(d->name) - 1;
        memcpy(d->name, name, (size_t)n); d->name[n] = 0;
    }
    d->rssi = (int8_t)rssi;
    d->smooth += 0.35f * ((float)rssi - d->smooth);
    d->adverts++;
    d->last_us = now_us;
    unlock();
}

static int by_strength(const void *a, const void *b)
{
    const float x = ((const ls_ble_heard_t *)a)->smooth, y = ((const ls_ble_heard_t *)b)->smooth;
    return x > y ? -1 : x < y ? 1 : 0;
}

int ls_ble_heard_list(ls_ble_heard_t *out, int cap, int64_t now_us, int64_t max_age_us)
{
    if (!out || cap <= 0 || !lock()) return 0;
    int n = 0;
    for (int i = 0; i < s_count && n < cap; i++)
        if (now_us - s_heard[i].last_us <= max_age_us) out[n++] = s_heard[i];
    unlock();
    qsort(out, (size_t)n, sizeof(out[0]), by_strength);
    return n;
}

bool ls_ble_heard_find(const uint8_t addr[6], ls_ble_heard_t *out)
{
    if (!addr || !lock()) return false;
    bool found = false;
    for (int i = 0; i < s_count && !found; i++)
        if (!memcmp(s_heard[i].addr, addr, 6)) { if (out) *out = s_heard[i]; found = true; }
    unlock();
    return found;
}

void ls_ble_heard_clear(void) { if (lock()) { s_count = 0; unlock(); } }
