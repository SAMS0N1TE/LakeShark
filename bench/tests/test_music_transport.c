#include "ls_test.h"
#include "ls_music_transport.h"
#include <limits.h>

LS_CASE(empty_and_invalid_selection) {
    LS_EQ_INT(ls_music_step(0,0,1),-1);
    LS_EQ_INT(ls_music_step(-5,3,0),0);
    LS_EQ_INT(ls_music_step(99,3,1),1);
    LS_EQ_INT(ls_music_next(-1,3,false,true),-1);
}
LS_CASE(wrap_without_signed_overflow) {
    LS_EQ_INT(ls_music_step(0,3,INT_MIN),2);
    LS_EQ_INT(ls_music_step(2,3,INT_MAX),0);
    LS_EQ_INT(ls_music_step(INT_MAX-1,INT_MAX,1),0);
    LS_EQ_INT(ls_music_step(0,1,-1),0);
}
LS_CASE(scroll_keeps_every_selection_visible) {
    for(int count=1;count<50;count++)for(int rows=1;rows<20;rows++)for(int sel=0;sel<count;sel++){
        int top=ls_music_scroll(sel,count,rows,INT_MAX);
        LS_CHECK(top>=0 && top<=sel && sel-top<rows);
        LS_CHECK(top<=(count>rows?count-rows:0));
    }
    LS_EQ_INT(ls_music_scroll(3,4,0,0),0);
}
LS_CASE(end_and_repeat) {
    LS_EQ_INT(ls_music_next(2,3,false,false),-1);
    LS_EQ_INT(ls_music_next(2,3,false,true),0);
    LS_EQ_INT(ls_music_next(2,3,true,true),2);
    LS_EQ_INT(ls_music_next(1,3,false,false),2);
}
