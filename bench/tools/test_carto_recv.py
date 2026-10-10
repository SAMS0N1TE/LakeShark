"""Fault-inject production receiver authentication, limits, idle and shutdown."""
import re
from pathlib import Path
import shutil
import subprocess
root=Path(__file__).resolve().parents[2]
out=root/'bench/build/recv-test'
out.mkdir(parents=True,exist_ok=True)
(out/'maps').mkdir(exist_ok=True)
source=(root/'components/apps/tui/ls_cartocore.c').read_text()
receiver=source[source.index('/* The supervisor owns'):source.index('static int recv_start(')]
code=r"""
#include "ls_tiles.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define MAP_DIR "bench/build/recv-test/maps"
#define FETCH_CHUNK 32768
#define TRANSFER_MARGIN 1048576ULL
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND -3
#define CONFIG_LWIP_IPV6 1
#define HTTPD_SOCK_ERR_TIMEOUT -2
#define MSG_DONTWAIT 0
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_408_REQ_TIMEOUT 408
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define pdMS_TO_TICKS(n) (n)
typedef int esp_err_t;
typedef int httpd_err_code_t;
typedef void *httpd_handle_t;
typedef struct { void *user_ctx; const char *uri; size_t content_len; const char *code; } httpd_req_t;
static atomic_bool transfer_cancel,fetch_running;
static ls_carto_transfer transfer;
static int transfer_mux;
static int64_t now,step;
static bool other_interface,idle,ipv6,native_ipv6;
static int rejected,published,stop_calls,freed,yields,body_reads,auth_reads;
static int64_t receive_delay;
static bool disconnected,zero_body,validation_fail;
static size_t fragment=FETCH_CHUNK;
static int lock_held,write_calls,close_calls;
static size_t write_sizes[1024];
static bool write_fail,close_fail;
static int host_peek(int fd,char *buf,int n,int flags) { (void)fd;(void)buf;(void)n;(void)flags;errno=EAGAIN;return disconnected?0:-1; }
#define recv host_peek
static ssize_t host_write(int fd,const void *buf,size_t n) { assert(write_calls<1024);write_sizes[write_calls++]=n;assert(n<=FETCH_CHUNK);return write_fail?-1:write(fd,buf,n); }
static int host_close(int fd) { close_calls++;int result=close(fd);return close_fail?-1:result; }
#define write host_write
#define close host_close
static int64_t esp_timer_get_time(void) { now+=step; return now; }
static void vTaskDelay(int n) { assert(n>0); yields++; }
static void vTaskDelete(void *arg) { assert(!arg); }
static unsigned uxTaskGetStackHighWaterMark(void *arg) { assert(!arg); return 2048; }
static size_t internal_free(void) { return 1; }
static size_t dma_free(void) { return 1; }
static void heaps(const char *s) { (void)s; }
static void heap_caps_free(void *p) { assert(stop_calls>=3); freed++; free(p); }
static int httpd_stop(void *s) { assert(s && !freed && atomic_load(&fetch_running)); return ++stop_calls<3?ESP_FAIL:ESP_OK; }
static int httpd_resp_send_err(httpd_req_t *r,int code,const char *msg) { (void)r; (void)msg; rejected=code; return ESP_OK; }
static void httpd_resp_set_status(httpd_req_t *r,const char *s) { (void)r; rejected=atoi(s); }
static void httpd_resp_set_type(httpd_req_t *r,const char *s) { (void)r; (void)s; }
static void httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *v) { (void)r; (void)k; (void)v; }
static int httpd_resp_sendstr(httpd_req_t *r,const char *s) { (void)r; (void)s; return ESP_OK; }
static int httpd_query_key_value(const char *q,const char *key,char *value,size_t size) {
    size_t n=strlen(key);
    for(const char *p=q;*p;) {
        const char *end=strchr(p,'&'); if(!end) end=p+strlen(p);
        if((size_t)(end-p)>n && !strncmp(p,key,n) && p[n]=='=') {
            size_t len=end-p-n-1; if(len>=size) return ESP_FAIL;
            memcpy(value,p+n+1,len); value[len]=0; return ESP_OK;
        }
        p=*end?end+1:end;
    }
    return ESP_ERR_NOT_FOUND;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *key) {
    assert(!strcmp(key,"X-Carto-Code")); return r->code?strlen(r->code):0;
}
static int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *key,char *out,size_t size) {
    if(!strcmp(key,"X-Carto-SHA256")) { snprintf(out,size,"%064d",0);return ESP_OK; }
    auth_reads++; assert(!strcmp(key,"X-Carto-Code")); if(!r->code) return ESP_ERR_NOT_FOUND;
    snprintf(out,size,"%s",r->code); return strlen(r->code)>=size?ESP_FAIL:ESP_OK;
}
static int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return 1; }
static int local_address(int fd,struct sockaddr *out,int *size) {
    (void)fd; uint32_t ip=inet_addr(other_interface?"10.0.0.1":"192.168.1.9");
    if(ipv6) {
        assert(*size>=(int)sizeof(struct sockaddr_in6));
        struct sockaddr_in6 *a=(struct sockaddr_in6 *)out;
        memset(a,0,sizeof(*a)); a->sin6_family=AF_INET6;
        unsigned char *p=(unsigned char *)&a->sin6_addr;
        p[10]=p[11]=0xff; memcpy(p+12,&ip,4);
        if(native_ipv6) p[0]=0x20;
        *size=sizeof(*a);
    } else {
        struct sockaddr_in *a=(struct sockaddr_in *)out;
        a->sin_family=AF_INET; a->sin_addr.s_addr=ip; *size=sizeof(*a);
    }
    return 0;
}
#define getsockname local_address
#define socklen_t int
static void transfer_stage(const char *stage) { snprintf(transfer.stage,sizeof(transfer.stage),"%s",stage); }
static void transfer_progress(uint64_t n,uint64_t total) { transfer.bytes=n; transfer.total=total; }
static void transfer_end(bool ok,const char *s) { transfer.state=ok?2:3; snprintf(transfer.message,sizeof(transfer.message),"%s",s); }
void ls_tiles_expected(const char *s,char sha[65],uint64_t *n) { (void)s; *n=0; sha[0]=0; }
static int esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *available) { (void)path; *total=*available=10000000; return ESP_OK; }
typedef struct { int ctx; } stream_digest;
static bool stream_begin(stream_digest *s) { s->ctx=0; return true; }
static bool stream_update(stream_digest *s,const uint8_t *p,size_t n) { (void)s; (void)p; (void)n; return true; }
static void mbedtls_sha256_free(int *ctx) { (void)ctx; }
static bool verify_transfer(stream_digest *stream,const char *sha,uint64_t expected,uint64_t bytes,int64_t deadline,char digest[65]) {
    (void)stream; (void)sha; (void)digest; return expected==bytes && now<deadline;
}
static int publish_map(const char *part,const char *path,bool force,const char *digest) { (void)digest; (void)force; published++; return rename(part,path); }
static int httpd_req_recv(httpd_req_t *r,char *out,size_t n) { (void)r; body_reads++; now+=receive_delay; if(idle) return HTTPD_SOCK_ERR_TIMEOUT; if(zero_body)return 0;if(n>fragment)n=fragment;memset(out,'A',n);return (int)n; }
static int host_mkdir(const char *p,int mode) { (void)mode; return mkdir(p); }
#define mkdir host_mkdir

static int command_lock=1,asset_installed;

static struct { char part[128]; uint64_t offset;bool active;unsigned char sha[32]; } upload;
#define pdTRUE 1
static int xSemaphoreTake(int lock,int ticks) { (void)ticks;assert(lock==command_lock && !lock_held);lock_held=1;return pdTRUE; }
static void xSemaphoreGive(int lock) { assert(lock==command_lock && lock_held);lock_held=0; }
static bool cells_safe_asset(const char *name) { return !strcmp(name,"cells/index.cci") || !strcmp(name,"places/places_base.ccpl") || !strcmp(name,"cells/catalog.ccm") || !strcmp(name,"cells/overview.ctile") || !strcmp(name,"cells/8-76-93.ctile"); }
static bool cells_upload_begin(const char *name,uint64_t n,const char *sha) { assert(cells_safe_asset(name) && n && strlen(sha)==64);strcpy(upload.part,MAP_DIR "/asset.part");upload.active=true;memset(upload.sha,0,32);return true; }
static bool cells_asset_valid_checked(const char *path,const char *name,bool (*check)(void *),void *ctx) { (void)path;(void)name;assert(!strcmp(transfer.stage,"verify"));return check(ctx) && !validation_fail; }
static bool cells_upload_finish(bool verified) { if(verified) { assert(!strcmp(transfer.stage,"install"));asset_installed++; } upload.active=false;return verified; }
static int mbedtls_sha256_finish(int *ctx,unsigned char out[32]) { (void)ctx;memset(out,0,32);return 0; }
static void transfer_begin(const char *name,uint64_t n) { (void)name;transfer.total=n;transfer.state=1; }
"""+receiver+r"""
static recv_job fresh(void) {
    recv_job j={0}; j.buf=malloc(FETCH_CHUNK); j.deadline=100000000; j.limit=1000;
    strcpy(j.ip,"192.168.1.9"); strcpy(j.code,"0123456789abcdef0123456789abcdef");
    atomic_init(&j.done,false); now=step=0; rejected=0; transfer.state=1; return j;
}
int main(void) {
    remove(MAP_DIR "/test.ctile"); remove(MAP_DIR "/test.ctile.part");
    recv_job j=fresh(); httpd_req_t r={&j,"/maps/test.ctile",64,NULL};
    assert(recv_put(&r)==ESP_FAIL && rejected==403 && !j.claimed && !published);
    r.code="ffffffffffffffffffffffffffffffff";
    assert(recv_put(&r)==ESP_FAIL && rejected==403 && !j.claimed);
    assert(!body_reads && !atomic_load(&j.done) && transfer.state==1 && now==0);
    r.code="0123456789abcdef0123456789abcdefEXTRA";
    assert(recv_put(&r)==ESP_FAIL && rejected==403 && !j.claimed && !body_reads);
    r.code="0123456789abcdef0123456789abcdefX";
    assert(recv_put(&r)==ESP_FAIL && rejected==403 && !j.claimed && !body_reads);
    r.uri="/maps/test.ctile?code=0123456789abcdef0123456789abcdefEXTRA"; r.code=j.code;
    assert(recv_put(&r)==ESP_FAIL && rejected==403 && !j.claimed && !body_reads);
    r.uri="/maps/test.ctile";
    r.code=j.code; other_interface=true;
    assert(recv_put(&r)==ESP_FAIL && rejected==403 && !j.claimed); other_interface=false;
    r.code=NULL; r.uri="/maps/test.ctile?code=0123456789abcdef0123456789abcdef&f=1";
    assert(recv_put(&r)==ESP_OK && rejected==201 && j.claimed && published==1);
    assert(recv_put(&r)==ESP_FAIL && rejected==503 && published==1); free(j.buf);
    remove(MAP_DIR "/test.ctile");
    j=fresh(); j.limit=400000; r.user_ctx=&j; r.uri="/maps/test.ctile?f=1"; r.code=j.code; r.content_len=356054;
    ipv6=true; other_interface=true;
    assert(recv_put(&r)==ESP_FAIL && rejected==403 && !j.claimed); other_interface=false;
    native_ipv6=true;
    assert(recv_put(&r)==ESP_FAIL && rejected==403 && !j.claimed); native_ipv6=false;
    int auth_before=auth_reads,reads_before=body_reads;
    receive_delay=11000000; j.deadline=200000000;
    assert(recv_put(&r)==ESP_OK && rejected==201 && j.claimed && published==2);
    assert(auth_reads==auth_before+1 && body_reads>reads_before+2 && transfer.bytes==356054);
    receive_delay=0; ipv6=false; free(j.buf);
    j=fresh(); r.user_ctx=&j; r.uri="/maps/test.ctile"; r.code=j.code; r.content_len=1001;
    assert(recv_put(&r)==ESP_FAIL && published==2 && transfer.state==3); free(j.buf);
    remove(MAP_DIR "/test.ctile");
    j=fresh(); r.user_ctx=&j; r.content_len=64; idle=true; step=2000000;
    assert(recv_put(&r)==ESP_FAIL && rejected==408 && published==2);
    assert(strstr(transfer.message,"idle timeout") && access(MAP_DIR "/test.ctile.part",F_OK)); free(j.buf);
    idle=false; step=0;
    j=fresh();r.user_ctx=&j;r.uri="/maps/cells/index.cci";r.code=NULL;r.content_len=64;
    int before=body_reads;assert(recv_put(&r)==ESP_FAIL && rejected==403 && before==body_reads);
    r.code=j.code;assert(recv_put(&r)==ESP_OK && rejected==200 && asset_installed==1 && !atomic_load(&j.done));
    r.uri="/maps/places/places_base.ccpl";assert(recv_put(&r)==ESP_OK && asset_installed==2 && !j.claimed);

    r.uri="/maps/cells/../bad";assert(recv_put(&r)==ESP_FAIL && rejected==400 && asset_installed==2);
    j.limit=10000000;j.deadline=INT64_MAX;r.content_len=8303308;r.uri="/maps/cells/8-76-93.ctile";
    fragment=997;int writes=write_calls,closes=close_calls;
    assert(recv_put(&r)==ESP_OK && asset_installed==3 && !lock_held && !upload.active);
    assert(write_calls-writes==(8303308+FETCH_CHUNK-1)/FETCH_CHUNK && close_calls-closes==1);
    for(int i=writes;i<write_calls-1;i++)assert(write_sizes[i]==FETCH_CHUNK);
    assert(write_sizes[write_calls-1]==8303308%FETCH_CHUNK);
    assert(transfer.bytes==8303308 && !strcmp(transfer.stage,"install"));
    r.content_len=64;idle=true;step=2000000;int saved=asset_installed;
    assert(recv_put(&r)==ESP_FAIL && rejected==408 && !upload.active && !lock_held && asset_installed==saved);
    idle=false;step=0;zero_body=true;
    assert(recv_put(&r)==ESP_FAIL && rejected==400 && !upload.active && !lock_held);zero_body=false;
    disconnected=true;assert(recv_put(&r)==ESP_FAIL && !upload.active && !lock_held && asset_installed==saved);disconnected=false;
    write_fail=true;assert(recv_put(&r)==ESP_FAIL && !upload.active && !lock_held && asset_installed==saved);write_fail=false;
    close_fail=true;assert(recv_put(&r)==ESP_FAIL && !upload.active && !lock_held && asset_installed==saved);close_fail=false;
    validation_fail=true;assert(recv_put(&r)==ESP_FAIL && !upload.active && !lock_held && asset_installed==saved);validation_fail=false;
    upload.active=true;before=body_reads;assert(recv_put(&r)==ESP_FAIL && rejected==500 && upload.active && !lock_held && body_reads==before);upload.active=false;
    j.deadline=now;before=body_reads;assert(recv_put(&r)==ESP_FAIL && rejected==408 && !lock_held && body_reads==before);j.deadline=INT64_MAX;
    assert(recv_put(&r)==ESP_OK && asset_installed==saved+1 && !upload.active && !lock_held);
    free(j.buf);

    recv_job *owned=malloc(sizeof(*owned)); *owned=fresh(); owned->server=(void *)1;
    atomic_store(&owned->done,true); atomic_store(&fetch_running,true);
    recv_supervisor(owned);
    assert(stop_calls==3 && freed==1 && yields>=2 && !atomic_load(&fetch_running));
    puts("PASS: rejected requests preserve session and never read body; auth once; IPv4/mapped IPv6 station; 356054-byte upload; late data vs real idle; size cap; 8.3 MB fragmented asset; 254 writes/one close; EOF/disconnect/idle/write/close/validation cleanup and retry; shutdown retries");
}
"""
(out/'test.c').write_text(code)
gcc=shutil.which('gcc') or 'C:/mingw64/bin/gcc.exe'
subprocess.run([gcc,'-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I',str(root/'components/apps/tui'),str(out/'test.c'),'-lws2_32','-o',str(out/'test.exe')],check=True)
subprocess.run([str(out/'test.exe')],cwd=root,check=True)

# Exercise the actual laptop sender against a local HTTP endpoint.
from http.server import BaseHTTPRequestHandler, HTTPServer
from threading import Thread
import sys
received=[]
class Put(BaseHTTPRequestHandler):
    def do_PUT(self):
        if self.headers.get('X-Carto-Code') != '0123456789abcdef0123456789abcdef':
            self.send_response(403)
            self.send_header('Content-Length', '21')
            self.send_header('Connection', 'close')
            self.end_headers(); self.wfile.write(b'Session code required'); self.wfile.flush()
            self.close_connection=True
            return
        received.append((self.path,self.headers.get('X-Carto-Code'),self.rfile.read(int(self.headers['Content-Length']))))
        self.send_response(200 if '/' in self.path.removeprefix('/maps/') else 201); self.end_headers(); self.wfile.write(b'Saved')
    def log_message(self,*args): pass
server=HTTPServer(('127.0.0.1',0),Put)
thread=Thread(target=server.serve_forever,daemon=True); thread.start()
sample=out/'sender.ctile'; sample.write_bytes(b'CTILE sender test')
try:
    code='0123456789abcdef0123456789abcdef'
    result=subprocess.run([sys.executable,str(root/'tools/carto_push.py'),str(sample),'127.0.0.1',str(server.server_port),'--code',code,'--force'],capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    assert received==[('/maps/sender.ctile?f=1',code,sample.read_bytes())]
    for extra in ([],['--code','bad']):
        result=subprocess.run([sys.executable,str(root/'tools/carto_push.py'),str(sample),'127.0.0.1',str(server.server_port),*extra],capture_output=True)
        assert result.returncode!=0 and len(received)==1
    sample.write_bytes(b'A' * (8 * 1024 * 1024))
    result=subprocess.run([sys.executable,str(root/'tools/carto_push.py'),str(sample),'127.0.0.1',str(server.server_port),'--code','f'*32],capture_output=True,text=True)
    assert result.returncode==1 and 'HTTP 403 Forbidden: Session code required' in result.stderr,result.stderr
    assert len(received)==1
    # A rejected connection leaves the same server available for the right code.
    result=subprocess.run([sys.executable,str(root/'tools/carto_push.py'),str(sample),'127.0.0.1',str(server.server_port),'--code',code],capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    assert len(received)==2 and received[-1][2]==sample.read_bytes()

    prepared=out/'push-dir'
    asset_names=['cells/catalog.ccm','places/places_base.ccpl','places/places_US.ccpl','cells/overview.ctile','cells/8/76/91.ctile','cells/8/76/92.ctile','cells/8/76/93.ctile','cells/index.cci']
    expected=[]
    for i,name in enumerate(asset_names):
        path=prepared/name;path.parent.mkdir(parents=True,exist_ok=True)
        path.write_bytes(bytes([i])* (8303308 if '93.ctile' in name else 65537))
        relative=re.sub(r'cells/8/(\d+)/(\d+)\.ctile',r'cells/8-\1-\2.ctile',name)
        expected.append(('/maps/'+relative,code,path.read_bytes()))
    count=len(received)
    result=subprocess.run([sys.executable,str(root/'tools/carto_push.py'),str(prepared),'127.0.0.1',str(server.server_port),'--code',code,'--assets','--country','US'],capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    assert received[count:]==expected
finally:
    server.shutdown(); server.server_close(); thread.join()
print('PASS: carto_push auth/rejection; 8-asset batch including 8.3 MB cell; index last and exact bodies')
