"""Production state publication: bounded work, atomic counts and install coalescing."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / 'components/apps/tui/ls_cells_states_worker.h').read_text()
header = (root / 'components/apps/tui/ls_cells.h').read_text()
types = header[header.index('#define LS_CELLS_HITS'):header.index('#if LS_CARTOCORE_AVAILABLE')]
rows = source[source.index('typedef struct { char code'):source.index('/* Optional LSOUT1')]
publish = source[source.index('static void cells_states_publish('):source.index('static void cells_state_command(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#define EXT_RAM_BSS_ATTR
#define CELLS_BITS 8192
#define CELLS_ROOT ""
#define FREE_MAX_ZOOM 12
''' + types + r'''
static ls_cells_view cells_work;
static struct {
    bool progress_loaded,queue_loaded;uint32_t generation;
    uint64_t queue_total,queue_done,queue_partial;char queue_name[64];
} cw;
static int64_t now=4000000,cost;
static unsigned checked,fail_id;
static int64_t esp_timer_get_time(void) { return now; }
static void cells_name(uint32_t id,char *name,size_t n,bool remote) { (void)remote;snprintf(name,n,"%u",id); }
static bool cells_entry_checked(uint32_t id,uint32_t *bytes,uint8_t sha[32],bool *error) {
    now+=cost;checked++;*bytes=64;memset(sha,0,32);
    if(id==fail_id) { *error=true;return false; }return true;
}
static bool cells_matches_checked(const char *path,uint64_t bytes,const uint8_t sha[32],bool *error) {
    (void)bytes;(void)sha;(void)error;unsigned id=(unsigned)atoi(path)-1;
    return id<65536?id%4<2:(id-65536)%3==0;
}
static int fake_stat(const char *path,struct stat *st) {
    (void)st;unsigned id=(unsigned)atoi(path)-1;
    if(id%4!=3)return 0;
    errno=ENOENT;return -1;
}
#define stat(path,st) fake_stat(path,st)
/* The worker's pause and its streaming flag: no file streams in this test. */
static void cells_pause(int64_t *since) { (void)since; }
static bool cells_receiving(void) { return false; }
''' + rows + publish + r'''
static unsigned finish(unsigned limit) {
    unsigned steps=0;
    do {
        unsigned before=checked;cells_states_publish();steps++;
        assert(checked-before<=limit);
    } while((cells_states_walk.active || cells_states_dirty || cells_states_walk.published!=cw.generation) && steps<1000);
    assert(steps<1000);return steps;
}
int main(void) {
    const unsigned counts[]={16,7,7,7,4,15,13,18,22};
    const char *codes[]={"ME","VT","NH","MA","CT","MS","AL","GA","FL"};
    char expected[8192]={0};size_t used=0;
    cells_states_count=9;cells_states_dirty=true;cw.generation=1;
    cw.progress_loaded=cw.queue_loaded=true;cw.queue_total=100;strcpy(cw.queue_name,"All states");
    for(unsigned i=0;i<9;i++) {
        cells_state_row *r=cells_states+i;strcpy(r->code,codes[i]);strcpy(r->name,codes[i]);
        r->count=counts[i];r->bytes=counts[i]*128;r->base_bytes=r->detail_bytes=counts[i]*64;
        unsigned installed=0,outdated=0;
        for(unsigned j=0;j<counts[i];j++) {
            unsigned id=i*256+j;r->cover[id/8]|=1u<<(id&7);
            installed+=id%4<2;outdated+=id%4==2;
        }
        used+=snprintf(expected+used,sizeof(expected)-used,"%s|%s|free|%u|%llu|%u|0|%llu|%llu|0|%u\n",
            r->code,r->name,r->count,(unsigned long long)r->bytes,installed,
            (unsigned long long)r->base_bytes,(unsigned long long)r->detail_bytes,outdated);
    }
    cost=15000;cells_states_publish();assert(cells_states_walk.active && !cells_work.states[0] && checked<=14);
    assert(finish(14)>9 && !strcmp(cells_work.states,expected));
    /* Chunk progress changes without touching the completed state scan. */
    unsigned before=checked;
    for(unsigned i=0;i<100;i++) {
        now+=10000;cw.queue_partial=i;cells_work.done=i;cells_work.total=100;cells_states_publish();
    }
    assert(checked==before && cells_work.maps_progress==0.99f && strstr(cells_work.progress,"99|100|All states"));
    now=cells_states_walk.began+3000001;cw.generation++;cells_states_publish();assert(cells_states_walk.active);
    assert(!strcmp(cells_work.states,expected));
    /* A second install invalidates private counts but cannot trigger another immediate scan. */
    cw.generation++;before=checked;cells_states_publish();assert(!cells_states_walk.active && checked==before);
    assert(!strcmp(cells_work.states,expected));
    now=cells_states_walk.began+3000001;cost=0;assert(finish(32)>1 && !strcmp(cells_work.states,expected));
    /* On completion, refresh immediately; no throttle or partial publication after errors. */
    cw.queue_loaded=false;cw.generation++;fail_id=1;before=checked;cells_states_publish();
    assert(cells_states_dirty && !cells_states_walk.active && checked>before && !strcmp(cells_work.states,expected));
    fail_id=0;assert(finish(32)>1 && !strcmp(cells_work.states,expected));
    /* Metadata replacement during a scan restarts against the new cover. */
    cells_states_dirty=true;cells_states_publish();assert(cells_states_walk.active);
    cells_states_count=0;cells_states_dirty=true;cells_states_publish();assert(!cells_work.states[0]);
    puts("PASS: nine-state counts, 200 ms/16-cell slices, atomic retry, chunk progress, three-second install coalescing");
}
'''
(out / 'test.c').write_text(code)
exe = out / 'test.exe'
subprocess.run([sys.argv[1], '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                str(out / 'test.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], cwd=out, check=True)
