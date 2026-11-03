#include "lakeshark_app.h"
#include <stdio.h>
static uint32_t due;
static int notice, desktop_request;
static lap_launch_data_t activation;
static lap_desktop_event_t desktop;
static char status[64]="Reminder in one second";
static void tick(void) {
    if(desktop_request>0) { int r=lap_desktop_request_status(desktop_request); if(r) { desktop_request=0; snprintf(status,sizeof(status),"Desktop result: %d",r); } }
    if (lap_launch_data(&activation)>0) {
        if (activation.action==1) { due=lap_millis()+5000; snprintf(status,sizeof(status),"Snoozed for five seconds"); }
        else snprintf(status,sizeof(status),"Reminder opened: %.*s",activation.data_len,activation.data);
    }
    if (due&&(int32_t)(lap_millis()-due)>=0) {
        lap_notification_t n={.size=sizeof(n),.version=1,.data_len=3};
        snprintf(n.title,sizeof(n.title),"REMINDER"); snprintf(n.body,sizeof(n.body),"Time for a break");
        snprintf(n.actions[0],16,"OPEN"); snprintf(n.actions[1],16,"SNOOZE");
        n.data[0]='t'; n.data[1]='e'; n.data[2]='a';
        notice=lap_notify_post(&n); due=0;
        snprintf(status,sizeof(status),notice>0?"Posted: close screen, tap notice":"Notice queue full; press R to retry");
    }
}
static void enter(void) {
    if (!lap_service_running()) {
        due=lap_millis()+1000; lap_service_t svc={.tick=tick,.period_ms=100}; lap_service_start(&svc);
    }
    lap_desktop_info(&desktop); tick();
}
static void draw(void) {
    tick(); while(lap_desktop_next(&desktop)>0){}
    lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,1,"REMINDER",LAP_ATTR(LAP_CYAN|LAP_BRIGHT,LAP_BLACK));
    lap_put_str(1,3,status,LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,5,"R schedule  D dismiss  U website  P PC save",LAP_ATTR(LAP_YELLOW,LAP_BLACK));
    lap_put_str(1,7,"Keeps ticking after its screen closes.",LAP_ATTR(LAP_WHITE,LAP_BLACK));
}
static bool key(lap_key_t k,char ch) {
    (void)k;
    if (ch=='r'||ch=='R') { due=lap_millis()+1500; return true; }
    if (ch=='d'||ch=='D') { if(notice>0) lap_notify_dismiss(notice); notice=0; return true; }
    if (ch=='u'||ch=='U') { int r=lap_desktop_open_url("https://lakeshark.org"); desktop_request=r>0?r:0; snprintf(status,sizeof(status),"Viewer URL request: %d",r); return true; }
    if (ch=='p'||ch=='P') { int r=lap_desktop_save_file(0,"reminder.txt","reminder.txt"); desktop_request=r>0?r:0; snprintf(status,sizeof(status),"PC save request: %d",r); return true; }
    return false;
}
LAP_APP(.id="reminder",.name="REMINDER",.sub="Actionable background notices",.version="1.0",.author="LakeShark",
        .icon=LAP_ICON_GEAR,.color=LAP_CYAN,.flags=LAP_CAP_SERVICE,.enter=enter,.draw=draw,.key=key);
