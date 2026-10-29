"""Compile production cell transfer/dispatch with deterministic HTTP, clock and RNG."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'maps/cells').mkdir(parents=True, exist_ok=True)
source = (root / 'components/apps/tui/ls_cells_worker.h').read_text()
header = (root / 'components/apps/tui/ls_cells.h').read_text()
types = header[header.index('#define LS_CELLS_HITS'):header.index('#if LS_CARTOCORE_AVAILABLE')]
public = source[source.index('static EXT_RAM_BSS_ATTR struct {'):source.index('static EXT_RAM_BSS_ATTR struct {\n    cc_places')]
transfer = source[source.index('/* Failure records survive'):source.index('static void cells_config_save(')]
dispatch = source[source.index('static void cells_step('):source.index('static void cells_service(')]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <direct.h>
#define EXT_RAM_BSS_ATTR
#define CELLS_ROOT "maps/"
#define CELLS_BITS 8192
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
static int64_t now=1000000;
static uint32_t random_value=UINT32_MAX/2+1;
static bool alloc_fail,hash_ok=true,format_ok=true,complete=true;
static int status=500,open_result,read_result;
static int64_t content_length;
static unsigned requests,part_bytes;
static const char *retry_header;
static int64_t esp_timer_get_time(void) { return now; }
static uint32_t esp_random(void) { return random_value; }
static time_t fake_time(time_t *out) { if(out)*out=1767225600;return 1767225600; }
#define time fake_time
static void *heap_caps_calloc(size_t n,size_t s,unsigned caps) { (void)caps;return alloc_fail?NULL:calloc(n,s); }
static void heap_caps_free(void *p) { free(p); }
static size_t heap_caps_get_free_size(unsigned caps) { (void)caps;return 100000; }
typedef int esp_err_t;
typedef void *esp_http_client_handle_t;
typedef struct { int event_id;const char *header_key,*header_value; } esp_http_client_event_t;
#define HTTP_EVENT_ON_HEADER 1
#define ESP_OK 0
#define esp_crt_bundle_attach NULL
typedef struct {
    const char *url;int timeout_ms;void *crt_bundle_attach;int buffer_size;
    esp_err_t (*event_handler)(esp_http_client_event_t *);bool keep_alive_enable;
} esp_http_client_config_t;
static esp_err_t (*on_header)(esp_http_client_event_t *);
static void *esp_http_client_init(const esp_http_client_config_t *cfg) { on_header=cfg->event_handler;return (void*)1; }
static void esp_http_client_set_header(void *h,const char *k,const char *v) { (void)h;(void)k;(void)v; }
static int esp_http_client_open(void *h,int n) { (void)h;(void)n;requests++;return open_result; }
static int64_t esp_http_client_fetch_headers(void *h) {
    (void)h;
    if(retry_header) { esp_http_client_event_t e={HTTP_EVENT_ON_HEADER,"rEtRy-AfTeR",retry_header};on_header(&e); }
    return content_length;
}
static int esp_http_client_get_status_code(void *h) { (void)h;return status; }
static int esp_http_client_read(void *h,char *buf,int n) {
    (void)h;int got=read_result;if(got>n)got=n;if(got>0)memset(buf,'x',(size_t)got);return got;
}
static bool esp_http_client_is_complete_data_received(void *h) { (void)h;return complete; }
static void esp_http_client_close(void *h) { (void)h; }
static void esp_http_client_cleanup(void *h) { (void)h; }
static const char *esp_err_to_name(int e) { (void)e;return "timeout"; }
''' + types + public + r'''
static struct {
    char server[256],path[256],queue_name[64];
    uint8_t queued[CELLS_BITS],scratch[CELLS_BITS],buf[65536];
    uint64_t queue_done,queue_partial;unsigned queue_key,next_cell,serial,picked,radius,hint_attempts;
    bool initialized,queue_loaded,catalog_refresh,states_refresh,preview_active,query_pending,base_open,direct;
    int catalog;struct { char iso2[3]; } shard;
} cw;
static struct { bool active;int64_t last_us; } upload;
static struct { uint32_t bytes;uint8_t sha[32]; } cells_outlines;
static bool cells_outlines_pending;
static char cells_token[1];
static void cells_message(const char *s) { snprintf(cells_work.message,sizeof(cells_work.message),"%s",s); }
static FILE *cells_fopen(const char *p,const char *m) { return fopen(p,m); }
static bool cells_safe_asset(const char *s) { (void)s;return true; }
static bool cells_id_allowed(uint32_t id) { (void)id;return true; }
static bool cells_parts(uint32_t id,uint32_t *bytes,uint32_t *count) { (void)id;*bytes=part_bytes;*count=2;return true; }
static bool cells_matches(const char *p,uint64_t n,const uint8_t *s) { (void)p;(void)n;(void)s;return false; }
static bool cells_hash(const char *p,const uint8_t *s,uint64_t n) { (void)p;(void)s;(void)n;return hash_ok; }
static bool cells_asset_valid(const char *p,const char *r) { (void)p;(void)r;return format_ok; }
static bool cells_install(const char *p,const char *r) { unlink(r);return rename(p,r)==0; }
static bool cells_token_load(void) { return true; }
''' + transfer + r'''
static void cells_init(void) { cw.initialized=true; }
static void cells_library(void) {}
static void cells_publish(void) { cells_pub=cells_work; }
static void cells_states_publish(void) {}
static void cells_preview(void) {}
static void cells_search(void) {}
static void cells_queue_step(void) {}
static void cells_config_save(void) {}
static bool cells_url_valid(const char *s) { (void)s;return true; }
static bool cells_catalog(void) { return true; }
static bool cells_states_parse(const char *p,bool a) { (void)p;(void)a;return true; }
static bool cells_outlines_needed(void) { return cells_outlines.bytes!=0; }
static bool cells_attach(const char *s) { (void)s;return true; }
static bool cells_delete(uint32_t k) { (void)k;return true; }
static void cells_state_command(unsigned a,uint32_t k) { (void)a;(void)k; }
static bool cells_record(uint32_t k,const char *n,const uint8_t *b,bool q) { (void)k;(void)n;(void)b;(void)q;return true; }
static bool cells_record_read(const char *p,uint8_t *b,char *n,bool *q) { (void)p;(void)b;*n=0;*q=true;return true; }
''' + dispatch + r'''
static const char *asset="cells/states.json";
static bool start(void) { return cells_download(7,asset,asset,0,NULL); }
static void reset(void) {
    cells_download_close();memset(&dl,0,sizeof(dl));cells_retry_reset(NULL);
    memset(&cw,0,sizeof(cw));strcpy(cw.server,"https://example.invalid/");
    status=500;open_result=read_result=0;content_length=0;retry_header=NULL;
    hash_ok=format_ok=complete=true;part_bytes=0;alloc_fail=false;
    cells_outlines_pending=false;cells_action_count=cells_action_head=0;
    memset(&cells_mail,0,sizeof(cells_mail));now=1000000;random_value=UINT32_MAX/2+1;
    unlink("maps/cells/8-1-2.ctile.part");unlink("maps/cells/8-1-2.ctile.part.meta");
}
static void explicit_action(unsigned action) {
    cells_pub.ready=true;cells_pub.busy=false;assert(ls_cells_action(action,0));cells_step();
}
int main(void) {
    const int64_t bases[]={5,10,20,40,80,160,320,640,900,900,900};
    reset();
    for(unsigned i=0;i<sizeof(bases)/sizeof(*bases);i++) {
        int64_t base=bases[i]*1000000;
        assert(cells_retry_delay(i+1,0)==base*4/5);
        assert(cells_retry_delay(i+1,UINT32_MAX/2+1)==base);
        assert(cells_retry_delay(i+1,UINT32_MAX)==(base*6/5>900000000?900000000:base*6/5));
        for(uint32_t r=0;r<1000;r++) {
            int64_t delay=cells_retry_delay(i+1,r*4294967u);
            assert(delay>=base*4/5 && delay<=base*6/5 && delay<=900000000);
        }
        assert(!start());cells_retry *failure=cells_retry_find(asset);
        assert(failure && failure->at-now==base);
        unsigned before=requests;now=failure->at-1;assert(!start() && requests==before);
        now++;assert(cells_retry_ready(asset));
    }
    assert(cells_retry_delay(UINT_MAX,UINT32_MAX)==900000000);
    puts("PASS: 5/10/20 second sequence, hard 15 minute cap, jitter bounds and actual request gates");

    const int permanent[]={400,401,403,404,410,422,499};
    for(unsigned i=0;i<sizeof(permanent)/sizeof(*permanent);i++) {
        reset();status=permanent[i];assert(!start());unsigned before=requests;
        assert(strstr(cells_work.message,asset) && strstr(cells_work.message,"REFRESH"));
        now+=INT64_C(864000000000);assert(!start() && requests==before);
        assert(ls_cells_query("Boston"));cells_step();assert(!start() && requests==before);
        /* An unrelated successful catalog must not clear the states stop. */
        status=200;assert(cells_download(1,"cells/catalog.ccm","cells/catalog.ccm",0,NULL));
        assert(cells_download_step()==1 && cells_retry_find(asset));
        assert(!start());explicit_action(4);assert(!cells_retry_find(asset));
        assert(start());assert(cells_download_step()==7);
    }
    const unsigned actions[]={1,3,4,7};
    for(unsigned i=0;i<sizeof(actions)/sizeof(*actions);i++) {
        reset();status=404;assert(!start());explicit_action(actions[i]);
        assert(!cells_retry_find(asset));status=200;assert(start());cells_download_step();
    }
    reset();status=404;assert(!start());explicit_action(2);assert(cells_retry_find(asset));
    reset();alloc_fail=true;assert(!start());now+=1000000000;unsigned before=requests;
    assert(!start() && requests==before);alloc_fail=false;explicit_action(4);assert(!cells_retry_oom);
    puts("PASS: permanent 4xx stop across time/queries/other successes, explicit UI/console actions reset, allocation fails closed");

    reset();status=429;assert(!start());assert(cells_retry_find(asset)->at-now==60000000);
    explicit_action(4);retry_header="120";assert(!start());assert(cells_retry_find(asset)->at-now==120000000);
    explicit_action(4);retry_header="1";assert(!start());assert(cells_retry_find(asset)->at-now==60000000);
    explicit_action(4);status=503;retry_header="1800";assert(!start());assert(cells_retry_find(asset)->at-now==1800000000);
    explicit_action(4);retry_header="Thu, 01 Jan 2026 00:02:00 GMT";assert(!start());assert(cells_retry_find(asset)->at-now==120000000);
    assert(cells_retry_after(" 17 \t")==17000000);
    assert(cells_retry_after("bad")==0 && cells_retry_after("2 garbage")==0);
    assert(cells_retry_after("Thu, 31 Feb 2026 00:02:00 GMT")==0);
    assert(cells_retry_after("Wed, 31 Dec 2025 23:59:59 GMT")==0);
    assert(cells_retry_after("9999999999999999999999999999")==INT64_MAX);
    explicit_action(4);retry_header="bad";assert(!start());assert(cells_retry_find(asset)->at-now==5000000);
    const int transient[]={408,500,502,503,504};
    for(unsigned i=0;i<sizeof(transient)/sizeof(*transient);i++) {
        reset();status=transient[i];assert(!start());assert(cells_retry_find(asset)->at-now==5000000);
    }
    reset();open_result=-1;assert(!start());assert(cells_retry_find(asset)->at-now==5000000);
    puts("PASS: 429 floor, 429/503 Retry-After seconds/date, invalid-header fallback, transient HTTP/network backoff");

    reset();assert(!start());now=cells_retry_find(asset)->at;status=200;
    assert(start());assert(cells_download_step()==7 && !cells_retry_find(asset));
    status=500;assert(!start());assert(cells_retry_find(asset)->failures==1 && cells_retry_find(asset)->at-now==5000000);
    reset();status=200;format_ok=false;assert(start());assert(!cells_download_step());
    assert(cells_retry_find(asset)->at==INT64_MAX && strstr(cells_work.message,"format"));
    explicit_action(4);format_ok=true;assert(start());assert(cells_download_step()==7);
    /* Known-size asset: hash failure, declared-size mismatch, clean short body,
       and incomplete/time-out body are distinct production failure paths. */
    uint8_t sha[32]={0};const char *tile="cells/8-1-2.ctile",*remote="cells/8/1/2.ctile";
    reset();status=200;hash_ok=false;read_result=4;
    assert(cells_download(6,remote,tile,4,sha));assert(!cells_download_step());
    assert(cells_retry_find(tile)->at==INT64_MAX && strstr(cells_work.message,"SHA256"));
    reset();status=200;content_length=3;
    assert(!cells_download(6,remote,tile,4,sha));assert(cells_retry_find(tile)->at==INT64_MAX);
    reset();status=200;assert(cells_download(6,remote,tile,4,sha));assert(!cells_download_step());
    assert(cells_retry_find(tile)->at==INT64_MAX);
    reset();status=200;complete=false;assert(cells_download(6,remote,tile,4,sha));assert(!cells_download_step());
    assert(cells_retry_find(tile)->at-now==5000000);
    puts("PASS: successful install resets streak, format/magic/hash/size failures stop, incomplete body backs off");

    reset();status=500;part_bytes=4;
    assert(!cells_download(6,remote,tile,8,sha));now=cells_retry_find(tile)->at;
    status=200;read_result=4;assert(cells_download(6,remote,tile,8,sha));
    assert(strstr(dl.remote,".p0"));assert(!cells_download_step() && !dl.active);
    assert(cells_retry_find(tile)->failures==1);status=500;
    assert(!cells_download(6,remote,tile,8,sha));assert(strstr(dl.remote,".p1"));
    assert(cells_retry_find(tile)->at-now==10000000);
    now=cells_retry_find(tile)->at;status=200;assert(cells_download(6,remote,tile,8,sha));
    assert(cells_download_step()==6 && !cells_retry_find(tile));
    reset();status=500;cw.states_refresh=true;cells_step();assert(cells_retry_find(asset));
    now=cells_retry_find(asset)->at;status=200;cells_step();assert(dl.active && dl.kind==7);
    cells_step();assert(!cw.states_refresh && !cells_retry_find(asset));
    cells_retry_reset(NULL);
    puts("PASS: part transitions retain streak until whole-asset success; states retry and completion dispatch");
}
'''
(out / 'test.c').write_text(code)
exe = out / 'test.exe'
subprocess.run([sys.argv[1], '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                str(out / 'test.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], cwd=out, check=True)
