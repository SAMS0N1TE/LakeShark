/* Host evidence setups, never linked into the .lap. Subsequent motion, shots,
 * docking and jumps use the production game systems and ordinary inputs. */
#include "../../app-sdk/apps/carrier/internal.h"
#include "../../app-sdk/apps/carrier/ui.h"
#include <stdlib.h>
void carrier_preview(const char *command)
{
    if (!cg) return;
    game_view *v = &cg->v;
    game_player *p = &v->player;
    if (!strcmp(command, "launch")) {
        p->docked = true; cg_actions(); game_context();
    } else if (!strcmp(command, "cruise")) {
        p->docked = p->docking = false;
        p->position = l3v(0, 0, 1000); p->attitude = (l3_quat){0,0,0,1};
        p->velocity = l3v(0,0,-50); game_throttle(.6f);
        for (int i=0; i<16; i++) if (v->contacts[i].active) {
            l3_vec at=l3_add(p->position,l3v((i%5-2)*52,(i%3-1)*28,-160-i*13));
            v->contacts[i].position=cg->desired[i]=at;
        }
        cg->poll=-60;
    } else if (!strcmp(command, "target") || !strcmp(command, "combat")) {
        bool combat=!strcmp(command,"combat");
        p->docked=p->docking=false;p->position=l3v(0,0,1000);p->attitude=(l3_quat){0,0,0,1};
        p->velocity=l3v(0,0,0);p->throttle=0;
        game_contact *c=&v->contacts[0]; memset(c,0,sizeof(*c));
        c->active=true;c->id=0xF00000;c->ship_class=SH_COBRA;c->faction=6;
        c->role=combat?ROLE_PIRATE:ROLE_TRADER;c->hostile=combat;
        c->attitude=l3_quat_euler(25,-8,12);c->position=l3v(0,0,combat?920:958);
        c->hull=combat?45:130;c->shield=combat?0:100;strcpy(c->callsign,"DEMO01");
        cg->desired[0]=c->position;cg->poll=-60;cg->contact_age[0]=0;
        v->selected=0;
    } else if (!strcmp(command, "dock")) {
        p->docked=p->docking=false;p->position=l3v(55,10,-280);p->velocity=l3v(0,0,0);
        p->attitude=l3_quat_euler(180,0,0);p->throttle=0;cg_actions();game_context();
    } else if (!strcmp(command, "jump")) {
        p->docked=p->docking=false;p->position=l3v(0,0,1200);p->attitude=(l3_quat){0,0,0,1};
        p->velocity=l3v(0,0,0);p->throttle=0;p->fuel=7;
        cg_flight(0);
        for(int i=0;i<GAME_SYSTEMS;i++)if(game_plot_jump(i)) {
            l3_vec direction;
            if(game_jump_direction(&direction))
                p->attitude=l3_quat_euler(atan2f(-direction.x,-direction.z)/CG_RAD,
                    asinf(cg_clamp(direction.y,-1,1))/CG_RAD,0);
            cg_actions();game_context();break;
        }
    } else if (!strcmp(command, "dogfight")) {     /* footage: a pirate pair coming at you */
        p->docked=p->docking=false;p->position=l3v(0,0,1500);p->attitude=(l3_quat){0,0,0,1};
        p->velocity=l3v(0,0,0);p->throttle=0;
        p->shield_fore=p->shield_aft=100;p->hull=game_player_ship()->hull;
        memset(v->contacts,0,sizeof(v->contacts));memset(cg->dead_until,0,sizeof(cg->dead_until));
        static const game_class cls[2]={SH_MORAY,SH_KRAIT};
        static const char *const nm[2]={"Red Tooth","Gill Knife"};   /* a Hammerhead and a Tiger */
        for(int i=0;i<2;i++) {
            game_contact *c=&v->contacts[i];
            c->active=true;c->id=0xD06F00+i;c->ship_class=cls[i];c->faction=2+i;c->role=ROLE_PIRATE;c->hostile=true;
            c->position=l3v(i?-45.f:35.f,i?-14.f:10.f,i?1385.f:1410.f);
            c->attitude=l3_quat_euler(i?160.f:200.f,5,i?-20.f:25.f);
            c->hull=260;c->shield=140;c->speed=55;
            snprintf(c->name,sizeof c->name,"%s",nm[i]);strcpy(c->callsign,i?"GK-2":"RT-1");
            cg->desired[i]=c->position;cg->contact_age[i]=0;
        }
        v->poi_count=0;cg->poll=-600;v->selected=0;
    } else if (!strcmp(command, "aim") || !strcmp(command, "aimfar")) {   /* footage: a pilot's hand keeping the nose on the target */
        /* the pirates weave in front instead of ramming: a scripted strafing path
           (aimfar: the same, twelve times further out, for the smart zoom) */
        const float far=command[3]=='f'?12.f:1.f;
        static const float rad0[2]={34,80}, dist0[2]={58,120}, ph[2]={0,2.1f};
        const float rad[2]={rad0[0]*far,rad0[1]*far}, dist[2]={dist0[0]*far,dist0[1]*far};
        for(int i=0;i<2;i++) {
            game_contact *c=&v->contacts[i];
            if(!c->active || c->id!=0xD06F00u+(uint32_t)i) continue;
            float tt=v->clock*.55f+ph[i];
            l3_vec at=l3_add(p->position,l3v(rad[i]*sinf(tt),rad[i]*.35f*sinf(tt*1.6f),-dist[i]+rad[i]*.3f*cosf(tt)));
            l3_vec vel=l3_scale(l3_sub(at,c->position),25);
            if(cg_len(vel)>1) {
                l3_vec d=l3_norm(vel);
                c->attitude=l3_quat_euler(atan2f(-d.x,-d.z)/CG_RAD,asinf(cg_clamp(d.y,-1,1))/CG_RAD,-35*sinf(tt));
            }
            c->velocity=vel;c->position=at;cg->desired[i]=at;c->speed=cg_len(vel);
        }
        p->heat=0;p->energy=100;          /* footage: the guns never overheat */
        int t=v->selected;
        static int last=-1; static uint32_t last_id; static l3_vec last_at; static float held=9;
        /* the game moves the target on the moment one dies: notice by id, and stay on the wreck */
        if(last>=0 && (!v->contacts[last].active || v->contacts[last].id!=last_id) && held>=9) held=0;
        if(held<2.2f) {                             /* stay on the wreck a moment: watch it burn */
            static float was_clock; held+=fminf(.1f,fmaxf(0,v->clock-was_clock)); was_clock=v->clock;
            l3_vec d=l3_norm(l3_sub(last_at,p->position));
            p->attitude=l3_quat_slerp(p->attitude,l3_quat_euler(atan2f(-d.x,-d.z)/CG_RAD,asinf(d.y)/CG_RAD,0),.1f);
            t=-2;
        }
        else if(t>=0 && v->contacts[t].active) { last=t; last_id=v->contacts[t].id; last_at=v->contacts[t].position; held=9; }
        if(held<9 && held>=2.2f) { held=9; last=-1; }
        if(t==-2) {}
        else if(t>=0 && v->contacts[t].active) {
            l3_vec d=l3_norm(l3_sub(v->contacts[t].position,p->position));
            float yaw=atan2f(-d.x,-d.z)/CG_RAD, pitch=asinf(d.y)/CG_RAD;
            p->attitude=l3_quat_slerp(p->attitude,l3_quat_euler(yaw,pitch,8*sinf(v->clock*.7f)),.22f);
        } else {
            for(int i=0;i<GAME_CONTACTS;i++) if(v->contacts[i].active && v->contacts[i].hostile) {v->selected=i;break;}
        }
    } else if (!strcmp(command, "hurt")) {         /* footage: a badly damaged ship */
        p->hull=game_player_ship()->hull*.16f;p->shield_fore=p->shield_aft=0;
    } else if (!strcmp(command, "dbgw")) {
        for(int m=0;m<GAME_MOUNTS;m++) printf("mount %d kind %d group %d firing %d heat %.2f\n",m,p->weapons[m].kind,p->weapons[m].group,p->weapons[m].firing,p->weapons[m].heat);
        { extern ui_state ui; printf("mounts %d firing %d cap %.2f latch %d cruising %d down %d grab %d\n",p->mounts,p->firing,p->weapon_capacitor,ui.fire_latch,p->cruising,ui.down,ui.grab); }
    } else if (!strcmp(command, "slotout")) {     /* half way out along the slot */
        ui.launch_t = UI_LAUNCH_S * .5f;
    } else if (!strcmp(command, "slotin")) {      /* half way in */
        ui.dock_t = UI_DOCK_S * .5f;
    } else if (!strcmp(command, "gfar")) {        /* GALAXY: choose a system some 25 ly off */
        int best = 0; float bd = 1e9f;
        for (int i = 0; i < GAME_SYSTEMS; i++) { float d = fabsf(game_distance(&v->systems[v->system], &v->systems[i]) - 25); if (d < bd) { bd = d; best = i; } }
        ui.galaxy_pick = best;
    } else if (!strcmp(command, "chase")) {       /* the VIEW placard: chase camera */
        ui.view_mode = UI_VIEW_CHASE;
    } else if (!strcmp(command, "tactical")) {    /* and the tactical plot */
        ui.view_mode = UI_VIEW_TACTICAL;
    } else if (!strcmp(command, "walk") || !strcmp(command, "walkhangar")) {   /* on foot, aboard or on the hangar deck */
        p->docked = true; p->docking = false;
        ui_walk_start();
        if (command[4] == 'h') walk_enter(&ui.walk, WALK_HANGAR, &ui.walk_ship);
    } else if (!strcmp(command, "wstats")) {      /* the walk view's engine cost for the last frame */
        extern int walk_drawn; extern l3_scene *walk_scene_now(void); l3_stats_t s; l3_stats(walk_scene_now(), &s);
        { int w, h, clear = 0, black = 0; l3_size(walk_scene_now(), &w, &h); const uint8_t *d = l3_dots(walk_scene_now());
          for (int i = 0; i < w * h; i++) { clear += d[i] == LAP_DOT_CLEAR; black += d[i] == LAP_BLACK; }
          printf("wdots %dx%d clear %d black %d\n", w, h, clear, black); }
        printf("wstats exp total %u raster %u shade %u resolve %u boxes %d tris %u shapes %u dots %u reused %u cores %u\n",
               (unsigned)s.us_total, (unsigned)s.us_raster, (unsigned)s.us_shade, (unsigned)s.us_resolve, walk_drawn,
               (unsigned)s.triangles, (unsigned)s.shapes, (unsigned)s.dots_shaded, (unsigned)s.dots_reused, (unsigned)s.cores);
    } else if (!strncmp(command, "nose", 4)) {    /* noseP,R: pitch P and roll R degrees, the station below */
        float pd = 0, rd = 0;
        sscanf(command + 4, "%f,%f", &pd, &rd);
        p->docked = p->docking = false; p->position = l3_add(v->contents.station, l3v(0, 250, 0)); p->velocity = l3v(0, 0, 0); p->throttle = 0;
        p->attitude = l3_quat_euler(0, pd, rd);
        ui.view_mode = UI_VIEW_COCKPIT;
    } else if (!strcmp(command, "keys")) {        /* the keyboard card, as ? opens it */
        ui.keys_help = true;
    } else if (!strncmp(command, "turn", 4)) {    /* turnN: yaw the ship N degrees (a world to the side) */
        p->attitude = l3_quat_mul(p->attitude, l3_quat_euler((float)atof(command + 4), 0, 0));
        p->velocity = l3v(0, 0, 0); p->throttle = 0;
    } else if (!strncmp(command, "panel", 5)) {   /* panelN: 1 STATUS 2 MARKET 3 GALAXY 4 LOG; marketN picks its tab */
        game_panel_open((game_panel)atoi(command + 5));
    } else if (!strncmp(command, "mtab", 4)) {
        ui.market_tab = atoi(command + 4);
    } else if (!strcmp(command, "wuse")) {        /* USE what is in reach */
        ui_walk_key(LAP_KEY_ENTER, 0);
    } else if (!strcmp(command, "wstop")) {       /* back in the seat */
        ui_walk_stop();
    } else if (!strncmp(command, "wyaw", 4)) {    /* wyawN: face N degrees */
        ui.walk.yaw = (float)atof(command + 4);
    } else if (!strcmp(command, "wdump")) {       /* the walk layout and the walker */
        const walk_box *b; int n = walk_boxes(&b);
        printf("wdump place %d feet %.2f %.2f %.2f yaw %.1f pitch %.1f boxes %d\n", ui.walk.place, ui.walk.feet.x,
               ui.walk.feet.y, ui.walk.feet.z, ui.walk.yaw, ui.walk.pitch, n);
        for (int i = 0; i < n; i++)
            printf("wdump box %2d kind %2d solid %d c %6.2f %6.2f %6.2f half %6.2f %6.2f %6.2f\n", i, b[i].kind, b[i].solid,
                   b[i].c.x, b[i].c.y, b[i].c.z, b[i].half.x, b[i].half.y, b[i].half.z);
    } else if (!strncmp(command, "wpitch", 6)) {  /* wpitchN: look N degrees up (negative: down) */
        ui.walk.pitch = (float)atof(command + 6);
    } else if (!strncmp(command, "wspot", 5)) {   /* wspotN: stand a pace from spot N, facing it */
        const walk_spot *sp; int n = walk_spots(&sp), i = atoi(command + 5);
        if (i >= 0 && i < n) {
            l3_vec d = l3_sub(sp[i].p, ui.walk.feet); d.y = 0;
            float len = sqrtf(l3_dot(d, d));
            if (len > .01f) {
                float back = fminf(sp[i].reach * .6f, len);
                ui.walk.feet = l3_add(ui.walk.feet, l3_scale(d, (len - back) / len));
                ui.walk.yaw = atan2f(d.x, -d.z) / CG_RAD;
                ui.walk.pitch = 0;
                walk_step(&ui.walk, 0, 0, 0, 0, .01f);
            }
        }
    } else if (!strcmp(command, "notips")) {      /* footage: the first-flight tips are done */
        extern ui_state ui;
        ui.tips = 3;
    } else if (!strncmp(command, "poi", 3)) {     /* poiN: find a system with kind N, park facing it */
        int want=atoi(command+3), found=-1;
        for(int system=0;system<GAME_SYSTEMS && found<0;system++) {
            cg_arrive(system);
            for(int i=0;i<v->poi_count;i++) if((int)v->pois[i].kind==want){found=i;break;}
        }
        if(found>=0) {
            game_poi *q=&v->pois[found];
            float back=q->kind==POI_NEBULA?q->radius*2.2f:q->kind==POI_WRECK?q->size*1.6f:q->size*3.5f+60;
            l3_vec eye=l3_add(q->position,l3v(back*.35f,back*.25f,back));
            l3_vec f=l3_norm(l3_sub(q->position,eye));
            p->docked=p->docking=false;p->position=eye;
            p->attitude=l3_quat_euler(atan2f(-f.x,-f.z)/CG_RAD,asinf(f.y)/CG_RAD,0);
            p->throttle=0;p->velocity=l3v(0,0,0);q->discovered=true;
        }
        printf("carrier poi %d -> %d\n", want, found);
    } else if (!strcmp(command, "approach")) {   /* low over a world, nose a little down, sinking */
        game_planet *b=&v->contents.planets[0];
        l3_vec out=l3_norm(l3_sub(v->contents.star,b->position));
        l3_vec eye=l3_add(b->position,l3_scale(out,b->radius+420));
        l3_vec side=l3_norm(l3_cross(out,l3v(0,1,0)));
        l3_vec fwd=l3_norm(l3_add(side,l3_scale(out,-.8f)));
        float yaw=atan2f(-fwd.x,-fwd.z)/CG_RAD, pitch=asinf(fwd.y)/CG_RAD;
        l3_quat q=l3_quat_euler(yaw,pitch,0);
        l3_vec u0=l3_quat_rotate(q,l3v(0,1,0)), want=l3_norm(l3_sub(out,l3_scale(fwd,l3_dot(out,fwd))));
        float roll=atan2f(l3_dot(l3_cross(u0,want),fwd),l3_dot(u0,want))/CG_RAD;
        q=l3_quat_mul(q,l3_quat_axis(l3v(0,0,-1),roll));
        p->docked=p->docking=false;p->position=eye;p->attitude=q;
        p->throttle=.3f;p->velocity=l3_scale(fwd,40);
    } else if (!strcmp(command, "planet")) {   /* the sunlit side of the first world, for looks */
        game_planet *b=&v->contents.planets[0];
        l3_vec to_star=l3_norm(l3_sub(v->contents.star,b->position));
        l3_vec side=l3_norm(l3_cross(to_star,l3v(0,1,0)));
        l3_vec eye=l3_add(b->position,l3_scale(l3_norm(l3_add(to_star,l3_scale(side,.8f))),b->radius*3.2f));
        l3_vec f=l3_norm(l3_sub(b->position,eye));
        float yaw=atan2f(-f.x,-f.z)/CG_RAD, pitch=asinf(f.y)/CG_RAD;
        p->docked=p->docking=false;p->position=eye;p->attitude=l3_quat_euler(yaw,pitch,0);
        p->throttle=0;p->velocity=l3v(0,0,0);
    } else if (!strcmp(command, "rings")) {
        int planet=-1;
        for(int system=0;system<GAME_SYSTEMS&&planet<0;system++) {
            cg_arrive(system);
            for(int i=0;i<v->contents.planet_count;i++)if(v->contents.planets[i].rings){planet=i;break;}
        }
        if(planet>=0) {
            game_planet *b=&v->contents.planets[planet];
            p->position=l3_add(b->position,l3v(0,b->radius*.75f,b->radius*3));
            p->attitude=l3_quat_euler(0,-14,0);p->throttle=0;p->velocity=l3v(0,0,0);
        }
    }
    { extern ui_state ui; ui.was_docked = cg->v.player.docked; ui.launch_t = 0; }   /* a teleport is not a launch */
    cg_contacts();cg_actions();
    int w=0,h=0;if(cg->scene)l3_size(cg->scene,&w,&h);
    int beams=0,debris=0;
    for(int i=0;i<CG_BEAMS;i++)beams+=cg->beams[i].life>0;
    for(int i=0;i<CG_DEBRIS;i++)debris+=cg->debris[i].life>0;
    printf("carrier %s: viewport %dx%d, system %d, position %.1f %.1f %.1f, contacts %d, jump %d, docked %d, kills %u, beams %d, debris %d\n",
        command,w/2,h/3+9,v->system,p->position.x,p->position.y,p->position.z,v->contact_count,v->jump,p->docked,(unsigned)p->kills,beams,debris);
    if(!strcmp(command,"att")) {    /* where the nose and the roof point */
        l3_vec f=l3_quat_rotate(p->attitude,l3v(0,0,-1)),u=l3_quat_rotate(p->attitude,l3v(0,1,0));
        printf("carrier att: fwd %.2f %.2f %.2f up %.2f %.2f %.2f throttle %.2f roll %.2f yaw %.2f pitch %.2f\n",
               f.x,f.y,f.z,u.x,u.y,u.z,p->throttle,p->roll,p->yaw,p->pitch);
    }
}
