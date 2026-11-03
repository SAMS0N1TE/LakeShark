"""Host fault injection and byte accounting for production feed allocation.

No device access. Usage: python -B bench/tests/test_rterm_memory.py <gcc>
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = root / 'bench/build-desk-mem'
out.mkdir(exist_ok=True)
src = (root / 'components/apps/tui/ls_rterm.c').read_text()
session = src[src.index('typedef struct {'):src.index('} session_t;') + len('} session_t;')]
allocator = src[src.index('static size_t s_buffer_cells;'):src.index('bool ls_rterm_prepare_grid(')]
serial = src[src.index('static void serial_stage('):src.index('static SemaphoreHandle_t s_mu;')]
ssh = src[src.index('static void ssh_out('):src.index('static void ssh_render(')]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_vt.h"
#include "ls_rterm_visual.h"
#define TERM_CELLS_MAX 43008
#define MALLOC_CAP_SPIRAM 1
static size_t live, calls;
static int fail_after=-1;
typedef struct { size_t n; unsigned long long guard; } header;
static void *heap_caps_calloc(size_t n,size_t size,int caps) {
    assert(caps==MALLOC_CAP_SPIRAM); calls++;
    if(fail_after==0) return NULL;
    if(fail_after>0) fail_after--;
    n*=size;
    header *p=calloc(1,sizeof(*p)+n+8); assert(p);
    p->n=n;p->guard=0x12345678;
    memset((char *)(p+1)+n,0xa5,8);live+=n;return p+1;
}
static size_t bytes(const void *p) { return p ? ((const header *)p)[-1].n : 0; }
static void heap_caps_free(void *p) {
    if(!p) return;
    header *h=(header *)p-1;assert(h->guard==0x12345678);
    for(int i=0;i<8;i++) assert(((unsigned char *)p)[h->n+i]==0xa5);
    live-=h->n;h->guard=0;free(h);
}
''' + session + r'''
static session_t s_ser,s_web,s_net,s_ssh;
static tui_cell *s_snap;
static uint8_t *s_glass;
static uint32_t *s_top,*s_bot;
static ls_vt_cell *s_now;
static char *s_serial_tx,*s_serial_inflight,*s_sshq,*s_frame;
static size_t s_serial_n,s_serial_cap,s_sshq_n,s_sshq_cap;
static bool s_serial_over,s_sshq_ready,s_sshq_over,s_ready;
static void *s_z,*s_zpay;
static int s_now_cols,s_now_rows;
''' + allocator + serial + ssh + r'''
static void stop(void) {
    s_ser.on=s_web.on=s_net.on=s_ssh.on=false;
    s_sshq_ready=false;assert(grid_buffers(0));assert(live==0);
}
static size_t serial_expected(size_t n) { return n*(4+8+12+12+64)+(n+7)/8+16384+4010; }
int main(void) {
    assert(sizeof(tui_cell)==4 && sizeof(ls_vt_cell)==12);
    assert(grid_buffers(43008));assert(live==0); // idle, even with a saved cap grid
    const size_t sizes[]={150*39,360*105,384*112};
    puts("cells,one_serial_viewer,all_four_viewers");
    for(size_t k=0;k<3;k++) {
        size_t n=sizes[k];
        s_ser.on=true;assert(grid_buffers(n));
        assert(bytes(s_snap)==n*4 && bytes(s_glass)==(n+7)/8);
        assert(bytes(s_top)==n*4 && bytes(s_bot)==n*4 && bytes(s_now)==n*12);
        assert(bytes(s_ser.shown)==n*12 && !s_web.shown && !s_net.shown && !s_ssh.shown);
        assert(s_serial_cap==n*64+16384 && live==serial_expected(n));
        size_t one=live;
        const size_t allocations=calls;
        s_ser.cols=150;s_ready=true;
        assert(grid_buffers(n) && calls==allocations && s_ready && s_ser.cols==150);
        s_net.on=s_web.on=s_ssh.on=true;assert(grid_buffers(n));
        assert(bytes(s_net.shown)==n*12 && bytes(s_web.shown)==n*12 && bytes(s_ssh.shown)==n*12);
        assert(s_sshq_cap==n*64+16384);
        assert(live==one+n*36+n*64+16384);
        printf("%zu,%zu,%zu\n",n,one,live);
        stop();
    }
    // Every allocation in a growth transaction can fail independently.
    s_ser.on=s_web.on=s_net.on=s_ssh.on=true;
    assert(grid_buffers(150*39));
    size_t baseline=live;
    tui_cell *snap=s_snap;char *tx=s_serial_tx;ls_vt_cell *shown=s_web.shown;
    s_snap[0].ch='Q';s_web.cols=150;s_web.rows=39;s_ready=true;
    // All twelve grid allocations grow; web TX is independently bounded and
    // fault-injected in test_desk_eth.py / test_ws_desktop.c.
    for(int failure=0;failure<12;failure++) {
        fail_after=failure;
        assert(!grid_buffers(43008));
        assert(live==baseline && s_buffer_cells==150*39 && s_ready);
        assert(s_snap==snap && s_snap[0].ch=='Q' && s_serial_tx==tx);
        assert(s_web.shown==shown && s_web.cols==150 && s_web.rows==39);
    }
    fail_after=-1;
    // Queue being sent on SSH is immutable while other buffers grow/shrink.
    s_sshq_ready=true;char *queue=s_sshq;s_sshq_n=10;memset(queue,'S',10);
    assert(grid_buffers(43008));assert(s_sshq==queue && s_sshq_n==10 && !memcmp(queue,"SSSSSSSSSS",10));
    s_sshq_n=0;s_sshq_ready=false;assert(grid_buffers(43008));
    assert(s_sshq_cap==43008*64+16384);
    // UART output can be blocking outside the lock during resize. The writer
    // retains the old allocation until it reacquires the lock after output.
    tx=s_serial_tx;s_serial_inflight=tx;size_t held=bytes(tx);memset(tx,'U',100);
    assert(grid_buffers(150*39));assert(tx!=s_serial_tx && !memcmp(tx,"UUUUUUUUUU",10));
    size_t with_held=live;heap_caps_free(tx);s_serial_inflight=NULL;assert(live==with_held-held);
    // Bounded serial and SSH staging must drop a whole overflowing update.
    char b[16]={0};s_serial_n=s_serial_cap-8;s_serial_over=false;
    serial_stage(b,16);assert(s_serial_over && s_serial_n==s_serial_cap-8);
    s_sshq_n=s_sshq_cap-8;s_sshq_over=false;
    ssh_out(NULL,b,16);assert(s_sshq_over && s_sshq_n==s_sshq_cap-8);
    stop();
    // Repeated reconnects and arbitrary shrink/growth leave no reservations.
    for(int i=0;i<40;i++) {
        s_web.on=true;assert(grid_buffers(i%2 ? 43008 : 150*39));
        s_web.on=false;s_net.on=true;assert(grid_buffers(360*105));
        assert(!s_web.shown && s_net.shown && !s_serial_tx && !s_sshq);
        stop();
    }
    // Compression scratch also leaves with the last viewer.
    s_ser.on=true;assert(grid_buffers(150*39));
    s_z=heap_caps_calloc(1,1234,MALLOC_CAP_SPIRAM);
    s_zpay=heap_caps_calloc(1,4000,MALLOC_CAP_SPIRAM);
    stop();assert(!s_z && !s_zpay);
    assert(!grid_buffers(43009));
    printf("PASS: %zu allocations, growth faults, shrink, reconnect, UART/SSH ownership, bounds\n",calls);
}
'''
(out / 'rterm_memory.c').write_text(code)
subprocess.run([sys.argv[1], '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-variable', '-Wno-unused-function', '-I' + str(root / 'components/apps/tui'),
                str(out / 'rterm_memory.c'), '-o', str(out / 'rterm_memory.exe')], check=True)
result = subprocess.run([str(out / 'rterm_memory.exe')], check=True, capture_output=True, text=True)
(out / 'memory.txt').write_text(result.stdout)
print(result.stdout, end='')
