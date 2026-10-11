/* Wire layout: ICAO UAT technical manual section 3.2.2 and FAA GDL90
   Public ICD Rev A sections 4 and 5. Independent implementation. */
#include "uat_weather.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR uat_weather_text_t s_text[UAT_WX_TEXT_SLOTS];
static EXT_RAM_BSS_ATTR uat_weather_block_t s_blocks[UAT_WX_BLOCK_SLOTS];
static EXT_RAM_BSS_ATTR uat_ground_station_t s_stations[UAT_WX_STATION_SLOTS];
static uat_weather_stats_t s_stats;

void uat_weather_reset(void)
{
    /* Called only with the receive task stopped. */
    memset(s_text, 0, sizeof(s_text));
    memset(s_blocks, 0, sizeof(s_blocks));
    memset(s_stations, 0, sizeof(s_stations));
    memset(&s_stats, 0, sizeof(s_stats));
}

static void count(uint32_t *counter)
{
    portENTER_CRITICAL(&s_lock);
    ++*counter;
    portEXIT_CRITICAL(&s_lock);
}

void uat_weather_stats(uat_weather_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_lock);
}

static bool fresh(bool valid, int64_t ts, int64_t now, int64_t age)
{
    return valid && now >= ts && now - ts <= age;
}

bool uat_weather_text_get(int slot, int64_t now, uat_weather_text_t *out)
{
    if (!out || slot < 0 || slot >= UAT_WX_TEXT_SLOTS) return false;
    portENTER_CRITICAL(&s_lock);
    bool ok = fresh(s_text[slot].valid, s_text[slot].received_us, now, UAT_WX_MAX_AGE_US);
    if (ok) *out = s_text[slot];
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

bool uat_weather_station_get(int slot, int64_t now, uat_ground_station_t *out)
{
    if (!out || slot < 0 || slot >= UAT_WX_STATION_SLOTS) return false;
    portENTER_CRITICAL(&s_lock);
    bool ok = fresh(s_stations[slot].valid, s_stations[slot].received_us, now, UAT_WX_MAX_AGE_US);
    if (ok) *out = s_stations[slot];
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

int uat_weather_blocks_at(double lat, double lon, int64_t now,
                          uat_weather_block_t *out, int capacity)
{
    if (!out || capacity <= 0 || !isfinite(lat) || !isfinite(lon) ||
        lat < -90 || lat > 90 || lon < -180 || lon > 180) return 0;
    int n = 0;
    /* One block per critical section, not a large array copy. */
    for (int i = 0; i < UAT_WX_BLOCK_SLOTS && n < capacity; i++) {
        portENTER_CRITICAL(&s_lock);
        const uat_weather_block_t *b = &s_blocks[i];
        double dx = lon - b->west;
        if (dx < 0) dx += 360;
        if (fresh(b->valid, b->received_us, now, UAT_RADAR_MAX_AGE_US) &&
            lat <= b->north && lat > b->north - b->height && dx < b->width)
            out[n++] = *b;
        portEXIT_CRITICAL(&s_lock);
    }
    return n;
}

static void save_text(uat_weather_text_t *r, int64_t now)
{
    r->text[sizeof(r->text) - 1] = 0;
    if (sscanf(r->text, "%11s %11s %15s", r->type, r->location, r->issued) != 3) return;
    r->received_us = now;
    r->valid = true;
    bool replace = !strcmp(r->type, "METAR") || !strcmp(r->type, "SPECI") ||
                   !strcmp(r->type, "TAF");
    portENTER_CRITICAL(&s_lock);
    int slot = 0;
    for (int i = 0; i < UAT_WX_TEXT_SLOTS; i++) {
        if (!s_text[i].valid || (replace && !strcmp(s_text[i].type, r->type) &&
            !strcmp(s_text[i].location, r->location)) || !strcmp(s_text[i].text, r->text)) {
            slot = i;
            break;
        }
        if (s_text[i].received_us < s_text[slot].received_us) slot = i;
    }
    s_text[slot] = *r;
    s_stats.text_records++;
    portEXIT_CRITICAL(&s_lock);
}

static unsigned bits(const uint8_t *p, unsigned offset, unsigned n)
{
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++) v = (v << 1) | ((p[(offset + i) / 8] >> (7 - (offset + i) % 8)) & 1);
    return v;
}

static void text_product(const uint8_t *p, size_t n, int64_t now)
{
    static const char alphabet[65] = "\003ABCDEFGHIJKLMNOPQRSTUVWXYZ\032\t\036\n| !\"#$%&'()*+,-./0123456789:;<=>?";
    uat_weather_text_t r = {0};
    size_t used = 0, at = 0;
    bool tab = false, overflow = false;
    while (at + 6 <= n * 8) {
        unsigned c = bits(p, (unsigned)at, 6);
        at += 6;
        if (tab) {
            tab = false;
            while (c--) {
                if (used + 1 < sizeof(r.text)) r.text[used++] = ' ';
                else overflow = true;
            }
        } else if (c == 28) tab = true;
        else if (c == 0 || c == 29) {
            r.text[used] = 0;
            if (overflow) count(&s_stats.truncated);
            else if (used) save_text(&r, now);
            memset(&r, 0, sizeof(r));
            used = 0;
            overflow = false;
            if (c == 0) return;
            at = (at + 7) & ~(size_t)7; /* each record is byte padded */
        } else if (c != 27) {
            if (used + 1 < sizeof(r.text)) r.text[used++] = alphabet[c];
            else overflow = true;
        }
    }
    /* A record must have a terminator. Do not publish partial text. */
    if (used || tab) count(&s_stats.malformed);
}

static bool bounds(uat_weather_block_t *b)
{
    static const unsigned scale[] = {1, 5, 9};
    if (b->scale > 2 || b->number >= 607500) return false;
    unsigned row = b->number / 450, col = b->number % 450;
    unsigned factor = scale[b->scale];
    unsigned wide = row >= 900 ? 2 : 1;
    if (wide == 2 && (col & 1)) return false;
    if (b->scale && (row % factor != factor - 1 || col % factor)) return false;
    double lat = row / 15.0;
    b->height = scale[b->scale] / 15.0f;
    b->width = 0.8f * wide * scale[b->scale];
    /* Scaled block numbers identify the high resolution block at the
       northwest corner, not the scaled block's southern edge. */
    b->north = b->south ? (float)-lat : (float)lat + 1.0f / 15;
    b->west = col * 0.8f;
    if (b->west >= 180) b->west -= 360;
    return fabs(b->north) <= 90;
}

static void save_block(uat_weather_block_t *b, int64_t now)
{
    if (!bounds(b)) return;
    b->valid = true;
    b->received_us = now;
    portENTER_CRITICAL(&s_lock);
    int slot = 0;
    for (int i = 0; i < UAT_WX_BLOCK_SLOTS; i++) {
        if (!s_blocks[i].valid || (s_blocks[i].number == b->number &&
            s_blocks[i].product == b->product && s_blocks[i].south == b->south &&
            s_blocks[i].scale == b->scale)) {
            slot = i;
            break;
        }
        if (s_blocks[i].received_us < s_blocks[slot].received_us) slot = i;
    }
    s_blocks[slot] = *b;
    s_stats.radar_blocks++;
    portEXIT_CRITICAL(&s_lock);
}

static bool radar_product(const uint8_t *p, size_t n, unsigned product,
                          unsigned hour, unsigned minute, int64_t now)
{
    if (n < 4) return false;
    uat_weather_block_t b = {.number = ((uint32_t)(p[0] & 15) << 16) | ((uint32_t)p[1] << 8) | p[2],
        .product = product, .scale = (p[0] >> 4) & 3, .south = (p[0] & 64) != 0,
        .hour = hour, .minute = minute};
    if (!bounds(&b)) return false;
    if (p[0] & 128) {
        unsigned used = 0;
        for (size_t i = 3; i < n; i++) {
            unsigned run = (p[i] >> 3) + 1;
            if (used + run > 128) return false;
            memset(b.pixels + used, p[i] & 7, run);
            used += run;
        }
        if (used != 128) return false;
        save_block(&b, now);
    } else {
        unsigned extra = p[3] & 15;
        if (n != 4 + extra) return false;
        /* Base block is empty. Bitmap bits describe following blocks,
           least significant first; the initial nibble has four bits. */
        save_block(&b, now);
        uint32_t base = b.number;
        static const unsigned scale[] = {1, 5, 9};
        unsigned step = b.scale ? scale[b.scale] : base / 450 >= 900 ? 2 : 1;
        for (unsigned i = 0; i < 4 + extra * 8; i++) {
            unsigned value = i < 4 ? (p[3] >> (4 + i)) & 1 : (p[4 + (i - 4) / 8] >> ((i - 4) % 8)) & 1;
            if (!value) continue;
            b.number = base / 450 * 450 + (base % 450 + (i + 1) * step) % 450;
            save_block(&b, now);
        }
    }
    return true;
}

bool uat_weather_apdu(const uint8_t *p, size_t n, int64_t now)
{
    if (!p || n < 4 || n > 422) { count(&s_stats.malformed); return false; }
    count(&s_stats.apdus);
    /* Optional product descriptors and segmented APDUs are rejected,
       rather than misreading them as independent weather products. */
    if ((p[0] & 224) || (p[1] & 2)) { count(&s_stats.unsupported); return false; }
    unsigned product = ((unsigned)(p[0] & 31) << 6) | (p[1] >> 2);
    unsigned opt = ((p[1] & 1) << 1) | (p[2] >> 7);
    unsigned time_bits[] = {11, 17, 20, 26};
    unsigned header = (17 + time_bits[opt] + 7) / 8;
    if (n < header) { count(&s_stats.malformed); return false; }
    unsigned time_at = 17 + (opt >= 2 ? 9 : 0);
    unsigned hour = bits(p, time_at, 5), minute = bits(p, time_at + 5, 6);
    if (hour > 23 || minute > 59) { count(&s_stats.malformed); return false; }
    if (product == 413) {
        text_product(p + header, n - header, now);
        return true;
    }
    if (product == 63 || product == 64) {
        bool ok = radar_product(p + header, n - header, product, hour, minute, now);
        if (!ok) count(&s_stats.malformed);
        return ok;
    }
    count(&s_stats.unsupported);
    return false;
}

void uat_weather_uplink(const uint8_t p[432], int64_t now)
{
    if (!p) return;
    uat_ground_station_t g = {.site = p[7] >> 4, .slot = p[6] & 31,
        .position_valid = (p[5] & 1) != 0, .valid = true, .received_us = now};
    uint32_t lat = ((uint32_t)p[0] << 15) | ((uint32_t)p[1] << 7) | (p[2] >> 1);
    uint32_t lon = ((uint32_t)(p[2] & 1) << 23) | ((uint32_t)p[3] << 15) | ((uint32_t)p[4] << 7) | (p[5] >> 1);
    g.lat = lat * (360.0 / 16777216);
    if (g.lat > 90) g.lat -= 180;
    g.lon = lon * (360.0 / 16777216);
    if (g.lon > 180) g.lon -= 360;
    portENTER_CRITICAL(&s_lock);
    int slot = 0;
    for (int i = 0; i < UAT_WX_STATION_SLOTS; i++) {
        if (!s_stations[i].valid || (s_stations[i].site == g.site &&
            s_stations[i].position_valid == g.position_valid &&
            fabsf(s_stations[i].lat - g.lat) < 0.0001f &&
            fabsf(s_stations[i].lon - g.lon) < 0.0001f)) { slot = i; break; }
        if (s_stations[i].received_us < s_stations[slot].received_us) slot = i;
    }
    s_stations[slot] = g;
    portEXIT_CRITICAL(&s_lock);
    if (!(p[6] & 32)) return; /* Application Data Valid */
    size_t at = 8;
    while (at + 2 <= 432) {
        unsigned n = ((unsigned)p[at] << 1) | (p[at + 1] >> 7);
        unsigned type = p[at + 1] & 15;
        if (!n) break;
        if (n > 422 || at + 2 + n > 432 || (p[at + 1] & 112)) {
            count(&s_stats.malformed);
            break;
        }
        if (!type) uat_weather_apdu(p + at + 2, n, now);
        else count(&s_stats.unsupported);
        at += 2 + n;
    }
}
