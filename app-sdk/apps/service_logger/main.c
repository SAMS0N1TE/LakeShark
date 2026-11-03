/* Run together with service_notifier, then open any foreground app.
   LAPSIM_SERVICES=service_logger,service_notifier LAPSIM=dice */
#include "lakeshark_app.h"
#include <stdio.h>
static unsigned sequence;
static bool started;
static void tick(void) {
    uint32_t record[2]={lap_millis(),++sequence};
    lap_ipc_publish("demo.logger",1,record,sizeof(record));
    if(!(sequence%4)) {
        lap_log("logger record %u",sequence);
        int fd=lap_file_open("logger.csv","a");
        if(fd>=0) {
            char row[64]; int n=snprintf(row,sizeof(row),"%lu,%lu\n",(unsigned long)record[0],(unsigned long)record[1]);
            lap_file_write(fd,row,(size_t)n); lap_file_close(fd);
        }
    }
}
static void enter(void) {
    if(started) return;
    lap_caps_t caps; if(lap_caps(&caps,sizeof(caps))) return;
    lap_service_t svc={.tick=tick,.period_ms=500};
    started=lap_service_start(&svc)==0;
}
static void draw(void) {
    char row[80]; lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_service_info_t services[LAP_SERVICES_MAX]; int n=lap_service_list(services,LAP_SERVICES_MAX);
    snprintf(row,sizeof(row),"LOGGER: %lu records; %d / %d services",(unsigned long)sequence,n,lap_service_limit());
    lap_put_str(1,2,row,LAP_ATTR(LAP_CYAN,LAP_BLACK));
    lap_put_str(1,4,"Close screen, launch NOTIFIER, then another app.",LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,6,"Stop individually in SETTINGS > SERVICES.",LAP_ATTR(LAP_WHITE,LAP_BLACK));
}
LAP_APP(.id="service_logger",.name="LOGGER",.sub="resident CSV and IPC logger",.version="1.0",.author="LakeShark",
    .icon=LAP_ICON_STAR,.color=LAP_CYAN,.flags=LAP_CAP_SERVICE,.enter=enter,.draw=draw);
