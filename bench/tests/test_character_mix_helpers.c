/* Real cell-grid regressions: clipping, endpoints, braille encoding, strokes. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "ls_braille.h"
#include "ls_glyph.h"
#include "ls_tui_ui.h"

/* These UI services must never be reached by the stateless drawing helpers. */
int64_t esp_timer_get_time(void) { assert(0); return 0; }
int ls_tui_corner_pad(int row) { (void)row; assert(0); return 0; }
bool ls_tui_keyboard_mode(void) { assert(0); return false; }

int main(void)
{
    tui_cell back[80], front[80];
    tui_surface sf;
    tui_surface_setup(&sf,back,front,16,5);
    tui_rect clip=tui_rect_make(2,1,12,3);
    const float levels[]={-1,0,.5f,1,2,NAN};
    for(unsigned k=0;k<sizeof(levels)/sizeof(levels[0]);++k) {
        tui_frame_begin(&sf);
        ls_meter_ascii(&sf,clip,2,2,12,levels[k],TUI_GREEN);
        assert(back[2*16+2].ch=='[' && back[2*16+13].ch==']');
        int lit=0;
        for(int x=3;x<13;++x) {
            assert(back[2*16+x].ch<128);
            if(back[2*16+x].attr==TUI_ATTR(TUI_GREEN,TUI_BLACK))++lit;
        }
        assert(lit==(k==2?5:k==3||k==4?10:0));
        for(int y=0;y<5;++y)for(int x=0;x<16;++x)
            if(y!=2 || x<2 || x>13)assert(back[y*16+x].ch==' ');
    }
    tui_frame_begin(&sf);
    ls_meter_ascii(&sf,clip,0,2,16,.5f,TUI_CYAN);
    assert(back[32].ch==' ' && back[47].ch==' ');
    ls_meter_ascii(&sf,clip,2,2,2,1,TUI_RED);
    assert(back[34].attr!=TUI_ATTR(TUI_RED,TUI_BLACK));
    const uint8_t left[]={0,0x40,0x44,0x46,0x47};
    const uint8_t right[]={0,0x80,0xa0,0xb0,0xb8};
    for(unsigned h=0;h<=4;++h) {
        assert(ls_braille_column(h,0)==left[h]);
        assert(ls_braille_column(h,1)==right[h]);
    }
    assert(ls_braille_column(99,0)==0x47);
    tui_frame_begin(&sf);
    ls_braille_put(&sf,clip,2,1,0x28ff,TUI_CYAN);
    ls_braille_put(&sf,clip,-1,1,0x28ff,TUI_CYAN);
    assert(back[18].ch==0x28ff && back[16].ch==' ');
    assert(ls_braille_pixel(0x28ff,1,1,8,16));
    assert(!ls_braille_pixel(0x2800,1,1,8,16));
    for(const char *p="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.";*p;++p) {
        tui_frame_begin(&sf);
        ls_glyph_draw_line(&sf,tui_rect_make(0,0,3,5),*p,TUI_YELLOW);
        unsigned strokes=0;
        for(int i=0;i<80;++i) {
            assert(back[i].ch==' ' || back[i].ch=='+' || back[i].ch=='-' || back[i].ch=='|' || back[i].ch=='.');
            if(back[i].ch!=' ')++strokes;
        }
        assert(strokes);
    }
    puts("PASS ASCII clamps/ticks/clipping, 2x4 braille cells, 37 stroke figures");
}
