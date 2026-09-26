#include "ls_trail.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

#define TRAIL_MAGIC 0x5452414Cu   /* "TRAL" */
#define TAG_LEN     12

typedef struct {
    char tag[TAG_LEN];
    uint32_t count;     /* stamps since boot */
    uint32_t at_ms;     /* uptime at the last stamp */
} spot_t;

typedef struct {
    uint32_t magic;
    spot_t spot[LS_TRAIL_SLOTS];
} trail_t;

RTC_NOINIT_ATTR static trail_t s_live;
static trail_t s_prev;
static int s_prev_reset = -1;

static const char *const NAMES[LS_TRAIL_SLOTS] = { "tui", "field", "find", "mixrf", "link" };

void ls_trail(ls_trail_slot_t slot, const char *tag)
{
    if ((unsigned)slot >= LS_TRAIL_SLOTS || s_live.magic != TRAIL_MAGIC) return;
    spot_t *s = &s_live.spot[slot];
    strncpy(s->tag, tag ? tag : "", TAG_LEN - 1);
    s->tag[TAG_LEN - 1] = '\0';
    s->count++;
    s->at_ms = esp_log_timestamp();
}

void ls_trail_boot(void)
{
    const esp_reset_reason_t r = esp_reset_reason();
    /* Only a reset nobody asked for leaves a trail worth reading; a power-on
       leaves RTC memory as noise. */
    const bool unasked = r == ESP_RST_WDT || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                         r == ESP_RST_PANIC || r == ESP_RST_CPU_LOCKUP || r == ESP_RST_BROWNOUT;
    if (unasked && s_live.magic == TRAIL_MAGIC) {
        s_prev = s_live;
        s_prev_reset = (int)r;
        ls_trail_print();
    }
    memset(&s_live, 0, sizeof(s_live));
    s_live.magic = TRAIL_MAGIC;
}

static void print_one(const char *title, const trail_t *t)
{
    printf("trail: %s\n", title);
    for (int i = 0; i < LS_TRAIL_SLOTS; i++) {
        const spot_t *s = &t->spot[i];
        if (!s->count) { printf("trail:   %-6s -\n", NAMES[i]); continue; }
        printf("trail:   %-6s %-11.11s  at %lu.%03lu s  (%lu stamps)\n", NAMES[i], s->tag,
               (unsigned long)(s->at_ms / 1000), (unsigned long)(s->at_ms % 1000), (unsigned long)s->count);
    }
}

void ls_trail_print(void)
{
    if (s_prev_reset >= 0) {
        char title[64];
        snprintf(title, sizeof(title), "the run before this boot, reset reason %d", s_prev_reset);
        print_one(title, &s_prev);
    }
    if (s_live.magic == TRAIL_MAGIC) print_one("this run", &s_live);
}
