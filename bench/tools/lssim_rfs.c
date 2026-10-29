/* LSSIM_RFS=1: an RF SURVEY walked through the real runtime before the
   first frame, on the SIM source along the simulated lawnmower walk, so
   RF SURVEY and MAP have heat to draw; and three FOX HUNT bearings taken
   along that walk toward the pretend fox, so FOX HUNT and MAP have a fix.
   LSSIM_RFS=live leaves the survey running for the screen's own frames. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "ls_fox_run.h"
#include "ls_rfs_run.h"
#include "ls_rfsim.h"

void lssim_rfs_seed(void)
{
    const char *e = getenv("LSSIM_RFS");
    if (!e) return;
    const double lat0 = 43.4420, lon0 = -71.6520;   /* on the MAP fixture */
    ls_rfsim_walk(lat0, lon0, 90, 1.6f);
    ls_rfs_settings_t s;
    ls_rfs_settings_default(&s);
    s.src = LS_RFS_SRC_SIM;
    s.step_m = 8; s.cell_m = 25;
    ls_rfs_preset(0, &s.plan);
    if (!ls_rfs_start(&s)) { printf("lssim: rfsurvey would not start\n"); return; }
    const int steps = atoi(e) > 10 ? atoi(e) : 2600;
    for (int i = 0; i < steps; i++) { ls_rfs_step(); ls_shim_time_advance(500000); }
    if (strcmp(e, "live")) { ls_rfs_stop(); ls_rfs_step(); }
    ls_rfs_show(0, LS_RFS_MAX);

    /* Three bearings toward the fox, a few degrees out, from along the walk. */
    double flat, flon;
    ls_rfsim_fox_at(&flat, &flon);
    static const double FROM[3][2] = { { 0, -60 }, { 260, -40 }, { 80, 330 } };   /* metres east, north */
    static const float ERR[3] = { 2.5f, -3.0f, 1.5f };
    ls_fox_new();
    for (int i = 0; i < 3; i++) {
        ls_df_bearing_t b = { .spread = 9 + 2 * i, .level = -80 + 3 * i };
        b.lat = lat0 + FROM[i][1] / 111132.0;
        b.lon = lon0 + FROM[i][0] / (111132.0 * cos(lat0 * M_PI / 180));
        const double dx = (flon - b.lon) * 111132.0 * cos(lat0 * M_PI / 180), dy = (flat - b.lat) * 111132.0;
        b.bearing = (float)fmod(atan2(dx, dy) * 180 / M_PI + 360 + ERR[i], 360);
        snprintf(b.source, sizeof(b.source), "SIM 146.565");
        ls_fox_keep(&b);
    }
    ls_rfs_step();                         /* the fox log's card write */
}
