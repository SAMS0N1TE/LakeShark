/* What each core was doing when it entered the panic handler.

   The panic handler can hang before it prints or saves a core dump - two
   cores entering it together stall each other (IDF-12900) - and then the
   hardware watchdog resets the chip with nothing recorded but the handler's
   own PC. This notes the trap frame on entry, in RTC memory that survives
   the reset, before anything that can hang. */

#include "panic_crumb.h"
#include "ls_errlog.h"

#include "esp_attr.h"
#include "esp_console.h"
#include "esp_cpu.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRUMB_MAGIC 0x4C534352u

/* Each core's ticks and the task its last tick interrupted. The interrupt
   watchdog only says which core caught it: on 2026-10-04 the SX board's
   core 1 caught one sitting idle, and the LR2021 board's core 0 another from
   inside a ROM cache routine, each because the other core had stopped
   ticking. A core that stops ticking stops writing here, so after the reset
   the one with fewer ticks is the core that froze and its task is what it
   was running. */
typedef struct {
    uint32_t magic;
    uint32_t ticks;
    char task[CONFIG_FREERTOS_MAX_TASK_NAME_LEN];
} tick_crumb_t;

RTC_NOINIT_ATTR static tick_crumb_t s_tick[2];
static tick_crumb_t s_tick_last[2];
static TaskHandle_t s_tick_task[2];

static void IRAM_ATTR tick_note(void)
{
    const int core = esp_cpu_get_core_id() & 1;
    tick_crumb_t *t = &s_tick[core];
    const TaskHandle_t h = xTaskGetCurrentTaskHandleForCore(core);
    if (h != s_tick_task[core] || t->magic != CRUMB_MAGIC) {
        s_tick_task[core] = h;
        const char *name = h ? pcTaskGetName(h) : "-";
        for (int i = 0; i < CONFIG_FREERTOS_MAX_TASK_NAME_LEN; i++) {
            t->task[i] = name[i];
            if (!name[i]) break;
        }
        t->task[CONFIG_FREERTOS_MAX_TASK_NAME_LEN - 1] = 0;
        t->magic = CRUMB_MAGIC;
    }
    t->ticks++;
}

typedef struct {
    uint32_t magic;
    uint32_t entries;   /* times this core entered the handler */
    uint32_t how;       /* 1 = interrupt-raised (watchdog, cache error), 2 = exception */
    uint32_t mepc, mcause, mtval, ra, sp;
} crumb_t;

RTC_NOINIT_ATTR static crumb_t s_crumb[2];
static crumb_t s_last[2];

void __real_panicHandler(void *frame);
void __real_xt_unhandled_exception(void *frame);

static void IRAM_ATTR note(void *frame, uint32_t how)
{
    const RvExcFrame *f = (const RvExcFrame *)frame;
    crumb_t *c = &s_crumb[esp_cpu_get_core_id() & 1];
    if (c->magic != CRUMB_MAGIC) {
        c->magic = CRUMB_MAGIC;
        c->entries = 0;
    }
    if (c->entries++ == 0) {
        c->how = how;
        c->mepc = f->mepc;
        c->mcause = f->mcause;
        c->mtval = f->mtval;
        c->ra = f->ra;
        c->sp = f->sp;
    }
}

void IRAM_ATTR __wrap_panicHandler(void *frame)
{
    note(frame, 1);
    __real_panicHandler(frame);
}

void IRAM_ATTR __wrap_xt_unhandled_exception(void *frame)
{
    note(frame, 2);
    __real_xt_unhandled_exception(frame);
}

/* "core1 stopped first: 'p25_rx' at tick 2534990, core0 ran on 412 ticks
   ('IDLE0')", or nothing when the run before had no ticks recorded */
static bool tick_line(char *out, size_t n)
{
    const tick_crumb_t *a = &s_tick_last[0], *b = &s_tick_last[1];
    if (a->magic != CRUMB_MAGIC || b->magic != CRUMB_MAGIC) return false;
    const int first = a->ticks <= b->ticks ? 0 : 1;
    const tick_crumb_t *f = &s_tick_last[first], *o = &s_tick_last[!first];
    snprintf(out, n, "core%d stopped first: '%.*s' at tick %lu, core%d ran on %lu ticks ('%.*s')",
             first, CONFIG_FREERTOS_MAX_TASK_NAME_LEN, f->task, (unsigned long)f->ticks, !first,
             (unsigned long)(o->ticks - f->ticks), CONFIG_FREERTOS_MAX_TASK_NAME_LEN, o->task);
    return true;
}

static void print_last(void)
{
    bool any = false;
    for (int core = 0; core < 2; core++) {
        const crumb_t *c = &s_last[core];
        if (c->magic != CRUMB_MAGIC) continue;
        any = true;
        printf("core%d %s mcause=0x%08lx mepc=0x%08lx mtval=0x%08lx ra=0x%08lx sp=0x%08lx entries=%lu\n",
               core, c->how == 1 ? "interrupt" : "exception",
               (unsigned long)c->mcause, (unsigned long)c->mepc, (unsigned long)c->mtval,
               (unsigned long)c->ra, (unsigned long)c->sp, (unsigned long)c->entries);
    }
    if (!any) printf("no panic recorded before this boot\n");
    char line[160];
    if (tick_line(line, sizeof(line))) printf("ticks: %s\n", line);
}

bool panic_crumb_text(char *out, size_t n)
{
    if (!out || n == 0) return false;
    out[0] = '\0';
    size_t k = 0;
    bool any = false;
    char line[160];
    for (int core = 0; core < 2; core++) {
        const crumb_t *c = &s_last[core];
        if (c->magic != CRUMB_MAGIC) continue;
        any = true;
        snprintf(line, sizeof(line), "core%d %s mcause %lx pc %08lx tval %lx ra %08lx sp %08lx\n", core,
                 c->how == 1 ? "int" : "exc", (unsigned long)c->mcause, (unsigned long)c->mepc,
                 (unsigned long)c->mtval, (unsigned long)c->ra, (unsigned long)c->sp);
        k += (size_t)snprintf(out + k, n - k, "%s", line);
        if (k >= n) return any;
    }
    if (tick_line(line, sizeof(line)) && k < n) snprintf(out + k, n - k, "ticks: %s", line);
    return any;
}

/* `crumb selftest panic|hang` ends this run on purpose, to watch the error
   log catch it: panic aborts, hang stops this core with its interrupts
   masked so that only the interrupt watchdog ends it. */
static void selftest(bool hang)
{
    ESP_LOGW("crumb", "self test: ending this run with a %s", hang ? "hang" : "panic");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!hang) abort();
    portDISABLE_INTERRUPTS();
    for (;;) { }
}

static int cmd_crumb(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "selftest")) {
        if (!strcmp(argv[2], "panic") || !strcmp(argv[2], "hang")) selftest(!strcmp(argv[2], "hang"));
        printf("crumb selftest panic|hang: ends this run on purpose\n");
        return 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "log")) {
        ls_errlog_print_all();
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        printf(ls_errlog_clear() ? "error log cleared\n" : "error log: clear failed\n");
        return 0;
    }
    print_last();
    printf("errors saved: %d ('crumb log' prints them, 'crumb clear' empties the log)\n", ls_errlog_count());
    return 0;
}

void panic_crumb_boot(void)
{
    for (int core = 0; core < 2; core++) {
        s_last[core] = s_crumb[core];
        if (s_last[core].magic == CRUMB_MAGIC)
            ESP_LOGW("crumb", "last panic, core%d: mcause=0x%08lx mepc=0x%08lx mtval=0x%08lx ra=0x%08lx",
                     core, (unsigned long)s_last[core].mcause, (unsigned long)s_last[core].mepc,
                     (unsigned long)s_last[core].mtval, (unsigned long)s_last[core].ra);
        memset(&s_crumb[core], 0, sizeof(s_crumb[core]));
    }
    /* the ticks matter after a panic: a clean restart leaves both cores level */
    for (int core = 0; core < 2; core++) {
        s_tick_last[core] = s_tick[core];
        memset(&s_tick[core], 0, sizeof(s_tick[core]));
    }
    char line[160];
    if ((s_last[0].magic == CRUMB_MAGIC || s_last[1].magic == CRUMB_MAGIC) && tick_line(line, sizeof(line)))
        ESP_LOGW("crumb", "ticks: %s", line);
    for (int core = 0; core < 2; core++)
        esp_register_freertos_tick_hook_for_cpu(tick_note, core);
}

void panic_crumb_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "crumb",
        .help = "Trap cause and PC of each core at the last panic, even one that hung; "
                "'log' prints the saved error records, 'clear' empties them",
        .hint = "[log|clear|selftest panic|hang]",
        .func = &cmd_crumb,
    };
    esp_console_cmd_register(&cmd);
}
