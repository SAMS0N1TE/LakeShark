#include "cartocore/arch.h"
#include "cartocore/cache.h"
#include <string.h>
#include "cartocore/profile.h"
#include "cartocore/simd.h"
struct cc_cache_entry {
    unsigned filled; int building;
    uint64_t age; unsigned z; int64_t x,y; cc_mode mode; uint32_t style;
    cc_scene scene; cc_cell cells[128*64];
    uint8_t planes[10][128*64];
};
static cc_cache_entry *entry(cc_cell_cache *c,size_t i) {
    return (cc_cache_entry*)(c->storage+i*CC_CACHE_SLOT_BYTES);
}
static int translate(cc_point *p,int64_t x,int64_t y) {
    int64_t a=(int64_t)p->x+x,b=(int64_t)p->y+y;
    if(a<INT32_MIN || a>INT32_MAX || b<INT32_MIN || b>INT32_MAX) return 0;
    *p=(cc_point){(int32_t)a,(int32_t)b}; return 1;
}
static int move_feature(cc_feature *f,int64_t x,int64_t y) {
    return translate(&f->anchor,x,y) && translate(&f->bounds_min,x,y) &&
        translate(&f->bounds_max,x,y) && translate(&f->path_offset,x,y);
}
int cc_cell_cache_init(cc_cell_cache *c,void *memory,size_t budget) {
    cc_arch_clear(c,sizeof(*c));
    if(!memory) return 0;
    size_t pad=(size_t)(0-(uintptr_t)memory)&(_Alignof(cc_cache_entry)-1);
    if(pad>budget || budget-pad<CC_CACHE_SLOT_BYTES) return 0;
    cc_encoder_init(&c->encoder); c->storage=(unsigned char*)memory+pad; c->slots=(budget-pad)/CC_CACHE_SLOT_BYTES;
    if(c->slots>1) { c->slots--; c->snapshot=(cc_cell*)(c->storage+c->slots*CC_CACHE_SLOT_BYTES); }
    cc_cell_cache_clear(c); return 1;
}
void cc_cell_cache_clear(cc_cell_cache *c) {
    for(size_t i=0;i<c->slots;i++) { entry(c,i)->age=0; entry(c,i)->building=0; }
    c->serial=0; c->hits=c->misses=c->evictions=0; c->source=0;
    c->frame_valid=0;
    c->scene_valid=0; c->pending=0;
}
static int matches(const cc_cache_entry *e,unsigned z,int64_t x,int64_t y,cc_mode mode,uint32_t style) {
    return e->z==z && e->x==x && e->y==y && e->mode==mode && e->style==style;
}
static cc_cache_entry *find(cc_cell_cache *c,unsigned z,int64_t x,int64_t y,cc_mode mode,uint32_t style) {
    for(size_t i=0;i<c->slots;i++) {
        cc_cache_entry *e=entry(c,i);
        if(e->age && matches(e,z,x,y,mode,style)) return e;
    }
    return 0;
}
static cc_cache_entry *fill(cc_cell_cache *c,const cc_ctile *map,unsigned z,int64_t x,int64_t y,
                         cc_mode mode,uint32_t style,cc_arena *scratch,unsigned rows,size_t *work) {
    cc_cache_entry *e=0; size_t mark=scratch->used;
    for(size_t i=0;i<c->slots;i++) {
        cc_cache_entry *q=entry(c,i);
        if((q->age || q->building) && matches(q,z,x,y,mode,style)) { e=q; break; }
        if(!e || q->age<e->age) e=q;
    }
    if(!e) return 0;
    if(e->age && matches(e,z,x,y,mode,style)) { e->age=++c->serial; c->hits++; return e; }
    if(!e->building || !matches(e,z,x,y,mode,style)) {
        c->misses++; if(e->age) c->evictions++;
        /* Borrowed overlay paths must never survive eviction. */
        c->scene_valid=0;
        e->age=0; e->building=1; e->filled=0;
        e->z=z; e->x=x; e->y=y; e->mode=mode; e->style=style;
        cc_arena own; cc_str tile;
        cc_arena_init(&own,(unsigned char*)e+sizeof(*e),CC_CACHE_SLOT_BYTES-sizeof(*e));
        cc_arch_clear(&e->scene,sizeof(e->scene));
        uint64_t stamp=cc_profiling?cc_ticks():0;
        if(x>=0 && y>=0 && x<(INT64_C(1)<<z) && y<(INT64_C(1)<<z)) {
            int found;
            if(map->source) {
                uint64_t off; uint32_t len;
                found=cc_ctile_locate(map,z,(uint32_t)x,(uint32_t)y,&off,&len);
                if(found==1 && !cc_ctile_source_tile(map,off,len,scratch,0,&tile)) found=-1;
            } else found=cc_ctile_find(map,z,(uint32_t)x,(uint32_t)y,&tile);
            if(found<0 || (found && !cc_ctile_tile_scene(tile,0,0,&own,&e->scene))) { e->building=0; scratch->used=mark; return 0; }
            /* Overlay retains geometry only; shade is retained in cell planes.
               In particular, never keep terrain pointers into source staging. */
            e->scene.terrain=0;
            if(found && map->source) {
                /* Retain only geometry/names; the slot's overlay never uses
                   phase tables. Large raw raster blobs need not fit the slot. */
                for(unsigned rank=0;rank<16;rank++) for(cc_feature *f=e->scene.first[rank];f;f=f->next) {
                    f->raster=0;
                    if(f->name.size) {
                        uint8_t *name=cc_arena_alloc(&own,f->name.size,1);
                        if(!name) { e->building=0; scratch->used=mark; return 0; }
                        cc_arch_copy(name,f->name.data,f->name.size); f->name.data=name;
                    }
                    for(size_t k=0;k<f->count;k++) {
                        cc_path *p=f->paths+k;
                        if(!p->xs) continue; /* CT5 points already belong to own. */
                        cc_point *points=cc_arena_array(&own,p->count,sizeof(*points),_Alignof(cc_point));
                        if(!points) { e->building=0; scratch->used=mark; return 0; }
                        for(size_t j=0;j<p->count;j++) points[j]=cc_path_point(p,j);
                        p->points=points; p->xs=p->ys=0; p->ox=p->oy=0;
                    }
                }
                scratch->used=mark;
            }
        }
        if(cc_profiling) cc_profiling->ticks[CC_DECODE]+=cc_ticks()-stamp;
    }
    if(rows>64-e->filled) rows=64-e->filled;
    cc_planes p; cc_scene scene;
    cc_cell *cells=cc_arena_alloc(scratch,128*rows*sizeof(cc_cell),_Alignof(cc_cell));
    if(!cells || !cc_planes_init(&p,scratch,128,(int)rows) ||
       !cc_ctile_scene_cached(map,z,x*256,y*256,128,64,scratch,&scene,c->decoded_cache)) {
        scratch->used=mark; return 0;
    }
    /* Keep the full tile's neighbor set and feature order. Buffered duplicate
       strokes can differ at tile boundaries; selecting neighbors per strip
       would change their priority/phase. Translate only after scene selection. */
    int dy=(int)e->filled*4;
    for(unsigned rank=0;rank<16;rank++) for(cc_feature *f=scene.first[rank];f;f=f->next) {
        if(!translate(&f->anchor,0,-dy) || !translate(&f->bounds_min,0,-dy) ||
           !translate(&f->bounds_max,0,-dy)) { scratch->used=mark; return 0; }
        for(size_t k=0;k<f->count;k++) {
            cc_path *path=f->paths+k; path->oy-=dy;
            if(!path->xs) for(size_t j=0;j<path->count;j++) path->points[j].y-=dy;
        }
    }
    for(cc_terrain *t=scene.terrain;t;t=t->next) t->oy-=dy;
    p.terrain_strength=c->terrain_strength;
    p.colors16=c->colors16; p.edges_mode=c->active_edges; p.native_unclipped_lines=1; cc_raster(&p,&scene);
    uint64_t stamp=cc_profiling?cc_ticks():0; cc_encode(&c->encoder,&p,mode,cells);
    if(cc_profiling) cc_profiling->ticks[CC_ENCODE]+=cc_ticks()-stamp;
    uint8_t *fields[]={p.dots,p.ink,p.dot_priority,p.fill,p.fill_priority,p.coverage,p.coverage_ink,p.coverage_bg,p.coverage_priority,p.shade};
    cc_arch_copy(e->cells+e->filled*128,cells,128*rows*sizeof(cc_cell));
    for(int k=0;k<10;k++) cc_arch_copy(e->planes[k]+e->filled*128,fields[k],128*rows);
    scratch->used=mark; e->filled+=rows; *work+=128*rows;
    if(e->filled==64) { e->building=0; e->age=++c->serial; }
    return e;
}
static cc_cache_entry *get(cc_cell_cache *c,const cc_ctile *map,unsigned z,int64_t x,int64_t y,
                         cc_mode mode,uint32_t style,cc_arena *scratch) {
    if(c->deferred) {
        cc_cache_entry *e=find(c,z,x,y,mode,style);
        if(e) { e->age=++c->serial; c->hits++; }
        return e;
    }
    size_t work=0; return fill(c,map,z,x,y,mode,style,scratch,64,&work);
}
size_t cc_cell_cache_step(cc_cell_cache *c,size_t budget,cc_arena *scratch) {
    size_t work=0;
    if(!c->pending || budget<128) return 0;
    for(int64_t x=c->pending_x0;x<=c->pending_x1;x++) for(int64_t y=c->pending_y0;y<=c->pending_y1;y++) {
        if(find(c,c->pending_z,x,y,c->pending_mode,c->pending_style)) continue;
        unsigned rows=(unsigned)((budget-work)/128>64?64:(budget-work)/128);
        if(!rows) return work;
        cc_cache_entry *e=fill(c,&c->pending_map,c->pending_z,x,y,c->pending_mode,c->pending_style,scratch,rows,&work);
        if(!e || !e->age) return work;
    }
    c->pending=0; return work;
}
static int overlay_scene(cc_scene *scene,const cc_scene *tile,int64_t ox,int64_t oy,int cols,int rows,int borrow,cc_arena *a) {
    for(unsigned rank=0;rank<16;rank++) for(const cc_feature *f=tile->first[rank];f;f=f->next) {
        if(!f->style.line && (!f->label_class || !f->name.size)) continue;
        if(cols && f->type==2 && f->bounds_valid && (f->bounds_max.x+ox<0 || f->bounds_min.x+ox>=cols*2 ||
            f->bounds_max.y+oy<0 || f->bounds_min.y+oy>=rows*4)) continue;
        cc_feature *q=cc_arena_alloc(a,sizeof(*q),_Alignof(cc_feature)); if(!q) return 0;
        *q=*f; q->next=0; q->label_next=0;
        if(!move_feature(q,ox,oy)) return 0;
        if(!borrow) {
            if(q->name.size) {
                uint8_t *name=cc_arena_alloc(a,q->name.size,1); if(!name) return 0;
                cc_arch_copy(name,q->name.data,q->name.size); q->name.data=name;
            }
            q->paths=cc_arena_array(a,q->count,sizeof(cc_path),_Alignof(cc_path)); if(!q->paths) return 0;
            cc_arch_copy(q->paths,f->paths,q->count*sizeof(cc_path));
            for(size_t k=0;k<q->count;k++) {
                cc_path *path=q->paths+k; size_t n=path->count;
                path->points=cc_arena_array(a,n,sizeof(cc_point),_Alignof(cc_point)); if(!path->points) return 0;
                for(size_t j=0;j<n;j++) path->points[j]=cc_path_point(f->paths+k,j);
                path->xs=path->ys=0; path->ox=path->oy=0;
            }
        }
        if(scene->last[rank]) scene->last[rank]->next=q; else scene->first[rank]=q;
        scene->last[rank]=q; scene->features++;
        unsigned label=q->label_class;
        if(label && q->name.size) {
            if(scene->label_last[label][rank]) scene->label_last[label][rank]->label_next=q; else scene->label_first[label][rank]=q;
            scene->label_last[label][rank]=q;
        }
        scene->labels_indexed=1;
    }
    return 1;
}
static void mark_line(uint8_t *delta,uint32_t *list,size_t *count,int cols,int rows,cc_point a,cc_point b) {
    int dx=b.x>a.x?b.x-a.x:a.x-b.x,dy=b.y>a.y?b.y-a.y:a.y-b.y;
    int sx=a.x<b.x?1:-1,sy=a.y<b.y?1:-1,err=dx-dy;
    for(;;) {
        if(a.x>=0 && a.y>=0 && a.x<cols*2 && a.y<rows*4) {
            size_t at=(size_t)(a.y/4)*cols+a.x/2; delta[at]^=cc_dot_bit(a.x,a.y); list[(*count)++]=(uint32_t)at;
        }
        if(a.x==b.x && a.y==b.y) break;
        int twice=2*err; if(twice>-dy) { err-=dy; a.x+=sx; } if(twice<dx) { err+=dx; a.y+=sy; }
    }
}
static int dirty_hit(const uint32_t *grid,int cols,int rows,cc_point lo,cc_point hi) {
    if(hi.x<0 || hi.y<0 || lo.x>=cols*2 || lo.y>=rows*4) return 0;
    int x0=lo.x<0?0:lo.x/2,y0=lo.y<0?0:lo.y/4,x1=hi.x>=cols*2?cols:hi.x/2+1,y1=hi.y>=rows*4?rows:hi.y/4+1;
    size_t stride=(size_t)cols+1;
    return grid[(size_t)y1*stride+x1]-grid[(size_t)y1*stride+x0]-grid[(size_t)y0*stride+x1]+grid[(size_t)y0*stride+x0]!=0;
}
static void add_dirty(uint8_t *dirty,uint32_t *list,size_t *count,uint32_t at) {
    if(!dirty[at]) { dirty[at]=1; list[(*count)++]=at; }
}
static int64_t floor256(int64_t x) { return x/256-(x<0 && x%256!=0); }
int cc_cell_cache_render(cc_cell_cache *c,const cc_ctile *map,unsigned z,int64_t left,int64_t top,
    int cols,int rows,cc_mode mode,uint32_t style,int labels,cc_arena *scratch,cc_cell *out) {
    if(map->cellset || (z>map->zmax && z<=16)) {
        size_t mark=scratch->used; cc_planes p; cc_scene s; cc_encoder e;
        int ok=cc_planes_init(&p,scratch,cols,rows) && cc_ctile_scene_cached(map,z,left,top,cols,rows,scratch,&s,c->decoded_cache);
        if(ok) { p.terrain_strength=c->terrain_strength; p.colors16=c->colors16; p.edges_mode=cc_edges_resolve(c->edges_mode,mode);
            cc_encoder_init(&e); cc_raster(&p,&s); cc_encode(&e,&p,mode,out); if(labels) cc_labels(&p,&s,out); }
        scratch->used=mark; return ok;
    }
    cc_edges edges=cc_edges_resolve(c->edges_mode,mode);
    if(c->terrain_strength!=c->active_terrain_strength || edges!=c->active_edges || c->colors16!=c->active_colors16) { cc_cell_cache_clear(c); c->active_edges=edges; c->active_colors16=c->colors16; c->active_terrain_strength=c->terrain_strength; }
    size_t mark=scratch->used; int ok=0;
    if(!c->slots || z<map->zmin || z>map->zmax || left%2 || top%4 || cols<1 || rows<1 || cols>4096 || rows>4096) return 0;
    int64_t world=(INT64_C(1)<<z)*256;
    if(left< -32768 || top< -32768 || left>world+32768 || top>world+32768) return 0;
    const uint8_t *identity=map->source?(const uint8_t*)map->source:map->bytes.data;
    uint64_t generation=map->source?map->source->generation:0;
    const void *owner=map->source?map->source->generation_owner:0;
    if(c->source!=identity || c->source_generation!=generation || c->source_owner!=owner) {
        cc_cell_cache_clear(c); c->source=identity; c->source_generation=generation; c->source_owner=owner;
    }
    if(c->deferred) {
        int64_t limit=INT64_C(1)<<z;
        int64_t x0=floor256(left-8),y0=floor256(top-8),x1=floor256(left+cols*2+8),y1=floor256(top+rows*4+8);
        /* Include off-world base tiles, which raster as empty cells. */
        if(x0<0 && left>=0) x0=0;
        if(y0<0 && top>=0) y0=0;
        if(x1>=limit && left+cols*2<=world) x1=limit-1;
        if(y1>=limit && top+rows*4<=world) y1=limit-1;
        c->pending_map=*map; c->pending_z=z; c->pending_mode=mode; c->pending_style=style;
        c->pending_x0=x0; c->pending_y0=y0; c->pending_x1=x1; c->pending_y1=y1;
        c->pending=0;
        for(int64_t x=x0;x<=x1;x++) for(int64_t y=y0;y<=y1;y++)
            { cc_cache_entry *e=find(c,z,x,y,mode,style);
              if(!e) c->pending=1; else e->age=++c->serial; }
        if(c->pending) {
            /* Oversized views use direct rendering rather than endless LRU churn. */
            if((uint64_t)(x1-x0+1)*(uint64_t)(y1-y0+1)>c->slots) c->pending=0;
            c->frame_valid=c->scene_valid=0; return 0;
        }
    }
    int shiftx=0,shifty=0,incremental=0;
    if(c->frame_valid && c->frame_cols==cols && c->frame_rows==rows && c->frame_z==z && c->frame_mode==mode && c->frame_style==style &&
       left-c->frame_left>=-2 && left-c->frame_left<=2 && top-c->frame_top>=-4 && top-c->frame_top<=4) {
        shiftx=(int)(left-c->frame_left); shifty=(int)(top-c->frame_top); incremental=1;
    }
    int64_t x0=floor256(left),y0=floor256(top),x1=floor256(left+cols*2-1),y1=floor256(top+rows*4-1);
    for(int64_t x=x0;x<=x1;x++) for(int64_t y=y0;y<=y1;y++) {
        cc_cache_entry *e=get(c,map,z,x,y,mode,style,scratch); if(!e) goto done;
        int vx=(int)((x*256-left)/2),vy=(int)((y*256-top)/4);
        int sx=vx<0?-vx:0,sy=vy<0?-vy:0,ex=cols-vx,ey=rows-vy;
        if(ex>128) ex=128;
        if(ey>64) ey=64;
        for(int row=sy;row<ey;row++) {
            int begin=sx,end=ex,oldrow=vy+row+shifty/4;
            if(incremental && oldrow>=0 && oldrow<rows) {
                int dx=shiftx/2;
                if(dx>=0) { int edge=cols-dx-vx; if(begin<edge) begin=edge; }
                else { int edge=-dx-vx; if(end>edge) end=edge; }
            }
            if(begin<end) cc_arch_copy(out+(size_t)(vy+row)*cols+vx+begin,e->cells+row*128+begin,(size_t)(end-begin)*sizeof(cc_cell));
        }
    }
    if(incremental) {
        int dx=shiftx/2,dy=shifty/4,x0=dx<0?-dx:0,x1=dx>0?cols-dx:cols,y0=dy<0?-dy:0,y1=dy>0?rows-dy:rows;
        for(int y=y0;y<y1;y++) cc_arch_copy(out+(size_t)y*cols+x0,c->snapshot+(size_t)(y+dy)*cols+x0+dx,(size_t)(x1-x0)*sizeof(cc_cell));
    }
    {
        uint64_t stamp=cc_profiling?cc_ticks():0,nested=cc_profiling?cc_profile_sum():0;
        cc_scene scene={0}; cc_planes p={0};
        p.colors16=c->colors16; p.cols=cols; p.rows=rows; p.words=((size_t)cols+CC_WORD_BITS)/CC_WORD_BITS;
        p.label_capacity=1; while(p.label_capacity<(size_t)cols*rows/3+1) p.label_capacity*=2;
        p.collision=cc_arena_alloc(scratch,p.words*rows*sizeof(cc_bitword),_Alignof(cc_bitword));
        p.label_seen=cc_arena_alloc(scratch,p.label_capacity*sizeof(cc_feature*),_Alignof(cc_feature*));
        if(!p.collision || !p.label_seen) goto done;
        x0=floor256(left-8); y0=floor256(top-8); x1=floor256(left+cols*2+8); y1=floor256(top+rows*4+8);
        if(x0<0) x0=0;
        if(y0<0) y0=0;
        int64_t limit=INT64_C(1)<<z;
        if(x1>=limit) x1=limit-1;
        if(y1>=limit) y1=limit-1;
        size_t keys=(size_t)(x1-x0+1)*(size_t)(y1-y0+1),nbytes=(size_t)cols*rows*sizeof(cc_cell);
        int64_t ux0=floor256(left),uy0=floor256(top),ux1=floor256(left+cols*2-1),uy1=floor256(top+rows*4-1);
        if(x0<ux0) ux0=x0;
        if(y0<uy0) uy0=y0;
        if(x1>ux1) ux1=x1;
        if(y1>uy1) uy1=y1;
        int borrow=c->slots>=(uint64_t)(ux1-ux0+1)*(uint64_t)(uy1-uy0+1);
        int retain=c->snapshot && c->slots>=keys && nbytes<CC_CACHE_SLOT_BYTES/2 && left>=0 && top>=0 && left+cols*2<=world && top+rows*4<=world;
        uint64_t key=(uint64_t)x0*UINT64_C(0x9e3779b185ebca87)^(uint64_t)y0*UINT64_C(0xc2b2ae3d27d4eb4f)^(uint64_t)x1*UINT64_C(0x165667b19e3779f9)^(uint64_t)y1*UINT64_C(0x85ebca77c2b2ae63)^z^((uint64_t)mode<<32)^style;
        cc_arena persistent; cc_arena *geometry=scratch;
        if(retain) { cc_arena_init(&persistent,(unsigned char*)c->snapshot+nbytes,CC_CACHE_SLOT_BYTES-nbytes); geometry=&persistent; }
        int reuse_scene=retain && c->scene_valid && c->scene_key==key && c->scene_x0==x0 && c->scene_y0==y0 && c->scene_x1==x1 && c->scene_y1==y1 &&
            c->frame_cols==cols && c->frame_rows==rows && c->frame_z==z && c->frame_mode==mode && c->frame_style==style;
        if(reuse_scene) {
            scene=c->retained_scene; int dx=(int)(left-c->scene_left),dy=(int)(top-c->scene_top);
            for(unsigned rank=0;rank<16;rank++) for(cc_feature *f=scene.first[rank];f;f=f->next) {
                if(!move_feature(f,-dx,-dy)) goto done;
            }
        } else {
            c->scene_valid=0;
            int geometry_ok=1;
            for(int64_t x=x0;x<=x1 && geometry_ok;x++) for(int64_t y=y0;y<=y1 && geometry_ok;y++) {
                cc_cache_entry *e=get(c,map,z,x,y,mode,style,scratch);
                if(!e) goto done;
                geometry_ok=overlay_scene(&scene,&e->scene,x*256-left,y*256-top,retain?0:cols,rows,borrow,geometry);
            }
            if(!geometry_ok) {
                if(!retain) goto done;
                retain=0; cc_arch_clear(&scene,sizeof(scene));
                for(int64_t x=x0;x<=x1;x++) for(int64_t y=y0;y<=y1;y++) {
                    cc_cache_entry *e=get(c,map,z,x,y,mode,style,scratch);
                    if(!e || !overlay_scene(&scene,&e->scene,x*256-left,y*256-top,cols,rows,borrow,scratch)) goto done;
                }
            }
            if(retain) {
                c->retained_scene=scene; c->scene_key=key; c->scene_valid=1;
                c->scene_x0=x0; c->scene_y0=y0; c->scene_x1=x1; c->scene_y1=y1;
            }
        }
        c->scene_left=left; c->scene_top=top;
        if(cc_profiling) { uint64_t elapsed=cc_ticks()-stamp,inner=cc_profile_sum()-nested; cc_profiling->ticks[CC_DECODE]+=elapsed>inner?elapsed-inner:0; stamp=cc_ticks(); }
        /* Reproduce viewport line clipping exactly. Cache fills/coverage remain
           tile-local; line and label geometry is retained, never re-decoded. */
        uint8_t *fields[10];
        for(int k=0;k<10;k++) { fields[k]=cc_arena_alloc(scratch,(size_t)cols*rows,1); if(!fields[k]) goto done; }
        p.dots=fields[0]; p.ink=fields[1]; p.dot_priority=fields[2]; p.fill=fields[3]; p.fill_priority=fields[4];
        p.coverage=fields[5]; p.coverage_ink=fields[6]; p.coverage_bg=fields[7]; p.coverage_priority=fields[8]; p.shade=fields[9];
        p.terrain_phase_x=(int)((left/2)&3); p.terrain_phase_y=(int)((top/4)&3);
        p.terrain_active=c->terrain_strength>0; p.terrain_strength=c->terrain_strength; p.terrain_palette_strength=-1;
        if(p.terrain_active) cc_terrain_palette(&p);
        int64_t bx0=floor256(left),by0=floor256(top),bx1=floor256(left+cols*2-1),by1=floor256(top+rows*4-1);
        size_t nx=(size_t)(bx1-bx0+1),ny=(size_t)(by1-by0+1);
        cc_cache_entry **tiles=cc_arena_alloc(scratch,nx*ny*sizeof(*tiles),_Alignof(cc_cache_entry*)); if(!tiles) goto done;
        for(int64_t x=bx0;x<=bx1;x++) for(int64_t y=by0;y<=by1;y++) {
            cc_cache_entry *e=get(c,map,z,x,y,mode,style,scratch); if(!e) goto done;
            tiles[(size_t)(y-by0)*nx+(size_t)(x-bx0)]=e;
            if(c->slots<nx*ny) {
                int vx=(int)((x*256-left)/2),vy=(int)((y*256-top)/4),sx=vx<0?-vx:0,sy=vy<0?-vy:0,ex=cols-vx,ey=rows-vy;
                if(ex>128) ex=128;
                if(ey>64) ey=64;
                for(int row=sy;row<ey;row++) for(int k=3;k<10;k++) cc_arch_copy(fields[k]+(size_t)(vy+row)*cols+vx+sx,e->planes[k]+row*128+sx,(size_t)(ex-sx));
            }
        }
        uint8_t *dirty=cc_arena_alloc(scratch,(size_t)cols*rows,1); if(!dirty) goto done;
        uint8_t *delta=cc_arena_alloc(scratch,(size_t)cols*rows,1); if(!delta) goto done;
        uint32_t *dirty_list=cc_arena_alloc(scratch,(size_t)cols*rows*sizeof(uint32_t),_Alignof(uint32_t)); if(!dirty_list) goto done;
        size_t dirty_count=0;
        cc_arch_clear(delta,(size_t)cols*rows); cc_arch_clear(dirty,(size_t)cols*rows);
        if(!incremental || shifty) for(int x=0;x<cols;x++) {
            add_dirty(dirty,dirty_list,&dirty_count,(uint32_t)x);
            add_dirty(dirty,dirty_list,&dirty_count,(uint32_t)((rows-1)*cols+x));
        }
        if(!incremental || shiftx) for(int y=0;y<rows;y++) {
            add_dirty(dirty,dirty_list,&dirty_count,(uint32_t)(y*cols));
            add_dirty(dirty,dirty_list,&dirty_count,(uint32_t)(y*cols+cols-1));
        }
        for(unsigned rank=0;rank<16;rank++) for(const cc_feature *f=scene.first[rank];f;f=f->next) {
            if(f->terrain_feature && !c->terrain_strength) continue;
            if(!f->style.line || f->type==1) continue;
            if(f->bounds_valid && (f->bounds_max.x<0 || f->bounds_min.x>=cols*2 || f->bounds_max.y<0 || f->bounds_min.y>=rows*4) &&
               (!incremental || (f->bounds_max.x+shiftx<0 || f->bounds_min.x+shiftx>=cols*2 || f->bounds_max.y+shifty<0 || f->bounds_min.y+shifty>=rows*4))) continue;
            if(f->bounds_valid && f->bounds_min.x>=0 && f->bounds_min.y>=0 &&
               f->bounds_max.x<cols*2 && f->bounds_max.y<rows*4 &&
               (!incremental || (f->bounds_min.x+shiftx>=0 && f->bounds_min.y+shifty>=0 &&
               f->bounds_max.x+shiftx<cols*2 && f->bounds_max.y+shifty<rows*4))) continue;
            for(size_t k=0;k<f->count;k++) {
                const cc_path *path=f->paths+k; size_t n=path->count;
                for(size_t j=0;j+1<n+(path->closed?1u:0u);j++) {
                    cc_point a=cc_feature_point(f,path,j),b=cc_feature_point(f,path,(j+1)%n);
                    if(a.x>=0 && a.y>=0 && a.x<cols*2 && a.y<rows*4 && b.x>=0 && b.y>=0 && b.x<cols*2 && b.y<rows*4 &&
                       (!incremental || (a.x+shiftx>=0 && a.y+shifty>=0 && a.x+shiftx<cols*2 && a.y+shifty<rows*4 &&
                       b.x+shiftx>=0 && b.y+shifty>=0 && b.x+shiftx<cols*2 && b.y+shifty<rows*4))) continue;
                    cc_point ca=a,cb=b;
                    int new_visible=cc_clip_line(cols*2,rows*4,&ca,&cb),old_visible=1;
                    if(incremental) {
                        a.x+=shiftx; b.x+=shiftx; a.y+=shifty; b.y+=shifty;
                        old_visible=cc_clip_line(cols*2,rows*4,&a,&b);
                        a.x-=shiftx; b.x-=shiftx; a.y-=shifty; b.y-=shifty;
                        if(old_visible==new_visible && (!old_visible || (a.x==ca.x && a.y==ca.y && b.x==cb.x && b.y==cb.y))) continue;
                    } else if(!new_visible) continue;
                    uint32_t list[1024]; size_t count=0;
                    if(old_visible) mark_line(delta,list,&count,cols,rows,a,b);
                    if(new_visible) mark_line(delta,list,&count,cols,rows,ca,cb);
                    for(size_t i=0;i<count;i++) if(delta[list[i]]) add_dirty(dirty,dirty_list,&dirty_count,list[i]);
                    for(size_t i=0;i<count;i++) delta[list[i]]=0;
                }
            }
        }
        cc_arch_clear(p.dots,(size_t)cols*rows); cc_arch_clear(p.dot_priority,(size_t)cols*rows);
        for(size_t i=0;i<dirty_count;i++) {
            size_t at=dirty_list[i]; int y=(int)(at/(size_t)cols),x=(int)(at%(size_t)cols);
            int64_t wx=left+x*2,wy=top+y*4,tx=floor256(wx),ty=floor256(wy);
            p.ink[at]=0;
            if(c->slots>=nx*ny) {
                cc_cache_entry *e=tiles[(size_t)(ty-by0)*nx+(size_t)(tx-bx0)];
                size_t src=(size_t)((wy-ty*256)/4)*128+(size_t)((wx-tx*256)/2);
                for(int k=3;k<10;k++) fields[k][at]=e->planes[k][src];
            }

        }
        uint32_t *integral=cc_arena_alloc(scratch,(size_t)(cols+1)*(rows+1)*sizeof(uint32_t),_Alignof(uint32_t)); if(!integral) goto done;
        cc_integral(dirty,integral,cols,rows);
        for(unsigned rank=0;rank<16;rank++) for(const cc_feature *f=scene.first[rank];f;f=f->next) {
            if(f->terrain_feature && !c->terrain_strength) continue;
            if(!f->style.line || (f->bounds_valid && !dirty_hit(integral,cols,rows,f->bounds_min,f->bounds_max))) continue;
            for(size_t k=0;k<f->count;k++) {
                const cc_path *path=f->paths+k;
                if(f->type==1) for(size_t j=0;j<path->count;j++) cc_dot(&p,cc_feature_point(f,path,j).x,cc_feature_point(f,path,j).y,f->style.line,f->style.priority);
                else {
                    for(size_t j=1;j<path->count;j++) {
                        cc_point a=cc_feature_point(f,path,j-1),b=cc_feature_point(f,path,j);
                        cc_point lo={a.x<b.x?a.x:b.x,a.y<b.y?a.y:b.y},hi={a.x>b.x?a.x:b.x,a.y>b.y?a.y:b.y};
                        int hit=dirty_hit(integral,cols,rows,lo,hi);
                        if(hit) cc_line(&p,a,b,f->style.line,f->style.priority);
                    }
                    if(path->closed && path->count) cc_line(&p,cc_feature_point(f,path,path->count-1),cc_feature_point(f,path,0),f->style.line,f->style.priority);
                }
            }
        }
        if(cc_profiling) { cc_profiling->ticks[CC_LINES]+=cc_ticks()-stamp; stamp=cc_ticks(); }
        cc_encode_list(&c->encoder,&p,mode,out,dirty_list,dirty_count);
        if(c->snapshot && (size_t)cols*rows*sizeof(cc_cell)<=CC_CACHE_SLOT_BYTES) {
            cc_arch_copy(c->snapshot,out,(size_t)cols*rows*sizeof(cc_cell)); c->frame_valid=1;
            c->frame_cols=cols; c->frame_rows=rows; c->frame_z=z; c->frame_left=left; c->frame_top=top; c->frame_mode=mode; c->frame_style=style;
        } else c->frame_valid=0;
        if(cc_profiling) { cc_profiling->ticks[CC_ENCODE]+=cc_ticks()-stamp; stamp=cc_ticks(); }
        if(labels) cc_labels(&p,&scene,out);
        if(cc_profiling) cc_profiling->ticks[CC_LABELS]+=cc_ticks()-stamp;
    }
    ok=1;
done: if(!ok) c->frame_valid=c->scene_valid=0; scratch->used=mark; return ok;
}
