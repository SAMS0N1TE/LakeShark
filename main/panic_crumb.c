/* What each core was doing when it entered the panic handler.

   The panic handler can hang before it prints or saves a core dump - two
   cores entering it together stall each other (IDF-12900) - and then the
   hardware watchdog resets the chip with nothing recorded but the handler's
   own PC. This notes the trap frame on entry, in RTC memory that survives
   the reset, before anything that can hang. */

#include "panic_crumb.h"

#include "esp_attr.h"
#include "esp_console.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "riscv/rvruntime-frames.h"

#include <stdio.h>
#include <string.h>

#define CRUMB_MAGIC 0x4C534352u

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
}

static int cmd_crumb(int argc, char **argv)
{
    (void)argc; (void)argv;
    print_last();
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
}

void panic_crumb_register_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "crumb",
        .help = "Trap cause and PC of each core at the last panic, even one that hung",
        .func = &cmd_crumb,
    };
    esp_console_cmd_register(&cmd);
}
