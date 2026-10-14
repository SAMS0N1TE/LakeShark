/* Deterministic SURVEY worker snapshots for host rendering; no radio or SD. */
#include "ls_survey.h"
#include <stdlib.h>
#include "esp_timer.h"
#include <string.h>
static ls_survey_view_t model;
static bool seeded, event_sent;
static int64_t seeded_us;
static void seed(void)
{
    if (seeded) return;
    seeded = true; seeded_us = esp_timer_get_time();
    const char *state = getenv("LSSIM_SURVEY");
    if (!state) state = "idle";
    model.options = (ls_survey_options_t){true,true,true,10,10};
    model.sd_known = true; model.free_bytes = 3900ULL * 1024 * 1024;
    if (!strcmp(state,"idle")) return;
    model.running = strcmp(state,"summary") != 0;
    model.busy = !strcmp(state,"busy");
    model.gps_fresh = model.utc_valid = strcmp(state,"wait") != 0;
    model.wifi = 1284; model.ble = 367; model.new_minute = 42;
    model.wifi_minute = 31; model.ble_minute = 11;
    model.rows = 2438; model.cached = 12; model.elapsed_s = 913;
    model.activity_revision = 28;
    model.security[0]=113; model.security[1]=7; model.security[2]=64;
    model.security[3]=918; model.security[4]=182;
    model.top_channel[0]=6; model.top_count[0]=412;
    model.top_channel[1]=1; model.top_count[1]=306;
    model.top_channel[2]=11; model.top_count[2]=228;
    snprintf(model.path,sizeof(model.path),"/sdcard/survey/20261014-081500.csv");
    snprintf(model.status,sizeof(model.status),"%s", model.busy ? "Closing session / flushing rows" : model.running ? "Passive survey / recording observations" : "Session saved");
}
bool ls_survey_run(bool start, const ls_survey_options_t *options)
{
    seed(); if (model.busy || model.running == start) return false;
    model.running = start; model.options = *options;
    snprintf(model.status,sizeof(model.status),"%s",start ? "Passive survey / waiting for observations" : "Session saved");
    return true;
}
void ls_survey_view(ls_survey_view_t *out)
{
    seed();
    /* One injected observation, never a repeating animation. */
    if(getenv("LSSIM_SURVEY_EVENT") && model.running && !event_sent &&
       esp_timer_get_time()-seeded_us>=300000) {
        event_sent=true; model.activity_revision++; model.wifi++; model.rows++;
    }
    *out = model;
}
bool ls_survey_at(unsigned rank, int sort, ls_survey_entry_t *out)
{
    seed(); if(rank >= model.cached) return false;
    memset(out,0,sizeof(*out));
    unsigned i = sort == 1 ? model.cached - 1 - rank : rank;
    out->kind = i % 3 == 2 ? LS_SURVEY_BLE : LS_SURVEY_WIFI;
    snprintf(out->name,sizeof(out->name),"%s %02u",out->kind == LS_SURVEY_WIFI ? "Lakeside AP" : "Trail beacon",i+1);
    out->addr[0]=2; out->addr[4]=0xa1; out->addr[5]=i;
    out->rssi=-38-(int)i*4; out->channel=1+i%11; out->auth=3;
    out->positioned=true; out->lat=43.4445; out->lon=-71.6473;
    out->samples=12+i; out->first_s=1791965700; out->last_s=1791966613; out->epoch=true;
    return true;
}
