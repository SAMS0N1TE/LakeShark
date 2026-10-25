/* Allocation and configuration/law regressions beyond the gameplay cases. */
#include "internal.h"
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include "adsb_state.h"
#include <assert.h>
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
static unsigned allocations;
void *__wrap_malloc(size_t n){allocations++;return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t s){allocations++;return __real_calloc(n,s);}
void *__wrap_realloc(void *p,size_t n){allocations++;return __real_realloc(p,n);}
int main(void) {
    adsb_state_init();ls_lap_api_begin("carrier-limits",1,0);ls_lap_sys_begin();ls_lap_api_callback(true);assert(game_init());
    l3_scene *scene=l3_create(52,49);assert(scene);game_render(scene);
    unsigned start=allocations;
    for(int i=0;i<100;i++){game_update(.04f);game_render(scene);}
    assert(allocations==start);
    cg->v.player.docked=false;cg->v.player.position=l3v(0,0,-65);cg->v.player.velocity=l3v(0,0,0);cg->v.player.throttle=0;
    cg_actions();game_context();for(int i=0;i<150&&!cg->v.player.docked;i++)game_update(.04f);
    assert(cg->v.player.docked&&allocations==start);
    /* An innocent ship retaliating cannot become a source of legal bounties. */
    game_contact *c=&cg->v.contacts[16];c->active=true;c->hostile=false;c->ship_class=SH_ADDER;c->hull=1;c->shield=0;
    unsigned credits=cg->v.player.credits;cg_damage(16,10,cg->v.player.position,true);
    assert(cg->v.player.credits==credits&&cg->v.player.legal>=20);
    /* A syntactically plausible file with an unsupported version is rejected. */
    game_shutdown();int fd=lap_file_open("rules.json","w");assert(fd>=0);lap_file_write(fd,"{\"version\":2}",13);lap_file_close(fd);
    assert(game_init()&&!cg->v.config_valid&&game_ship(SH_COBRA)->hull>0);
    game_stick(NAN,INFINITY);game_throttle(NAN);game_update(NAN);assert(isfinite(cg->v.player.pitch)&&isfinite(cg->v.player.roll));
    printf("limits: zero allocations in 100 frames and docking; schema rejection, finite inputs, innocent kill law passed; state %u bytes\n",(unsigned)sizeof(*cg));
    l3_destroy(scene);game_shutdown();ls_lap_api_callback(false);ls_lap_sys_end();ls_lap_api_cleanup();return 0;
}
