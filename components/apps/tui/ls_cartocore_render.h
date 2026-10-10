/* Shared single-owner render state; host supplies allocation/source adapters. */
#define DECODED_BYTES (2u*1024u*1024u)
#define CELL_BYTES (4u*1024u*1024u)
#include "ls_carto_budget.h"
static bool carto_memory_failed;
static carto_budget carto_last_budget;
typedef struct {
    void *memory,*hot_memory,*decoded_memory,*cell_memory,*idle_memory;
    cc_arena idle_arena;
    cc_cell *cells;
    cc_arena arena,hot_arena; cc_renderer renderer; cc_ctile map;
    cc_decoded_cache decoded; cc_cell_cache tiles;
    size_t tile_fallbacks;
    cc_ctile coarse_map; unsigned coarse_frames; int coarse;
    int cols,rows,labels; unsigned z; int64_t left,top;
    struct { uint64_t offset; uint32_t length; } hints[64];
    unsigned hint_count,hint_next;
} render_state;
typedef struct { size_t cold,hot,decoded,idle,cache; } carto_arena_hw;
static carto_arena_hw carto_hw;
static void carto_hw_max(size_t *peak,size_t used) { if(used>*peak) *peak=used; }
static void carto_hw_sample(render_state *s) {
    if(!s) return;
    carto_hw_max(&carto_hw.cold,s->arena.peak);
    carto_hw_max(&carto_hw.hot,s->hot_arena.peak);
    carto_hw_max(&carto_hw.decoded,s->decoded.peak);
    carto_hw_max(&carto_hw.idle,s->idle_arena.peak);
    /* Raster slots are allocated/occupied as one fixed pool. */
    carto_hw_max(&carto_hw.cache,s->cell_memory?CELL_BYTES:0);
}
static void release(render_state *s)
{
    if (!s) return;
    carto_hw_sample(s);
    heap_caps_free(s->idle_memory); heap_caps_free(s->cell_memory); heap_caps_free(s->decoded_memory);
    heap_caps_free(s->cells); heap_caps_free(s->hot_memory); heap_caps_free(s->memory); heap_caps_free(s);
}
/* One bounded diagnostic for all MAP failure paths, at most once per 5 s. */
static void carto_failure(const char *step,int cols,int rows,unsigned z) {
    static int64_t next;
    int64_t now=esp_timer_get_time();if(now<next) return;next=now+5000000;
    printf("carto MAP: failed step=%s grid=%dx%d z%u hot=%zu cold=%zu cells=%zu decoded=%zu cache=%zu idle=%zu free_internal=%zu free_psram=%zu\n",
        step,cols,rows,z,cols>0 && rows>0?cc_renderer_hot_bytes(cols,rows):0,
        carto_last_budget.cold,cols>0 && rows>0?(size_t)cols*rows*sizeof(cc_cell):0,
        carto_last_budget.decoded,carto_last_budget.cache,carto_last_budget.idle,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
}
static render_state *create(int cols,int rows,unsigned z,bool external,bool tilecache)
{
    carto_memory_failed=true;
    uint32_t caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    cc_ctile map;
    if(!open_map(&map)) { carto_memory_failed=false;carto_failure("map",cols,rows,z);return NULL; }
    bool v7=(map.source?map.source->header[8]:map.bytes.data[8])==7;
    size_t hot_bytes=cc_renderer_hot_bytes(cols,rows);
    size_t overhead=sizeof(render_state)+hot_bytes+(size_t)cols*rows*(sizeof(cc_cell)+2*sizeof(tui_cell));
    size_t free_bytes=heap_caps_get_free_size(caps),largest=heap_caps_get_largest_free_block(caps);
    if(!carto_plan_budget(free_bytes,largest,overhead,v7,map.cellset!=NULL,map.source!=NULL,tilecache,&carto_last_budget)) {
        carto_failure("not enough memory (PSRAM budget)",cols,rows,z);return NULL;
    }
    size_t capacity=carto_last_budget.cold;
    printf("carto budget free=%zu reserve=%u guard=%u cold=%zu decoded=%zu cache=%zu idle=%zu overhead=%zu\n",
           free_bytes,CARTO_PSRAM_RESERVE,256u*1024u,capacity,carto_last_budget.decoded,
           carto_last_budget.cache,carto_last_budget.idle,overhead);
    render_state *s=heap_caps_calloc(1,sizeof(*s),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s) { carto_failure("state allocation",cols,rows,z); return NULL; }
    (void)external; /* Both console options now use split storage. */
    s->hot_memory=heap_caps_aligned_alloc(16,hot_bytes,v7?caps:MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!s->hot_memory) s->hot_memory=heap_caps_aligned_alloc(16,hot_bytes,caps);
    s->cols=cols; s->rows=rows; s->z=z; s->labels=1;
    s->memory=capacity?heap_caps_aligned_alloc(16,capacity,caps):NULL;
    s->cells=heap_caps_aligned_alloc(16,(size_t)cols*rows*sizeof(cc_cell),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s->decoded_memory=heap_caps_aligned_alloc(16,carto_last_budget.decoded,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    const char *step="hot allocation (internal and PSRAM)";
    if(!s->hot_memory) goto fail;
    step="cold allocation";if(!s->memory) goto fail;
    step="cell allocation";if(!s->cells) goto fail;
    step="decoded allocation";if(!s->decoded_memory) goto fail;
    s->map=map;
    cc_arena_init(&s->arena,s->memory,capacity);
    cc_arena_init(&s->hot_arena,s->hot_memory,hot_bytes);
    step="renderer"; if (!cc_renderer_init_split(&s->renderer,&s->hot_arena,&s->arena,cols,rows,s->cells)) goto fail;
    printf("carto arenas hot=%s used=%zu capacity=%zu cold=PSRAM capacity=%zu output-row=%zu saved-frame=PSRAM\n",
           esp_ptr_internal(s->hot_memory)?"internal":"PSRAM fallback",s->hot_arena.used,hot_bytes,capacity,(size_t)cols*sizeof(cc_cell));
    step="decoded"; if (!cc_decoded_cache_init(&s->decoded,s->decoded_memory,carto_last_budget.decoded)) goto fail;
    s->renderer.decoded_cache=&s->decoded; s->renderer.colors16=1;
    s->renderer.terrain_strength=saved.terrain_on?saved.terrain_strength:0;
    /* The raster cache retains borrowed tile scenes; a callback's staging
       is ephemeral. Streaming uses the source-aware renderer/decoded cache. */
    if (tilecache && !s->map.source) {
        s->cell_memory=heap_caps_aligned_alloc(16,CELL_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        step="cellcache"; if (!s->cell_memory || !cc_cell_cache_init(&s->tiles,s->cell_memory,CELL_BYTES)) goto fail;
        s->tiles.terrain_strength=s->renderer.terrain_strength; s->tiles.colors16=1; s->tiles.decoded_cache=&s->decoded; s->tiles.deferred=1;
    }
    if(carto_last_budget.idle) {
        size_t idle_bytes=carto_last_budget.idle;
        s->idle_memory=heap_caps_malloc(idle_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        step="idle scratch"; if(!s->idle_memory) goto fail;
        cc_arena_init(&s->idle_arena,s->idle_memory,idle_bytes);
    }
    if(s->map.cellset) {
        /* Single worker: idle scratch borrows cold only after scene invalidation.
           Keep persistent renderer planes below mark intact. */
        cc_arena_init(&s->idle_arena,(uint8_t*)s->memory+s->renderer.mark,
                      capacity-s->renderer.mark);
    }
    origin(cols,rows,z,&s->left,&s->top);
    s->left-=s->left%2; s->top-=s->top%4;
    carto_memory_failed=false;
    return s;
fail:
    carto_failure(step,cols,rows,z);
    release(s); return NULL;
}
static int render_frame(render_state *s,cc_mode mode,cc_edges edges,int64_t left,int64_t top)
{
    carto_memory_failed=false;
#ifdef LS_CARTOCORE_HOST
    /* Poison unfinished output: a failed render must never reach publication. */
    static bool injected;
    if(!injected && s->z==13 && getenv("LSSIM_CARTO_FAIL_FRAME")) {
        injected=true;
        for(int i=0;i<s->cols*s->rows;i++) s->cells[i]=(cc_cell){0xdead,0,0};
        puts("carto host: injected incomplete render");return 0;
    }
#endif
    s->renderer.edges_mode=edges; s->tiles.edges_mode=edges;
    if (s->cell_memory && !(left%2) && !(top%4)) {
        cc_renderer_invalidate(&s->renderer);
        s->arena.used=s->renderer.mark;
        if (cc_cell_cache_render(&s->tiles,&s->map,s->z,left,top,s->cols,s->rows,mode,0,s->labels,&s->arena,s->cells)) { carto_hw_sample(s);return 1; }
        s->tile_fallbacks++;
        /* A failed tile fill must not leave a retained renderer scene alive. */
        cc_renderer_invalidate(&s->renderer);
    }
#ifdef LS_CARTOCORE_HOST
    size_t original_size=s->arena.size;
    static bool exhausted;
    if(!exhausted && s->z==13 && getenv("LSSIM_CARTO_EXHAUST_FRAME")) {
        exhausted=true;cc_renderer_invalidate(&s->renderer);
        s->arena.size=s->renderer.mark+64;
        puts("carto host: real arena exhaustion for one frame");
    }
#endif
    s->coarse=0;
    int ok=cc_render(&s->renderer,&s->map,s->z,left,top,mode,s->labels,s->cells);
    if(!ok && s->map.cellset) {
        /* Same virtual source, lower source zoom, requested display projection.
           A stable copy outlives retained renderer state. Missing cells still
           use the normal cellset path first; this is a bounded last resort. */
        unsigned detail=s->z<s->map.zmax?s->z:s->map.zmax;
        s->coarse_map=s->map;s->coarse_map.cellset=NULL;
        while(!ok && detail>s->map.zmin) {
            s->coarse_map.zmax=--detail;
            cc_renderer_invalidate(&s->renderer);
            ok=cc_render(&s->renderer,&s->coarse_map,s->z,left,top,mode,s->labels,s->cells);
        }
        if(ok) { s->coarse=1;s->coarse_frames++; }
    }
#ifdef LS_CARTOCORE_HOST
    s->arena.size=original_size;
#endif
    carto_hw_sample(s);
    /* Failure is transactional: invalidate scratch, keep the published frame.
       MAP retries through its existing backoff; never publish partial output. */
    if(!ok) cc_renderer_invalidate(&s->renderer);
    return ok;
}
