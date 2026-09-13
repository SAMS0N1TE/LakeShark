#include "rec_watch.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#ifdef _WIN32
#include <io.h>
#define fsync _commit
#endif

bool rec_watch_export(const char *dir, const rec_watch_catalog_t *c, uint32_t id,
                      uint64_t free_bytes, char *result, size_t result_size)
{
    if (!result || !result_size) return false;
    snprintf(result,result_size,"Export failed: invalid pattern");
    if (!dir || !c || !id) return false;
    const rec_watch_record_t *r=NULL;
    for (int i=0;i<REC_WATCH_SLOTS;i++) if(c->record[i].event.id==id) r=&c->record[i];
    if (!r || !r->event.frequency || r->event.edges<6 || r->event.edges>REC_WATCH_EDGES) return false;
    for (int i=0;i<r->event.edges;i++) if(!r->pulse[i] || r->pulse[i]==INT32_MIN) return false;
    if (free_bytes==UINT64_MAX || free_bytes<REC_WATCH_RESERVE+65536u) {
        snprintf(result,result_size,"Export refused: SD missing or low space");return false;
    }
    char path[256],name[64];
    snprintf(name,sizeof(name),"pattern-%016llx-%lu.sub",(unsigned long long)c->archive_id,(unsigned long)id);
    if(snprintf(path,sizeof(path),"%s/%s",dir,name)>=(int)sizeof(path)) return false;
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0664);
    if(fd<0) {snprintf(result,result_size,"Export refused: file exists or SD unavailable");return false;}
    FILE *f=fdopen(fd,"w");
    if(!f) {close(fd);unlink(path);return false;}
    bool ok=fprintf(f,"Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: %lu\n"
        "Preset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\n",(unsigned long)r->event.frequency)>0;
    for(int i=0;ok && i<r->event.edges;i++) {
        if(i%512==0) ok=fputs("RAW_Data:",f)>=0;
        if(ok) ok=fprintf(f," %ld",(long)r->pulse[i])>0;
        if(ok && (i%512==511 || i+1==r->event.edges)) ok=fputc('\n',f)!=EOF;
    }
    if(fflush(f)!=0) ok=false;
    if(ok && fsync(fileno(f))!=0) ok=false;
    if(fclose(f)!=0) ok=false;
    if(!ok) unlink(path);
    if(ok) snprintf(result,result_size,"Saved #%lu .sub in /subghz",(unsigned long)id);
    else snprintf(result,result_size,"Export failed: SD write error");
    return ok;
}
/* Two fixed-size generations. A failed write leaves the other intact.
   The private binary format is versioned and CRC checked, not a .sub file. */
typedef struct { uint32_t magic, version, bytes, crc; uint64_t sequence, archive_id; } header_t;
#define MAGIC 0x5752534cU
static bool read_header(const char *dir, int slot, header_t *h)
{
    char path[256];
    if (snprintf(path,sizeof(path),"%s/watch%d.bin",dir,slot) >= (int)sizeof(path)) return false;
    FILE *f=fopen(path,"rb"); if (!f) return false;
    bool ok=fread(h,1,sizeof(*h),f)==sizeof(*h) && h->magic==MAGIC &&
        h->version==2 && h->bytes==sizeof(rec_watch_catalog_t);
    uint32_t crc=UINT32_MAX;
    uint8_t buf[512]; size_t remaining=sizeof(rec_watch_catalog_t);
    while (ok && remaining) {
        size_t n=remaining<sizeof(buf)?remaining:sizeof(buf);
        if (fread(buf,1,n,f)!=n) {ok=false;break;}
        if (remaining==sizeof(rec_watch_catalog_t)) {
            uint64_t id,sequence;
            memcpy(&id,buf,sizeof(id));memcpy(&sequence,buf+8,sizeof(sequence));
            if(id!=h->archive_id || sequence!=h->sequence) {ok=false;break;}
        }
        for(size_t i=0;i<n;i++) {crc^=buf[i];for(int b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
        remaining-=n;
    }
    ok=ok && ~crc==h->crc && fgetc(f)==EOF && !ferror(f);
    fclose(f); return ok;
}
bool rec_watch_store(const char *dir, const rec_watch_catalog_t *c, uint64_t free_bytes)
{
    if (!dir || !c || free_bytes==UINT64_MAX ||
        free_bytes < REC_WATCH_RESERVE + sizeof(*c) + sizeof(header_t)) return false;
    header_t a={0}, b={0};
    bool va=read_header(dir,0,&a), vb=read_header(dir,1,&b);
    if ((va && a.archive_id!=c->archive_id) || (vb && b.archive_id!=c->archive_id)) return false;
    if(!va && !vb) {
        char existing[256];
        for(int i=0;i<2;i++) {
            if(snprintf(existing,sizeof(existing),"%s/watch%d.bin",dir,i)>=(int)sizeof(existing))return false;
            if(access(existing,F_OK)==0)return false;
        }
    }
    int slot=!va?0:!vb?1:a.sequence<=b.sequence?0:1;
    char path[256];
    if (snprintf(path,sizeof(path),"%s/watch%d.bin",dir,slot) >= (int)sizeof(path)) return false;
    FILE *f=fopen(path,"wb"); if(!f)return false;
    header_t h={.magic=MAGIC,.version=2,.bytes=sizeof(*c),
        .crc=rec_watch_crc(c,sizeof(*c)),.sequence=c->sequence,.archive_id=c->archive_id};
    bool ok=fwrite(&h,1,sizeof(h),f)==sizeof(h) && fwrite(c,1,sizeof(*c),f)==sizeof(*c);
    if (fflush(f)!=0) ok=false;
    if (ok && fsync(fileno(f))!=0) ok=false;
    if (fclose(f)!=0) ok=false;
    return ok;
}
bool rec_watch_restore(const char *dir, rec_watch_catalog_t *c)
{
    if(!dir || !c)return false;
    header_t a={0},b={0};bool va=read_header(dir,0,&a),vb=read_header(dir,1,&b);
    memset(c,0,sizeof(*c));
    if(!va&&!vb)return false;
    int slot=vb && (!va || b.sequence>a.sequence)?1:0;
    header_t h=slot?b:a;
    char path[256];snprintf(path,sizeof(path),"%s/watch%d.bin",dir,slot);
    FILE *f=fopen(path,"rb");if(!f)return false;
    bool ok=fseek(f,sizeof(header_t),SEEK_SET)==0 && fread(c,1,sizeof(*c),f)==sizeof(*c);
    fclose(f);
    ok=ok && c->sequence==h.sequence && rec_watch_crc(c,sizeof(*c))==h.crc;
    for(int i=0;ok && i<REC_WATCH_SLOTS;i++) {
        rec_watch_event_t *e=&c->record[i].event;
        if(e->id && (!e->count || e->edges<6 || e->edges>REC_WATCH_EDGES ||
            e->order>c->sequence || e->id>c->next_id)) ok=false;
    }
    if(!ok)memset(c,0,sizeof(*c));
    return ok;
}
