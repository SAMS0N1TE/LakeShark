/* Phase 3 system services. No app callbacks or filesystem operations on I/O
 * producer tasks. Broker records contain identities and copied bytes only. */
#include "ls_lap_system.h"
#include "desk/ls_desk_host.h"
#include "ls_lap_sys.h"
#include "ls_lap_service.h"
#include "ls_picker.h"
#include "ls_lap_kit.h"
#include "../../../app-sdk/include/lakeshark_ui.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#define SYS_ALLOC(n) heap_caps_calloc(1,(n),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)
#define SYS_FREE(p) heap_caps_free(p)
#else
#define EXT_RAM_BSS_ATTR
#define SYS_ALLOC(n) calloc(1,(n))
#define SYS_FREE(p) free(p)
#endif
#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
extern void ls_picker_note(const char *) __attribute__((weak));
#endif
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <io.h>
#define fsync _commit
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif
#ifndef LS_LAP_SD_ROOT
#define LS_LAP_SD_ROOT "/sdcard"
#endif
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#define DEVICE 1
#include "ls_notify.h"
#include "ls_app.h"
#include "ls_rterm.h"
#include "desk/ls_desk.h"
#else
#define DEVICE 0
#endif

enum { S_READ=1, S_WRITE, S_LIST, S_PICK, S_EXPORT };
enum { FREE, QUEUED, RUNNING, DONE };
typedef struct {
    atomic_int state;
    atomic_bool cancelled, orphan;
    uint32_t serial, generation, offset, length, flags, next;
    int op, stream, result;
    char owner[32], path[256];
    union { uint8_t bytes[4096]; lap_dir_entry_t entries[16]; } data;
} storage_job;
EXT_RAM_BSS_ATTR static storage_job work_slots[LS_LAP_CONTEXTS][4];
#define work work_slots[ls_lap_context_index()]
static uint32_t serial;
static void (*wake_worker)(void);
void ls_lap_system_wake(void (*wake)(void)) { wake_worker = wake; }
static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }

typedef struct { int id; uint32_t flags; bool folder; char path[256]; } scope_t;
typedef struct { int id, scope; uint32_t flags, pos; char path[256]; } stream_t;
typedef struct {
    char id[32];
    bool ephemeral;
    uint32_t generation;
    scope_t scopes[4];
    stream_t streams[4];
    lap_desktop_event_t desktop, events[16];
    unsigned event_head, event_count, event_dropped;
    int desktop_request, desktop_job;
    struct {
        int id, job, result, choice;
        bool shown, dirty;
        uint32_t flags, cursor, next;
        char title[32], path[256];
        int count;
        lap_dir_entry_t entries[16];
    } picker;
} system_t;
static system_t *ctx(void) { return *ls_lap_api_extension(); }
static unsigned picker_owner;
static bool owns_picker(void) { return picker_owner==ls_lap_context_index()+1; }
bool ls_lap_system_picker_active(void) { return ctx()&&ctx()->picker.id&&!ctx()->picker.result; }
static bool live(void) { return ctx() && ls_lap_api_inside(); }
static bool text(char *out, size_t cap, const char *src) {
    if (!src) return false;
    for (size_t i=0;i<cap;i++) {
        if (!ls_lap_api_readable(src+i,1)) return false;
        out[i]=src[i]; if (!out[i]) return true;
    }
    return false;
}
static int new_id(void) { serial=serial%0x07ffffff+1; return (int)serial; }
static bool terminated(const char *s, size_t cap) { return memchr(s,0,cap)!=NULL; }

/* Persistent, bounded identity mailboxes intentionally outlive an ELF. */
typedef struct {
    char id[32], topics[4][32];
    lap_ipc_message_t queue[8];
    unsigned head, count, dropped, publishes;
    uint32_t window, notice_at;
    bool noticed;
} mailbox_t;
EXT_RAM_BSS_ATTR static mailbox_t mail[8];
static mailbox_t *mailbox(const char *id, bool create) {
    for (int i=0;i<8;i++) if (!strcmp(mail[i].id,id)) return &mail[i];
    if (create) for (int i=0;i<8;i++) if (!mail[i].id[0]) {
        snprintf(mail[i].id,sizeof(mail[i].id),"%s",id); return &mail[i];
    }
    return NULL;
}
static bool topic_copy(char out[32], const char *src) {
    if (!text(out,32,src) || !out[0]) return false;
    for (const unsigned char *p=(unsigned char *)out;*p;p++)
        if (!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("._/-",*p))) return false;
    return true;
}
int lap_ipc_subscribe(const char *topic) {
    char t[32]; if (!live() || !topic_copy(t,topic)) return LAP_SYS_EINVAL;
    mailbox_t *m=mailbox(ctx()->id,true); if (!m) return LAP_SYS_ELIMIT;
    for (int i=0;i<4;i++) if (!strcmp(m->topics[i],t)) return 0;
    for (int i=0;i<4;i++) if (!m->topics[i][0]) { strcpy(m->topics[i],t); return 0; }
    return LAP_SYS_ELIMIT;
}
int lap_ipc_unsubscribe(const char *topic) {
    char t[32]; if (!live() || !topic_copy(t,topic)) return LAP_SYS_EINVAL;
    mailbox_t *m=mailbox(ctx()->id,false); if (!m) return 0;
    for (int i=0;i<4;i++) if (!strcmp(m->topics[i],t)) m->topics[i][0]=0;
    unsigned n=m->count, kept=0;
    for (unsigned i=0;i<n;i++) {
        unsigned from=(m->head+i)%8, to=(m->head+kept)%8;
        if (strcmp(m->queue[from].topic,t)) { m->queue[to]=m->queue[from]; kept++; }
    }
    m->count=kept; return 0;
}
int lap_ipc_publish(const char *topic, uint32_t type, const void *data, uint32_t len) {
    char t[32]; if (!live() || !topic_copy(t,topic) || len>192 || (len&&!ls_lap_api_readable(data,len))) return LAP_SYS_EINVAL;
    mailbox_t *sender=mailbox(ctx()->id,true); if (!sender) return LAP_SYS_ELIMIT;
    uint32_t now=tick_ms();
    if (now-sender->window>=1000) { sender->window=now; sender->publishes=0; }
    if (sender->publishes>=8) return LAP_SYS_ELIMIT;
    sender->publishes++;
    int delivered=0;
    for (int i=0;i<8;i++) for (int j=0;j<4;j++) if (mail[i].id[0]&&!strcmp(mail[i].topics[j],t)) {
        mailbox_t *m=&mail[i];
        if (m->count==8) { m->dropped++; break; }
        lap_ipc_message_t *msg=&m->queue[(m->head+m->count++)%8];
        memset(msg,0,sizeof(*msg)); msg->size=sizeof(*msg); msg->version=1; msg->type=type; msg->data_len=len;
        strcpy(msg->sender,ctx()->id); strcpy(msg->topic,t); if (len) memcpy(msg->data,data,len);
        delivered++; break;
    }
    return delivered;
}
int lap_ipc_next(lap_ipc_message_t *out) {
    if (!live() || !ls_lap_api_readable(out,sizeof(*out))) return LAP_SYS_EINVAL;
    mailbox_t *m=mailbox(ctx()->id,false); if (!m||!m->count) return 0;
    *out=m->queue[m->head]; out->dropped=m->dropped; m->dropped=0;
    m->head=(m->head+1)%8; m->count--; return 1;
}

typedef struct { int id; char owner[32]; lap_notification_t notice; } notice_t;
#define NOTICE_MAX (LS_LAP_CONTEXTS*2>8?LS_LAP_CONTEXTS*2:8)
EXT_RAM_BSS_ATTR static notice_t notices[NOTICE_MAX];
static struct { char owner[32]; lap_launch_data_t data; bool pending; uint32_t when; } launch;
#if DEVICE
static void notice_activate(uint32_t id, int action) { ls_lap_system_activate((int)id,action); }
#endif
int lap_notify_post(const lap_notification_t *notice) {
    lap_notification_t n;
    if (!live() || !ls_lap_copy_input(&n,notice,sizeof(n)) || n.size!=sizeof(n) || n.version!=1 ||
        n.reserved || n.data_len>128 || !terminated(n.title,20) || !terminated(n.body,128) ||
        !terminated(n.actions[0],16)||!terminated(n.actions[1],16)||(!n.actions[0][0]&&n.actions[1][0])) return LAP_SYS_EINVAL;
    mailbox_t *m=mailbox(ctx()->id,true); if (!m) return LAP_SYS_ELIMIT;
    uint32_t now=tick_ms(); if (m->noticed&&now-m->notice_at<1000) return LAP_SYS_ELIMIT;
    unsigned owned=0;
    for(int i=0;i<NOTICE_MAX;i++) owned+=notices[i].id&&!strcmp(notices[i].owner,ctx()->id);
    if(owned>=(ctx()->ephemeral?2u:8u)) return LAP_SYS_ELIMIT;
    notice_t *slot=NULL; for (int i=0;i<NOTICE_MAX;i++) if (!notices[i].id) { slot=&notices[i]; break; }
    if (!slot) return LAP_SYS_ELIMIT;
    slot->id=new_id(); strcpy(slot->owner,ctx()->id); slot->notice=n;
    m->notice_at=now; m->noticed=true;
#if DEVICE
    ls_notice_t banner={0};
    snprintf(banner.title,sizeof(banner.title),"%s",ctx()->id);
    snprintf(banner.body,sizeof(banner.body),"%.19s: %.105s",n.title,n.body);
    banner.screen=-1; banner.accent=TUI_CYAN; banner.steady=true; banner.notify_visible=true;
    banner.item=slot->id; banner.activate=notice_activate;
    memcpy(banner.actions,n.actions,sizeof(banner.actions));
    ls_notify_post(&banner);
#endif
    return slot->id;
}
int lap_notify_dismiss(int id) {
    if (!live()) return LAP_SYS_EINVAL;
    for (int i=0;i<NOTICE_MAX;i++) if (notices[i].id==id && !strcmp(notices[i].owner,ctx()->id)) {
#if DEVICE
        ls_notify_dismiss_action((uint32_t)id,notice_activate);
#endif
        memset(&notices[i],0,sizeof(notices[i])); return 0;
    }
    return LAP_SYS_ENOENT;
}
bool ls_lap_system_activate(int id, int action) {
    if (action < -1 || action>1) return false;
    for (int i=0;i<NOTICE_MAX;i++) if (notices[i].id==id) {
        notice_t *n=&notices[i];
        if (action>=0&&!n->notice.actions[action][0]) return false;
#if DEVICE
        const ls_app_t *app=ls_app_by_id(n->owner);
        if (!app) return false;
#endif
        memset(&launch,0,sizeof(launch)); strcpy(launch.owner,n->owner);
        launch.pending=true; launch.when=tick_ms(); launch.data.size=sizeof(launch.data); launch.data.version=1;
        launch.data.notification=id; launch.data.action=action; launch.data.data_len=n->notice.data_len;
        memcpy(launch.data.data,n->notice.data,launch.data.data_len);
        n->id=0;
#if DEVICE
#if defined(CONFIG_LS_PANEL_RUNTIME) && CONFIG_LS_PANEL_RUNTIME
        if (ls_desk_enabled()) return ls_desk_open_app(launch.owner)>=0;
#endif
        for (int j=0;j<ls_app_count();j++) if (ls_app_at(j)==app) { ls_app_open(j); return true; }
        launch.pending=false; return false;
#else
        return true;
#endif
    }
    return false;
}
int lap_launch_data(lap_launch_data_t *out) {
    if (!live()||!ls_lap_api_readable(out,sizeof(*out))) return LAP_SYS_EINVAL;
    if (!launch.pending||strcmp(launch.owner,ctx()->id)) return 0;
    if (tick_ms()-launch.when>60000) { launch.pending=false; return 0; }
    *out=launch.data; launch.pending=false; return 1;
}

/* Path grant checks are repeated by the worker; never trust a cached stat.
   FAT has no symlinks; host explicitly rejects them component by component. */
static bool relative_ok(const char *p) {
    if (!p||strlen(p)>127||*p=='/'||strchr(p,'\\')||strchr(p,':')) return false;
    for (const char *q=p;*q;q++) {
        if ((unsigned char)*q<32) return false;
        if ((q==p||q[-1]=='/') && (*q=='.'||*q=='/')) return false;
    }
    return true;
}
static bool external_ok(const char *p) {
    const size_t n=strlen(LS_LAP_SD_ROOT);
    if (strncmp(p,LS_LAP_SD_ROOT,n)||p[n]!='/') return false;
    const char *rel=p+n+1;
    if (!relative_ok(rel)) return false;
    /* Shared public content only. Other app data, installed code, grants,
       hidden material and unknown top-level firmware directories stay out. */
    static const char *allowed[]={"recordings","captures","documents","downloads","music","tracks","exports","shared"};
    size_t part=strcspn(rel,"/");
    for (unsigned i=0;i<sizeof(allowed)/sizeof(*allowed);i++)
        if (strlen(allowed[i])==part&&!strncasecmp(rel,allowed[i],part)) return true;
    return false;
}
static bool no_links(const char *path) {
#if DEVICE
    /* ESP-IDF mounts the card as FAT, which has no symbolic links. */
    (void)path;
#elif !defined(_WIN32)
    char part[256]; snprintf(part,sizeof(part),"%s",path);
    for (char *p=part+1;;p++) if (!*p||*p=='/') {
        char c=*p; *p=0; struct stat st;
        if (!lstat(part,&st)&&S_ISLNK(st.st_mode)) return false;
        *p=c; if (!c) break;
    }
#else
    /* Windows reparse points include junctions as well as symlinks. */
    char part[256]; snprintf(part,sizeof(part),"%s",path);
    for (char *p=part+1;;p++) if (!*p||*p=='/') {
        char c=*p; *p=0;
        DWORD attr=GetFileAttributesA(part);
        if (attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        *p=c; if (!c) break;
    }
#endif
    return true;
}
static scope_t *scope_find(int id) {
    for (int i=0;i<4;i++) if (ctx()->scopes[i].id==id&&id>0) return &ctx()->scopes[i];
    return NULL;
}
static stream_t *stream_find(int id) {
    if (!live()) return NULL;
    for (int i=0;i<4;i++) if (ctx()->streams[i].id==id&&id>0) return &ctx()->streams[i];
    return NULL;
}
static bool resolve(int scope, const char *relative, bool write, bool folder, char out[256]) {
    char rel[128]; if (!live()||!text(rel,sizeof(rel),relative)||!relative_ok(rel)) return false;
    if (!scope) {
        snprintf(out,256,"%s/apps_data/%s/files%s%s",LS_LAP_SD_ROOT,ctx()->id,rel[0]?"/":"",rel); return true;
    }
    scope_t *s=scope_find(scope);
    if (!s||(write&&!(s->flags&LAP_FILE_WRITE))||(!s->folder&&(rel[0]||folder))) return false;
    int n=snprintf(out,256,"%s%s%s",s->path,rel[0]?"/":"",rel);
    return n>0&&n<256&&external_ok(out);
}
static storage_job *job_find(int id) {
    if (!live()||id<=0) return NULL;
    for (int i=0;i<4;i++) if (work[i].serial==(unsigned)id && atomic_load(&work[i].state)!=FREE &&
        !atomic_load(&work[i].orphan)&&work[i].generation==ctx()->generation&&!strcmp(work[i].owner,ctx()->id)) return &work[i];
    return NULL;
}
static int enqueue(int op, const char *path, int stream, uint32_t offset, uint32_t len, uint32_t flags, const void *data) {
    if (!live()) return LAP_SYS_EINVAL;
    for (int i=0;i<4;i++) if (atomic_load(&work[i].state)!=FREE&&!atomic_load(&work[i].orphan)&&
        work[i].generation==ctx()->generation&&!strcmp(work[i].owner,ctx()->id)) return LAP_SYS_EBUSY;
    for (int i=0;i<4;i++) if (atomic_load(&work[i].state)==FREE) {
        storage_job *j=&work[i]; j->serial=new_id(); j->generation=ctx()->generation;
        j->op=op; j->stream=stream; j->offset=offset; j->length=len; j->flags=flags; j->next=0; j->result=0;
        strcpy(j->owner,ctx()->id); snprintf(j->path,sizeof(j->path),"%s",path);
        if (data&&len) memcpy(j->data.bytes,data,len);
        atomic_store(&j->cancelled,false); atomic_store(&j->orphan,false);
        atomic_store_explicit(&j->state,QUEUED,memory_order_release);
        if (wake_worker) wake_worker();
        return j->serial;
    }
    return LAP_SYS_EBUSY;
}
int lap_stream_open(int scope, const char *path, uint32_t flags) {
    char full[256]; if (!live() || !(ls_lap_sys_caps()&LAP_HW_SD)) return LAP_SYS_EUNSUPPORTED;
    if (!(flags&LAP_FILE_READ)||(flags&~(LAP_FILE_READ|LAP_FILE_WRITE))) return LAP_SYS_EINVAL;
    if (!resolve(scope,path,flags&LAP_FILE_WRITE,false,full)) return LAP_SYS_EACCES;
    for (int i=0;i<4;i++) if (!ctx()->streams[i].id) {
        stream_t *s=&ctx()->streams[i]; s->id=new_id(); s->scope=scope; s->pos=0; s->flags=flags; strcpy(s->path,full); return s->id;
    }
    return LAP_SYS_ELIMIT;
}
static bool stream_busy(int id) {
    for (int i=0;i<4;i++) if (job_find(work[i].serial)&&work[i].stream==id) return true;
    return false;
}
int lap_stream_read(int id, uint32_t bytes) {
    stream_t *s=stream_find(id); if (!s||!bytes||bytes>4096||bytes>0x7fffffffu-s->pos) return LAP_SYS_EINVAL;
    return enqueue(S_READ,s->path,id,s->pos,bytes,s->flags,NULL);
}
int lap_stream_write(int id, const void *data, uint32_t bytes) {
    stream_t *s=stream_find(id); if (!s||!bytes||bytes>4096||bytes>0x7fffffffu-s->pos||!ls_lap_api_readable(data,bytes)) return LAP_SYS_EINVAL;
    if (!(s->flags&LAP_FILE_WRITE)) return LAP_SYS_EACCES;
    return enqueue(S_WRITE,s->path,id,s->pos,bytes,s->flags,data);
}
int lap_stream_seek(int id, uint64_t offset) {
    stream_t *s=stream_find(id); if (!s||offset>0x7fffffff) return LAP_SYS_EINVAL;
    if (stream_busy(id)) return LAP_SYS_EBUSY;
    s->pos=(uint32_t)offset; return 0;
}
int lap_stream_close(int id) {
    stream_t *s=stream_find(id); if (!s) return LAP_SYS_EINVAL;
    if (stream_busy(id)) return LAP_SYS_EBUSY;
    memset(s,0,sizeof(*s)); return 0;
}
int lap_file_scope_close(int id) {
    if (!live()||!scope_find(id)) return LAP_SYS_EINVAL;
    for (int i=0;i<4;i++) if (ctx()->streams[i].id&&ctx()->streams[i].scope==id) return LAP_SYS_EBUSY;
    memset(scope_find(id),0,sizeof(scope_t)); return 0;
}
int lap_dir_list(int scope, const char *path, uint32_t cursor) {
    char full[256]; if (!live()||!(ls_lap_sys_caps()&LAP_HW_SD)) return LAP_SYS_EUNSUPPORTED;
    if (!resolve(scope,path,false,true,full)) return LAP_SYS_EACCES;
    return enqueue(S_LIST,full,0,cursor,0,0,NULL);
}
int lap_storage_poll(int id, void *out, uint32_t cap, int32_t *result, uint32_t *next) {
    storage_job *j=job_find(id);
    if (!j||(j->op==S_PICK||j->op==S_EXPORT)||!ls_lap_api_readable(result,sizeof(*result))||!ls_lap_api_readable(next,sizeof(*next))) return LAP_SYS_EINVAL;
    if (atomic_load_explicit(&j->state,memory_order_acquire)!=DONE) return 0;
    int r=atomic_load(&j->cancelled)?LAP_SYS_ECANCEL:j->result;
    uint32_t n=r>0 ? (j->op==S_LIST?(uint32_t)r*sizeof(lap_dir_entry_t):j->op==S_READ?(uint32_t)r:0) : 0;
    if (n&&(cap<n||!ls_lap_api_readable(out,n))) return LAP_SYS_EINVAL;
    if (n) memcpy(out,j->data.bytes,n);
    *result=r; *next=j->next;
    if (r>=0&&j->stream) { stream_t *s=stream_find(j->stream); if (s) s->pos+=(uint32_t)r; }
    atomic_store(&j->state,FREE); return 1;
}
int lap_storage_cancel(int id) {
    storage_job *j=job_find(id); if (!j||j->op==S_PICK) return LAP_SYS_EINVAL;
    atomic_store(&j->cancelled,true); return 0;
}

static int io_error(void) { return errno==ENOENT?LAP_SYS_ENOENT:errno==EACCES?LAP_SYS_EACCES:LAP_SYS_EIO; }
static void parents(const char *path) {
    char p[256]; strcpy(p,path);
    for (char *s=p+strlen(LS_LAP_SD_ROOT)+1;*s;s++) if (*s=='/') { *s=0; MKDIR(p); *s='/'; }
}
static bool entry_ok(const char *name) { return name[0]&&name[0]!='.'&&!strchr(name,'\\')&&strlen(name)<64; }
static void run(storage_job *j) {
    if (atomic_load(&j->cancelled)) { j->result=LAP_SYS_ECANCEL; return; }
    if (!(ls_lap_sys_caps()&LAP_HW_SD)) { j->result=LAP_SYS_EIO; return; }
    if (!no_links(j->path)) { j->result=LAP_SYS_EACCES; return; }
    if (j->op==S_LIST||j->op==S_PICK) {
        /* Create only the app's own root on first use; browsing never creates
           directories on the public card. */
        char root[256]; snprintf(root,sizeof(root),"%s/apps_data/%s/files/",LS_LAP_SD_ROOT,j->owner);
        if (!strncmp(j->path,root,strlen(root)-1)) parents(root);
        DIR *d=opendir(j->path); if (!d) { j->result=io_error(); return; }
        struct dirent *de; unsigned seen=0, n=0; bool more=false;
        while ((de=readdir(d))) {
            if (!entry_ok(de->d_name)) continue;
            char full[512]; int path_len=snprintf(full,sizeof(full),"%s/%.63s",j->path,de->d_name);
            if (path_len<0||path_len>=256) continue;
            if (j->op==S_PICK&&!external_ok(full)) continue;
            if (!no_links(full)) continue;
            struct stat st; if (stat(full,&st)||(!S_ISDIR(st.st_mode)&&!S_ISREG(st.st_mode))) continue;
            if (seen++<j->offset) continue;
            if (n==16) { more=true; break; }
            lap_dir_entry_t *e=&j->data.entries[n++]; memset(e,0,sizeof(*e));
            e->size=sizeof(*e); e->version=1; e->dir=S_ISDIR(st.st_mode);
            e->bytes=(uint64_t)st.st_size>UINT32_MAX?UINT32_MAX:(uint32_t)st.st_size;
            strcpy(e->name,de->d_name);
        }
        closedir(d); j->result=n; j->next=more?j->offset+n:0; return;
    }
    if (j->op==S_EXPORT) {
        struct stat st;
        if (stat(j->path,&st)) { j->result=io_error(); return; }
        if (!S_ISREG(st.st_mode)) { j->result=LAP_SYS_EACCES; return; }
        if (st.st_size<0||(uint64_t)st.st_size>LS_DESK_HOST_MAX) { j->result=LAP_SYS_ELIMIT; return; }
        uint8_t *bytes=SYS_ALLOC(LS_DESK_HOST_MAX+1);
        if (!bytes) { j->result=LAP_SYS_ELIMIT; return; }
        FILE *f=fopen(j->path,"rb");
        if (!f) { SYS_FREE(bytes); j->result=io_error(); return; }
        size_t n=fread(bytes,1,LS_DESK_HOST_MAX+1,f);
        bool failed=ferror(f); if(fclose(f)) failed=true;
        j->result=failed?LAP_SYS_EIO:n>LS_DESK_HOST_MAX?LAP_SYS_ELIMIT:
            atomic_load(&j->cancelled)?LAP_SYS_ECANCEL:ls_desk_host_payload(j->stream,bytes,n);
        SYS_FREE(bytes); return;
    }
    if (j->op==S_WRITE) {
        char own[128]; snprintf(own,sizeof(own),"%s/apps_data/%s/files/",LS_LAP_SD_ROOT,j->owner);
        if (!strncmp(j->path,own,strlen(own))) parents(j->path);
    }
    struct stat st;
    if (!stat(j->path,&st)&&!S_ISREG(st.st_mode)) { j->result=LAP_SYS_EACCES; return; }
    /* Stream writes preserve existing bytes; explicit seek(0)+write overwrites
       from the beginning without implicitly truncating the tail. */
    FILE *f=fopen(j->path,j->op==S_READ?"rb":"r+b");
    if (!f&&j->op==S_WRITE&&errno==ENOENT) f=fopen(j->path,"w+b");
    if (!f) { j->result=io_error(); return; }
    if (fseek(f,(long)j->offset,SEEK_SET)) { fclose(f); j->result=LAP_SYS_EIO; return; }
    size_t n=j->op==S_READ?fread(j->data.bytes,1,j->length,f):fwrite(j->data.bytes,1,j->length,f);
    bool failed=ferror(f)||(j->op==S_WRITE&&n!=j->length);
    if (j->op==S_WRITE) {
        if (fflush(f)) failed=true;
        if (fsync(fileno(f))) failed=true;
    }
    if (fclose(f)) failed=true;
    j->result=failed?LAP_SYS_EIO:(int)n;
}
bool ls_lap_system_service(void) {
    for (int i=0;i<4;i++) {
        storage_job *j=&work[i]; int expect=QUEUED;
        if (atomic_compare_exchange_strong(&j->state,&expect,RUNNING)) {
            run(j); atomic_store_explicit(&j->state,DONE,memory_order_release); return true;
        }
        if (atomic_load(&j->state)==DONE&&atomic_load(&j->orphan)) atomic_store(&j->state,FREE);
    }
    return false;
}

/* Picker pagination uses the same bounded worker; only copied row names ever
   reach the UI. Navigation/picking executes firmware code, never app code. */
static void picker_request(void) {
    system_t *s=ctx();
    s->picker.job=enqueue(S_PICK,s->picker.path,0,s->picker.cursor,0,0,NULL);
    s->picker.dirty=s->picker.job<0;
}
static void picker_back(void) {
    system_t *s=ctx(); if (!s||!s->picker.id) return;
    size_t root=strlen(LS_LAP_SD_ROOT);
    if (strlen(s->picker.path)>root) { char *p=strrchr(s->picker.path,'/'); if (p) *p=0; }
    s->picker.cursor=0; s->picker.shown=false; s->picker.dirty=true;
}
static void picker_chosen(int index) {
    system_t *s=ctx(); if (!s||!s->picker.id) return;
    s->picker.shown=false;
    if (index==0) { s->picker.result=LAP_SYS_ECANCEL; return; }
    if (index==1) { picker_back(); return; }
    if (index==2) {
        if (!(s->picker.flags&LAP_FILE_FOLDER)||!external_ok(s->picker.path)) { s->picker.dirty=true; return; }
    } else if (index==3) {
        s->picker.cursor=s->picker.next; s->picker.dirty=true; return;
    } else {
        int row=index-4;
        if (row<0||row>=s->picker.count) { s->picker.dirty=true; return; }
        lap_dir_entry_t *e=&s->picker.entries[row];
        char full[256]; int n=snprintf(full,sizeof(full),"%s/%s",s->picker.path,e->name);
        if (n>=256||!external_ok(full)) { s->picker.dirty=true; return; }
        if (e->dir) { strcpy(s->picker.path,full); s->picker.cursor=0; s->picker.dirty=true; return; }
        if (s->picker.flags&LAP_FILE_FOLDER) { s->picker.dirty=true; return; }
        strcpy(s->picker.path,full);
    }
    for (int i=0;i<4;i++) if (!s->scopes[i].id) {
        scope_t *g=&s->scopes[i]; g->id=new_id(); g->flags=s->picker.flags;
        g->folder=(s->picker.flags&LAP_FILE_FOLDER)!=0; strcpy(g->path,s->picker.path);
        s->picker.result=g->id; return;
    }
    s->picker.result=LAP_SYS_ELIMIT;
}
int lap_file_picker(const lap_file_picker_config_t *cfg) {
    lap_file_picker_config_t c;
    if (!live()||!ls_lap_copy_input(&c,cfg,sizeof(c))||c.size!=sizeof(c)||c.version!=1||
        !terminated(c.title,32)||!(c.flags&LAP_FILE_READ)||(c.flags&~7u)) return LAP_SYS_EINVAL;
    if (!(ls_lap_sys_caps()&LAP_HW_SD)) return LAP_SYS_EUNSUPPORTED;
    if (!lap_screen_open()) return LAP_SYS_EACCES;
    if (ctx()->picker.id||ls_picker_active()||lap_ui_overlay()) return LAP_SYS_EBUSY;
    bool free_scope=false; for (int i=0;i<4;i++) if (!ctx()->scopes[i].id) free_scope=true;
    if (!free_scope) return LAP_SYS_ELIMIT;
    memset(&ctx()->picker,0,sizeof(ctx()->picker)); ctx()->picker.id=new_id();
    ctx()->picker.flags=c.flags; strcpy(ctx()->picker.title,c.title);
    strcpy(ctx()->picker.path,LS_LAP_SD_ROOT); ctx()->picker.dirty=true;
    return ctx()->picker.id;
}
int lap_file_picker_poll(int id) {
    if (!live()||id<=0||ctx()->picker.id!=id) return LAP_SYS_EINVAL;
    int r=ctx()->picker.result;
    if (!r) return 0;
    if (ctx()->picker.job>0) {
        storage_job *j=job_find(ctx()->picker.job);
        if (j) { atomic_store(&j->cancelled,true); atomic_store(&j->orphan,true); }
    }
    if (owns_picker()&&ls_picker_is(picker_chosen)) { ls_picker_close(); picker_owner=0; }
    memset(&ctx()->picker,0,sizeof(ctx()->picker)); return r;
}

static void desktop_event(unsigned kind) {
    system_t *s=ctx(); if (!s) return;
    if (s->event_count==16) { s->event_head=(s->event_head+1)%16; s->event_count--; s->event_dropped++; }
    lap_desktop_event_t e=s->desktop; e.kind=kind;
    s->events[(s->event_head+s->event_count++)%16]=e;
}
void ls_lap_system_input(unsigned kind, int w, int h) {
    system_t *s=ctx(); if (!s) return;
    if (kind==LAP_INPUT_RESIZE) {
        if (s->desktop.width_px==w&&s->desktop.height_px==h) return;
        s->desktop.width_px=w; s->desktop.height_px=h; desktop_event(LAP_DESKTOP_RESIZE);
    } else if (kind==LAP_INPUT_FOCUS_GAINED||kind==LAP_INPUT_FOCUS_LOST) {
        bool focused=kind==LAP_INPUT_FOCUS_GAINED;
        if (s->desktop.focused==focused) return;
        s->desktop.focused=focused; desktop_event(LAP_DESKTOP_FOCUS);
    }
}
static void connected(bool on) {
    system_t *s=ctx(); if (!s||s->desktop.connected==on) return;
    s->desktop.connected=on; desktop_event(on?LAP_DESKTOP_CONNECTED:LAP_DESKTOP_DISCONNECTED);
}
int lap_desktop_info(lap_desktop_event_t *out) {
    if (!live()||!ls_lap_api_readable(out,sizeof(*out))) return LAP_SYS_EINVAL;
    *out=ctx()->desktop; out->dropped=ctx()->event_dropped; return 0;
}
int lap_desktop_next(lap_desktop_event_t *out) {
    if (!live()||!ls_lap_api_readable(out,sizeof(*out))) return LAP_SYS_EINVAL;
    system_t *s=ctx(); if (!s->event_count) return 0;
    *out=s->events[s->event_head]; out->dropped=s->event_dropped; s->event_dropped=0;
    s->event_head=(s->event_head+1)%16; s->event_count--; return 1;
}
uint32_t lap_desktop_features(void) { return live()?ls_desk_host_features(NULL):0; }
int lap_desktop_open_url(const char *url) {
    char u[256]; if (!live()||!text(u,sizeof(u),url)||strncmp(u,"https://",8)||!u[8]) return LAP_SYS_EINVAL;
    const char *authority=u+8, *end=strpbrk(authority,"/?#");
    size_t authority_len=end?(size_t)(end-authority):strlen(authority);
    if(!authority_len||memchr(authority,'@',authority_len)) return LAP_SYS_EINVAL;
    for (const unsigned char *p=(const unsigned char *)u;*p;p++) if(*p<=32||*p==127||*p=='\\') return LAP_SYS_EINVAL;
    if(ctx()->desktop_request) return LAP_SYS_EBUSY;
    int id=ls_desk_host_begin(LS_DESK_URL,u);
    if(id>0) ctx()->desktop_request=id;
    return id;
}
int lap_desktop_save_file(int scope, const char *path, const char *suggested_name) {
    char full[256], name[64];
    if (!resolve(scope,path,false,false,full)||!text(name,sizeof(name),suggested_name)||!name[0]||!relative_ok(name)||strchr(name,'/')) return LAP_SYS_EINVAL;
    for(const unsigned char *p=(const unsigned char *)name;*p;p++) if(*p<32||strchr("<>:\\|?*",*p)) return LAP_SYS_EINVAL;
    if(ctx()->desktop_request) return LAP_SYS_EBUSY;
    int id=ls_desk_host_begin(LS_DESK_SAVE,name); if(id<0) return id;
    int job=enqueue(S_EXPORT,full,id,0,0,0,NULL);
    if(job<0) { ls_desk_host_cancel(id); ls_desk_host_status(id,1); return job; }
    ctx()->desktop_request=id; ctx()->desktop_job=job; return id;
}
int lap_desktop_request_status(int id) {
    if(!live()||id<=0||ctx()->desktop_request!=id) return LAP_SYS_EINVAL;
    storage_job *j=job_find(ctx()->desktop_job);
    if(j&&atomic_load_explicit(&j->state,memory_order_acquire)==DONE) {
        if(j->result<0) ls_desk_host_reply(id,j->result);
        atomic_store(&j->state,FREE); ctx()->desktop_job=0;
    }
    int result=ls_desk_host_status(id,1);
    if(result) { ctx()->desktop_request=0; if(j) { atomic_store(&j->cancelled,true); atomic_store(&j->orphan,true); } }
    return result;
}
int lap_desktop_request_cancel(int id) {
    if(!live()||id<=0||ctx()->desktop_request!=id) return LAP_SYS_EINVAL;
    storage_job *j=job_find(ctx()->desktop_job); if(j) atomic_store(&j->cancelled,true);
    ls_desk_host_cancel(id); return 0;
}

void ls_lap_system_begin(const char *id, uint32_t generation) {
    ls_lap_system_end();
    system_t *s=SYS_ALLOC(sizeof(*s)); *ls_lap_api_extension()=s;
    if (!s) return;
    snprintf(s->id,sizeof(s->id),"%s",id); s->generation=generation;
    s->desktop.size=sizeof(s->desktop); s->desktop.version=1;
}
void ls_lap_system_end(void) {
    system_t *s=ctx(); if (!s) return;
    if (owns_picker()&&ls_picker_is(picker_chosen)) { ls_picker_close(); picker_owner=0; }
    if(s->desktop_request) { ls_desk_host_cancel(s->desktop_request); ls_desk_host_status(s->desktop_request,1); }
    for (int i=0;i<4;i++) if (work[i].generation==s->generation&&!strcmp(work[i].owner,s->id)&&atomic_load(&work[i].state)!=FREE) {
        atomic_store(&work[i].cancelled,true); atomic_store(&work[i].orphan,true);
    }
    SYS_FREE(s); *ls_lap_api_extension()=NULL;
}
void ls_lap_system_detach(void) {
    system_t *s=ctx(); if(!s) return;
    if(s->ephemeral) {
        mailbox_t *m=mailbox(s->id,false); if(m) memset(m,0,sizeof(*m));
        for(int i=0;i<NOTICE_MAX;i++) if(notices[i].id&&!strcmp(notices[i].owner,s->id)) {
#if DEVICE
            ls_notify_dismiss_action((uint32_t)notices[i].id,notice_activate);
#endif
            memset(&notices[i],0,sizeof(notices[i]));
        }
    }
    s->ephemeral=false;
}
void ls_lap_system_ephemeral(bool on) { if(ctx()) ctx()->ephemeral=on; }
void ls_lap_system_hide(void) {
    system_t *s=ctx(); if(!s) return;
    if(owns_picker()) { if(ls_picker_is(picker_chosen)) ls_picker_close(); picker_owner=0; }
    if(s->picker.id&&!s->picker.result) s->picker.result=LAP_SYS_ECANCEL;
}
void ls_lap_system_frame(void) {
    system_t *s=ctx(); if (!s) return;
#if DEVICE
    connected(ls_rterm_active());
#endif
    unsigned scale=0; ls_desk_host_features(&scale);
    if(s->desktop.scale_milli!=scale) { s->desktop.scale_milli=scale; desktop_event(LAP_DESKTOP_SCALE); }
    int cw=0,ch=0; ls_tui_geometry(NULL,NULL,&cw,&ch);
    if (s->desktop.cell_width_px!=cw||s->desktop.cell_height_px!=ch) {
        s->desktop.cell_width_px=cw; s->desktop.cell_height_px=ch; desktop_event(LAP_DESKTOP_SCALE);
    }
    if (!s->picker.id||s->picker.result) return;
    if (!ls_lap_service_shown()) {
        if (owns_picker()&&ls_picker_is(picker_chosen)) { ls_picker_close(); picker_owner=0; }
        s->picker.result=LAP_SYS_ECANCEL; return;
    }
    bool prior=ls_lap_api_inside(); if (!prior) ls_lap_api_callback(true);
    if (s->picker.job>0) {
        storage_job *j=job_find(s->picker.job);
        if (j&&atomic_load_explicit(&j->state,memory_order_acquire)==DONE) {
            s->picker.count=j->result; s->picker.next=j->next;
            if (j->result>=0) memcpy(s->picker.entries,j->data.entries,j->result*sizeof(lap_dir_entry_t));
            atomic_store(&j->state,FREE); s->picker.job=0;
            if (s->picker.count<0) { s->picker.result=s->picker.count; goto done; }
            char title[32];
            snprintf(title,sizeof(title),"%.15s %s",s->id,(s->picker.flags&LAP_FILE_WRITE)?"READ + WRITE":"READ ONLY");
            ls_picker_open(title,picker_chosen);
            picker_owner=ls_lap_context_index()+1;
#if !DEVICE
            if (ls_picker_note)
#endif
            {
                char note[80];
                /* Tail identifies the selected directory even on deep paths. */
                const char *shown=s->picker.path;
                if (strlen(shown)>60) shown+=strlen(shown)-60;
                snprintf(note,sizeof(note),"%s: %.60s",(s->picker.flags&LAP_FILE_FOLDER)?"FOLDER":"FILE",shown);
                ls_picker_note(note);
            }
            ls_picker_add("CANCEL",NULL); ls_picker_add("UP",NULL);
            ls_picker_add("USE THIS FOLDER",(s->picker.flags&LAP_FILE_FOLDER)?
                ((s->picker.flags&LAP_FILE_WRITE)?"GRANT READ + WRITE":"GRANT READ ONLY"):"folders: open to browse");
            ls_picker_add(s->picker.next?"NEXT PAGE":"FIRST PAGE",NULL);
            for (int i=0;i<s->picker.count;i++) ls_picker_add(s->picker.entries[i].name,s->picker.entries[i].dir?"FOLDER":
                (s->picker.flags&LAP_FILE_WRITE)?"READ + WRITE":"READ ONLY");
            s->picker.shown=true;
        }
    } else if (s->picker.dirty) { s->picker.dirty=false; picker_request(); }
    else if (s->picker.shown&&!ls_picker_is(picker_chosen)) s->picker.result=LAP_SYS_ECANCEL;
done:
    if (!prior) ls_lap_api_callback(false);
}
bool ls_lap_system_script(const char *c) {
#if !DEVICE
    int a,b;
    if (sscanf(c,"desktop=%d",&a)==1) { connected(a!=0); if(!a) ls_desk_host_disconnect(); return true; }
    if (sscanf(c,"desktopcaps=%d,%d",&a,&b)==2) { ls_desk_host_connect(a,b); connected(true); return true; }
    if (sscanf(c,"desktopresult=%d",&a)==1) { if(ctx()) ls_desk_host_reply(ctx()->desktop_request,a); return true; }
    if (sscanf(c,"notice=%d,%d",&a,&b)==2) { ls_lap_system_activate(a,b); return true; }
    if (sscanf(c,"noticeaction=%d",&a)==1) {
        int latest=0;
        for (int i=0;i<NOTICE_MAX;i++) if (ctx()&&!strcmp(notices[i].owner,ctx()->id)&&notices[i].id>latest) latest=notices[i].id;
        if (latest) ls_lap_system_activate(latest,a);
        return true;
    }
    if (!strncmp(c,"pick=",5)) {
        if (ctx()&&ctx()->picker.shown) { ls_picker_close(); picker_chosen(atoi(c+5)); } return true;
    }
    if (!strcmp(c,"pickcancel")) {
        if (ctx()&&ctx()->picker.id) { if (ls_picker_is(picker_chosen)) ls_picker_close(); ctx()->picker.result=LAP_SYS_ECANCEL; } return true;
    }
#else
    (void)c;
#endif
    return false;
}

#include "ls_lap_usage.h"
ls_lap_usage_t ls_lap_system_usage(unsigned slot) {
    ls_lap_usage_t v={sizeof(work_slots[slot])+sizeof(system_t),0};
    system_t *s=ls_lap_instance_extension_at(slot);
    if(s) {
        for(int i=0;i<4;i++) { v.handles+=s->streams[i].id>0; v.handles+=s->scopes[i].id>0; }
        mailbox_t *m=mailbox(s->id,false);
        if(m) for(int i=0;i<4;i++) v.handles+=m->topics[i][0]!=0;
        for(int i=0;i<NOTICE_MAX;i++) v.handles+=notices[i].id&&!strcmp(notices[i].owner,s->id);
    }
    for(int i=0;i<4;i++) v.handles+=atomic_load(&work_slots[slot][i].state)!=FREE;
    return v;
}
