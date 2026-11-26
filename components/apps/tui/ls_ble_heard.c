#include "ls_ble_heard.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

EXT_RAM_BSS_ATTR static ls_ble_heard_t s_heard[LS_BLE_HEARD_MAX];
static int s_count;
static StaticSemaphore_t s_lock_memory;
static SemaphoreHandle_t s_lock;
static unsigned s_lock_once;
static void lock_init(void)
{
    unsigned expected = 0;
    if (__atomic_compare_exchange_n(&s_lock_once, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_memory);
        __atomic_store_n(&s_lock_once, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&s_lock_once, __ATOMIC_ACQUIRE) != 2) vTaskDelay(1);
    }
}

static bool lock(void)
{
    lock_init();
    return s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}
static void unlock(void) { xSemaphoreGive(s_lock); }

void ls_ble_heard_note_ad(const uint8_t addr[6], const uint8_t *ad, int len)
{
    if (!addr || !ad || len <= 0) return;
    uint8_t have = 0;
    uint16_t company = 0, uuid = 0, appearance = 0;
    int16_t mfg0 = -1, svc0 = -1;
    bool svc_data = false;
    for (int i = 0; i + 1 < len;) {
        const int l = ad[i];
        if (l == 0 || i + 1 + l > len) break;
        const uint8_t t = ad[i + 1];
        const uint8_t *v = ad + i + 2;
        const int n = l - 1;
        if (t == 0xFF && n >= 2 && !(have & 1)) {
            company = (uint16_t)(v[0] | v[1] << 8);
            mfg0 = n >= 3 ? v[2] : -1;
            have |= 1;
        } else if (t == 0x16 && n >= 2 && !svc_data) {     /* service data, 16-bit UUID: wins over a list */
            uuid = (uint16_t)(v[0] | v[1] << 8);
            svc0 = n >= 3 ? v[2] : -1;
            svc_data = true;
            have |= 2;
        } else if (t == 0x19 && n >= 2 && !(have & 4)) {
            appearance = (uint16_t)(v[0] | v[1] << 8);
            have |= 4;
        } else if ((t == 0x02 || t == 0x03) && n >= 2 && !(have & 2)) {
            uuid = (uint16_t)(v[0] | v[1] << 8);
            svc0 = -1;
            have |= 2;
        }
        i += 1 + l;
    }
    if (!have || !lock()) return;
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_heard[i].addr, addr, 6)) continue;
        ls_ble_heard_t *d = &s_heard[i];
        if (have & 1) { d->company = company; d->mfg0 = mfg0; }
        if (have & 2) { d->uuid16 = uuid; d->svc0 = svc0; }
        if (have & 4) d->appearance = appearance;
        d->ad_have |= have;
        break;
    }
    unlock();
}

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
        s_heard[slot].addr_type = LS_BLE_HEARD_TYPE_UNKNOWN;
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

void ls_ble_heard_note_type(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr || !lock()) return;
    for (int i = 0; i < s_count; i++)
        if (!memcmp(s_heard[i].addr, addr, 6)) { s_heard[i].addr_type = addr_type; break; }
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
