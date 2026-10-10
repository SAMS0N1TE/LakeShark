/* Clip a decoded ownership region before merging into the shared scene.
 * Geometry stays vector geometry; labels are laid out once after the merge. */
static int inside(cc_point p,unsigned edge,int32_t bound) {
    int32_t v=(edge&1)?p.y:p.x;return edge<2?v>=bound:v<=bound;
}
static cc_point intersection(cc_point a,cc_point b,unsigned edge,int32_t bound) {
    int64_t av=(edge&1)?a.y:a.x,bv=(edge&1)?b.y:b.x;
    if(bv==av) return a;
    if(edge&1) return (cc_point){a.x+(int32_t)(((int64_t)b.x-a.x)*(bound-av)/(bv-av)),bound};
    return (cc_point){bound,a.y+(int32_t)(((int64_t)b.y-a.y)*(bound-av)/(bv-av))};
}
static int clip_scene(cc_scene *s,int32_t x0,int32_t y0,int32_t x1,int32_t y1,cc_arena *a) {
    /* Vector edges reach the boundary; subtracting a source pixel here
     * opens a visible gap after overview overzoom. Terrain/anchors are half-open. */
    int32_t bounds[]={x0,y0,x1,y1};
    for(unsigned k=0;k<16;k++) for(cc_feature *f=s->first[k];f;f=f->next) {
        if(f->anchor.x<x0 || f->anchor.x>=x1 || f->anchor.y<y0 || f->anchor.y>=y1) f->name.size=0;
        int32_t fx0=INT32_MAX,fy0=INT32_MAX,fx1=INT32_MIN,fy1=INT32_MIN;
        for(size_t j=0;j<f->count;j++)for(size_t i=0;i<f->paths[j].count;i++) {
            cc_point q=cc_feature_point(f,f->paths+j,i);
            if(q.x<fx0)fx0=q.x;
            if(q.y<fy0)fy0=q.y;
            if(q.x>fx1)fx1=q.x;
            if(q.y>fy1)fy1=q.y;
        }
        if(fx1<x0 || fy1<y0 || fx0>=x1 || fy0>=y1) { f->count=0;f->raster=0;continue; }
        if(fx0>=x0 && fy0>=y0 && fx1<x1 && fy1<y1)continue;
        size_t count=0;for(size_t j=0;j<f->count;j++) count+=f->paths[j].closed?1:f->paths[j].count;
        cc_path *paths=cc_arena_array(a,count,sizeof(*paths),_Alignof(cc_path));if(count && !paths) return 0;
        size_t used=0;
        for(size_t j=0;j<f->count;j++) {
            cc_path *p=f->paths+j;
            if(!p->count)continue;
            cc_point first=cc_feature_point(f,p,0);int32_t minx=first.x,maxx=first.x,miny=first.y,maxy=first.y;
            for(size_t i=1;i<p->count;i++) { cc_point q=cc_feature_point(f,p,i);
                if(q.x<minx)minx=q.x;
                if(q.x>maxx)maxx=q.x;
                if(q.y<miny)miny=q.y;
                if(q.y>maxy)maxy=q.y;
            }
            if(maxx<x0 || maxy<y0 || minx>=x1 || miny>=y1)continue;
            if(minx>=x0 && miny>=y0 && maxx<x1 && maxy<y1) { paths[used++]=*p;continue; }
            if(p->closed) {
                /* A rectangle adds at most two intersections per original edge.
                 * Temporary clipping storage is reclaimed for every ring. */
                size_t scratch=a->used,cap=p->count*4+16;
                cc_point *in=cc_arena_array(a,cap,sizeof(*in),_Alignof(cc_point));
                cc_point *out=cc_arena_array(a,cap,sizeof(*out),_Alignof(cc_point));if(!in || !out) return 0;
                cc_point *compact=in;size_t compact_at=(size_t)((uint8_t*)compact-(uint8_t*)a->data);
                size_t n=p->count;for(size_t i=0;i<n;i++) in[i]=cc_feature_point(f,p,i);
                for(unsigned edge=0;edge<4 && n;edge++) {
                    size_t m=0;cc_point prev=in[n-1];int pin=inside(prev,edge,bounds[edge]);
                    for(size_t i=0;i<n;i++) {
                        cc_point cur=in[i];int cin=inside(cur,edge,bounds[edge]);
                        if(pin!=cin) { if(m==cap) return 0;out[m++]=intersection(prev,cur,edge,bounds[edge]); }
                        if(cin) { if(m==cap) return 0;out[m++]=cur; }
                        prev=cur;pin=cin;
                    }
                    cc_point *tmp=in;in=out;out=tmp;n=m;
                }
                /* Compact into the start of scratch before rewinding it. */
                if(n>=3) {
                    memmove(compact,in,n*sizeof(*in));a->used=compact_at+n*sizeof(*in);
                    paths[used++]=(cc_path){compact,n,1,0,0,0,0};
                } else a->used=scratch;
            } else for(size_t i=1;i<p->count;i++) {
                cc_point u=cc_feature_point(f,p,i-1),v=cc_feature_point(f,p,i);
                u.x-=x0;u.y-=y0;v.x-=x0;v.y-=y0;
                if(!cc_clip_line(x1-x0+1,y1-y0+1,&u,&v)) continue;
                cc_point *points=cc_arena_array(a,2,sizeof(*points),_Alignof(cc_point));if(!points) return 0;
                points[0]=(cc_point){u.x+x0,u.y+y0};points[1]=(cc_point){v.x+x0,v.y+y0};
                paths[used++]=(cc_path){points,2,0,0,0,0,0};
            }
        }
        f->paths=paths;f->count=used;f->raster=0;f->path_offset=(cc_point){0,0};f->bounds_valid=0;
    }
    for(cc_terrain *t=s->terrain;t;t=t->next) {
        t->clipped=1;t->clip[0]=x0;t->clip[1]=y0;t->clip[2]=x1;t->clip[3]=y1;
    }
    return 1;
}
static void merge_scene(cc_scene *dst,cc_scene *src) {
    for(unsigned k=0;k<16;k++) {
        if(src->first[k]) {
            if(dst->last[k]) dst->last[k]->next=src->first[k];else dst->first[k]=src->first[k];
            dst->last[k]=src->last[k];
        }
        for(unsigned rank=0;rank<6;rank++) if(src->label_first[rank][k]) {
            if(dst->label_last[rank][k]) dst->label_last[rank][k]->label_next=src->label_first[rank][k];
            else dst->label_first[rank][k]=src->label_first[rank][k];
            dst->label_last[rank][k]=src->label_last[rank][k];
        }
    }
    if(src->terrain) { cc_terrain *t=src->terrain;while(t->next) t=t->next;t->next=dst->terrain;dst->terrain=src->terrain; }
    dst->features+=src->features;dst->points+=src->points;dst->labels_indexed=1;
}
