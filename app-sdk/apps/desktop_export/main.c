/* API 2.6: asynchronous scoped file -> PC save dialog. */
#include "lakeshark_app.h"
#include <stdio.h>
#include <string.h>
static int stream, write_job, request, result;
static bool ready;
static void enter(void) {
    stream=write_job=request=result=0; ready=false;
    lap_caps_t caps; if(lap_caps(&caps,sizeof(caps))<0||!(caps.hardware&LAP_HW_SD)) return;
    stream=lap_stream_open(0,"report.csv",LAP_FILE_READ|LAP_FILE_WRITE);
    if(stream>0) { const char report[]="source,value\nLakeShark,42\n"; write_job=lap_stream_write(stream,report,sizeof(report)-1); }
}
static void leave(void) { if(request>0) lap_desktop_request_cancel(request); if(write_job>0) lap_storage_cancel(write_job); }
static bool key(lap_key_t k,char ch) {
    (void)k;
    if(ch=='c'||ch=='C') { if(request>0) lap_desktop_request_cancel(request); return true; }
    if(request>0) return false;
    if((ch=='u'||ch=='U')&&(lap_desktop_features()&1)) request=lap_desktop_open_url("https://lakeshark.org");
    else if((ch=='s'||ch=='S')&&ready&&(lap_desktop_features()&2)) request=lap_desktop_save_file(0,"report.csv","lakeshark-report.csv");
    else return false;
    if(request<0) { result=request; request=0; } return true;
}
static void draw(void) {
    if(write_job>0) { int32_t r; uint32_t next; int done=lap_storage_poll(write_job,NULL,0,&r,&next);
        if(done>0) { write_job=0; ready=r>=0; result=r<0?r:0; lap_stream_close(stream); stream=0; } }
    if(request>0) { int r=lap_desktop_request_status(request); if(r) { result=r; request=0; } }
    lap_desktop_event_t info; lap_desktop_info(&info);
    lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,1,"EXPORT TO PC",LAP_ATTR(LAP_CYAN|LAP_BRIGHT,LAP_BLACK));
    lap_put_str(1,3,lap_desktop_features()?"U website  S save CSV  C cancel":"Connect an updated desktop viewer",LAP_ATTR(LAP_YELLOW,LAP_BLACK));
    char line[100]; snprintf(line,sizeof(line),"CSV %s  request %d  result %d  scale %u/1000",ready?"ready":"unavailable",request,result,info.scale_milli);
    lap_put_str(1,5,line,LAP_ATTR(LAP_WHITE,LAP_BLACK));
}
LAP_APP(.id="desktop_export",.name="EXPORT TO PC",.sub="Desktop export and DPI",.version="1.0",.author="LakeShark",
    .icon=LAP_ICON_GEAR,.color=LAP_CYAN,.enter=enter,.leave=leave,.draw=draw,.key=key);
