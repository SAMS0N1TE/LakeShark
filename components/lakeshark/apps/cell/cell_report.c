/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cell_report.h"
#include "ls_sdcard.h"
#include "ls_gps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static char target[17],last_text[49];
static uint32_t boot,sequence;
static int64_t last_attempt;
static bool initialized,queued;
static unsigned last_pass;
static char last_kind;
static uint32_t last_lte_hz;
static int last_lte_pci=-1;
__attribute__((weak)) bool cell_report_transport_known(const char *p){(void)p;return false;}
__attribute__((weak)) bool cell_report_transport_send(const char *p,const char *t){(void)p;(void)t;return false;}
__attribute__((weak)) int cell_report_transport_state(const char *p,const char *t){(void)p;(void)t;return -1;}
static bool valid_peer(const char *p)
{if(!p || strlen(p)!=16)return false;for(int i=0;i<16;i++)if(!isxdigit((unsigned char)p[i]))return false;return true;}
void cell_report_init(void)
{
    portENTER_CRITICAL(&lock);bool ready=initialized;
    if(!ready){initialized=true;boot=esp_random();}
    portEXIT_CRITICAL(&lock);
    if(ready)return;
    char peer[18]={0};FILE *f=fopen("/sdcard/cell/report.txt","rb");
    if(f){size_t n=fread(peer,1,17,f);fclose(f);if(n==16 && valid_peer(peer)){
        portENTER_CRITICAL(&lock);memcpy(target,peer,17);portEXIT_CRITICAL(&lock);
    }}
}
bool cell_report_target(const char *peer)
{
    if(!peer || (*peer && (!valid_peer(peer) || !cell_report_transport_known(peer))))return false;
    cell_report_init();
    if(!ls_sdcard_mounted())return false;
    mkdir("/sdcard/cell",0775);
    const char *path="/sdcard/cell/report.txt",*tmp="/sdcard/cell/report.new";
    FILE *f=fopen(tmp,"wb");if(!f)return false;
    bool ok=fwrite(peer,1,strlen(peer),f)==strlen(peer);
    if(fflush(f) || fsync(fileno(f)))ok=false;
    if(fclose(f))ok=false;
    /* FAT rename cannot replace. Keep a recoverable previous file. */
    const char *bak="/sdcard/cell/report.bak";
    if(ok){remove(bak);rename(path,bak);ok=rename(tmp,path)==0;if(!ok)rename(bak,path);}
    if(!ok){remove(tmp);return false;}
    portENTER_CRITICAL(&lock);
    snprintf(target,sizeof(target),"%s",peer);last_text[0]=0;queued=false;last_attempt=0;last_pass=0;last_kind=0;
    last_lte_hz=0;last_lte_pci=-1;
    portEXIT_CRITICAL(&lock);return true;
}
static void emit(const cell_status_t *s,char kind,bool force)
{
    cell_report_init();int64_t now=esp_timer_get_time();char peer[17];
    cell_report_record_t record={.pci=-1,.lat_e5=INT32_MIN,.lon_e5=INT32_MIN,
        .band=s->band,.kind=kind,.changed=s->compared?s->changed:0};
    if(kind=='L' && s->lte_count) {
        unsigned i=s->lte_count-1;
        record.pci=s->cell[i].pci;record.frequency_khz=s->cell[i].hz/1000;record.flags|=32;
    }
    portENTER_CRITICAL(&lock);
    bool novel_lte=kind=='L' && (record.frequency_khz!=last_lte_hz || record.pci!=last_lte_pci);
    bool transition=strchr("DSCF",kind) && kind!=last_kind;
    bool immediate=force || novel_lte || transition;
    unsigned progress=kind=='P'?s->tune:s->passes;
    if(!target[0] || (last_attempt && now-last_attempt<30000000 && !immediate) ||
       (!immediate && kind==last_kind && progress==last_pass)) {portEXIT_CRITICAL(&lock);return;}
    if(!immediate && kind==last_kind && now-last_attempt<120000000) {portEXIT_CRITICAL(&lock);return;}
    snprintf(peer,sizeof(peer),"%s",target);last_attempt=now;last_kind=kind;last_pass=progress;
    if(kind=='L'){last_lte_hz=record.frequency_khz;last_lte_pci=record.pci;}
    record.boot=boot;record.sequence=++sequence;
    portEXIT_CRITICAL(&lock);
    time_t epoch=time(NULL);if(epoch>1735689600){record.epoch=(uint32_t)epoch;record.flags|=2;}
    ls_gps_state_t gps;ls_gps_get(&gps);
    if(gps.fix && gps.last_fix_us>0 && now-gps.last_fix_us>=0 && now-gps.last_fix_us<5000000 &&
       gps.hdop>0 && gps.hdop<=5 && isfinite(gps.lat_deg) && isfinite(gps.lon_deg) && fabs(gps.lat_deg)<=90 && fabs(gps.lon_deg)<=180) {
        record.lat_e5=(int32_t)llround(gps.lat_deg*100000);record.lon_e5=(int32_t)llround(gps.lon_deg*100000);record.flags|=1;
    }
    if(s->compared)record.flags|=4;
    if(s->quiet)record.flags|=8;
    if(!s->receiver_missing)record.flags|=16;
    if(!s->lte && s->rows){record.frequency_khz=s->row[0].hz/1000;record.rise=s->compared?s->row[0].delta:0;}
    if(kind=='P' || kind=='C')record.frequency_khz=s->frequency/1000;
    char text[49];if(!cell_report_encode(&record,text,sizeof(text)))return;
    bool accepted=cell_report_transport_send(peer,text);
    portENTER_CRITICAL(&lock);
    snprintf(last_text,sizeof(last_text),"%s",text);queued=accepted;
    portEXIT_CRITICAL(&lock);
    printf("CELLREPORT %s %s\n",accepted?"queued":"refused",text);
}
void cell_report_observe(const cell_status_t *s)
{
    if(!s->passes && !s->receiver_missing && !s->lte_count)return;
    emit(s,s->receiver_missing?'D':s->lte && s->lte_count?'L':s->compared?(s->changed?'R':'Q'):'U',false);
}
void cell_report_activity(const cell_status_t *s,char kind)
{
    if(kind && strchr("PSCF",kind))emit(s,kind,false);
}
void cell_report_status(char *text,size_t size)
{
    char peer[17],payload[49];bool accepted;int64_t when;
    portENTER_CRITICAL(&lock);
    memcpy(peer,target,sizeof(peer));memcpy(payload,last_text,sizeof(payload));accepted=queued;when=last_attempt;
    portEXIT_CRITICAL(&lock);
    const char *state="waiting for a complete pass";
    if(!peer[0])state="off; choose REPORT destination";
    else if(payload[0]) {
        int delivery=accepted?cell_report_transport_state(peer,payload):-1;
        state=!accepted?"blocked: check Mesh TX and receiver":delivery==4?"recipient ACK received":
              delivery==2 || delivery==3?"sent; awaiting recipient ACK":
              esp_timer_get_time()-when>30000000?"no delivery confirmation":"queued for MeshCore";
    }
    snprintf(text,size,"LoRa: %s",state);
}
int cell_report_command(int argc,char **argv)
{
    cell_report_init();
    if(argc==3 && !strcmp(argv[1],"target"))printf("CELLREPORT target %s\n",cell_report_target(argv[2])?"saved":"rejected");
    else if(argc==2 && !strcmp(argv[1],"off"))printf("CELLREPORT off %s\n",cell_report_target("")?"saved":"failed");
    else if(argc==2 && !strcmp(argv[1],"test")) {cell_status_t s;cell_monitor_get(&s);emit(&s,'T',false);}
    else if(argc!=1 && !(argc==2 && !strcmp(argv[1],"status")))puts("cellreport target <16-hex-peer> | off | test | status");
    char text[100];cell_report_status(text,sizeof(text));puts(text);return 0;
}
