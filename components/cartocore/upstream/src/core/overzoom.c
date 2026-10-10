#include "cartocore/core.h"
#include <limits.h>
static int scale(int32_t *v,int factor,int32_t remainder) {
    int64_t n=(int64_t)*v*factor-remainder;
    if(n<INT32_MIN || n>INT32_MAX) return 0;
    *v=(int32_t)n; return 1;
}
int cc_scene_overzoom(cc_scene *s,unsigned shift,int32_t rx,int32_t ry,cc_arena *a) {
    if(shift>16) return 0;
    int factor=1<<shift;
    for(int k=0;k<16;k++) for(cc_feature *f=s->first[k];f;f=f->next) {
        for(size_t j=0;j<f->count;j++) {
            cc_path *p=f->paths+j; cc_point *points=cc_arena_array(a,p->count,sizeof(*points),_Alignof(cc_point));
            if(!points) return 0;
            for(size_t i=0;i<p->count;i++) {
                points[i]=cc_feature_point(f,p,i);
                if(!scale(&points[i].x,factor,rx) || !scale(&points[i].y,factor,ry)) return 0;
            }
            p->points=points; p->xs=p->ys=0; p->ox=p->oy=0;
        }
        f->path_offset=(cc_point){0,0}; f->bounds_valid=0; f->raster=0;
        if(!scale(&f->anchor.x,factor,rx) || !scale(&f->anchor.y,factor,ry)) return 0;
    }
    for(cc_terrain *t=s->terrain;t;t=t->next) {
        if(!scale(&t->ox,factor,rx) || !scale(&t->oy,factor,ry)) return 0;
        if(t->clipped) for(unsigned k=0;k<4;k++)
            if(!scale(&t->clip[k],factor,(k&1)?ry:rx)) return 0;
        if(t->zoom_shift+shift>16) return 0;
        t->zoom_shift+=(uint8_t)shift;
    }
    return 1;
}
