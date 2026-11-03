#include "lakeshark_app.h"
#include <stdio.h>
static lap_dir_entry_t rows[16];
static int job,picker,scope,stream,selected,count;
static uint32_t cursor,next;
static bool reading,writing,write_access;
static char status[160]="P pick file  F pick folder  W read/write mode";
static char preview[256];
static void list(void) { job=lap_dir_list(scope,"",cursor); reading=writing=false; }
static void enter(void) {
    lap_caps_t c; lap_caps(&c,sizeof(c));
    if (!(c.hardware&LAP_HW_SD)) { snprintf(status,sizeof(status),"SD storage not fitted"); return; }
    if (!job&&!picker&&!stream) list();
}
static void poll(void) {
    if (picker>0) {
        int r=lap_file_picker_poll(picker);
        if (r) { picker=0; if(r>0) { if(scope) lap_file_scope_close(scope); scope=r; cursor=0;
            /* Opening empty path works for a selected file. A folder instead
               returns an I/O error on read; F immediately starts its listing. */
            if(reading) { stream=lap_stream_open(scope,"",LAP_FILE_READ|(write_access?LAP_FILE_WRITE:0)); if(stream>0) job=lap_stream_read(stream,255); }
            else list();
        } else snprintf(status,sizeof(status),"Picker cancelled/error: %d",r); }
    }
    if(job>0) {
        int32_t result; uint32_t page; int done=lap_storage_poll(job,reading?preview:(void *)rows,reading?255:sizeof(rows),&result,&page);
        if(done==1) {
            job=0;
            if(result<0) snprintf(status,sizeof(status),"Storage result: %ld",(long)result);
            else if(reading) { preview[result]=0; snprintf(status,sizeof(status),"Read %ld bytes: %.100s",(long)result,preview); }
            else if(writing) snprintf(status,sizeof(status),"Written %ld bytes",(long)result);
            else { count=result; next=page; selected=0; snprintf(status,sizeof(status),"%d rows, cursor %lu; Enter reads a file",count,(unsigned long)cursor); }
            if(stream>0) { lap_stream_close(stream); stream=0; }
            reading=writing=false;
        }
    }
}
static void draw(void) {
    poll(); lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,1,"SCOPED FILES",LAP_ATTR(LAP_CYAN|LAP_BRIGHT,LAP_BLACK));
    lap_put_str(1,2,status,LAP_ATTR(LAP_YELLOW,LAP_BLACK));
    for(int i=0;i<count&&i<lap_rows()-6;i++) {
        lap_put_str(1,4+i,rows[i].name,LAP_ATTR(i==selected?LAP_BLACK:LAP_WHITE,i==selected?LAP_CYAN:LAP_BLACK));
    }
    lap_put_str(1,lap_rows()-2,"P file F folder N next H home W mode S write C cancel",LAP_ATTR(LAP_WHITE,LAP_BLACK));
}
static bool key(lap_key_t k,char ch) {
    if(ch=='c'||ch=='C') { if(job>0) lap_storage_cancel(job); return true; }
    if(job>0||picker>0) return false;
    if(k==LAP_KEY_UP) { if(selected) selected--; return true; }
    if(k==LAP_KEY_DOWN) { if(selected+1<count) selected++; return true; }
    if(ch=='w'||ch=='W') { write_access=!write_access; snprintf(status,sizeof(status),write_access?"Request READ + WRITE":"Request READ ONLY"); return true; }
    if(ch=='p'||ch=='P'||ch=='f'||ch=='F') {
        bool folder=ch=='f'||ch=='F';
        lap_file_picker_config_t cfg={.size=sizeof(cfg),.version=1,.flags=LAP_FILE_READ|(write_access?LAP_FILE_WRITE:0)|(folder?LAP_FILE_FOLDER:0)};
        snprintf(cfg.title,sizeof(cfg.title),"%s %s",folder?"FOLDER":"FILE",write_access?"READ + WRITE":"READ ONLY");
        picker=lap_file_picker(&cfg); reading=!folder; return true;
    }
    if(ch=='h'||ch=='H') { if(scope) lap_file_scope_close(scope); scope=0; cursor=0; list(); return true; }
    if(ch=='n'||ch=='N') { cursor=next; list(); return true; }
    if(k==LAP_KEY_ENTER&&count&&!rows[selected].dir) {
        stream=lap_stream_open(scope,rows[selected].name,LAP_FILE_READ); reading=true;
        if(stream>0) job=lap_stream_read(stream,255); return true;
    }
    if(ch=='s'||ch=='S') {
        /* Explicit app workflow: write only in its own streaming folder. */
        stream=lap_stream_open(0,"note.txt",LAP_FILE_READ|LAP_FILE_WRITE); writing=true; reading=false;
        if(stream>0) { lap_stream_seek(stream,0); job=lap_stream_write(stream,"Hello from SCOPED FILES\n",24); } return true;
    }
    return false;
}
LAP_APP(.id="scoped_files",.name="SCOPED FILES",.sub="Paged storage and file grants",.version="1.0",.author="LakeShark",
        .icon=LAP_ICON_FILES,.color=LAP_CYAN,.enter=enter,.draw=draw,.key=key);
