/* Public ABI exercised against real receiver state, trails and demo producer. */
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include "adsb_demo.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
extern uint32_t (*ls_lap_api_host_clock)(void);
static uint32_t ms=120000;
static uint32_t clock_ms(void){return ms;}
static lap_aircraft_t out[16];
int main(void) {
    ls_lap_api_host_clock=clock_ms;adsb_state_init();ls_lap_api_begin("aircraft-test",1,ms);ls_lap_sys_begin();
    assert(lap_aircraft(out,16)==0);ls_lap_api_callback(true);
    assert(!lap_aircraft(NULL,1)&&!lap_aircraft(out,0)&&!lap_aircraft(out,-1));
    adsb_aircraft_t *a=adsb_state_find_or_create(0x123456);assert(a);a->active=true;
    memcpy(a->callsign,"TST12345",9);a->pos_valid=true;a->lat=43.1f;a->lon=-71;
    a->altitude=31400;a->altitude_valid=true;a->velocity=487;a->velocity_valid=true;
    a->heading=271;a->heading_valid=true;a->heading_is_track=true;a->vert_rate=-1400;a->vert_rate_valid=true;
    a->emitter_tc=4;a->emitter_ca=3;a->last_seen_us=118000000;
    adsb_state_set_squawk(a,7700);adsb_state_set_squawk(a,7700);
    adsb_state_set_emergency(a,3);adsb_state_set_emergency(a,3);
    assert(lap_aircraft(out,16)==1);assert(out[0].icao==0x123456&&!strcmp(out[0].callsign,"TST12345"));
    assert(out[0].has_pos&&out[0].has_alt&&out[0].has_speed&&out[0].has_track&&out[0].has_vrate);
    assert(out[0].alt_ft==31400&&out[0].speed_kt==487&&out[0].track_deg==271&&out[0].vrate_fpm==-1400);
    assert(out[0].age_ms==2000&&out[0].squawk==7700&&out[0].emergency==3&&!strcmp(out[0].category,"A3")&&out[0].range_km<0);
    ls_lap_sys_host_t *h=ls_lap_sys_host();h->gps=true;h->g.fix=true;h->g.lat=43;h->g.lon=-71;
    adsb_aircraft_t *b=adsb_state_find_or_create(0x234567);assert(b);b->active=true;b->pos_valid=true;b->lat=43.01f;b->lon=-71;b->emitter_tc=3;b->emitter_ca=6;
    adsb_aircraft_t *c=adsb_state_find_or_create(0x345678);assert(c);c->active=true;
    assert(lap_aircraft(out,1)==1&&out[0].icao==b->icao);
    assert(lap_aircraft(out,16)==3&&out[1].icao==a->icao&&out[2].range_km<0);
    assert(out[0].range_km>1&&out[0].range_km<1.2f&&fabsf(out[0].bearing_deg)<.01f&&!strcmp(out[0].category,"B6"));
    a->pos_ts_us=120000000;adsb_state_push_position(a);a->lat=43.2f;a->altitude=32000;a->pos_ts_us+=4000000;adsb_state_push_position(a);
    float lat[48],lon[48];int32_t alt[48];
    assert(lap_aircraft_track(a->icao,lat,lon,alt,48)==2&&lat[0]<lat[1]&&alt[0]==31400&&alt[1]==32000);
    assert(lap_aircraft_track(a->icao,NULL,lon,alt,48)==0&&lap_aircraft_track(0x999999,lat,lon,alt,48)==0);
    assert(lap_aircraft_track(a->icao,lat,lon,alt,0)==0);
    a->heading_is_track=false;a->emitter_tc=0;assert(lap_aircraft(out,16)==3&&!out[1].has_track&&!out[1].category[0]);
    adsb_state_init();lap_aircraft_demo(true);assert(adsb_demo_count()==8&&lap_aircraft(out,16)==8);
    double first=out[0].lat;ms+=1000;adsb_demo_tick();assert(lap_aircraft(out,16)==8&&fabs(out[0].lat-first)>1e-7);
    lap_aircraft_demo(false);assert(adsb_demo_count()==0&&lap_aircraft(out,16)==0);
    ls_lap_api_callback(false);assert(lap_aircraft(out,16)==0);lap_aircraft_demo(true);assert(!adsb_demo_count());
    ls_lap_api_cleanup();puts("test_lap_aircraft: 20 checks passed");return 0;
}
