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
/* Shift persistent planes. Terrain interpolation is refreshed in full.
   Cell alignment is required: an odd-dot move changes polygon sample phase. */
static void save_pixel_state(cc_renderer *r,int dx,int dy) {
    cc_planes *p=&r->planes;
    for(int y=0;y<p->rows;y++) for(int x=0;x<p->cols;x++) {
        uint8_t *v=(uint8_t*)&r->saved[(size_t)y*p->cols+x];
        int sx=x+dx,sy=y+dy;
        if(sx<0 || sy<0 || sx>=p->cols || sy>=p->rows) {v[10]=0;continue;}
        size_t at=(size_t)sy*p->cols+sx;
        v[0]=p->dots[at];v[1]=p->ink[at];v[2]=p->dot_priority[at];
        v[3]=p->fill[at];v[4]=p->fill_priority[at];v[5]=p->coverage[at];
        v[6]=p->coverage_ink[at];v[7]=p->coverage_bg[at];v[8]=p->coverage_priority[at];
        v[9]=p->shade[at];v[10]=1;
    }
}
static void shift_fill(cc_planes *p,int dx,int dy) {
    uint8_t *v[]={p->fill,p->fill_priority,p->coverage,p->coverage_ink,p->coverage_bg,p->coverage_priority,p->dots,p->ink,p->dot_priority};
    int x0=dx<0?-dx:0,x1=dx>0?p->cols-dx:p->cols;
    int y0=dy<0?-dy:0,y1=dy>0?p->rows-dy:p->rows;
    for(unsigned k=0;k<sizeof(v)/sizeof(*v);k++)
        for(int j=0;j<y1-y0;j++) {
            int y=dy<0?y1-1-j:y0+j;
            memmove(v[k]+(size_t)y*p->cols+x0,v[k]+(size_t)(y+dy)*p->cols+x0+dx,(size_t)(x1-x0));
        }
    p->keep_x0=x0;p->keep_x1=x1;p->keep_y0=y0;p->keep_y1=y1;p->reuse_fill=1;
}
static void clipped_strokes(cc_planes *p,const cc_scene *scene,int dx,int dy) {
    if(p->native_unclipped_lines)return;
    int w=p->cols*2,h=p->rows*4;
    for(unsigned pr=0;pr<16;pr++)for(const cc_feature *f=scene->first[pr];f;f=f->next) {
        if(!f->style.line || f->type==1)continue;
        for(size_t k=0;k<f->count;k++) {
            const cc_path *path=&f->paths[k];
            for(size_t j=0;j<path->count;j++) {
                if(j+1==path->count && !path->closed)break;
                cc_point a=cc_path_point(path,j),b=cc_path_point(path,(j+1)%path->count);
                int x0=a.x<b.x?a.x:b.x,x1=a.x>b.x?a.x:b.x;
                int y0=a.y<b.y?a.y:b.y,y1=a.y>b.y?a.y:b.y;
                if(x1< -2 || y1< -2 || x0>w+1 || y0>h+1)continue;
                /* The two-dot halo includes bold parallel strokes and cell
                   foreground ownership, which is shared by all eight dots. */
                if(x0<0) {int64_t edge=((int64_t)x1-dx)/2+2;if(edge>p->keep_x0)p->keep_x0=edge>p->cols?p->cols:(int)edge;}
                if(x1>=w) {int64_t edge=((int64_t)x0-dx)/2-2;if(edge<p->keep_x1)p->keep_x1=edge<0?0:(int)edge;}
                if(y0<0) {int64_t edge=((int64_t)y1-dy)/4+2;if(edge>p->keep_y0)p->keep_y0=edge>p->rows?p->rows:(int)edge;}
                if(y1>=h) {int64_t edge=((int64_t)y0-dy)/4-2;if(edge<p->keep_y1)p->keep_y1=edge<0?0:(int)edge;}
            }
        }
    }
}
int cc_render(cc_renderer *r,const cc_ctile *map,unsigned z,int64_t left,int64_t top,
              cc_mode mode,int labels,cc_cell *cells) {
    r->pixel_reuse=0;
    cc_edges edges=cc_edges_resolve(r->edges_mode,mode);
    int changed=r->terrain_strength!=r->active_terrain_strength || r->colors16!=r->active_colors16 || edges!=r->active_edges || !r->valid || r->map!=map || r->z!=z || r->left!=left || r->top!=top;
    if(changed) {
        int64_t dx=(int64_t)((uint64_t)left-(uint64_t)r->left),dy=(int64_t)((uint64_t)top-(uint64_t)r->top);
        int reuse=!cells && !r->encoded && r->valid && r->map==map && r->z==z &&
            r->active_edges==edges && r->active_colors16==r->colors16 &&
            r->active_terrain_strength==r->terrain_strength &&
            dx%2==0 && dy%4==0 && dx>-r->planes.cols*2 && dx<r->planes.cols*2 &&
            dy>-r->planes.rows*4 && dy<r->planes.rows*4;
        r->pixel_reuse=reuse;r->pixel_dx=reuse?(int)dx:0;r->pixel_dy=reuse?(int)dy:0;
        if(reuse) {
            save_pixel_state(r,(int)(dx/2),(int)(dy/4));
            r->planes.keep_x0=dx<0?(int)(-dx/2):0;r->planes.keep_x1=dx>0?r->planes.cols-(int)(dx/2):r->planes.cols;
            r->planes.keep_y0=dy<0?(int)(-dy/4):0;r->planes.keep_y1=dy>0?r->planes.rows-(int)(dy/4):r->planes.rows;
            clipped_strokes(&r->planes,&r->scene,(int)dx,(int)dy);
        }
        int keep_x0=r->planes.keep_x0,keep_x1=r->planes.keep_x1;
        int keep_y0=r->planes.keep_y0,keep_y1=r->planes.keep_y1;
        r->planes.reuse_fill=0;
        r->valid=0;
        if(!r->scene_ready || r->map!=map || r->z!=z || r->left!=left || r->top!=top) {
            r->scene_ready=0; r->arena->used=r->mark;
            if(!cc_ctile_scene_cached(map,z,left,top,r->planes.cols,r->planes.rows,r->arena,&r->scene,r->decoded_cache)) return 0;
            r->scene_ready=1;
        }
        r->planes.colors16=r->active_colors16=r->colors16; r->planes.edges_mode=edges; r->active_edges=edges;
        r->planes.terrain_strength=r->active_terrain_strength=r->terrain_strength;
        r->planes.zoom=(uint8_t)(z>255?255:z);
        if(reuse) {
            shift_fill(&r->planes,(int)(dx/2),(int)(dy/4));
            r->planes.keep_x0=keep_x0;r->planes.keep_x1=keep_x1;
            r->planes.keep_y0=keep_y0;r->planes.keep_y1=keep_y1;
            clipped_strokes(&r->planes,&r->scene,0,0);
        }
        cc_raster(&r->planes,&r->scene);
        r->planes.reuse_fill=0;
    }
    if(!cells) {
        if(changed) r->encoded=0;
        r->map=map; r->z=z; r->left=left; r->top=top; r->mode=mode; r->valid=1;
        return 1;
    }
    if(changed || !r->encoded || r->mode!=mode || r->labels!=labels) {
        uint64_t stamp=cc_profiling?cc_ticks():0;
        encode_frame(r,mode);
        if(cc_profiling) { cc_profiling->ticks[CC_ENCODE]+=cc_ticks()-stamp; stamp=cc_ticks(); }
        if(labels) cc_labels(&r->planes,&r->scene,r->saved);
        if(cc_profiling) cc_profiling->ticks[CC_LABELS]+=cc_ticks()-stamp;
    }
    r->map=map; r->z=z; r->left=left; r->top=top; r->mode=mode; r->labels=labels; r->valid=1; r->encoded=1;
    if(cells!=r->saved) memcpy(cells,r->saved,(size_t)r->planes.cols*r->planes.rows*sizeof(*cells));
    return 1;
}
