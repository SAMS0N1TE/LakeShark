#include "cartocore/render.h"
#include "cartocore/profile.h"
#include "cartocore/arch.h"
#include <string.h>
int cc_renderer_init(cc_renderer *r,cc_arena *a,int cols,int rows,cc_cell *saved) {
    memset(r,0,sizeof(*r)); r->arena=a; r->saved=saved;
    cc_encoder_init(&r->encoder);
    if(!saved || !cc_planes_init(&r->planes,a,cols,rows)) return 0;
    r->mark=a->used; return 1;
}
size_t cc_renderer_hot_bytes(int cols,int rows) {
    if(cols<1 || rows<1 || cols>16384 || rows>16384) return 0;
    size_t n=(size_t)cols*rows,cap=1;
    while(cap<n/9+1) cap*=2;
    return 5*n+2*(((size_t)cols+CC_WORD_BITS)/CC_WORD_BITS)*rows*sizeof(cc_bitword)
           +cap*sizeof(cc_feature*)+(size_t)cols*sizeof(cc_cell)+4*15;
}
int cc_renderer_init_split(cc_renderer *r,cc_arena *hot,cc_arena *cold,int cols,int rows,cc_cell *saved) {
    if(hot==cold) return cc_renderer_init(r,cold,cols,rows,saved);
    size_t hm=hot->used,cm=cold->used;
    memset(r,0,sizeof(*r)); r->arena=cold; r->saved=saved;
    cc_encoder_init(&r->encoder);
    if(!saved || !cc_planes_init_split(&r->planes,hot,cold,cols,rows) ||
       !(r->output_row=cc_arena_array(hot,(size_t)cols,sizeof(cc_cell),16))) {
        hot->used=hm; cold->used=cm; return 0;
    }
    r->mark=cold->used; return 1;
}
void cc_renderer_invalidate(cc_renderer *r) { r->valid=0; r->scene_ready=0; }
void cc_renderer_reraster(cc_renderer *r) { r->valid=0; }
static void encode_frame(cc_renderer *r,cc_mode mode) {
    if(!r->output_row) { cc_encode(&r->encoder,&r->planes,mode,r->saved); return; }
    cc_encode_staged(&r->encoder,&r->planes,mode,r->saved,r->output_row);
}
int cc_render(cc_renderer *r,const cc_ctile *map,unsigned z,int64_t left,int64_t top,
              cc_mode mode,int labels,cc_cell *cells) {
    cc_edges edges=cc_edges_resolve(r->edges_mode,mode);
    int changed=r->terrain_strength!=r->active_terrain_strength || r->colors16!=r->active_colors16 || edges!=r->active_edges || !r->valid || r->map!=map || r->z!=z || r->left!=left || r->top!=top;
    if(changed) {
        r->valid=0;
        if(!r->scene_ready || r->map!=map || r->z!=z || r->left!=left || r->top!=top) {
            r->scene_ready=0; r->arena->used=r->mark;
            if(!cc_ctile_scene_cached(map,z,left,top,r->planes.cols,r->planes.rows,r->arena,&r->scene,r->decoded_cache)) return 0;
            r->scene_ready=1;
        }
        r->planes.colors16=r->active_colors16=r->colors16; r->planes.edges_mode=edges; r->active_edges=edges;
        r->planes.terrain_strength=r->active_terrain_strength=r->terrain_strength;
        cc_raster(&r->planes,&r->scene);
    }
    if(changed || r->mode!=mode || r->labels!=labels) {
        uint64_t stamp=cc_profiling?cc_ticks():0;
        encode_frame(r,mode);
        if(cc_profiling) { cc_profiling->ticks[CC_ENCODE]+=cc_ticks()-stamp; stamp=cc_ticks(); }
        if(labels) cc_labels(&r->planes,&r->scene,r->saved);
        if(cc_profiling) cc_profiling->ticks[CC_LABELS]+=cc_ticks()-stamp;
    }
    r->map=map; r->z=z; r->left=left; r->top=top; r->mode=mode; r->labels=labels; r->valid=1;
    if(cells!=r->saved) memcpy(cells,r->saved,(size_t)r->planes.cols*r->planes.rows*sizeof(*cells));
    return 1;
}
