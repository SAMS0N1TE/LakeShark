/* See ls_vitals.h. */
#include "ls_vitals.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>

static ls_trend_t s_internal;
static ls_trend_t s_largest;
static ls_trend_t s_psram;
static ls_trend_t s_dma;
static uint32_t   s_internal_low;
static uint32_t   s_psram_low;
static uint32_t   s_dma_low;
static uint32_t   s_samples;
static int64_t    s_next_us;

/* A sample is sixteen bits, so the unit has to suit the quantity - and getting that wrong flattens the only thing the trace is for. */

#define VITALS_INTERNAL_UNIT 64u
#define VITALS_PSRAM_UNIT    1024u

static uint16_t as_units(size_t bytes, unsigned unit)
{
    const size_t n = bytes / unit;
    /* A pegged trace is a wrong answer that looks wrong. A wrapped one looks
       like the heap just emptied. */
    return n > 65535u ? (uint16_t)65535u : (uint16_t)n;
}

void ls_vitals_tick(int64_t now_us)
{
    if (s_next_us && now_us < s_next_us) return;
    s_next_us = now_us + LS_VITALS_INTERVAL_US;

    multi_heap_info_t hi;

    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    ls_trend_push(&s_internal,
                  as_units(hi.total_free_bytes, VITALS_INTERNAL_UNIT));
    ls_trend_push(&s_largest,
                  as_units(hi.largest_free_block, VITALS_INTERNAL_UNIT));
    s_internal_low = (uint32_t)hi.minimum_free_bytes;

    heap_caps_get_info(&hi, MALLOC_CAP_SPIRAM);
    ls_trend_push(&s_psram, as_units(hi.total_free_bytes, VITALS_PSRAM_UNIT));
    s_psram_low = (uint32_t)hi.minimum_free_bytes;

    /* In the internal unit, because that is what it is: DMA-capable
       memory is a subset of internal RAM with its own placement rules, and
       it is measured in the same kilobytes. */
    heap_caps_get_info(&hi, MALLOC_CAP_DMA);
    ls_trend_push(&s_dma, as_units(hi.total_free_bytes, VITALS_INTERNAL_UNIT));
    s_dma_low = (uint32_t)hi.minimum_free_bytes;

    if (s_samples < 0xFFFFFFFFu) s_samples++;
}

const ls_trend_t *ls_vitals_internal(void) { return &s_internal; }
const ls_trend_t *ls_vitals_largest(void)  { return &s_largest; }
const ls_trend_t *ls_vitals_psram(void)    { return &s_psram; }
const ls_trend_t *ls_vitals_dma(void)      { return &s_dma; }

uint32_t ls_vitals_internal_low(void) { return s_internal_low; }
uint32_t ls_vitals_psram_low(void)    { return s_psram_low; }
uint32_t ls_vitals_dma_low(void)      { return s_dma_low; }

uint32_t ls_vitals_watched_s(void)
{
    return (uint32_t)((uint64_t)s_samples * LS_VITALS_INTERVAL_US / 1000000u);
}

static ls_vitals_mark_t s_marks[LS_VITALS_MARKS];
static int              s_mark_n;

void ls_vitals_mark(const char *name)
{
    if (!name || !*name) return;
    if (s_mark_n >= LS_VITALS_MARKS) return;
    /* A boot that revisits a stage - a retry, a safe-mode second pass - must
       not push the early readings out, because the early ones are the point. */
    for (int i = 0; i < s_mark_n; i++)
        if (!strncmp(s_marks[i].name, name, sizeof(s_marks[i].name) - 1)) return;

    ls_vitals_mark_t *m = &s_marks[s_mark_n];
    snprintf(m->name, sizeof(m->name), "%s", name);

    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    m->internal = (uint32_t)hi.total_free_bytes;
    heap_caps_get_info(&hi, MALLOC_CAP_DMA);
    m->dma = (uint32_t)hi.total_free_bytes;
    heap_caps_get_info(&hi, MALLOC_CAP_SPIRAM);
    m->psram_kb = (uint32_t)(hi.total_free_bytes / 1024u);

    s_mark_n++;
}

int ls_vitals_mark_count(void) { return s_mark_n; }

bool ls_vitals_mark_at(int i, ls_vitals_mark_t *out)
{
    if (!out || i < 0 || i >= s_mark_n) return false;
    *out = s_marks[i];
    return true;
}
