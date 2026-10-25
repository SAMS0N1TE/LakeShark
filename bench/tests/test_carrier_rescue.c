/* Timed supplies transfer and UI purchase quotes use the actual systems. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <assert.h>
int main(void) {
    adsb_state_init();ls_lap_api_begin("carrier-rescue",1,0);ls_lap_sys_begin();ls_lap_api_callback(true);assert(game_init());
    game_equipment_offer offer;assert(game_equipment_info(EQ_CARGO,&offer)&&offer.available&&offer.price==3000&&!offer.owned);
    assert(game_equip(EQ_CARGO));assert(game_equipment_info(EQ_CARGO,&offer)&&offer.owned&&!offer.available);
    cg->v.player.hull-=10;cg->v.player.fuel=6;assert(game_repair_cost()==80&&game_refuel_cost()==12);
    game_player *p=&cg->v.player;p->docked=false;p->position=l3v(0,0,1100);p->velocity=l3v(0,0,0);p->throttle=0;p->cargo[0]=1;
    game_contact *c=&cg->v.contacts[0];memset(c,0,sizeof(*c));c->active=true;c->id=0xF00000;c->ship_class=SH_ADDER;c->role=ROLE_RESCUE;c->hull=100;c->position=l3v(0,0,1060);
    cg->desired[0]=c->position;cg->poll=-60;cg->v.selected=0;cg_contacts();cg_actions();assert(cg->v.action==ACT_RESCUE);
    unsigned credits=p->credits;game_context();for(int i=0;i<120;i++)game_update(.04f);assert(p->cargo[0]==1&&p->credits==credits&&cg->v.rescue_progress>0);
    for(int i=0;i<10;i++)game_update(.04f);assert(p->cargo[0]==0&&p->credits==credits+500&&c->role==ROLE_TRADER&&cg->rescued[0]);
    for(int i=0;i<150;i++)game_update(.04f);assert(p->credits==credits+500);
    c->role=ROLE_RESCUE;p->cargo[0]=1;cg->v.selected=0;cg_contacts();game_context();game_update(.04f);c->position=l3v(0,0,500);game_update(.04f);assert(cg->rescue_target==-1&&p->cargo[0]==1&&cg->v.rescue_progress==0);
    puts("rescue: 9 checks passed: purchase quotes, five-second proximity transfer, single award, interruption");
    game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
