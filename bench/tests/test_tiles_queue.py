"""Fault injection against production cell mailbox, dispatch and completion."""
from pathlib import Path
import shutil
import subprocess
import struct

root = Path(__file__).resolve().parents[2]
out = root / 'bench/build/tiles-queue'
out.mkdir(parents=True, exist_ok=True)
(out / 'maps/cells').mkdir(parents=True, exist_ok=True)
h = bytearray(128)
h[:8] = b'CCMAPS1\0'
struct.pack_into('<II', h, 8, 1, 64)
struct.pack_into('<I', h, 48, 128)
(out / 'maps/cells/catalog.ccm').write_bytes(h + struct.pack('<II32s', 0, 64, bytes(32)))
source = (root / 'components/apps/tui/ls_cells_worker.h').read_text()
header = (root / 'components/apps/tui/ls_cells.h').read_text()
types = header[header.index('#define LS_CELLS_HITS'):header.index('#if LS_CARTOCORE_AVAILABLE')]
public = source[source.index('static EXT_RAM_BSS_ATTR struct {'):source.index('static EXT_RAM_BSS_ATTR struct {\n    cc_places')]
progress = source[source.index('static EXT_RAM_BSS_ATTR uint8_t cells_credited'):source.index('static uint32_t cells_u32(')]
queue = source[source.index('static void cells_queue_step('):source.index('static void cells_metadata_activate(')]
catalog = source[source.index('static bool cells_catalog('):source.index('static bool cells_entry_checked(')]
dispatch = source[source.index('static void cells_step('):source.index('    if(cells_request.serial!=cw.serial)')]+ '\n}\n'
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define EXT_RAM_BSS_ATTR
#define CELLS_BITS 8192
#define CELLS_ROOT "maps/"
''' + types + public + r'''
static struct {
    bool queue_loaded,initialized,preview_active,catalog_refresh,direct,progress_loaded,want_detail;
    uint32_t next_cell,queue_key,selected_key,index_size,base_size,entries,stride,progress_revision;
    uint64_t queue_total,queue_done,queue_partial;
    int catalog;
    char queue_name[64],path[256];
    uint8_t queued[CELLS_BITS],cover[CELLS_BITS],scratch[CELLS_BITS],index_sha[32],base_sha[32];
} cw;
''' + progress + r'''
static struct { bool active;int64_t last_us; } upload;
static struct { bool active; } dl;
static struct { char path[128]; } saved;
static struct { bool set; } *sd_map;
static unsigned records,deletes,downloads;
static bool missing,present=true,save_ok=true,change_prior;
static uint32_t recorded_key;
static uint8_t recorded[CELLS_BITS];
static void cells_message(const char *s) { snprintf(cells_work.message,sizeof(cells_work.message),"%s",s); }
static bool cells_matches(const char *p,uint64_t size,const uint8_t *sha) { (void)p;(void)size;(void)sha;return true; }
static bool cells_entry(uint32_t id,uint32_t *size,uint8_t *sha) {
    *size=64;memset(sha,0,32);return !(missing && id==1);
}
static bool cells_entry_checked(uint32_t id,uint32_t *size,uint8_t *sha,bool *error) {
    (void)error;return cells_entry(id,size,sha);
}
static bool cells_cached_digest(const char *path,const struct stat *st,const uint8_t sha[32],bool *known,bool *error) {
    (void)path;(void)st;(void)sha;(void)error;*known=false;return false;
}
static bool cells_present(uint32_t id,uint32_t size) {
    (void)size;return id==0 || (present && !(change_prior && id==1));
}
static void cells_name(uint32_t id,char *out,size_t n,bool remote) { (void)remote;snprintf(out,n,"%u",id); }
static bool cells_download(unsigned kind,const char *remote,const char *local,uint64_t size,const uint8_t *sha) {
    (void)kind;(void)remote;(void)local;(void)size;(void)sha;downloads++;return true;
}
static bool cells_record(uint32_t key,const char *name,const uint8_t *bits,bool queued) {
    (void)name;(void)queued;records++;recorded_key=key;memcpy(recorded,bits,CELLS_BITS);return save_ok;
}
static void cells_library(void) {}
static void select_map(const char *s) { (void)s; }
static void save_view(void) {}
static void cells_preview(void) {}
static void cells_init(void) {}
static void cells_retry_reset(const char *relative) { (void)relative; }
static int64_t esp_timer_get_time(void) { return 0; }
static bool cells_delete(uint32_t key) { (void)key;deletes++;return true; }
static bool cells_record_read(const char *path,uint8_t *bits,char *name,bool *queued) {
    (void)path;bits[0]=2;strcpy(name,"Update");*queued=false;return true;
}
static void cells_publish(void) { cells_pub=cells_work;cells_plus=cells_plus_work; }
/* State downloads (ls_cells_states_worker.h) are outside this test's scope. */
static void cells_states_publish(void) {}
typedef struct { char code[3],name[64];bool plus,partial;uint8_t cover[CELLS_BITS];unsigned count;uint64_t bytes; } cells_state_row;
static cells_state_row cells_states[52] __attribute__((unused));
static unsigned cells_states_count;
static bool cells_token_load(void) { cells_plus_work=false;return cells_plus_work; }
static bool cells_asset_valid(const char *path,const char *relative) { (void)path;(void)relative;return true; }
static void cells_state_command(unsigned action,uint32_t key) { (void)action;(void)key; }
static uint32_t cells_u32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static bool carto_sd_read_at(int fd,uint64_t size,uint64_t off,void *dst,size_t n,void *unused,int zero) {
    (void)size;(void)unused;(void)zero;return lseek(fd,(long)off,SEEK_SET)>=0 && read(fd,dst,n)==(int)n;
}
''' + catalog + queue + dispatch + r'''
int main(void) {
    cw.catalog=-1;cw.next_cell=400;assert(cells_catalog() && cw.next_cell==0);
    cw.next_cell=65536;assert(cells_catalog() && cw.next_cell==0);close(cw.catalog);
    cw.initialized=true;cw.catalog=1;cw.queue_loaded=true;cw.queued[0]=1;
    missing=true;cells_queue_step();cells_queue_step();
    assert(cw.next_cell==0 && cw.queue_loaded && !records && !cells_work.downloading);
    assert(strstr(cells_work.message,"Blocked"));
    missing=false;present=false;cells_queue_step();assert(cw.next_cell==0 && downloads==1 && !records);
    present=true;cw.next_cell=65536;change_prior=true;cells_queue_step();
    assert(cw.next_cell==0 && cw.queue_loaded && !records);
    change_prior=false;save_ok=false;cells_queue_step();assert(cw.queue_loaded && records==1);
    save_ok=true;cells_queue_step();assert(!cw.queue_loaded && records==2);
    records=0;cw.queue_loaded=true;cw.queue_key=42;cw.queued[0]=4;cw.next_cell=400;cells_pub.ready=true;cells_preview_mail.key=42;
    strcpy(cells_preview_mail.name,"Original");cells_preview_mail.cover[0]=1;
    assert(ls_cells_action(1,0));assert(ls_cells_query("changed"));
    cells_preview_mail.key=99;cells_preview_mail.cover[0]=2;
    assert(ls_cells_server("https://server.example"));
    cells_step();assert(records==1 && recorded_key==42 && recorded[0]==1 && cells_work.action_done==1);
    assert(cw.queued[0]==1 && cw.next_cell==0);
    for(unsigned i=0;i<CELLS_ACTIONS;i++)assert(ls_cells_action(3,i));
    assert(!ls_cells_action(3,77));assert(ls_cells_query(""));
    for(unsigned i=0;i<CELLS_ACTIONS;i++) { cells_step();assert(recorded_key==i); }
    assert(cells_action_count==0 && cells_work.action_done==5);
    dl.active=true;assert(ls_cells_action(2,42));cells_step();
    assert(cells_action_count==1 && !deletes);
    dl.active=false;cells_step();assert(!cells_action_count && deletes==1);
    save_ok=false;cells_pub.ready=true;assert(ls_cells_action(1,0));cells_step();
    assert(strstr(cells_pub.message,"Cannot save"));
    puts("PASS: missing cells, final revalidation, save failures, immutable accepted actions, FIFO/full rejection, delayed delete");
}
'''
(out / 'test.c').write_text(code)
gcc = shutil.which('gcc') or 'C:/mingw64/bin/gcc.exe'
exe = out / 'test.exe'
subprocess.run([gcc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(out / 'test.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], cwd=out, check=True)
