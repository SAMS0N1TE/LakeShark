/* TUI entry points are RAM-only, nonblocking mailbox producers. The existing
 * carto-idle task owns source I/O, rendering and deferred close/recovery. */
typedef struct {
    double lat,lon; unsigned z,serial; int cols,rows; bool braille,labels;
    bool pixels; int tcols,trows; /* picture mode: planes cols x rows, names in a tcols x trows grid */
} map_request;
static EXT_RAM_BSS_ATTR struct {
    map_request request,completed;
    char path[256]; bool select,follow,source_ready,fit_pending,frame_ready;
    double lat,lon; unsigned z,lo,hi,epoch;
    int64_t retry_after;
} map_mail;
static void map_invalidate_frame(void) { map_mail.frame_ready=false; } /* view_lock held */
/* Pictures for MAP's CartoCore view: one dot per picture pixel, painted by
   cc_pixels_rgb565 into the back buffer and swapped under frame_take. The UI
   copies the front one out, so neither side writes what the other reads. */
static EXT_RAM_BSS_ATTR struct {
    uint16_t *buf[2]; size_t cap[2]; unsigned front,serial; int w,h; double lat,lon; unsigned z; bool valid;
    size_t count,back_count; ls_carto_label names[LS_CARTO_LABEL_MAX],back_names[LS_CARTO_LABEL_MAX];
} pix;
static EXT_RAM_BSS_ATTR cc_pixel_style pix_style; /* view_lock */
static bool pix_style_set;
static uint32_t pix_render_us; /* the last picture, raster to publish */
static bool pix_commit(render_state *s,const map_request *r);
/* MAP left: the pictures go back to PSRAM. Worker only. */
static void pix_release(void) {
    while(!frame_take()) vTaskDelay(1);
    for(unsigned i=0;i<2;i++) { heap_caps_free(pix.buf[i]);pix.buf[i]=NULL;pix.cap[i]=0; }
    pix.valid=false;pix.count=0;frame_give();
}
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
        /* Shown folded to ASCII (the fonts have nothing else); matched against
         * the engine's names in the UTF-8 they are stored in, so a name with
         * Polish letters still finds its class. */
        static EXT_RAM_BSS_ATTR char utf8[128*4];
        size_t u=0;
        for(int k=0;k<n;k++) {
            uint32_t cp=cells[y*cols+start+k].codepoint;
            l->text[k]=cp!=127?tui_fold_codepoint(cp):'?';
            if(cp<0x80) utf8[u++]=(char)cp;
            else if(cp<0x800) { utf8[u++]=(char)(0xC0|cp>>6);utf8[u++]=(char)(0x80|(cp&63)); }
            else if(cp<0x10000) { utf8[u++]=(char)(0xE0|cp>>12);utf8[u++]=(char)(0x80|((cp>>6)&63));utf8[u++]=(char)(0x80|(cp&63)); }
            else { utf8[u++]=(char)(0xF0|cp>>18);utf8[u++]=(char)(0x80|((cp>>12)&63));utf8[u++]=(char)(0x80|((cp>>6)&63));utf8[u++]=(char)(0x80|(cp&63)); }
        }
        l->text[n]=0;
        unsigned index=cc_index16(c->fg);
        l->label_class=index==14?3:index==11?4:2;
        /* Preserve engine metadata before the clean terrain render clears it.
         * Hash lookup is bounded by the existing label table; no extra arena. */
        const cc_planes *p=&state->renderer.planes;
        for(unsigned rank=1;rank<=5 && p->label_capacity;rank++) {
            uint64_t h=UINT64_C(14695981039346656037)^rank;
            for(size_t k=0;k<u;k++) h=(h^(uint8_t)utf8[k])*UINT64_C(1099511628211);
            size_t slot=(size_t)h&(p->label_capacity-1);
            for(size_t tries=0;tries<p->label_capacity && p->label_seen[slot];tries++) {
                const cc_feature *f=p->label_seen[slot];
                if(f->label_class==rank && f->name.size==u && !memcmp(f->name.data,utf8,u)) {
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
    /* A few tries before giving up: the renderer holds this lock in short spells. */
    bool got=false;
    for(unsigned i=0;view_lock && i<2000 && !(got=xSemaphoreTake(view_lock,0)==pdTRUE);i++) __asm__ volatile("" ::: "memory");
    if(!got) return 0;
    size_t n=map_file_count<cap?map_file_count:cap;
    memcpy(paths,map_files,n*sizeof(map_files[0]));xSemaphoreGive(view_lock);return n;
}
static void map_scan_files(void) {
    xSemaphoreTake(file_lock,portMAX_DELAY);xSemaphoreTake(view_lock,portMAX_DELAY);
    map_file_count=0;
    struct stat cst; /* downloaded states first: TILES > STATES */
    if(!stat("/sdcard/maps/cells/catalog.ccm",&cst) || !stat("/sdcard/maps/cells/index.cci",&cst))
        snprintf(map_files[map_file_count++],sizeof(map_files[0]),"/sdcard/maps/cells");
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
/* MAP's STYLE over CartoCore frames (2026-10-20: the CartoCore views ignored
   it, so NIGHT and the night-vision tints only worked in FIELD). Colours are
   the theme's 16, so a style is a remap of each cell as it is copied out. */
static uint8_t carto_style_tint;    /* 0, or the one TUI colour everything takes */
static uint8_t carto_style_ground;  /* 0 theme, 1 dark, 2 light (PAPER) */
void ls_carto_map_style(uint8_t tint,uint8_t ground) { carto_style_tint=tint&7; carto_style_ground=ground; }
static uint8_t carto_style_attr(uint8_t a) {
    uint8_t fg=TUI_ATTR_FG(a), bg=TUI_ATTR_BG(a);
    if(carto_style_tint) {
        /* night vision: one hue, bright where the map is bright, on black */
        fg=(uint8_t)(carto_style_tint|(fg&TUI_BRIGHT || (fg&7)==TUI_WHITE?TUI_BRIGHT:0));
        if((a&0x0F)==TUI_BLACK) fg=TUI_BLACK;
        bg=TUI_BLACK;
    } else if(carto_style_ground==1) {
        bg&=7;                        /* fills drop to their dim shade */
        if(bg==TUI_WHITE) bg=TUI_BLACK;
    } else if(carto_style_ground==2) {
        /* PAPER: dark ink on a light ground */
        if(bg==TUI_BLACK || (bg&7)==TUI_WHITE) bg=TUI_WHITE;
        fg&=7;
        if(fg==TUI_WHITE || fg==TUI_YELLOW) fg=TUI_BLACK;
        if(fg==bg) fg=TUI_BLACK;
    }
    return TUI_ATTR(fg,bg);
}
bool ls_carto_map_draw(tui_surface *sf,tui_rect area,double lat,double lon,unsigned z,bool braille,bool labels) {
    ui_map_fresh=false;
    if(!view_lock || area.w<1 || area.h<1 || z>22 || !atomic_load(&map_wanted)) return false;
    if(xSemaphoreTake(view_lock,0)==pdTRUE) {
        map_request *r=&map_mail.request;
        if(r->pixels || r->lat!=lat || r->lon!=lon || r->z!=z || r->cols!=area.w || r->rows!=area.h ||
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
        /* Same place and zoom in an area of another size (HIDE and SHOW): the
           frame is exactly the middle of the new view, so it goes there unscaled
           and the new edges wait as bare ground. Stretched to fit, it was a
           different looking map until the new frame came. Markers keep their
           places; the place names wait for the frame of this size. */
        if(ready && !ui_map_fresh && f->lat==lat && f->lon==lon && f->z==z && f->braille==braille) {
            const int ox=(area.w-f->cols)/2,oy=(area.h-f->rows)/2;
            uint8_t ground=f->cells[0].attr;
            if(carto_style_tint || carto_style_ground) ground=carto_style_attr(ground);
            ground=TUI_ATTR(TUI_ATTR_BG(ground),TUI_ATTR_BG(ground));
            for(int y=0;y<area.h;y++) for(int x=0;x<area.w;x++) {
                if(!tui_rect_contains(tui_surface_rect(sf),area.x+x,area.y+y)) continue;
                const int fx=x-ox,fy=y-oy;
                tui_cell c={' ',ground};
                if(fx>=0 && fy>=0 && fx<f->cols && fy<f->rows) {
                    c=f->cells[fy*f->cols+fx];
                    if(carto_style_tint || carto_style_ground) c.attr=carto_style_attr(c.attr);
                }
                sf->back[(area.y+y)*sf->w+area.x+x]=c;
            }
            ui_name_count=0;ui_map_area=area;ui_map_drawn=true;ui_map_fresh=true;
            frame_give();return true;
        }
        if(ready && (ui_map_fresh || !held)) {
            for(int y=0;y<area.h;y++) for(int x=0;x<area.w;x++)
                if(tui_rect_contains(tui_surface_rect(sf),area.x+x,area.y+y))
                    { tui_cell c=f->cells[(y*f->rows/area.h)*f->cols+x*f->cols/area.w];
                      if(carto_style_tint || carto_style_ground) c.attr=carto_style_attr(c.attr);
                      sf->back[(area.y+y)*sf->w+area.x+x]=c; }
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
static uint32_t pix_mix(uint32_t a,uint32_t b,int n,int d) {
    uint32_t out=0;
    for(int k=0;k<24;k+=8) { int x=(int)((a>>k)&255),y=(int)((b>>k)&255);out|=(uint32_t)(x+(y-x)*n/d)<<k; }
    return out;
}
/* MAP's palette (ground, water, park, building, road_lo, road_hi, major) to
   one colour per CartoCore ink, ranked the way FIELD draws them. */
void ls_carto_map_pixel_style(const uint32_t pal[7],int relief) {
    const uint32_t ground=pal[0],water=pal[1],park=pal[2],building=pal[3],lo=pal[4],hi=pal[5],major=pal[6];
    uint32_t rgb[CC_INK_COUNT];
    rgb[CC_NONE]=rgb[CC_LAND]=ground;
    rgb[CC_RESIDENTIAL]=pix_mix(ground,building,1,4);
    rgb[CC_INDUSTRIAL]=pix_mix(ground,building,1,2);
    rgb[CC_FARM]=pix_mix(ground,park,1,3);
    rgb[CC_PARK]=park;
    rgb[CC_WOOD]=pix_mix(ground,park,7,8);
    rgb[CC_WATER]=water;
    rgb[CC_BUILDING]=building;
    rgb[CC_SERVICE]=pix_mix(ground,lo,2,3);
    rgb[CC_MINOR]=lo;
    rgb[CC_SECONDARY]=pix_mix(lo,hi,1,2);
    rgb[CC_MAJOR]=major;
    rgb[CC_MOTORWAY]=pix_mix(major,0xffffff,1,4);
    rgb[CC_RAIL]=pix_mix(building,lo,1,2);
    rgb[CC_BOUNDARY]=pix_mix(ground,lo,1,2);
    rgb[CC_CONTOUR]=pix_mix(ground,park,1,2);
    rgb[CC_INDEX_CONTOUR]=park;
    const uint32_t river=pix_mix(water,0xffffff,1,3);
    if(!view_lock || xSemaphoreTake(view_lock,0)!=pdTRUE) return;
    bool same=pix_style_set && pix_style.river==river && pix_style.relief==relief;
    for(unsigned k=0;k<CC_INK_COUNT && same;k++) same=pix_style.rgb[k]==rgb[k];
    if(!same) {
        for(unsigned k=0;k<CC_INK_COUNT;k++) pix_style.rgb[k]=rgb[k];
        pix_style.river=river;pix_style.relief=relief;
        pix_style_set=true;map_mail.request.serial++;map_mail.retry_after=0;map_mail.frame_ready=false;
    }
    xSemaphoreGive(view_lock);
}
bool ls_carto_map_pixels(double lat,double lon,unsigned z,int pw,int ph,int tw,int th,bool labels,
                         uint16_t *dst,uint32_t *serial,double *flat,double *flon,unsigned *fz) {
    ui_map_fresh=false;
    if(!view_lock || pw<2 || ph<4 || z>22 || !atomic_load(&map_wanted)) return false;
    if(xSemaphoreTake(view_lock,0)==pdTRUE) {
        map_request *r=&map_mail.request;
        /* Planes come in 2x4 dots: render up to the next whole cell, copy out pw x ph. */
        const int cols=(pw+1)/2,rows=(ph+3)/4;
        if(!r->pixels || r->lat!=lat || r->lon!=lon || r->z!=z || r->cols!=cols || r->rows!=rows ||
           r->tcols!=tw || r->trows!=th || r->labels!=labels) {
            *r=(map_request){lat,lon,z,r->serial+1,cols,rows,false,labels,true,tw,th};
            map_mail.retry_after=0;
        }
        xSemaphoreGive(view_lock);
    }
    bool have=false;
    if(frame_take()) {
        if(pix.valid && pix.w==(pw+1)/2*2 && pix.h==(ph+3)/4*4) {
            if(*serial!=pix.serial) {
                for(int y=0;y<ph;y++) memcpy(dst+(size_t)y*pw,pix.buf[pix.front]+(size_t)y*pix.w,(size_t)pw*sizeof(uint16_t));
                *serial=pix.serial;
                ui_name_count=pix.count;memcpy(ui_names,pix.names,pix.count*sizeof(ui_names[0]));
            }
            ui_map_fresh=pix.lat==lat && pix.lon==lon && pix.z==z;
            *flat=pix.lat;*flon=pix.lon;*fz=pix.z;
            ui_map_area=tui_rect_make(0,0,tw,th);ui_map_drawn=true;have=true;
        }
        frame_give();
    }
    return have;
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
    strcpy(map_mail.path,path);map_mail.select=true;map_mail.follow=false;map_mail.retry_after=0;
    atomic_store(&map_wanted,true);atomic_fetch_add(&map_epoch,1);atomic_store(&map_status,0);
    xSemaphoreGive(view_lock);return true;
}
/* Worker-only. This is deliberately outside the call graph of all TUI APIs. */
static void map_service_locked(void) {
    if(atomic_load(&view_active)) return; /* console overlay owns the shared renderer */
    if(!atomic_load(&map_wanted)) {
        if(map_worker_session) { close_hidden();pix_release();map_worker_session=false; }
        return;
    }
    if(xSemaphoreTake(view_lock,0)!=pdTRUE) return;
    unsigned epoch=atomic_load(&map_epoch);
    map_request r=map_mail.request;
    bool prepare=!map_prepared || !map_mail.source_ready || map_mail.epoch!=epoch;
    if(esp_timer_get_time()<map_mail.retry_after) { xSemaphoreGive(view_lock);return; }
    bool select=map_mail.select;map_mail.select=false;
    /* A switch made by following the view keeps the view as it is. */
    bool follow=map_mail.follow;map_mail.follow=false;
    if(select) strcpy(map_worker_path,map_mail.path);
    xSemaphoreGive(view_lock);
    if(prepare) {
        map_worker_session=true;
        map_scan_files();
        if(map_owned) close_hidden();
        int failed=select?select_source(map_worker_path):prepare_source(r.lat,r.lon,r.z);
        xSemaphoreTake(view_lock,portMAX_DELAY);
        cc_ctile map;
        bool ok=!failed && open_map(&map) && carto_fit_map(&map,&r.lat,&r.lon,&r.z,!follow);
        if(epoch==atomic_load(&map_epoch) && atomic_load(&map_wanted)) {
            map_mail.epoch=epoch;map_mail.source_ready=map_prepared=ok;
            map_mail.frame_ready=false;map_mail.fit_pending=ok && !follow;
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
    /* Follow the view: onto downloaded state cells when it enters one, and
       back to a single-file map that covers it when it leaves them. Checked
       once per z8 cell the centre moves into, so panning costs one stat.
       Leaving them goes only to a map that covers the view at this zoom. */
    {
        static unsigned follow_key=~0u;
        unsigned key=(unsigned)floor((r.lon+180.0)/360.0*256.0)*256u+
            (unsigned)floor((1.0-asinh(tan(r.lat*3.14159265358979323846/180.0))/3.14159265358979323846)/2.0*256.0)+
            r.z*65536u; /* a zoom change asks again: a map refused at z4 may draw z12 */
        if(key!=follow_key) {
            follow_key=key;
            static char want[256]; want[0]=0;
            if(cells_cover(r.lat,r.lon)) { if(!source_is_cells()) snprintf(want,sizeof(want),"cells"); }
            else if(source_is_cells() && !carto_file_covering("/sdcard/maps",r.lat,r.lon,r.z,want,sizeof(want))) want[0]=0;
            if(want[0] && strlen(want)<sizeof(map_mail.path)) {
                printf("carto MAP: follow -> %s at z%u\n",want,r.z);
                strcpy(map_mail.path,want);map_mail.select=map_mail.follow=true;map_mail.retry_after=0;
                atomic_fetch_add(&map_epoch,1);atomic_store(&map_status,0);
                xSemaphoreGive(view_lock);return;
            }
        }
    }
    if(!map_owned || !shown || shown->cols!=r.cols || shown->rows!=r.rows) {
        release(shown);shown=NULL;
        shown=create(r.cols,r.rows,r.z,true,true);map_owned=true;
    }
    bool ok=false;
    if(shown) {
        shown->z=r.z;shown->labels=r.pixels?0:r.labels;shown->raster_only=r.pixels;centre_lat=r.lat;centre_lon=r.lon;
        origin(r.cols,r.rows,r.z,&shown->left,&shown->top);
        cc_mode mode=r.braille?CC_BRAILLE:CC_QUADRANT;
        const int64_t render_start=esp_timer_get_time();
        ok=render_frame(shown,mode,CC_EDGES_SMOOTH,shown->left,shown->top);
        map_label_count=0;
        if(r.pixels) {
            if(ok) ok=pix_commit(shown,&r);
            pix_render_us=(uint32_t)(esp_timer_get_time()-render_start);
            if(!ok) carto_failure("picture",r.cols,r.rows,r.z);
            if(ok) { map_mail.completed=r;shown->hint_count=shown->hint_next=0; }
        } else if(ok && r.labels) {
            map_collect_labels(shown);
            /* Publish clean terrain; MAP places whole names around its own markers. */
            shown->labels=0;
            ok=render_frame(shown,mode,CC_EDGES_SMOOTH,shown->left,shown->top);
            shown->labels=r.labels;
        }
#ifdef LS_CARTOCORE_HOST
        if(!ok)printf("render failure arena peak=%zu used=%zu\n",shown->arena.peak,shown->arena.used);
#endif
        if(!r.pixels) {
        if(!ok) carto_failure("render/decode",r.cols,r.rows,r.z);
        if(ok) ok=frame_commit(shown,false,r.lat,r.lon,r.braille,r.labels,map_labels,map_label_count);
        if(!ok) carto_failure("complete frame publication",r.cols,r.rows,r.z);
        }
        if(ok && !r.pixels) {
            map_mail.completed=r;
            shown->hint_count=shown->hint_next=0;
            if(shown->map.source) cc_ctile_prefetch_hint(&shown->map,r.z,shown->left,shown->top,r.cols,r.rows,0,0,queue_hint,shown);
        }
    }
    map_mail.frame_ready=ok;atomic_store(&map_status,ok?1:(carto_memory_failed?-3:-1));
    if(!ok) map_mail.retry_after=esp_timer_get_time()+5000000;
    xSemaphoreGive(view_lock);
}

/* Place, water and road names out of the scene, for MAP to place among its
   own markers: one per name, at its anchor (a line's middle point), in the
   UI's cell grid. Worker only. */
static size_t pix_labels(const render_state *s,int tw,int th,ls_carto_label *out,size_t cap) {
    const cc_scene *sc=&s->renderer.scene;const int w=s->cols*2,h=s->rows*4;size_t n=0;
    if(tw<1 || th<1) return 0;
    for(unsigned rank=1;rank<=5 && n<cap;rank++) for(unsigned pr=0;pr<16 && n<cap;pr++)
        for(const cc_feature *f=sc->labels_indexed?sc->label_first[rank][pr]:sc->first[pr];f && n<cap;
            f=sc->labels_indexed?f->label_next:f->next) {
            if(f->label_class!=rank || !f->name.size) continue;
            cc_point a=f->anchor;
            if(f->type!=1 && f->type!=3) {
                if(!f->count || !f->paths[0].count) continue;
                a=cc_feature_point(f,&f->paths[0],f->paths[0].count/2);
            }
            if(a.x<0 || a.y<0 || a.x>=w || a.y>=h) continue;
            ls_carto_label *l=&out[n];size_t at=0;int k=0;
            while(at<f->name.size && k<(int)sizeof(l->text)-1) {
                uint32_t c=f->name.data[at++];unsigned extra=c>=0xf0?3:c>=0xe0?2:c>=0xc0?1:0;
                if(extra) { c&=0x3f>>extra; while(extra-- && at<f->name.size) c=(c<<6)|(f->name.data[at++]&63); }
                l->text[k++]=c>=32 && c!=127?(char)tui_fold_codepoint(c):'?';
            }
            l->text[k]=0;
            bool seen=false;for(size_t j=0;j<n && !seen;j++) seen=!strcmp(out[j].text,l->text);
            if(seen || !k) continue;
            l->x=(int16_t)(a.x*tw/w-k/2);l->y=(int16_t)(a.y*th/h);
            l->attr=TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK);l->label_class=(uint8_t)rank;n++;
        }
    return n;
}
static bool pix_commit(render_state *s,const map_request *r) {
    const unsigned back=pix.front^1;const size_t need=(size_t)s->cols*2*s->rows*4;
    if(pix.cap[back]<need) {
        uint16_t *p=heap_caps_malloc(need*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!p) { carto_memory_failed=true;return false; }
        heap_caps_free(pix.buf[back]);pix.buf[back]=p;pix.cap[back]=need;
    }
    if(!pix_style_set) return false;
    cc_pixels_rgb565(&s->renderer.planes,&pix_style,pix.buf[back]);
    pix.back_count=r->labels?pix_labels(s,r->tcols,r->trows,pix.back_names,LS_CARTO_LABEL_MAX):0;
    while(!frame_take()) vTaskDelay(1);
    pix.front=back;pix.w=s->cols*2;pix.h=s->rows*4;pix.lat=r->lat;pix.lon=r->lon;pix.z=s->z;
    pix.count=pix.back_count;memcpy(pix.names,pix.back_names,pix.count*sizeof(pix.names[0]));
    pix.serial++;pix.valid=true;frame_give();return true;
}
static void map_service(void) {
    if(xSemaphoreTake(command_lock,0)!=pdTRUE) return;
    map_service_locked();xSemaphoreGive(command_lock);
}
