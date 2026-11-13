/* System API contracts, real worker/broker, recording firmware picker. */
#include "ls_lap_system.h"
#include "desk/ls_desk_host.h"
#include "ls_lap_sys.h"
#include "ls_lap_service.h"
#include "ls_lap_kit.h"
#include "../shims/lap_kit_widgets.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p,0777)
#endif
static uint32_t gen;
static void begin(const char *id) {
    ls_lap_api_begin(id,++gen,0); ls_lap_sys_begin();
    lap_app_t app={.api_major=2,.api_minor=5};
    ls_lap_service_begin(&app);
    ls_lap_sys_host()->hardware=LAP_HW_SD|LAP_HW_DESKTOP;
    ls_lap_api_callback(true);
}
static void end(void) { ls_lap_sys_end(); ls_lap_api_callback(false); ls_lap_api_cleanup(); while(ls_lap_system_service()){} }
static int complete(int job, void *buf, uint32_t cap, uint32_t *next) {
    int32_t result=999; assert(job>0);
    assert(lap_storage_poll(job,buf,cap,&result,next)==0);
    ls_lap_system_service();
    assert(lap_storage_poll(job,buf,cap,&result,next)==1);
    assert(lap_storage_poll(job,buf,cap,&result,next)==LAP_SYS_EINVAL);
    return result;
}
static void storage(void) {
    begin("storage");
    char path[128]="nested/test.txt", bad[32]="../other", empty[1]="", data[8]="abcdef", out[16]={0}; uint32_t next;
    assert(lap_stream_open(0,bad,LAP_FILE_READ)==LAP_SYS_EACCES);
    assert(lap_stream_open(0,(char *)1,LAP_FILE_READ)==LAP_SYS_EACCES);
    int h=lap_stream_open(0,path,LAP_FILE_READ|LAP_FILE_WRITE); assert(h>0);
    int j=lap_stream_write(h,data,6); data[0]='z';
    assert(lap_stream_seek(h,0)==LAP_SYS_EBUSY);
    assert(lap_stream_close(h)==LAP_SYS_EBUSY);
    assert(lap_stream_write(h,data,1)==LAP_SYS_EBUSY);
    assert(complete(j,NULL,0,&next)==6);
    assert(lap_stream_seek(h,0)==0);
    j=lap_stream_read(h,6); ls_lap_system_service(); int32_t r;
    assert(lap_storage_poll(j,out,2,&r,&next)==LAP_SYS_EINVAL); /* retry allowed */
    assert(lap_storage_poll(j,out,sizeof(out),&r,&next)==1&&r==6&&!memcmp(out,"abcdef",6));
    assert(lap_stream_seek(h,UINT64_MAX)==LAP_SYS_EINVAL);
    assert(lap_stream_close(h)==0);
    char name[40], full[256];
    for(int i=0;i<35;i++) { snprintf(full,sizeof(full),"lap-system-sd/apps_data/storage/files/f%02d",i); FILE *f=fopen(full,"wb"); assert(f); fclose(f); }
    unsigned count=0,cursor=0; lap_dir_entry_t rows[16];
    do { j=lap_dir_list(0,empty,cursor); int n=complete(j,rows,sizeof(rows),&next); assert(n>0&&n<=16); count+=n; cursor=next; } while(cursor);
    assert(count==36);
    strcpy(name,"absent"); h=lap_stream_open(0,name,LAP_FILE_READ); assert(h>0);
    j=lap_stream_read(h,10); assert(complete(j,out,sizeof(out),&next)==LAP_SYS_ENOENT); lap_stream_close(h);
    h=lap_stream_open(0,path,LAP_FILE_READ); j=lap_stream_read(h,6);
    assert(lap_storage_cancel(j)==0); assert(complete(j,out,sizeof(out),&next)==LAP_SYS_ECANCEL); lap_stream_close(h);
    j=lap_dir_list(0,empty,0); end();
    begin("replacement"); assert(lap_storage_poll(j,rows,sizeof(rows),&r,&next)==LAP_SYS_EINVAL);
    ls_lap_sys_host()->hardware=0; assert(lap_dir_list(0,empty,0)==LAP_SYS_EUNSUPPORTED); end();
}
static void notices(void) {
    begin("notifier");
    lap_notification_t n={.size=sizeof(n),.version=1,.data_len=4};
    strcpy(n.title,"REMINDER"); strcpy(n.body,"Tea is ready"); strcpy(n.actions[0],"OPEN"); strcpy(n.actions[1],"SNOOZE"); memcpy(n.data,"tea!",4);
    int id=lap_notify_post(&n); assert(id>0); assert(lap_notify_post(&n)==LAP_SYS_ELIMIT); end();
    assert(ls_lap_system_activate(id,1));
    begin("wrong-app"); lap_launch_data_t data; assert(lap_launch_data(&data)==0); end();
    begin("notifier"); assert(lap_launch_data(&data)==1&&data.action==1&&data.notification==(unsigned)id&&!memcmp(data.data,"tea!",4));
    assert(lap_launch_data(&data)==0&&!ls_lap_system_activate(id,0));
    n.data_len=129; assert(lap_notify_post(&n)==LAP_SYS_EINVAL);
    assert(lap_notify_dismiss(id)==LAP_SYS_ENOENT); end();
}
static void messaging(void) {
    char topic[32]="demo.temperature", invalid[32]="bad topic", body[8]="21.5";
    begin("reader"); assert(lap_ipc_subscribe(topic)==0&&lap_ipc_subscribe(topic)==0); assert(lap_ipc_subscribe(invalid)==LAP_SYS_EINVAL); end();
    begin("writer");
    for(int i=0;i<8;i++) assert(lap_ipc_publish(topic,42,body,5)==1);
    assert(lap_ipc_publish(topic,42,body,5)==LAP_SYS_ELIMIT);
    assert(lap_ipc_publish(topic,42,body,193)==LAP_SYS_EINVAL); end();
    begin("writer-two"); assert(lap_ipc_publish(topic,42,body,5)==0); end(); /* full -> drop */
    begin("reader"); lap_ipc_message_t m;
    assert(lap_ipc_next(&m)==1&&m.type==42&&m.dropped==1&&!strcmp(m.sender,"writer")&&!memcmp(m.data,body,5));
    assert(lap_ipc_unsubscribe(topic)==0&&lap_ipc_next(&m)==0); end();
}
static void picker(void) {
    MKDIR("lap-system-sd/shared"); FILE *f=fopen("lap-system-sd/shared/hello.txt","wb"); assert(f); fputs("hello",f); fclose(f);
    begin("picker");
    lap_file_picker_config_t cfg={.size=sizeof(cfg),.version=1,.flags=LAP_FILE_READ};
    strcpy(cfg.title,"OPEN FILE"); int p=lap_file_picker(&cfg); assert(p>0);
    assert(lap_file_picker_poll(p)==0); ls_lap_system_frame(); ls_lap_system_service(); ls_lap_system_frame();
    assert(lap_kit_seen.picker_rows>=5); lap_kit_pick(4); /* shared */
    ls_lap_system_frame(); ls_lap_system_service(); ls_lap_system_frame(); lap_kit_pick(4); /* hello.txt */
    int scope=lap_file_picker_poll(p); assert(scope>0);
    char empty[1]="", bad[8]="escape"; int h=lap_stream_open(scope,empty,LAP_FILE_READ); assert(h>0);
    assert(lap_stream_open(scope,empty,LAP_FILE_READ|LAP_FILE_WRITE)==LAP_SYS_EACCES);
    assert(lap_stream_open(scope,bad,LAP_FILE_READ)==LAP_SYS_EACCES);
    assert(lap_file_scope_close(scope)==LAP_SYS_EBUSY);
    char out[8]={0}; uint32_t next; assert(complete(lap_stream_read(h,5),out,sizeof(out),&next)==5&&!memcmp(out,"hello",5));
    lap_stream_close(h); assert(lap_file_scope_close(scope)==0);
    p=lap_file_picker(&cfg); assert(p>0); ls_lap_system_script("pickcancel"); assert(lap_file_picker_poll(p)==LAP_SYS_ECANCEL);
    p=lap_file_picker(&cfg); assert(p>0); ls_lap_system_frame(); /* cancel during directory I/O */
    ls_lap_system_script("pickcancel"); assert(lap_file_picker_poll(p)==LAP_SYS_ECANCEL);
    while(ls_lap_system_service()){}
    assert(complete(lap_dir_list(0,empty,0),NULL,0,&next)==0); /* cancelled picker leaves no busy job */
    cfg.flags=LAP_FILE_READ|LAP_FILE_WRITE|LAP_FILE_FOLDER; p=lap_file_picker(&cfg); assert(p>0);
    ls_lap_system_frame(); ls_lap_system_service(); ls_lap_system_frame(); lap_kit_pick(4);
    ls_lap_system_frame(); ls_lap_system_service(); ls_lap_system_frame(); lap_kit_pick(2);
    scope=lap_file_picker_poll(p); assert(scope>0); h=lap_stream_open(scope,bad,LAP_FILE_READ|LAP_FILE_WRITE); assert(h>0);
    assert(complete(lap_stream_write(h,out,5),NULL,0,&next)==5); lap_stream_close(h); end();
    begin("picker"); assert(lap_stream_open(scope,empty,LAP_FILE_READ)==LAP_SYS_EACCES); end();
}
static void desktop(void) {
    begin("desktop"); lap_desktop_event_t e;
    assert(lap_desktop_next(&e)>0); while(lap_desktop_next(&e)>0){}
    assert(ls_lap_system_script("desktop=1")); assert(lap_desktop_next(&e)==1&&e.kind==LAP_DESKTOP_CONNECTED);
    ls_lap_lifecycle(LAP_INPUT_RESIZE,800,480); assert(lap_desktop_next(&e)==1&&e.width_px==800);
    ls_lap_lifecycle(LAP_INPUT_FOCUS_LOST,0,0); assert(lap_desktop_next(&e)==1&&!e.focused);
    for(int i=0;i<30;i++) ls_lap_system_input(LAP_INPUT_RESIZE,800+i,480);
    assert(lap_desktop_next(&e)==1&&e.dropped==13);
    char url[64]="https://example.com", name[16]="out.txt";
    assert(lap_desktop_open_url(url)==LAP_SYS_EUNSUPPORTED);
    assert(lap_desktop_save_file(0,name,name)==LAP_SYS_EUNSUPPORTED);
    assert(lap_desktop_info(&e)==0&&e.connected&&e.width_px==829);
    end();
}
static void desktop_requests(void) {
    begin("desktop-export");
    char url[]="https://example.com", outname[]="out.bin", big[]="big.bin", absent[]="absent", badpath[]="../bad", badname[]="C:\\out.bin";
    assert(lap_desktop_features()==0);
    assert(lap_desktop_open_url("http://example.com")==LAP_SYS_EINVAL);
    assert(lap_desktop_open_url("https://example.com/a b")==LAP_SYS_EINVAL);
    assert(ls_lap_system_script("desktopcaps=3,1500"));
    ls_lap_system_frame(); lap_desktop_event_t info; lap_desktop_info(&info);
    assert(lap_desktop_features()==3&&info.scale_milli==1500);
    int id=lap_desktop_open_url(url); assert(id>0);
    assert(lap_desktop_request_status(id)==0);
    assert(lap_desktop_open_url(url)==LAP_SYS_EBUSY);
    char line[900]; assert(ls_desk_host_line(line,sizeof(line))>0&&strstr(line,"68747470733a2f2f"));
    assert(!ls_desk_host_line(line,sizeof(line)));
    ls_desk_host_reply(id+1,1); assert(!lap_desktop_request_status(id));
    ls_desk_host_reply(id,1); assert(lap_desktop_request_status(id)==1);
    assert(lap_desktop_request_status(id)==LAP_SYS_EINVAL);
    id=lap_desktop_open_url(url); assert(id>0);
    assert(!lap_desktop_request_cancel(id)); assert(lap_desktop_request_status(id)==LAP_SYS_ECANCEL);
    assert(ls_desk_host_line(line,sizeof(line))>0&&strstr(line,"deskcancel"));
    char path[]="binary.dat"; uint32_t next;
    int h=lap_stream_open(0,path,LAP_FILE_READ|LAP_FILE_WRITE); assert(h>0);
    unsigned char bytes[256]; for(int i=0;i<256;i++) bytes[i]=i;
    assert(complete(lap_stream_write(h,bytes,sizeof(bytes)),NULL,0,&next)==sizeof(bytes)); lap_stream_close(h);
    assert(lap_desktop_save_file(0,badpath,outname)==LAP_SYS_EINVAL);
    assert(lap_desktop_save_file(0,path,badname)==LAP_SYS_EINVAL);
    id=lap_desktop_save_file(0,path,outname); assert(id>0);
    assert(!ls_desk_host_line(line,sizeof(line))); /* worker has not read */
    assert(!lap_desktop_request_status(id)); ls_lap_system_service(); assert(!lap_desktop_request_status(id));
    int chunks=0; unsigned offset=0;
    while(ls_desk_host_line(line,sizeof(line))) {
        int rid,op; unsigned at,total; char name[513],hex[193];
        assert(sscanf(line,"term: desk1 %d %d %u %u %512s %192s",&rid,&op,&at,&total,name,hex)==6);
        assert(rid==id&&op==2&&at==offset&&total==256);
        for(unsigned i=0;i<strlen(hex)/2;i++) { unsigned byte; assert(sscanf(hex+2*i,"%2x",&byte)==1&&byte==bytes[offset+i]); }
        offset+=strlen(hex)/2; chunks++;
    }
    assert(chunks==3&&offset==256);
    ls_lap_system_script("desktopresult=-8"); assert(lap_desktop_request_status(id)==LAP_SYS_ECANCEL);
    while(ls_desk_host_line(line,sizeof(line))) {}
    id=lap_desktop_save_file(0,absent,outname); assert(id>0); ls_lap_system_service();
    assert(lap_desktop_request_status(id)==LAP_SYS_ENOENT);
    /* Exact cap and cap+1, including binary data and EOF. */
    FILE *f=fopen("lap-system-sd/apps_data/desktop-export/files/big.bin","wb"); assert(f);
    for(int i=0;i<256;i++) { assert(fwrite(bytes,1,256,f)==256); }
    fclose(f);
    id=lap_desktop_save_file(0,big,big); assert(id>0); ls_lap_system_service();
    assert(!lap_desktop_request_status(id)); assert(!lap_desktop_request_cancel(id)); assert(lap_desktop_request_status(id)==LAP_SYS_ECANCEL);
    f=fopen("lap-system-sd/apps_data/desktop-export/files/big.bin","ab"); assert(f); fputc(1,f); fclose(f);
    id=lap_desktop_save_file(0,big,big); assert(id>0); ls_lap_system_service();
    assert(lap_desktop_request_status(id)==LAP_SYS_ELIMIT);
    id=lap_desktop_open_url(url); assert(id>0); ls_lap_system_script("desktop=0");
    assert(lap_desktop_request_status(id)==LAP_SYS_ECANCEL&&lap_desktop_features()==0);
    ls_lap_system_script("desktopcaps=1,2000"); id=lap_desktop_open_url(url); assert(id>0);
    end(); begin("different-app"); assert(lap_desktop_request_status(id)==LAP_SYS_EINVAL); end();
    ls_desk_host_disconnect();
}
static void isolation(void) {
    ls_lap_instance_t *a=ls_lap_instance_current(),*b=ls_lap_instance_new(); assert(b);
    begin("instance-a"); void *p=ls_lap_malloc(80); assert(p&&ls_lap_api_bytes()>=80);
    size_t allocated=ls_lap_api_bytes();
    char name[32]="isolation.txt";
    int stream=lap_stream_open(0,name,LAP_FILE_READ); assert(stream>0);
    lap_app_t app={.api_major=2,.api_minor=5,.flags=LAP_CAP_SERVICE};
    ls_lap_service_begin(&app);
    ls_lap_service_set_shown(false);
    ls_lap_input_key(LAP_KEY_LEFT,0); assert(lap_key_down(LAP_KEY_LEFT));
    assert(!ls_lap_instance_select(b)); ls_lap_api_callback(false); assert(ls_lap_instance_select(b));
    begin("instance-b"); assert(!ls_lap_api_owns(p,1)&&ls_lap_api_bytes()==0&&!lap_key_down(LAP_KEY_LEFT));
    assert(lap_stream_close(stream)==LAP_SYS_EINVAL&&lap_screen_open());
    char topic[32]="contexts"; assert(lap_ipc_subscribe(topic)==0); end();
    assert(ls_lap_instance_select(a)); ls_lap_api_callback(true);
    assert(ls_lap_api_owns(p,80)&&ls_lap_api_bytes()==allocated&&lap_key_down(LAP_KEY_LEFT));
    assert(lap_stream_close(stream)==0&&!lap_screen_open()); ls_lap_free(p); end();
    assert(ls_lap_instance_delete(b));
}
int main(void) {
    MKDIR("lap-system-sd");
    assert(lap_api_version()==0x20007);
    storage(); notices(); messaging(); picker(); desktop(); desktop_requests(); isolation();
    puts("system API 2.5 contracts PASS"); return 0;
}
