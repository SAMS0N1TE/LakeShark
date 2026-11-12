/* Worker adapter over the firmware's checksummed/staged scan profile files. */
#include "ls_lap_radio_io.h"
#include "scan_channels.h"
#include "scan_sd_store.h"
#include <stdlib.h>
#include <string.h>
#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#define _mkdir(path) mkdir((path), 0777)
#endif
#endif
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "esp_heap_caps.h"
#define ALLOC(n) heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define ALLOC(n) calloc(1, n)
#endif
int ls_lap_io_channels(const char *bank, uint32_t first, lap_channel_t *rows, int count,
                       bool write) {
    if (!scan_sd_name(bank))
        return LAP_RX_EINVAL;
    scan_channel_t *all = ALLOC(sizeof(*all) * SCAN_MAX_CHANNELS);
    if (!all)
        return LAP_RX_ENOMEM;
    int n = 0, e = 0;
    if (write) {
#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
        _mkdir("sdcard/channels");
#endif
        for (int i = 0; i < count; i++) {
            scan_channel_t *c = all + i;
            lap_channel_t *r = rows + i;
            memcpy(c->name, r->name, 16);
            c->freq_hz = r->frequency_hz;
            c->mode = r->mode;
            c->flags = r->flags;
            c->zone = r->bank;
            c->lat_e7 = r->lat_e7;
            c->lon_e7 = r->lon_e7;
            c->radius_m = r->radius_m;
            c->rsv = r->tone & 255;
            if (r->tone & 256)
                c->flags |= SCAN_FLAG_TONE_HIGH;
            else
                c->flags &= ~SCAN_FLAG_TONE_HIGH;
            if (!c->name[0] || c->freq_hz < 1000000 || c->freq_hz > 2000000000 ||
                c->lat_e7 < -900000000 || c->lat_e7 > 900000000 || c->lon_e7 < -1800000000 ||
                c->lon_e7 > 1800000000 || c->radius_m > 500000) {
                free(all);
                return LAP_RX_ERANGE;
            }
        }
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
        bool ok = !strcmp(bank, "active") ? scan_channels_replace(all, count)
                                          : scan_sd_write(bank, all, count);
#else
        bool ok = scan_sd_write(bank, all, count);
#endif
        e = ok ? count : LAP_RX_EIO;
    } else {
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
        if (!strcmp(bank, "active")) {
            n = scan_channels_count();
            for (int i = 0; i < n; i++) {
                const scan_channel_t *c = scan_channel_get(i);
                if (c)
                    all[i] = *c;
            }
        } else
            n = scan_sd_read(bank, all);
#else
        n = scan_sd_read(bank, all);
#endif
        if (n < 0)
            e = LAP_RX_EIO;
        else {
            for (uint32_t i = first; i < (uint32_t)n && e < count; i++, e++) {
                scan_channel_t *c = all + i;
                lap_channel_t *r = rows + e;
                memset(r, 0, sizeof(*r));
                r->size = sizeof(*r);
                r->version = 1;
                r->id = i;
                r->frequency_hz = c->freq_hz;
                r->mode = c->mode;
                r->flags = c->flags;
                r->bank = c->zone;
                r->lat_e7 = c->lat_e7;
                r->lon_e7 = c->lon_e7;
                r->radius_m = c->radius_m;
                r->tone = scan_channel_tone(c);
                memcpy(r->name, c->name, 16);
            }
        }
    }
    free(all);
    return e;
}
