#include "ls_input_ev.h"
#include "esp_timer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

static bool active, consume;
static ls_input_ev_t seen[128];
static unsigned seen_n, key_n, tap_n, finger_n;
static atomic_uint wakes;
static bool concurrent;
static bool received[4][2000];
static unsigned delivered;
static atomic_uint accepted, producers_done;
static ls_tk_t keys[128];
static char last_ch;
static int tap_col, tap_row, finger_col, finger_row, finger_px, finger_py;
static bool finger_down;
static int64_t test_us;
int64_t esp_timer_get_time(void) { return test_us; }
void ls_shim_time_set(int64_t us) { test_us=us; }
void ls_shim_time_advance(int64_t us) { test_us+=us; }
bool ls_desk_active(void) { return active; }
bool ls_desk_input(const ls_input_ev_t *e)
{
    if (concurrent) {
        if (e->kind == LS_IEV_RELEASE_ALL) return true;
        const unsigned id=e->source, i=e->key;
        assert(id<4 && i<2000 && e->kind==LS_IEV_KEY_UP);
        assert(e->col==-(int)i && e->row==(int)i && e->seq==id*2000+i+1);
        assert(!received[id][i]); received[id][i]=true; delivered++;
        return true;
    }
    assert(seen_n < 128); seen[seen_n++] = *e; return consume;
}
void ls_input_wake(void) { wakes++; }
bool ls_tui_router_key(ls_tk_t key, char ch) { assert(key_n < 128); keys[key_n++] = key; last_ch = ch; return true; }
bool ls_tui_router_touch(int c, int r) { tap_n++; tap_col = c; tap_row = r; return true; }
void ls_tui_touch_remote(int c, int r, int x, int y, bool down)
{ finger_n++; finger_col=c; finger_row=r; finger_px=x; finger_py=y; finger_down=down; }

static void hex_event(const ls_input_ev_t *e, char *out)
{
    uint8_t b[LS_INPUT_WIRE_SIZE]; ls_input_encode(e, b);
    for (unsigned i=0; i<sizeof b; i++) sprintf(out+i*2, "%02x", b[i]);
}
static void post(ls_input_ev_t e) { assert(ls_input_post(&e)); assert(ls_input_drain()==1); }
static void *producer(void *arg)
{
    const unsigned id=(unsigned)(uintptr_t)arg;
    for (unsigned i=0; i<2000; i++) {
        const ls_input_ev_t e={.kind=LS_IEV_KEY_UP,.source=id,.key=i,.col=-(int)i,.row=i,.seq=id*2000+i+1};
        if (ls_input_post(&e)) atomic_fetch_add(&accepted,1);
    }
    atomic_fetch_add(&producers_done,1);
    return NULL;
}

int main(void)
{
    active = consume = true;
    char line[LS_INPUT_WIRE_SIZE*2*LS_INPUT_WIRE_MAX+1] = {0};
    ls_input_ev_t ev[7];
    for (unsigned i=0; i<7; i++) {
        ev[i]=(ls_input_ev_t){.kind=i, .source=LS_ISRC_TERM, .mods=15, .button=2, .buttons=7,
            .clicks=2, .col=-123, .row=200, .px=-30000, .py=32767, .wheel=-32768,
            .key=LS_TK_F12, .ch='~', .seq=0x12340000+i};
        hex_event(&ev[i], line+i*LS_INPUT_WIRE_SIZE*2);
    }
    /* Golden byte offsets, endian and signed values; no struct-padding ABI. */
    assert(!strncmp(line,"0100000f02070285ffc800d08aff7f00801d007e00003412",48));
    assert(ls_input_decode_hex(line));
    assert(seen_n==0);               /* parser never invokes app code */
    assert(ls_input_applied_seq()==0);
    assert(ls_input_drain()==7 && seen_n==7);
    for (unsigned i=0; i<7; i++) {
        uint8_t a[24], b[24]; ls_input_encode(&ev[i],a); ls_input_encode(&seen[i],b);
        assert(!memcmp(a,b,sizeof a));
    }
    assert(ls_input_applied_seq()==0x12340006);
    const unsigned before=wakes;
    /* A malformed later record must not enqueue an earlier valid record. */
    line[48]='0'; line[49]='2'; assert(!ls_input_decode_hex(line));
    line[49]='1'; line[50]='f'; line[51]='f'; assert(!ls_input_decode_hex(line));
    line[50]='0'; line[51]='1'; line[50]='q'; assert(!ls_input_decode_hex(line));
    line[50]='0'; line[strlen(line)-1]=0; assert(!ls_input_decode_hex(line));
    assert(!ls_input_decode_hex("") && !ls_input_decode_hex(NULL));
    assert(!ls_input_decode_hex("0"));
    assert(ls_input_drain()==0 && wakes==before);

    /* All 64 slots usable; overflow's reset follows accepted events, cannot
     * be displaced by another producer, and wakes the UI as well. */
    seen_n=0;
    ls_input_ev_t e={.kind=LS_IEV_PTR_DOWN, .buttons=1};
    for (unsigned i=0; i<64; i++) assert(ls_input_post(&e));
    assert(!ls_input_post(&e)); assert(!ls_input_post(&e));
    assert(ls_input_drain()==65 && seen_n==65);
    assert(seen[64].kind==LS_IEV_RELEASE_ALL && !finger_down);
    post((ls_input_ev_t){.kind=LS_IEV_KEY_UP});

    active=false; key_n=tap_n=finger_n=0;
    post((ls_input_ev_t){.kind=LS_IEV_KEY_DOWN,.key=LS_TK_CHAR,.ch='a'});
    assert(key_n==1 && keys[0]==LS_TK_CHAR && last_ch=='a');
    post((ls_input_ev_t){.kind=LS_IEV_KEY_UP,.key=LS_TK_CHAR,.ch='a'});
    assert(key_n==1);
    post((ls_input_ev_t){.kind=LS_IEV_PTR_DOWN,.button=LS_IBTN_RIGHT});
    assert(keys[1]==LS_TK_ESC);
    post((ls_input_ev_t){.kind=LS_IEV_WHEEL,.wheel=-2});
    post((ls_input_ev_t){.kind=LS_IEV_WHEEL,.wheel=3});
    assert(key_n==7 && keys[2]==LS_TK_WHEEL_UP && keys[3]==LS_TK_WHEEL_UP);
    assert(keys[4]==LS_TK_WHEEL_DOWN && keys[6]==LS_TK_WHEEL_DOWN);
    post((ls_input_ev_t){.kind=LS_IEV_PTR_MOVE,.col=9}); assert(finger_n==0);
    post((ls_input_ev_t){.kind=LS_IEV_PTR_DOWN,.col=3,.row=4,.px=35,.py=76});
    assert(finger_down && finger_col==3 && finger_row==4 && finger_px==35 && finger_py==76);
    post((ls_input_ev_t){.kind=LS_IEV_PTR_UP,.col=3,.row=4});
    assert(!finger_down && tap_n==1 && tap_col==3 && tap_row==4);
    post((ls_input_ev_t){.kind=LS_IEV_PTR_DOWN,.col=3,.row=4});
    post((ls_input_ev_t){.kind=LS_IEV_PTR_MOVE,.buttons=1,.col=5,.row=6,.px=55,.py=110});
    assert(finger_down && finger_col==5 && finger_row==6);
    post((ls_input_ev_t){.kind=LS_IEV_PTR_UP,.col=3,.row=4});
    assert(!finger_down && tap_n==1); /* moving away and back is still a drag */
    post((ls_input_ev_t){.kind=LS_IEV_PTR_DOWN});
    post((ls_input_ev_t){.kind=LS_IEV_RELEASE_ALL});
    assert(!finger_down && tap_n==1);
    active=true; consume=false; seen_n=0;
    post((ls_input_ev_t){.kind=LS_IEV_KEY_DOWN,.key=LS_TK_ENTER});
    assert(seen_n==1 && keys[key_n-1]==LS_TK_ENTER);

    ls_shim_time_set(0);
    assert(!ls_input_latency(NULL,NULL));
    ls_input_latency_report(12,25);
    int p50,p95; assert(ls_input_latency(&p50,&p95) && p50==12 && p95==25);
    ls_input_latency_report(30,20); /* invalid reports cannot refresh expiry */
    ls_shim_time_advance(9999999); assert(ls_input_latency(NULL,NULL));
    ls_shim_time_advance(1); assert(!ls_input_latency(NULL,NULL));
    /* New viewer: lower sequence numbers are valid again, but only once
     * the UI has applied the release barrier and drawn in the new epoch. */
    seen_n=0; consume=true;
    const uint32_t epoch=ls_input_begin_session();
    assert(ls_input_applied_epoch()!=epoch);
    assert(ls_input_post(&(ls_input_ev_t){.kind=LS_IEV_KEY_UP,.seq=1}));
    assert(ls_input_drain()==2 && seen[0].kind==LS_IEV_RELEASE_ALL);
    assert(ls_input_applied_epoch()==epoch && ls_input_applied_seq()==1);
    /* Four task producers while the UI drains: no torn events, duplicate
     * deliveries, or loss of any event for which post returned true. */
    concurrent=true; consume=active=true;
    pthread_t threads[4];
    for (unsigned i=0; i<4; i++) assert(!pthread_create(&threads[i],NULL,producer,(void *)(uintptr_t)i));
    while (atomic_load(&producers_done)<4) ls_input_drain();
    for (unsigned i=0; i<4; i++) assert(!pthread_join(threads[i],NULL));
    while (ls_input_drain()) {}
    assert(delivered==atomic_load(&accepted));
    puts("desktop input: codec, malformed batches, overflow/reset, fallback, latency, concurrent producers PASS");
    return 0;
}
