#include "ls_test.h"
#include "../../components/apps/tui/ls_map_label_policy.h"

typedef struct { int obstacle; int x,y,w; } scene;
static bool free_slot(void *ctx,int x,int y,int w,int rows) {
    scene *s=ctx;(void)rows;
    if(s->obstacle && x>=30) return false;
    return !s->w || y!=s->y || x+w<=s->x || x>=s->x+s->w;
}
static bool choose(map_sticky_history *h,uint32_t id,int heading,int ax,int ay,
                   int64_t now,scene *s,int *x,int *y) {
    int spots[][2]={{ax+3,ay},{ax-10,ay},{ax-3,ay-3},{ax-3,ay+3}};
    return map_sticky_choose(h,map_object_key(1,id),map_air_label_side(heading),
        ax,ay,7,1,100,50,spots,2,4,now,free_slot,s,x,y);
}
LS_CASE(moving_aircraft_keeps_side_across_heading_quadrants_and_1hz_updates) {
    for(int orientation=0;orientation<2;orientation++) {
        map_sticky_history h={0};scene s={0};int x,y;
        for(int t=0;t<60;t++) {
            map_sticky_begin(&h);
            int ax=20+t/2,ay=10+orientation*10;
            LS_CHECK(choose(&h,0xabc123,260+t,ax,ay,t*1000000LL,&s,&x,&y));
            LS_EQ_INT(ax+3,x);LS_EQ_INT(ay,y);
            map_sticky_end(&h);
        }
    }
}
LS_CASE(continuous_collision_moves_once_after_two_seconds_and_holds_five) {
    map_sticky_history h={0};scene s={0};int x,y;
    LS_CHECK(choose(&h,1,270,30,10,0,&s,&x,&y));LS_EQ_INT(33,x);
    s.obstacle=1;
    LS_CHECK(!choose(&h,1,270,30,10,1000000,&s,&x,&y));
    LS_CHECK(!choose(&h,1,270,30,10,2999999,&s,&x,&y));
    LS_CHECK(choose(&h,1,270,30,10,3000000,&s,&x,&y));LS_EQ_INT(20,x);
    s.obstacle=0;s.x=20;s.y=10;s.w=7;
    for(int t=4;t<8;t++) LS_CHECK(!choose(&h,1,270,30,10,t*1000000LL,&s,&x,&y));
    LS_CHECK(choose(&h,1,270,30,10,8000000,&s,&x,&y));LS_EQ_INT(33,x);
    s.w=0;
    for(int t=9;t<20;t++) {
        LS_CHECK(choose(&h,1,270,30,10,t*1000000LL,&s,&x,&y));LS_EQ_INT(33,x);
    }
}
LS_CASE(intermittent_obstruction_resets_timer_and_never_changes_side) {
    map_sticky_history h={0};scene s={0};int x,y;
    LS_CHECK(choose(&h,1,270,30,10,0,&s,&x,&y));
    for(int t=1;t<20;t++) {
        s.obstacle=t%2;
        bool visible=choose(&h,1,270,30,10,t*1000000LL,&s,&x,&y);
        LS_EQ_INT(!s.obstacle,visible);if(visible) LS_EQ_INT(33,x);
    }
}
LS_CASE(clearing_the_original_collision_does_not_pull_the_label_back) {
    map_sticky_history h={0};scene s={0};int x,y;
    LS_CHECK(choose(&h,1,270,30,10,0,&s,&x,&y));
    s.obstacle=1;
    LS_CHECK(!choose(&h,1,270,30,10,1000000,&s,&x,&y));
    LS_CHECK(choose(&h,1,270,30,10,3000000,&s,&x,&y));LS_EQ_INT(20,x);
    s.obstacle=0;
    for(int t=4;t<30;t++) {
        LS_CHECK(choose(&h,1,270,30,10,t*1000000LL,&s,&x,&y));LS_EQ_INT(20,x);
    }
}
LS_CASE(two_converging_aircraft_do_not_ping_pong_even_when_snapshot_order_changes) {
    map_sticky_history h={0};int x,y,last[2]={-1,-1},changes[2]={0};
    for(int t=0;t<30;t++) {
        scene s={0};map_sticky_begin(&h);
        for(int j=0;j<2;j++) {
            int i=(j+t)%2,ax=i?55-t/3:25+t/3;
            if(choose(&h,i+1,i?90:270,ax,10,t*1000000LL,&s,&x,&y)) {
                int side=map_label_side(x-ax,y-10,7);
                if(last[i]>=0 && last[i]!=side) changes[i]++;
                last[i]=side;s.x=x;s.y=y;s.w=7;
            }
        }
        map_sticky_end(&h);
    }
    LS_CHECK(changes[0]<=1);LS_CHECK(changes[1]<=1);
}
LS_CASE(offscreen_overrides_dwell_and_absent_objects_are_evicted) {
    map_sticky_history h={0};scene s={0};int x,y;
    LS_CHECK(choose(&h,1,270,85,10,0,&s,&x,&y));
    LS_CHECK(choose(&h,1,270,97,10,1000000,&s,&x,&y));LS_EQ_INT(87,x);
    map_sticky_begin(&h);map_sticky_end(&h);
    for(int i=0;i<MAP_STICKY_MEMORY;i++) LS_CHECK(!h.at[i].key);
    LS_CHECK(choose(&h,1,90,40,10,2000000,&s,&x,&y));LS_EQ_INT(30,x);
}
LS_CASE(identity_domains_and_label_width_changes_preserve_left_attachment) {
    map_sticky_history h={0};scene s={0};int x,y;
    LS_CHECK(choose(&h,123,90,40,10,0,&s,&x,&y));LS_EQ_INT(30,x);
    int spots[][2]={{43,10},{27,10}};
    LS_CHECK(map_sticky_choose(&h,map_object_key(1,123),MAP_LABEL_RIGHT,
        40,10,10,1,100,50,spots,2,2,1000000,free_slot,&s,&x,&y));LS_EQ_INT(27,x);
    LS_CHECK(map_object_key(1,123)!=map_object_key(2,123));
}
LS_CASE(retention_preserves_an_offscreen_object_and_capacity_never_evicts_live_labels) {
    map_sticky_history h={0};scene s={0};int x,y;
    for(int i=0;i<MAP_STICKY_MEMORY;i++)
        LS_CHECK(choose(&h,i+1,270,30,10,0,&s,&x,&y));
    LS_CHECK(!choose(&h,1000,270,30,10,1000000,&s,&x,&y));
    map_sticky_begin(&h);
    map_sticky_retain(&h,map_object_key(1,1));
    map_sticky_end(&h);
    LS_CHECK(choose(&h,1,90,40,10,2000000,&s,&x,&y));LS_EQ_INT(43,x);
    LS_CHECK(choose(&h,1000,90,40,10,2000000,&s,&x,&y));LS_EQ_INT(30,x);
}
LS_CASE(cardinal_defaults_and_dead_bands_are_deterministic) {
    LS_EQ_INT(MAP_LABEL_RIGHT,map_air_label_side(270));
    LS_EQ_INT(MAP_LABEL_LEFT,map_air_label_side(90));
    LS_EQ_INT(MAP_LABEL_BELOW,map_air_label_side(0));
    LS_EQ_INT(MAP_LABEL_ABOVE,map_air_label_side(180));
    LS_EQ_INT(map_air_label_side(44),map_air_label_side(46));
    LS_EQ_INT(map_air_label_side(314),map_air_label_side(316));
    LS_EQ_INT(MAP_LABEL_RIGHT,map_air_label_side(-1));
}
