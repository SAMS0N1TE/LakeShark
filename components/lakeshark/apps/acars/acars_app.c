
#include "acars_app.h"
#include "acars.h"

#include "esp_timer.h"
#include "esp_attr.h"
#include "ls_time.h"

#include <string.h>
#include <time.h>

/* LS-725: the eight-message display ring is ordinary UI/decoder data, never
   DMA, ISR or cache-off state.  Its fixed sizeof(acars_state_t) payload
   belongs in PSRAM rather than consuming scarce internal DRAM for the whole
   boot. */
static EXT_RAM_BSS_ATTR acars_state_t s_state;

const acars_state_t *acars_app_state(void)     { return &s_state; }
acars_state_t       *acars_app_state_mut(void) { return &s_state; }

void acars_app_clear(void)
{
    memset(&s_state, 0, sizeof(s_state));
}

static void copy_ascii(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    dst[0] = 0;
    if (!src) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void acars_app_inject(const char *reg, const char *label, const char *text)
{
    acars_msg_out_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.ts_us    = esp_timer_get_time();
    msg.ts_epoch = ls_time_is_synced() ? (int64_t)time(NULL) : 0;
    msg.mode     = '2';
    copy_ascii(msg.reg,   sizeof(msg.reg),   reg   ? reg   : "");
    copy_ascii(msg.label, sizeof(msg.label), label ? label : "");
    msg.block_id = '1';
    msg.tak      = 0x15;

    copy_ascii(msg.text, sizeof(msg.text), text ? text : "");
    msg.text_len = (int)strlen(msg.text);
    msg.crc_ok   = true;

    s_state.msgs[s_state.msg_head] = msg;
    s_state.msg_head = (s_state.msg_head + 1) % ACARS_MSG_LOG_MAX;
    if (s_state.msg_count < ACARS_MSG_LOG_MAX) s_state.msg_count++;
    s_state.n_delivered++;
}

size_t acars_flight_from_text(const char *text, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!text) return 0;

    /* The flight token is [A-Z]{2,3}[0-9]{2,4}[A-Z]?.  Only whitespace is
       stripped in front - trying to guess past a punctuation prefix (# for
       a sublabel, / for a subformat) risks mis-parsing the field that came
       after it, and a "-" in the panel is safer than a wrong flight ID
       next to a real registration.  Stop at the first byte that breaks the
       shape; do not fish deeper into the text. */
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;

    int letters = 0;
    while (letters < 3 && p[letters] >= 'A' && p[letters] <= 'Z') letters++;
    if (letters < 2) return 0;

    int digits = 0;
    while (digits < 4 && p[letters + digits] >= '0' && p[letters + digits] <= '9')
        digits++;
    if (digits < 2) return 0;

    int total = letters + digits;
    if (p[total] >= 'A' && p[total] <= 'Z' &&
        !(p[total + 1] >= 'A' && p[total + 1] <= 'Z') &&
        !(p[total + 1] >= '0' && p[total + 1] <= '9'))
    {
        total++;
    }

    if ((size_t)total >= cap) total = (int)cap - 1;
    memcpy(out, p, (size_t)total);
    out[total] = 0;
    return (size_t)total;
}
