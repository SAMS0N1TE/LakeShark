/* State metadata and entitlement policy. Included on the serialized cells worker. */
static bool cells_record(uint32_t,const char *,const uint8_t *,bool);
static bool cells_delete(uint32_t);
static void cells_library(void);
typedef struct { char code[3],name[64];bool partial;uint8_t cover[CELLS_BITS];unsigned count;uint64_t bytes,base_bytes,detail_bytes; } cells_state_row;
static EXT_RAM_BSS_ATTR cells_state_row cells_states[52];
static unsigned cells_states_count;
static bool cells_states_dirty;
/* Optional LSOUT1 asset advertised beside the states; owned by carto-idle. */
typedef struct { uint32_t bytes;uint8_t sha[32]; } cells_outline_asset;
static EXT_RAM_BSS_ATTR cells_outline_asset cells_outlines;
static bool cells_outlines_pending;
static bool cells_outlines_parse(const cells_jparser *p,int object,cells_outline_asset *out) {
    char file[32],hex[65];double bytes;
    if(!cells_jstring(p,cells_jget(p,object,"file"),file,sizeof(file)) || strcmp(file,"places/outlines.lso") ||
       !cells_jnumber(p,cells_jget(p,object,"bytes"),&bytes) || bytes<20 || bytes>4*1024*1024 || floor(bytes)!=bytes ||
       !cells_jstring(p,cells_jget(p,object,"sha256"),hex,sizeof(hex)) || strlen(hex)!=64)return false;
    for(unsigned i=0;i<32;i++) {
        unsigned char a=hex[2*i],b=hex[2*i+1];
        if(!isxdigit(a) || !isxdigit(b))return false;
        out->sha[i]=(uint8_t)(((a<='9'?a-'0':(a|32)-'a'+10)<<4)|(b<='9'?b-'0':(b|32)-'a'+10));
    }
    out->bytes=(uint32_t)bytes;return true;
}
static bool cells_outlines_needed(void) {
    return cells_outlines.bytes && !cells_matches(CELLS_ROOT "outlines.lso",cells_outlines.bytes,cells_outlines.sha);
}
static EXT_RAM_BSS_ATTR char cells_token[192];
#ifndef LS_CARTOCORE_HOST
static esp_err_t cells_token_job(void *unused) {
    (void)unused;nvs_handle_t n;char token[sizeof(cells_token)];
    esp_err_t e=nvs_open("p25packs",NVS_READONLY,&n);
    if(e==ESP_OK) {
        size_t bytes=sizeof(token);e=nvs_get_str(n,"token",token,&bytes);nvs_close(n);
        if(e==ESP_OK)snprintf(cells_token,sizeof(cells_token),"%s",token);
    }
    if(e==ESP_ERR_NVS_NOT_FOUND)cells_token[0]=0;
    return e;
}
#endif
static bool cells_token_load(void) {
#ifdef LS_CARTOCORE_HOST
    const char *token=getenv("LSSIM_MAPS_TOKEN");snprintf(cells_token,sizeof(cells_token),"%s",token?token:"");
#else
    esp_err_t e=ls_nvs_run(cells_token_job,NULL,0);
    if(e!=ESP_OK && e!=ESP_ERR_NVS_NOT_FOUND)return cells_plus_work;
#endif
    /* Header values cannot contain HTTP delimiters. */
    for(const char *p=cells_token;*p;p++)if((unsigned char)*p<32 || (unsigned char)*p==127) { cells_token[0]=0;break; }
    cells_plus_work=cells_token[0]!=0;
    return cells_plus_work;
}
// States contain cell coordinates, not per-part rows. Bound transient PSRAM
// to 1 MiB text + 65536 tokens (about 1.25 MiB), released after parsing.
#define CELLS_STATES_JSON_BYTES (1024u*1024u)
#define CELLS_STATES_JSON_TOKENS 65536u
static bool cells_json_bounds(const char *s,size_t n) {
    unsigned depth=0;bool quoted=false,escape=false;
    if(n>CELLS_STATES_JSON_BYTES)return false;
    for(size_t i=0;i<n;i++) {
        char c=s[i];if(quoted) { if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quoted=false;continue; }
        if(c=='"')quoted=true;
        else if(c=='{' || c=='[') { if(++depth>8)return false; }
        else if(c=='}' || c==']') { if(!depth)return false;depth--; }
    }return !depth && !quoted;
}
static bool cells_states_parse(const char *path,bool activate) {
    FILE *f=cells_fopen(path,"rb");if(!f)return false;
    struct stat st;bool ok=!fstat(fileno(f),&st) && st.st_size>2 && (uint64_t)st.st_size<=CELLS_STATES_JSON_BYTES;
    char *text=ok?heap_caps_malloc((size_t)st.st_size+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    if(!text) { fclose(f);return false; }
    ok=fread(text,1,(size_t)st.st_size,f)==(size_t)st.st_size;fclose(f);text[st.st_size]=0;
    cells_jtoken *tokens=heap_caps_calloc(CELLS_STATES_JSON_TOKENS,sizeof(*tokens),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    cells_jparser parser={.s=text,.n=(size_t)st.st_size,.t=tokens,.cap=CELLS_STATES_JSON_TOKENS};
    ok=ok && tokens && cells_json_bounds(text,(size_t)st.st_size) && cells_jparse(&parser) && tokens[0].type=='{';
    cells_state_row *rows=ok?heap_caps_calloc(52,sizeof(*rows),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    ok=ok && rows;unsigned count=0,at=1;cells_outline_asset outlines={0};bool seen_outlines=false;
    for(unsigned item=0;ok && item<tokens[0].count;item++) {
        char key[16];int value=(int)at+1;
        if(cells_jstring(&parser,(int)at,key,sizeof(key)) && !strcmp(key,"outlines")) {
            ok=!seen_outlines && cells_outlines_parse(&parser,value,&outlines);seen_outlines=true;at=tokens[value].next;continue;
        }
        if(cells_jstring(&parser,(int)at,key,sizeof(key)) && !strcmp(key,"free_max_zoom")) {
            double cutoff;ok=cells_jnumber(&parser,value,&cutoff) && cutoff==FREE_MAX_ZOOM;
            at=tokens[value].next;continue;
        }
        if(count==52) { ok=false;break; }
        int object=(int)at+1;cells_state_row *r=rows+count;
        if(!cells_jstring(&parser,(int)at,r->code,sizeof(r->code)) || strlen(r->code)!=2 || tokens[object].type!='{') { ok=false;break; }
        if(r->code[0]<'A' || r->code[0]>'Z' || r->code[1]<'A' || r->code[1]>'Z') { ok=false;break; }
        for(unsigned j=0;j<count;j++)if(!strcmp(rows[j].code,r->code))ok=false;

        if(!cells_jstring(&parser,cells_jget(&parser,object,"name"),r->name,sizeof(r->name)) || !r->name[0] || strchr(r->name,'|')) { ok=false;break; }
        /* Legacy tier is accepted but never gates a state. */
        int cells=cells_jget(&parser,object,"cells"),partial=cells_jget(&parser,object,"partial");double bytes;
        if(!cells_jnumber(&parser,cells_jget(&parser,object,"bytes"),&bytes) || bytes<0 || bytes>20000000000.0 || floor(bytes)!=bytes || cells<0 || tokens[cells].type!='[') { ok=false;break; }
        r->bytes=(uint64_t)bytes;r->base_bytes=r->bytes;
        int base=cells_jget(&parser,object,"base_bytes"),detail=cells_jget(&parser,object,"detail_bytes");
        if(base>=0 || detail>=0) {
            double b,d;
            if(!cells_jnumber(&parser,base,&b) || !cells_jnumber(&parser,detail,&d) ||
               b<0 || d<0 || b>20000000000.0 || d>20000000000.0 || floor(b)!=b || floor(d)!=d || b+d!=bytes) { ok=false;break; }
            r->base_bytes=(uint64_t)b;r->detail_bytes=(uint64_t)d;
        }
        r->partial=partial>=0 && tokens[partial].type=='[' && tokens[partial].count>0;
        unsigned cell=(unsigned)cells+1;
        for(unsigned i=0;ok && i<tokens[cells].count;i++) {
            if(tokens[cell].type!='[' || tokens[cell].count!=2) { ok=false;break; }
            double x,y;unsigned xi=cell+1,yi=tokens[xi].next;
            if( !cells_jnumber(&parser,(int)xi,&x) || !cells_jnumber(&parser,(int)yi,&y) ||
               x<0 || x>255 || y<0 || y>255 || floor(x)!=x || floor(y)!=y) { ok=false;break; }
            unsigned id=(unsigned)x*256+(unsigned)y;
            if(r->cover[id/8]&(1u<<(id&7))) { ok=false;break; }
            r->cover[id/8]|=(1u<<(id&7));r->count++;cell=tokens[cell].next;
        }
        if(!r->count)ok=false;
        count++;at=tokens[object].next;
    }
    if(!count)ok=false;
    if(ok && activate) { memcpy(cells_states,rows,count*sizeof(*rows));cells_states_count=count;cells_states_dirty=true;cells_outlines=outlines;cells_outlines_pending=true; }
    heap_caps_free(rows);heap_caps_free(tokens);heap_caps_free(text);return ok;
}
static bool cells_id_allowed(uint32_t id) {
    return id<=65536 || cells_token_load();
}
static void cells_states_publish(void) {
    static uint32_t generation;
    snprintf(cells_work.free_max_zoom,sizeof(cells_work.free_max_zoom),"%u",FREE_MAX_ZOOM);
    cells_work.maps_progress=cw.progress_loaded && cw.queue_total?(float)((double)(cw.queue_done+cw.queue_partial)/cw.queue_total):0;
    if(cw.queue_loaded && cells_work.maps_progress>0.99f)cells_work.maps_progress=0.99f;
    if(cells_work.maps_progress>1)cells_work.maps_progress=1;
    snprintf(cells_work.state,sizeof(cells_work.state),"%s",cells_work.message);
    snprintf(cells_work.progress,sizeof(cells_work.progress),"%llu|%llu|%s",(unsigned long long)cells_work.done,(unsigned long long)cells_work.total,cw.queue_loaded?cw.queue_name:"");
    if(!cells_states_dirty && generation==cw.generation)return;
    static EXT_RAM_BSS_ATTR char next[sizeof(cells_work.states)];
    size_t used=0;next[0]=0;bool error=false;
    for(unsigned i=0;i<cells_states_count;i++) {
        cells_state_row *r=cells_states+i;unsigned installed=0,detail_installed=0,outdated=0;
        for(unsigned id=0;id<65536;id++)if(r->cover[id/8]&(1u<<(id&7))) {
            uint32_t bytes;uint8_t sha[32];
            char name[64],path[96];struct stat st;cells_name(id+1,name,sizeof(name),false);
            snprintf(path,sizeof(path),CELLS_ROOT "%s",name);
            if(cells_entry_checked(id+1,&bytes,sha,&error) && cells_matches_checked(path,bytes,sha,&error))installed++;
            else {
                /* On the card but not the server's revision: an update, not a
                   gap. Counted apart so TILES can say UPDATE instead of PART. */
                if(!stat(path,&st))outdated++;
                else if(errno!=ENOENT && errno!=ENOTDIR)error=true;
            }
            cells_name(id+1+65536,name,sizeof(name),false);snprintf(path,sizeof(path),CELLS_ROOT "%s",name);
            if(cells_entry_checked(id+1+65536,&bytes,sha,&error) && cells_matches_checked(path,bytes,sha,&error))detail_installed++;
            if(error)return;
        }
        int n=snprintf(next+used,sizeof(next)-used,"%s|%s|%s|%u|%llu|%u|%u|%llu|%llu|%u|%u\n",r->code,r->name,"free",r->count,(unsigned long long)r->bytes,installed,r->partial,(unsigned long long)r->base_bytes,(unsigned long long)r->detail_bytes,detail_installed==r->count,outdated);
        if(n<0 || (size_t)n>=sizeof(next)-used)return;
        used+=(size_t)n;
    }
    memcpy(cells_work.states,next,used+1);generation=cw.generation;cells_states_dirty=false;
}
static void cells_state_command(unsigned action,uint32_t key) {
    char st[3]={(char)(key>>8),(char)key,0};cells_state_row *r=NULL;
    for(unsigned i=0;i<cells_states_count;i++)if(!strcmp(st,cells_states[i].code))r=cells_states+i;
    if(!r) { cw.catalog_refresh=true;cells_message("State metadata unavailable; refresh then retry");return; }
    key=0x53540000u|key;
    if(action==8) { if(!cells_delete(key))cells_message("State removal failed; retry");else cells_message("State removed; shared cells retained");cells_states_dirty=true;return; }
    bool detail=cells_token_load();uint64_t needed=0;
    for(unsigned id=0;id<65536;id++)if(r->cover[id/8]&(1u<<(id&7))) {
        uint32_t bytes;uint8_t sha[32];if(!cells_entry(id+1,&bytes,sha)) { cells_message("State maps are not available yet");return; }
        if(!cells_present(id+1,bytes))needed+=bytes;
        if(detail && cells_entry(id+65537,&bytes,sha) && !cells_present(id+65537,bytes))needed+=bytes;
    }
#ifndef LS_CARTOCORE_HOST
    uint64_t total=0,free=0;ls_sdcard_size(&total,&free);
    if(needed>free) { cells_message("Not enough SD space for state");return; }
#else
    (void)needed;
#endif
    if(!cells_record(key,r->code,r->cover,true)) { cells_message("Cannot save state queue");return; }
    if(cw.queue_loaded && cw.queue_key==key) { memcpy(cw.queued,r->cover,CELLS_BITS);cw.next_cell=0;cells_progress_reset(); }
    cells_library();cells_message("State queued; safe to leave");
}
bool ls_cells_maps_text(unsigned which,char *out,size_t cap) {
    if(!out || !cap || !cells_take_reader())return false;
    if(which==4) snprintf(out,cap,"%u",FREE_MAX_ZOOM);
    else if(which==5) snprintf(out,cap,"%d",cells_plus?1:0);
    else snprintf(out,cap,"%s",which==0?cells_pub.states:which==1?cells_pub.progress:which==3?cells_pub.outlines:cells_pub.state);
    cells_give();return true;
}
bool ls_cells_state_action(bool remove,const char *st) {
    if(!st || strlen(st)!=2 || st[0]<'A' || st[0]>'Z' || st[1]<'A' || st[1]>'Z')return false;
    return ls_cells_action(remove?8:7,((uint32_t)(uint8_t)st[0]<<8)|(uint8_t)st[1]);
}

bool ls_cells_maps_progress(float *out) {
    if(!out || !cells_take_reader())return false;
    *out=cells_pub.maps_progress;cells_give();return true;
}
