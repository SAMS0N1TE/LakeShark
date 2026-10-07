#include <stdlib.h>
/* Deterministic receive-only flight history for the real TUI renderer. */
#include "experiments/rs41_store.h"
#include "ls_compass_live.h"
#include "ls_route_live.h"
#include "esp_timer.h"
#include <stdio.h>
#include <math.h>
void lssim_seed_rs41(void)
{
    rs41_store_clear();
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < 96; ++i) for (int slot = 0; slot < 3; ++slot) {
        rs41_report_t r = {.status=true,.position=true,.battery=3.1f,.sats=9};
        snprintf(r.serial,sizeof(r.serial),"N123456%d",slot);
        r.frame = (uint16_t)(400+i);
        r.lat = 43.448+0.001*slot+0.00004*i; r.lon = -71.656+0.002*slot+0.00006*i;
        r.alt = 200+30000*sin(i*3.141592653589793/110); r.climb = i < 55 ? 5.2f : -8.1f;
        rs41_store_put(&r,now-(95-i)*1000000LL-1);
    }
    /* An empty list on a running receiver, for the hint it shows. */
    if (getenv("LSSIM_RS41_EMPTY")) rs41_store_clear();
}

void lssim_report_rs41_target(void)
{
    char name[32]; double lat,lon;
    if (ls_compass_target(&lat,&lon,name,sizeof(name)))
        printf("SONDE TARGET %s %.6f %.6f / ROUTE %s\n",name,lat,lon,ls_route_live()->valid ? "on" : "off");
}
