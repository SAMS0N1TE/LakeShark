#include "ble_hid_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static hid_input_t input;
static ls_input_ev_t events[512];
static unsigned count;
static bool reject;
static bool sink(const ls_input_ev_t *e, void *ctx)
{
    (void)ctx;
    if (reject)
        return false;
    assert(count < 512);
    events[count++] = *e;
    return true;
}
static void send(unsigned p, unsigned r, hid_sample_t *s, unsigned t)
{
    hid_input_report(&input, p, r, s, t, 80, 24, 8, 16, sink, NULL);
}
int main(void)
{
    hid_sample_t s = {0};
    s.keys[0] = 1 << 4;
    send(0, 0, &s, 100);
    assert(count == 1 && events[0].kind == LS_IEV_KEY_DOWN && events[0].ch == 'a' &&
           events[0].source == LS_ISRC_BLE);
    send(0, 0, &s, 110);
    assert(count == 1);
    hid_input_repeat(&input, 499, sink, NULL);
    assert(count == 1);
    hid_input_repeat(&input, 500, sink, NULL);
    assert(count == 2 && events[1].clicks == 0);
    hid_input_repeat(&input, 544, sink, NULL);
    assert(count == 2);
    hid_input_repeat(&input, 545, sink, NULL);
    assert(count == 3);
    send(0, 1, &s, 550);
    memset(&s, 0, sizeof(s));
    send(0, 0, &s, 560);
    assert(count == 3);
    send(0, 1, &s, 570);
    assert(count == 4 && events[3].kind == LS_IEV_KEY_UP);
    s.keys[0] = 1 << 4;
    send(0, 0, &s, 600);
    send(1, 0, &s, 600);
    unsigned n = count;
    hid_input_release(&input, 0, 610, sink, NULL);
    assert(count == n);
    hid_input_release(&input, 1, 620, sink, NULL);
    assert(count == n + 1);
    s = (hid_sample_t){0};
    s.keys[28] = 2;
    s.keys[0] = 1 << 4;
    send(0, 0, &s, 700);
    assert(events[count - 2].ch == 'A');
    n = count;
    hid_input_repeat(&input, 1100, sink, NULL);
    assert(count == n + 1 && events[count - 1].ch == 'A');
    hid_input_release(&input, 0, 1110, sink, NULL);
    n = count;
    hid_input_repeat(&input, 2000, sink, NULL);
    assert(count == n);
    s = (hid_sample_t){.pointer = true, .x = 100000, .y = -1000, .buttons = 2, .wheel = 1};
    send(0, 2, &s, 2200);
    assert(input.x == 639 && input.y == 0);
    assert(events[count - 2].kind == LS_IEV_PTR_DOWN && events[count - 2].button == LS_IBTN_RIGHT);
    assert(events[count - 1].kind == LS_IEV_WHEEL && events[count - 1].wheel == -1);
    s = (hid_sample_t){.pointer = true, .x = -5};
    send(1, 2, &s, 2210);
    assert(input.x == 639); /* drag owner */
    hid_input_release(&input, 0, 2220, sink, NULL);
    assert(events[count - 1].kind == LS_IEV_PTR_UP && input.held_buttons == 0);
    s = (hid_sample_t){.rollover = true};
    s.keys[0] = 16;
    n = count;
    send(0, 0, &s, 2300);
    assert(count == n);
    reject = true;
    s = (hid_sample_t){0};
    s.keys[0] = 16;
    send(0, 0, &s, 2400);
    assert(input.resync && input.dropped);
    reject = false;
    hid_input_repeat(&input, 2410, sink, NULL);
    assert(events[n].kind == LS_IEV_RELEASE_ALL);
    assert(events[n + 1].kind == LS_IEV_KEY_DOWN);
    puts("BLE HID input edges, repeat, merge, pointer and overflow passed");
    return 0;
}
