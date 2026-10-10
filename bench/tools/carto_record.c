/* Feed the exact firmware cell conversion through lssim's real P4 blitter.
 * Usage: carto_record MINI.ctile OUT.lsrec COLS ROWS LANDSCAPE Z */
#include "cartocore/render.h"
#include "ls_cartocore_cells.h"
#include "ls_theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc!=7) return 2;
    int cols=atoi(argv[3]),rows=atoi(argv[4]),landscape=atoi(argv[5]),z=atoi(argv[6]);
    if (cols<1 || cols>240 || rows<1 || rows>100 || z<13 || z>15) return 2;
    FILE *f=fopen(argv[1],"rb"); if (!f) return 1;
    fseek(f,0,SEEK_END); long length=ftell(f); rewind(f);
    if (length<=0) { fclose(f); return 1; }
    void *data=malloc(length),*memory=malloc(2*1024*1024);
    cc_cell *cells=malloc((size_t)cols*rows*sizeof(*cells));
    if (!data || !memory || !cells) return 1;
    if (fread(data,1,length,f)!=(size_t)length) return 1;
    fclose(f);
    cc_ctile map; cc_arena arena; cc_renderer renderer;
    cc_arena_init(&arena,memory,2*1024*1024);
    if (!cc_ctile_open((cc_str){data,length},&map) || !cc_renderer_init(&renderer,&arena,cols,rows,cells)) return 1;
    renderer.colors16=1; renderer.edges_mode=CC_EDGES_CRISP;
    double pi=3.14159265358979323846,scale=256.0*(1u<<z);
    int64_t left=(int64_t)llround((-71.6473+180)/360*scale)-cols;
    int64_t top=(int64_t)llround((1-log(tan(pi/4+43.4445*pi/360))/pi)/2*scale)-rows*2;
    left-=left%2; top-=top%4;
    if (!cc_render(&renderer,&map,z,left,top,CC_QUADRANT,1,cells)) return 1;
    f=fopen(argv[2],"wb"); if (!f) return 1;
    fprintf(f,"LSREC1 %d %d %d 0 Terminal_Bay 0\n",cols,rows,landscape);
    for (int i=0;i<cols*rows;i++) {
        unsigned char pair[2]={(unsigned char)glyph(cells[i].codepoint),
            TUI_ATTR(carto_colour(cells[i].fg),carto_colour(cells[i].bg))};
        if (fwrite(pair,1,2,f)!=2) return 1;
    }
    int result=fclose(f)!=0;
    printf("CartoCore z%d %dx%d arena peak=%zu -> %s\n",z,cols,rows,arena.peak,argv[2]);
    free(cells); free(memory); free(data);
    return result;
}
