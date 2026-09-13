/* Host-only scanner controls; RF progression is tested separately. */
#include "scan_engine.h"
#include "scan_channels.h"
#include <stdio.h>
#include <string.h>
static scan_channel_t channels[SCAN_MAX_CHANNELS];
static int count, zone = -1, candidate = -1;
static int threshold=4,hang=3000;
static bool active, hold;
static scan_src_t source;
static uint32_t lo = 150000000, hi = 162000000, step = 12500;
int scan_channels_count(void)
{
    return count;
}
const scan_channel_t *scan_channel_get(int i)
{
    return i >= 0 && i < count ? &channels[i] : NULL;
}
int scan_channel_add(const char *name, uint32_t hz, scan_mode_t mode, uint8_t z)
{
    if (count == SCAN_MAX_CHANNELS)
        return -1;
    scan_channel_t *c = &channels[count];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->freq_hz = hz;
    c->mode = mode;
    c->zone = z;
    c->flags = SCAN_FLAG_ENABLED;
    return count++;
}
int scan_channel_find_freq_zone(uint32_t hz, uint8_t z)
{
    for (int i = 0; i < count; i++)
        if (channels[i].freq_hz == hz && channels[i].zone == z)
            return i;
    return -1;
}
bool scan_channel_set_enabled(int i, bool enabled)
{
    if (i < 0 || i >= count)
        return false;
    if (enabled)
        channels[i].flags |= SCAN_FLAG_ENABLED;
    else
        channels[i].flags &= ~SCAN_FLAG_ENABLED;
    return true;
}
void scan_engine_start(void)
{
    active = true;
    hold = false;
    candidate = count ? 0 : -1;
}
void scan_engine_stop(void)
{
    active = hold = false;
    candidate = -1;
}
bool scan_engine_active(void)
{
    return active;
}
void scan_engine_next(void)
{
    hold = false;
    if (count)
        candidate = (candidate + 1) % count;
}
void scan_engine_skip(void)
{
    scan_engine_next();
}
void scan_engine_hold(bool h)
{
    hold = active && candidate >= 0 && h;
}
bool scan_engine_manual_hold(void)
{
    return hold;
}
int scan_engine_candidate(void)
{
    return candidate;
}
int scan_engine_get_hang_ms(void)
{
    return hang;
}
int scan_engine_get_zone(void)
{
    return zone;
}
void scan_engine_set_zone(int z)
{
    zone = z;
}
scan_src_t scan_engine_get_source(void)
{
    return source;
}
void scan_engine_set_source(scan_src_t s)
{
    source = s;
}
void scan_engine_get_band(uint32_t *a, uint32_t *b, uint32_t *s)
{
    if (a)
        *a = lo;
    if (b)
        *b = hi;
    if (s)
        *s = step;
}
bool scan_engine_set_band(uint32_t a, uint32_t b, uint32_t s)
{
    lo = a;
    hi = b;
    step = s;
    return true;
}
void scan_engine_status(char *s, size_t n)
{
    snprintf(s, n, "%s / HOST CONTROL FIXTURE",
             active ? (count ? "SCANNING" : "NO SAVED CHANNELS") : "OFF");
}

void scan_channels_clear(void)
{
    count = 0;
    scan_engine_stop();
}

int scan_engine_get_threshold_pct(void){return threshold;}
void scan_engine_set_threshold_pct(int v){threshold=v;}
void scan_engine_set_hang_ms(int v){hang=v;}
