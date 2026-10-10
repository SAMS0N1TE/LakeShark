/* TUI entry points are RAM-only, nonblocking mailbox producers. The existing
 * carto-idle task owns source I/O, rendering and deferred close/recovery. */
typedef struct {
    double lat,lon; unsigned z,serial; int cols,rows; bool braille,labels;
} map_request;
static EXT_RAM_BSS_ATTR struct {
    map_request request,completed;
    char path[256]; bool select,source_ready,fit_pending,frame_ready;
    double lat,lon; unsigned z,lo,hi,epoch;
    int64_t retry_after;
} map_mail;
static void map_invalidate_frame(void) { map_mail.frame_ready=false; } /* view_lock held */
static EXT_RAM_BSS_ATTR ls_carto_label map_labels[LS_CARTO_LABEL_MAX];
static size_t map_label_count;
/* Called with view_lock held, on the worker. Text spaces retain their plate. */
static void map_collect_labels(render_state *state) {
    const cc_cell *cells=state->cells;int cols=state->cols,rows=state->rows;
    map_label_count=0;
    for(int y=0;y<rows;y++) for(int x=0;x<cols;) {
        const cc_cell *c=&cells[y*cols+x];
        if(c->codepoint<=32 || c->codepoint>=0x2580) { x++;continue; }
        int start=x;
        while(x<cols && cells[y*cols+x].codepoint>=32 && cells[y*cols+x].codepoint<0x2580 &&
              cells[y*cols+x].fg==c->fg && cells[y*cols+x].bg==c->bg) x++;
        int n=x-start;
        while(n>0 && cells[y*cols+start+n-1].codepoint==32) n--;
        if(!n || n>=128 || map_label_count>=LS_CARTO_LABEL_MAX) continue;
        ls_carto_label *l=&map_labels[map_label_count++];
        l->x=start;l->y=y;l->attr=carto_attr(*c);
        for(int k=0;k<n;k++) { uint32_t cp=cells[y*cols+start+k].codepoint;l->text[k]=cp<127?(char)cp:'?'; }
        l->text[n]=0;
        unsigned index=cc_index16(c->fg);
        l->label_class=index==14?3:index==11?4:2;
        /* Preserve engine metadata before the clean terrain render clears it.
         * Hash lookup is bounded by the existing label table; no extra arena. */
        const cc_planes *p=&state->renderer.planes;
        for(unsigned rank=1;rank<=5 && p->label_capacity;rank++) {
            uint64_t h=UINT64_C(14695981039346656037)^rank;
            for(int k=0;k<n;k++) h=(h^(uint8_t)l->text[k])*UINT64_C(1099511628211);
            size_t slot=(size_t)h&(p->label_capacity-1);
            for(size_t tries=0;tries<p->label_capacity && p->label_seen[slot];tries++) {
                const cc_feature *f=p->label_seen[slot];
                if(f->label_class==rank && f->name.size==(size_t)n && !memcmp(f->name.data,l->text,n)) {
                    l->label_class=rank;break;
                }
                slot=(slot+1)&(p->label_capacity-1);
            }
        }
    }
}
static atomic_bool map_wanted;
static atomic_uint map_epoch;
static bool map_worker_session; /* worker-owned, includes cancelled opens */
static atomic_int map_status; /* 0 opening, 1 ready, -1 failed, -2 absent, -3 memory */
static EXT_RAM_BSS_ATTR char map_worker_path[256]; /* sole idle worker */

static EXT_RAM_BSS_ATTR char map_files[32][128];
static size_t map_file_count;
static EXT_RAM_BSS_ATTR ls_carto_label ui_names[LS_CARTO_LABEL_MAX];
static size_t ui_name_count;
static bool ui_map_drawn,ui_map_fresh;
static tui_rect ui_map_area;
bool ls_carto_map_fresh(void) { return ui_map_fresh; }
size_t ls_carto_map_labels(ls_carto_label *labels,size_t cap,int cols,int rows) {
    size_t n=ui_map_fresh && cols==ui_map_area.w && rows==ui_map_area.h?ui_name_count:0;
    if(n>cap) n=cap;
    memcpy(labels,ui_names,n*sizeof(*labels));return n;
}
size_t ls_carto_map_files(char (*paths)[128],size_t cap) {
    if(!view_lock || xSemaphoreTake(view_lock,0)!=pdTRUE) return 0;
    size_t n=map_file_count<cap?map_file_count:cap;
    memcpy(paths,map_files,n*sizeof(map_files[0]));xSemaphoreGive(view_lock);return n;
}
static void map_scan_files(void) {
    xSemaphoreTake(file_lock,portMAX_DELAY);xSemaphoreTake(view_lock,portMAX_DELAY);
    map_file_count=0;
    DIR *d=opendir("/sdcard/maps");struct dirent *e;
    if(d) {
        while((e=readdir(d)) && map_file_count<32) {
            size_t n=strlen(e->d_name);
            if(n<=6 || n+14>sizeof(map_files[0]) || strcasecmp(e->d_name+n-6,".ctile")) continue;
            snprintf(map_files[map_file_count++],sizeof(map_files[0]),"/sdcard/maps/%s",e->d_name);
        }
        closedir(d);
    }
    xSemaphoreGive(view_lock);xSemaphoreGive(file_lock);
}

const char *ls_carto_map_status(void) {
    int status=atomic_load(&map_status);
    return status==-3?"Not enough memory for map":status==-2?"No map installed":status<0?"CartoCore map unavailable":"opening map...";
}
bool ls_carto_map_prepare(double *lat,double *lon,unsigned *z) {
    if(!view_lock || xSemaphoreTake(view_lock,0)!=pdTRUE) return false;
    if(!atomic_exchange(&map_wanted,true)) {
        atomic_fetch_add(&map_epoch,1);map_mail.request.cols=0;map_mail.retry_after=0;
        map_mail.request.lat=*lat;map_mail.request.lon=*lon;map_mail.request.z=*z;
        atomic_store(&map_status,0);
    }
    bool ready=map_mail.source_ready && map_prepared && map_mail.epoch==atomic_load(&map_epoch);
    if(ready) {
        if(map_mail.fit_pending) {
            *lat=map_mail.lat;*lon=map_mail.lon;*z=map_mail.z;map_mail.fit_pending=false;
        }
        if(*z<map_mail.lo) *z=map_mail.lo;
        if(*z>map_mail.hi) *z=map_mail.hi;
    }
    xSemaphoreGive(view_lock);return ready;
}
bool ls_carto_map_draw(tui_surface *sf,tui_rect area,double lat,double lon,unsigned z,bool braille,bool labels) {
    ui_map_fresh=false;
    if(!view_lock || area.w<1 || area.h<1 || z>22 || !atomic_load(&map_wanted)) return false;
    if(xSemaphoreTake(view_lock,0)==pdTRUE) {
        map_request *r=&map_mail.request;
        if(r->lat!=lat || r->lon!=lon || r->z!=z || r->cols!=area.w || r->rows!=area.h ||
           r->braille!=braille || r->labels!=labels) {
            *r=(map_request){lat,lon,z,r->serial+1,area.w,area.h,braille,labels};
            map_mail.retry_after=0;
        }
        xSemaphoreGive(view_lock);
    }
    bool held=ui_map_drawn && area.x==ui_map_area.x && area.y==ui_map_area.y &&
        area.w==ui_map_area.w && area.h==ui_map_area.h;
    if(frame_take()) {
        const carto_frame *f=&frames[frame_front];
        bool ready=atomic_load(&frame_valid) && !f->console && f->cells;
        ui_map_fresh=ready && f->lat==lat && f->lon==lon && f->z==z && f->braille==braille &&
            f->labels==labels && f->cols==area.w && f->rows==area.h;
        if(ready && (ui_map_fresh || !held)) {
            for(int y=0;y<area.h;y++) for(int x=0;x<area.w;x++)
                if(tui_rect_contains(tui_surface_rect(sf),area.x+x,area.y+y))
                    sf->back[(area.y+y)*sf->w+area.x+x]=f->cells[(y*f->rows/area.h)*f->cols+x*f->cols/area.w];
            ui_name_count=f->count;memcpy(ui_names,f->names,f->count*sizeof(*ui_names));
            ui_map_area=area;ui_map_drawn=true;frame_give();return true;
        }
        frame_give();
    }
    if(held) for(int y=0;y<area.h;y++) for(int x=0;x<area.w;x++) {
        if(!tui_rect_contains(tui_surface_rect(sf),area.x+x,area.y+y)) continue;
        size_t i=(area.y+y)*sf->w+area.x+x;sf->back[i]=sf->front[i];
    }
    return held;
}
void ls_carto_map_leave(void) {
    ui_map_drawn=ui_map_fresh=false;ui_name_count=0;
    atomic_store(&map_wanted,false);atomic_fetch_add(&map_epoch,1);
}
bool ls_carto_map_limits(unsigned *lo,unsigned *hi) {
    if(!view_lock || xSemaphoreTake(view_lock,0)!=pdTRUE) return false;
    bool ok=map_mail.source_ready && map_prepared && map_mail.epoch==atomic_load(&map_epoch);
    if(ok) { *lo=map_mail.lo;*hi=map_mail.hi; }
    xSemaphoreGive(view_lock);return ok;
}
bool ls_carto_map_select(const char *path) {
    if(!view_lock || !path || strlen(path)>=sizeof(map_mail.path) || xSemaphoreTake(view_lock,0)!=pdTRUE) return false;
    strcpy(map_mail.path,path);map_mail.select=true;map_mail.retry_after=0;
    atomic_store(&map_wanted,true);atomic_fetch_add(&map_epoch,1);atomic_store(&map_status,0);
    xSemaphoreGive(view_lock);return true;
}
/* Worker-only. This is deliberately outside the call graph of all TUI APIs. */
static void map_service_locked(void) {
    if(atomic_load(&view_active)) return; /* console overlay owns the shared renderer */
    if(!atomic_load(&map_wanted)) {
        if(map_worker_session) { close_hidden();map_worker_session=false; }
        return;
    }
    if(xSemaphoreTake(view_lock,0)!=pdTRUE) return;
    unsigned epoch=atomic_load(&map_epoch);
    map_request r=map_mail.request;
    bool prepare=!map_prepared || !map_mail.source_ready || map_mail.epoch!=epoch;
    if(esp_timer_get_time()<map_mail.retry_after) { xSemaphoreGive(view_lock);return; }
    bool select=map_mail.select;map_mail.select=false;
    if(select) strcpy(map_worker_path,map_mail.path);
    xSemaphoreGive(view_lock);
    if(prepare) {
        map_worker_session=true;
        map_scan_files();
        if(map_owned) close_hidden();
        int failed=select?select_source(map_worker_path):prepare_source(r.lat,r.lon);
        xSemaphoreTake(view_lock,portMAX_DELAY);
        cc_ctile map;
        bool ok=!failed && open_map(&map) && carto_fit_map(&map,&r.lat,&r.lon,&r.z,true);
        if(epoch==atomic_load(&map_epoch) && atomic_load(&map_wanted)) {
            map_mail.epoch=epoch;map_mail.source_ready=map_prepared=ok;
            map_mail.frame_ready=false;map_mail.fit_pending=ok;
            if(ok) {
                map_mail.lat=r.lat;map_mail.lon=r.lon;map_mail.z=r.z;
                map_mail.lo=map.zmin;map_mail.hi=(map.source?map.source->header[8]:map.bytes.data[8])==7?16:map.zmax;map_mail.retry_after=0;
                atomic_store(&map_status,0);
            } else {
                map_mail.retry_after=esp_timer_get_time()+5000000;atomic_store(&map_status,sd_map?-1:-2);
                carto_failure("source selection/open",r.cols,r.rows,r.z);
            }
        }
        xSemaphoreGive(view_lock);return;
    }
    xSemaphoreTake(view_lock,portMAX_DELAY);
    if(epoch!=atomic_load(&map_epoch) || !atomic_load(&map_wanted) || r.cols<1 || r.rows<1 ||
       map_mail.fit_pending || (map_mail.frame_ready && map_mail.completed.serial==r.serial)) {
        xSemaphoreGive(view_lock);return;
    }
    if(!map_owned || !shown || shown->cols!=r.cols || shown->rows!=r.rows) {
        release(shown);shown=NULL;
        shown=create(r.cols,r.rows,r.z,true,true);map_owned=true;
    }
    bool ok=false;
    if(shown) {
        shown->z=r.z;shown->labels=r.labels;centre_lat=r.lat;centre_lon=r.lon;
        origin(r.cols,r.rows,r.z,&shown->left,&shown->top);
        cc_mode mode=r.braille?CC_BRAILLE:CC_QUADRANT;
        ok=render_frame(shown,mode,CC_EDGES_SMOOTH,shown->left,shown->top);
        map_label_count=0;
        if(ok && r.labels) {
            map_collect_labels(shown);
            /* Publish clean terrain; MAP places whole names around its own markers. */
            shown->labels=0;
            ok=render_frame(shown,mode,CC_EDGES_SMOOTH,shown->left,shown->top);
            shown->labels=r.labels;
        }
#ifdef LS_CARTOCORE_HOST
        if(!ok)printf("render failure arena peak=%zu used=%zu\n",shown->arena.peak,shown->arena.used);
#endif
        if(!ok) carto_failure("render/decode",r.cols,r.rows,r.z);
        if(ok) ok=frame_commit(shown,false,r.lat,r.lon,r.braille,r.labels,map_labels,map_label_count);
        if(!ok) carto_failure("complete frame publication",r.cols,r.rows,r.z);
        if(ok) {
            map_mail.completed=r;
            shown->hint_count=shown->hint_next=0;
            if(shown->map.source) cc_ctile_prefetch_hint(&shown->map,r.z,shown->left,shown->top,r.cols,r.rows,0,0,queue_hint,shown);
        }
    }
    map_mail.frame_ready=ok;atomic_store(&map_status,ok?1:(carto_memory_failed?-3:-1));
    if(!ok) map_mail.retry_after=esp_timer_get_time()+5000000;
    xSemaphoreGive(view_lock);
}

static void map_service(void) {
    if(xSemaphoreTake(command_lock,0)!=pdTRUE) return;
    map_service_locked();xSemaphoreGive(command_lock);
}
