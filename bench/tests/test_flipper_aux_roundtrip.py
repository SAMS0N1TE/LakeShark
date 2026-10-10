"""Compile the production AUX encoder with the actual Flipper v2.10 decoder.
Usage: python bench/tests/test_flipper_aux_roundtrip.py [path/to/ls-flipper-29]
Only reads the app checkout; generated C/exe stay under bench/build.
"""
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
app = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root.parent / "ls-flipper-29"
header = (app / "ls_link.h").read_text()
header = header[header.index("typedef enum {"):header.index("typedef struct LsLink")]
link = (app / "ls_link.c").read_text()
parser = link[link.index("static const char* kv("):link.index("static void parse_prof(LsLink* link, char* line);")]
source = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "flipper_link_aux.h"
static size_t strlcpy(char *dst,const char *src,size_t cap) {
    size_t n=strlen(src);if(cap) {size_t k=n<cap-1?n:cap-1;memcpy(dst,src,k);dst[k]=0;}return n;
}
#define FURI_LOG_I(...) ((void)0)
#define FURI_LOG_W(...) ((void)0)
static uint32_t furi_get_tick(void) {return 123;}
#define FuriWaitForever 0
#define furi_mutex_acquire(...) ((void)0)
#define furi_mutex_release(...) ((void)0)
""" + header + r"""
typedef struct {
    LsTelemetry parsed,tel;
    bool have_tel;
    uint32_t last_tel_tick,frames;
} LsLink;
""" + parser + r"""
static void decode(LsLink *link,char *buf) {
    size_t n=strlen(buf);assert(n>0 && buf[n-1]=='\n');buf[n-1]=0;
    parse_eq_line(link,buf);
}
int main(void) {
    LsLink link={0};char buf[384];
    ls_link_aux_t s={.connected=1,.saved=8,.running=1,.muted=1,
        .ssid="Lake House=AP\tTest",.ip="10.0.0.2",.drone_id="RID = 123",
        .counts={1,2,3,4,5},.drones=2,.nearest_m=111,
        .serial={UINT32_MAX,22},.rssi={-128,-67},.category={0,3}};
    int n=ls_link_aux_encode(buf,sizeof(buf),&s);
    assert(n==(int)strlen(buf) && n<LS_AUX_WIRE_MAX);
    decode(&link,buf);
    assert(link.tel.aux.version==1 && link.tel.aux.tick==123);
    assert(link.tel.aux.wifi_connected==1 && link.tel.aux.wifi_saved==8);
    assert(!strcmp(link.tel.aux.wifi_ssid,"Lake House AP Test"));
    assert(!strcmp(link.tel.aux.wifi_ip,"10.0.0.2"));
    assert(link.tel.aux.sweep_running==1 && link.tel.aux.sweep_muted==1);
    for(unsigned i=0;i<5;i++) assert(link.tel.aux.counts[i]==(int)i+1);
    assert(!strcmp(link.tel.aux.contacts[0],"C4294967295:-128"));
    assert(!strcmp(link.tel.aux.contacts[1],"T22:-67"));
    assert(link.tel.aux.drones==2 && link.tel.aux.nearest_m==111);
    assert(!strcmp(link.tel.aux.drone_id,"RID   123"));
    char rec[]="md=REC f=433420000";parse_telemetry(&link,rec);
    char eq[]="eq=2";parse_eq_line(&link,eq);
    assert(link.tel.aux.nearest_m==111 && link.tel.aux.tick==123);
    memset(&s,0,sizeof(s));s.saved=-1;s.nearest_m=-1;
    assert(ls_link_aux_encode(buf,sizeof(buf),&s)>0);decode(&link,buf);
    assert(link.tel.aux.version==1 && link.tel.aux.wifi_saved==-1);
    assert(!link.tel.aux.wifi_ssid[0] && !link.tel.aux.wifi_ip[0]);
    assert(!link.tel.aux.contacts[0][0] && !link.tel.aux.drone_id[0]);
    assert(link.tel.aux.nearest_m==-1 && !link.tel.aux.drones && !link.tel.aux.sweep_running);
    memset(s.ssid,'X',sizeof(s.ssid));memset(s.ip,'Y',sizeof(s.ip));
    memset(s.drone_id,'Z',sizeof(s.drone_id));
    for(unsigned i=0;i<5;i++) s.counts[i]=256;
    s.serial[0]=s.serial[1]=UINT32_MAX;s.rssi[0]=s.rssi[1]=-128;
    s.drones=32;s.nearest_m=INT_MAX;
    n=ls_link_aux_encode(buf,sizeof(buf),&s);assert(n>0 && n<320);
    assert(ls_link_aux_encode(buf,(size_t)n+1,&s)==n);
    assert(ls_link_aux_encode(buf,(size_t)n,&s)==0 && !buf[0]);
    assert(ls_link_aux_encode(buf,1,&s)==0 && !buf[0]);
    assert(ls_link_aux_encode(NULL,0,&s)==0);
    puts("AUX production encoder / Flipper v2.10 decoder round-trip passed");
    return 0;
}
"""
out = root / "bench/build/flipper-aux-roundtrip"
out.mkdir(parents=True, exist_ok=True)
c = out / "roundtrip.c"
c.write_text(source)
exe = out / "roundtrip.exe"
subprocess.run([shutil.which("gcc") or "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-I", str(root / "main"), str(c),
                str(root / "main/flipper_link_aux.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
