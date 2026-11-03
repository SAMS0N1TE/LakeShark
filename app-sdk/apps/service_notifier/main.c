#include "lakeshark_app.h"
#include <stdio.h>
#include <string.h>
static unsigned received;
static bool started;
static void tick(void) {
    lap_ipc_message_t msg;
    for(int i=0;i<4&&lap_ipc_next(&msg)==1;i++) received++;
    if(received&&!(received%4)) {
        lap_log("notifier received %u",received);
        lap_notification_t n={.size=sizeof(n),.version=1};
        strcpy(n.title,"LOGGER ACTIVITY"); snprintf(n.body,sizeof(n.body),"%lu records received",(unsigned long)received);
        int id=lap_notify_post(&n);
        if(id>0) lap_notify_dismiss(id); /* bounded demo; no retained notification quota */
    }
}
static void enter(void) {
    if(started) return;
    lap_caps_t caps; if(lap_caps(&caps,sizeof(caps))) return;
    lap_ipc_subscribe("demo.logger");
    lap_service_t svc={.tick=tick,.period_ms=500}; started=lap_service_start(&svc)==0;
}
static void draw(void) {
    lap_service_info_t logger; int found=lap_service_query("service_logger",&logger);
    char row[80]; lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    snprintf(row,sizeof(row),"NOTIFIER: %lu records; logger %s",(unsigned long)received,found==1?"running":"absent");
    lap_put_str(1,2,row,LAP_ATTR(LAP_GREEN,LAP_BLACK));
    lap_put_str(1,4,"Both services keep running when another app opens.",LAP_ATTR(LAP_WHITE,LAP_BLACK));
}
LAP_APP(.id="service_notifier",.name="NOTIFIER",.sub="resident IPC notifications",.version="1.0",.author="LakeShark",
    .icon=LAP_ICON_STAR,.color=LAP_GREEN,.flags=LAP_CAP_SERVICE,.enter=enter,.draw=draw);
