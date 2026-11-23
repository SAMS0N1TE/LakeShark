/* Shared single-owner render state; host supplies allocation/source adapters. */
#define DECODED_BYTES (2u*1024u*1024u)
#define CELL_BYTES (4u*1024u*1024u)
/* Internal (= DMA-capable) RAM MAP leaves for BLE adverts, Wi-Fi and SPI. */
#define CARTO_INTERNAL_KEEP (48u*1024u)
/* What the SD bounce buffer leaves of the DMA pool. */
#define CARTO_DMA_KEEP (24u*1024u)
/* PSRAM held back from the plan for what MAP allocates after it, so the
   board keeps about 3 MB with MAP open (reserve 2 MiB + guard + this). */
#define CARTO_MAP_HEADROOM (1u*1024u*1024u)
#include "ls_carto_budget.h"
static bool carto_memory_failed;
static bool carto_lean;     /* the open map runs on the lean plan */
static carto_budget carto_last_budget;
typedef struct {
    void *memory,*hot_memory,*decoded_memory,*cell_memory,*idle_memory;
    cc_arena idle_arena;
    cc_cell *cells;
    cc_arena arena,hot_arena; cc_renderer renderer; cc_ctile map;
    cc_decoded_cache decoded; cc_cell_cache tiles;
    size_t tile_fallbacks;
    cc_ctile coarse_map; unsigned coarse_frames,degraded_frames; int coarse;
    int cols,rows,labels; unsigned z; int64_t left,top;
    bool raster_only; /* MAP's picture: planes only, no cells */
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
    /* MAP's own buffers (picture, cell handles and blocks, labels) come after
       this plan: without room for them the board fell to 1.3 MB free with MAP
       open (2026-11-20, LR2021). The planner's reserve stays as measured. */
    const size_t free_all=free_bytes;
    free_bytes=free_bytes>CARTO_MAP_HEADROOM?free_bytes-CARTO_MAP_HEADROOM:0;
    carto_lean=false;
    if(!carto_plan_budget(free_bytes,largest,overhead,v7,map.cellset!=NULL,map.source!=NULL,tilecache,&carto_last_budget)) {
        /* Short of memory: a smaller map, not none (ls_carto_budget.h). */
        if(!carto_plan_budget_lean(free_all,largest,overhead,v7,map.cellset!=NULL,&carto_last_budget)) {
            carto_failure("not enough memory (PSRAM budget)",cols,rows,z);return NULL;
        }
        carto_lean=true;free_bytes=free_all;
        printf("carto MAP: lean plan, %zu free largest %zu\n",free_all,largest);
    }
#ifdef LS_CARTOCORE_HOST
    /* Host measurements of the arena/decoded trade (ls_carto_budget.h), in 256 KiB units. */
    if(getenv("LSSIM_CARTO_COLD")) carto_last_budget.cold=(size_t)atoi(getenv("LSSIM_CARTO_COLD"))*256u*1024u;
    if(getenv("LSSIM_CARTO_DECODED")) carto_last_budget.decoded=(size_t)atoi(getenv("LSSIM_CARTO_DECODED"))*256u*1024u;
#endif
    size_t capacity=carto_last_budget.cold;
    printf("carto budget free=%zu reserve=%u guard=%u cold=%zu decoded=%zu cache=%zu idle=%zu overhead=%zu\n",
           free_bytes,CARTO_PSRAM_RESERVE,256u*1024u,capacity,carto_last_budget.decoded,
           carto_last_budget.cache,carto_last_budget.idle,overhead);
    render_state *s=heap_caps_calloc(1,sizeof(*s),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s) { carto_failure("state allocation",cols,rows,z); return NULL; }
    (void)external; /* Both console options now use split storage. */
    s->cols=cols; s->rows=rows; s->z=z; s->labels=1;
    /* Largest block first. The budget reads one free total and one largest
       block, but cold and decoded need two blocks: with 16.8 MB free and a
       6 MiB largest block, cold took the 6 MiB block and decoded never fit
       again. Decoded steps down until it fits; cold then takes what is left. */
    const char *step="decoded allocation";
    size_t decoded=carto_last_budget.decoded;
    const size_t decoded_min=carto_lean?512u*1024u:map.cellset?CARTO_CELLS_DECODED_MIN:decoded;
    while(!(s->decoded_memory=heap_caps_aligned_alloc(16,decoded,caps)) && decoded>decoded_min)
        decoded=decoded>2u*decoded_min?decoded-decoded_min:decoded_min;
    if(!s->decoded_memory) goto fail;
    carto_last_budget.decoded=decoded;
    size_t left=heap_caps_get_largest_free_block(caps);
    left=left>64u*1024u?(left-64u*1024u)/(256u*1024u)*(256u*1024u):0;
    if(capacity>left) capacity=left;
    carto_last_budget.cold=capacity;
    step="cold allocation";
    if(capacity<(carto_lean?CARTO_LEAN_COLD_MIN:2u*CARTO_MIB) || !(s->memory=heap_caps_aligned_alloc(16,capacity,caps))) goto fail;
    /* Internal RAM is the DMA pool BLE, Wi-Fi and the SPI radios share; the
       hot arena only goes there while that pool keeps its headroom. */
    if(!v7 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>hot_bytes+CARTO_INTERNAL_KEEP)
        s->hot_memory=heap_caps_aligned_alloc(16,hot_bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!s->hot_memory) s->hot_memory=heap_caps_aligned_alloc(16,hot_bytes,caps);
    s->cells=heap_caps_aligned_alloc(16,(size_t)cols*rows*sizeof(cc_cell),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    step="hot allocation (internal and PSRAM)";
    if(!s->hot_memory) goto fail;
    step="cell allocation";if(!s->cells) goto fail;
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
    if (tilecache && !s->map.source && carto_last_budget.cache) {
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
/* Hill shading is built at z12 resolution at most (cc_terrain_max_zoom) from a
   surface model that includes buildings. Past z13 each shade cell is stretched
   4x and more, which on flat cities (Buenos Aires, 11-05) paints large soft
   facets over the street grid. Full strength to z12, half at z13, none above. */
static int carto_terrain_for_zoom(int strength,int z)
{
    if(strength<=0 || z>=14) return 0;
    return z==13?(strength+1)/2:strength;
}
/* MAP's picture puts six pixels across a text cell where the text views put
   two dots, so at one zoom it shows three times the ground across: far out
   the classes chosen for cells (core.c ink_shown) made a picture of every
   road and river in the region, and decoded every z8 cell under it. Far out
   it draws what a view this many zooms farther would. z6 and z7 (a state
   with its main roads) and z9 on are as they were; z8 draws z7's classes
   from the overview (it read and decoded every z8 cell in view, ~0.7 s, for
   county lines and land use the eye cannot place at that scale); z5 and
   out keep only the rivers that span a good part of the picture, z4 and out
   lose the motorways too (land, water and the outlines carry a continent). */
static int carto_picture_detail_less(unsigned z)
{
    return z>=9 ? 0 : z==8 ? 1 : z>=6 ? 0 : z>=4 ? 1 : 2;
}
/* The highest zoom whose picture shows only what the cells' overview holds
   (motorways, trunk and main roads, water, rivers: the classes to z7), so it
   is drawn from the overview scaled up and reads no z8 cell. */
static unsigned carto_overview_through(void)
{
    for(unsigned z=10;z>=8;z--) if((int)z-carto_picture_detail_less(z)<=7) return z;
    return 0;
}
static int render_frame(render_state *s,cc_mode mode,cc_edges edges,int64_t left,int64_t top)
{
    carto_memory_failed=false;
    {
        const int less=s->raster_only?carto_picture_detail_less(s->z):0;
        s->renderer.detail_less=less;
        if(s->map.cellset) {
            const unsigned through=less?carto_overview_through():0;
            if(s->map.cellset->overview_through!=through) {
                s->map.cellset->overview_through=through;cc_renderer_invalidate(&s->renderer);
            }
        }
    }
    {
        /* The cell cache clears itself when the strength changes (cache.c). */
        const int t=carto_terrain_for_zoom(saved.terrain_on?saved.terrain_strength:0,s->z);
        s->renderer.terrain_strength=t; s->tiles.terrain_strength=t;
    }
#ifdef LS_CARTOCORE_HOST
    { const char *never=getenv("LSSIM_CARTO_FAIL_ZOOM");if(never && s->z==(unsigned)atoi(never)) return 0; }
    /* Poison unfinished output: a failed render must never reach publication. */
    static bool injected;
    if(!injected && s->z==13 && getenv("LSSIM_CARTO_FAIL_FRAME")) {
        injected=true;
        for(int i=0;i<s->cols*s->rows;i++) s->cells[i]=(cc_cell){0xdead,0,0};
        puts("carto host: injected incomplete render");return 0;
    }
#endif
    s->renderer.edges_mode=edges; s->tiles.edges_mode=edges;
    /* The raster cache holds cells, never planes: a picture cannot use it. */
    cc_cell *const out=s->raster_only?NULL:s->cells;
    if (out && s->cell_memory && !(left%2) && !(top%4)) {
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
    int ok=cc_render(&s->renderer,&s->map,s->z,left,top,mode,s->labels,out);
    if(ok && s->renderer.scene.degraded) {
        /* Some tiles did not fit the arena and came from a coarser zoom; the
           rest of the frame is at full detail. Said at most every 5 s. */
        static int64_t next_note;
        const int64_t now_us=esp_timer_get_time();
        s->degraded_frames++;
        if(now_us>=next_note) {
            next_note=now_us+5000000;
            LS_PRINTF(fmt_ls_cartocore_carto_map_coarser_tiles, "carto MAP: z%u drew %zu tile(s) coarser; arena peak=%zu size=%zu\n",
                      s->z,s->renderer.scene.degraded,s->arena.peak,s->arena.size);
        }
    }
    if(!ok && s->map.cellset) {
        /* Say why, at most every 5 s: a frame drawn coarse is a frame that
           looks broken, and the counter alone does not say which budget ran out. */
        static int64_t next_why;
        const int64_t now_us=esp_timer_get_time();
        if(now_us>=next_why) {
            next_why=now_us+5000000;
            printf("carto MAP: z%u full render failed; arena used=%zu peak=%zu size=%zu, falling back coarse\n",
                   s->z,s->arena.used,s->arena.peak,s->arena.size);
        }
        /* Same virtual source, lower source zoom, requested display projection.
           A stable copy outlives retained renderer state. Missing cells still
           use the normal cellset path first; this is a bounded last resort. */
        unsigned detail=s->z<s->map.zmax?s->z:s->map.zmax;
        s->coarse_map=s->map;s->coarse_map.cellset=NULL;
        while(!ok && detail>s->map.zmin) {
            s->coarse_map.zmax=--detail;
            cc_renderer_invalidate(&s->renderer);
            ok=cc_render(&s->renderer,&s->coarse_map,s->z,left,top,mode,s->labels,out);
        }
        if(ok) { s->coarse=1;s->coarse_frames++; }
    }
#ifdef LS_CARTOCORE_HOST
    s->arena.size=original_size;
#endif
#ifdef LS_CARTOCORE_HOST
    if(getenv("LSSIM_CARTO_COLD")) printf("carto host: render z%u ok=%d coarse=%d degraded=%zu arena peak=%zu size=%zu decoded peak=%zu\n",s->z,ok,s->coarse,ok && !s->coarse?s->renderer.scene.degraded:0,s->arena.peak,s->arena.size,s->decoded.peak);
#endif
    carto_hw_sample(s);
    /* Failure is transactional: invalidate scratch, keep the published frame.
       MAP retries through its existing backoff; never publish partial output. */
    if(!ok) cc_renderer_invalidate(&s->renderer);
    return ok;
}
