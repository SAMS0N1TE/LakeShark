#include "rec_watch.h"
#include "ls_sub_fsk.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define DRAM_ATTR
#endif
#ifdef _WIN32
#include <io.h>
#define fsync _commit
#endif

/* FAT/SDMMC cannot DMA directly to the catalog in PSRAM.  Keep every disk
   transfer behind one sector-sized internal buffer instead of asking the
   fragmented runtime heap for a large bounce allocation.  The watch runtime
   serializes restore, checkpoint and export on its single I/O worker. */
static DRAM_ATTR uint8_t s_watch_io[512];

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
    /* What the preset will say, settled before anything is created: a
       modulation the CC1101 cannot be given is a file that would load,
       transmit, and not be the signal that was recorded. Refusing here
       leaves nothing on the card to mistake for a good capture. */
    const ls_sub_mod_t mod = { r->event.bitrate, r->event.deviation_hz };
    char preset[320];
    const size_t preset_len = ls_sub_preset_text(&mod, preset, sizeof(preset));
    if (!preset_len || preset_len >= sizeof(preset)) {
        snprintf(result,result_size,
                 "Export refused: %lu baud %lu Hz is not a modulation a "
                 "Flipper can be given",
                 (unsigned long)r->event.bitrate,
                 (unsigned long)r->event.deviation_hz);
        return false;
    }
    char path[256],name[64];
    snprintf(name,sizeof(name),"pattern-%016llx-%lu.sub",(unsigned long long)c->archive_id,(unsigned long)id);
    if(snprintf(path,sizeof(path),"%s/%s",dir,name)>=(int)sizeof(path)) return false;
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0664);
    if(fd<0) {snprintf(result,result_size,"Export refused: file exists or SD unavailable");return false;}
    FILE *f=fdopen(fd,"w");
    if(!f) {close(fd);unlink(path);return false;}
    setvbuf(f,(char *)s_watch_io,_IOFBF,sizeof(s_watch_io));
    bool ok=fprintf(f,"Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: %lu\n",
        (unsigned long)r->event.frequency)>0;
    if(ok) ok=fputs(preset,f)>=0;
    /* The numbers the preset was built from, so the file says what it is
       to somebody reading it and not only to a radio. */
    if(ok && r->event.bitrate)
        ok=fprintf(f,"# %lu baud, %lu Hz deviation, sync %08lX, "
            "%u-bit preamble\n",
            (unsigned long)r->event.bitrate,
            (unsigned long)r->event.deviation_hz,
            (unsigned long)r->event.sync_word,
            (unsigned)r->event.preamble_bits)>0;
    if(ok) ok=fprintf(f,"Protocol: RAW\n")>0;
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

/* The catalog as versions 2 and 3 wrote it. Read-only: rec_watch_store always
   writes the current layout, so an archive migrates the first time it saves.
   Declared here rather than in the header because nothing outside this file
   should be able to make one. */
/* Which layout a file holds, decided by the size it declares rather than by
   its version number alone - the two have to agree or it is not readable
   either way. 0 means neither. */
/* The two layouts are told apart by the size the file declares, so they have
   to BE different sizes. Today they differ by the modulation fields times the
   slot count, which is not a fact anyone would think to preserve while
   editing an event - and if a future field ever made them equal, a v3 archive
   would take the v4 path, pass its CRC (the bytes are unchanged either way)
   and load as sixteen captures with modulation read out of edge timings.
   That is a silent wrong answer, so it is worth one line to make it a build
   failure instead. */
_Static_assert(sizeof(rec_watch_catalog_t) != sizeof(rec_watch_catalog_v3_t),
    "v3 and v4 catalogs must differ in size - payload_bytes_for has nothing "
    "else to tell them apart");
_Static_assert(sizeof(rec_watch_catalog_t) > sizeof(rec_watch_catalog_v3_t),
    "v4 adds fields to the event, so it cannot be the smaller of the two");

static size_t payload_bytes_for(const header_t *h)
{
    if (h->version==4 && h->bytes==sizeof(rec_watch_catalog_t))
        return sizeof(rec_watch_catalog_t);
    if ((h->version==2 || h->version==3) && h->bytes==sizeof(rec_watch_catalog_v3_t))
        return sizeof(rec_watch_catalog_v3_t);
    return 0;
}
static bool read_header(const char *dir, int slot, header_t *h,
    void (*pump)(void *), void *context)
{
    char path[256];
    if (snprintf(path,sizeof(path),"%s/watch%d.bin",dir,slot) >= (int)sizeof(path)) return false;
    FILE *f=fopen(path,"rb"); if (!f) return false;
    setvbuf(f,NULL,_IONBF,0);
    bool ok=fread(s_watch_io,1,sizeof(*h),f)==sizeof(*h);
    if(ok)memcpy(h,s_watch_io,sizeof(*h));
    const size_t payload=ok?payload_bytes_for(h):0;
    ok=ok && h->magic==MAGIC && payload!=0;
    uint32_t crc=UINT32_MAX;
    uint8_t *buf=s_watch_io; size_t remaining=payload;
    while (ok && remaining) {
        size_t n=remaining<sizeof(s_watch_io)?remaining:sizeof(s_watch_io);
        if (fread(buf,1,n,f)!=n) {ok=false;break;}
        if (remaining==payload) {
            uint64_t id,sequence;
            memcpy(&id,buf,sizeof(id));memcpy(&sequence,buf+8,sizeof(sequence));
            if(id!=h->archive_id || sequence!=h->sequence) {ok=false;break;}
        }
        for(size_t i=0;i<n;i++) {crc^=buf[i];for(int b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
        remaining-=n;
        if(pump)pump(context);
    }
    ok=ok && ~crc==h->crc && fgetc(f)==EOF && !ferror(f);
    fclose(f); return ok;
}

/* Keep damaged generations as evidence instead of overwriting them.  This is
   also the recovery path after an older firmware attempted a direct PSRAM to
   SD transfer: both files can exist while neither has a usable CRC. */
static bool quarantine_invalid(const char *dir, int slot)
{
    char path[256], quarantine[256];
    if (snprintf(path,sizeof(path),"%s/watch%d.bin",dir,slot) >= (int)sizeof(path))
        return false;
    if (access(path,F_OK)!=0) return true;
    for (unsigned suffix=0;suffix<100;suffix++) {
        int n=suffix
            ? snprintf(quarantine,sizeof(quarantine),"%s/watch%d.invalid.%u",dir,slot,suffix)
            : snprintf(quarantine,sizeof(quarantine),"%s/watch%d.invalid",dir,slot);
        if(n<0 || n>=(int)sizeof(quarantine)) return false;
        if(access(quarantine,F_OK)==0) continue;
        return rename(path,quarantine)==0;
    }
    return false;
}

bool rec_watch_store_pumped(const char *dir, const rec_watch_catalog_t *c,
    uint64_t free_bytes, void (*pump)(void *), void *context)
{
    if (!dir || !c || free_bytes==UINT64_MAX ||
        free_bytes < REC_WATCH_RESERVE + sizeof(*c) + sizeof(header_t)) return false;
    header_t a={0}, b={0};
    bool va=read_header(dir,0,&a,pump,context), vb=read_header(dir,1,&b,pump,context);
    if ((va && a.archive_id!=c->archive_id) || (vb && b.archive_id!=c->archive_id)) return false;
    if(!va && !vb) {
        if(!quarantine_invalid(dir,0) || !quarantine_invalid(dir,1)) return false;
    }
    int slot=!va?0:!vb?1:a.sequence<=b.sequence?0:1;
    char path[256];
    if (snprintf(path,sizeof(path),"%s/watch%d.bin",dir,slot) >= (int)sizeof(path)) return false;
    FILE *f=fopen(path,"wb"); if(!f)return false;
    setvbuf(f,NULL,_IONBF,0);
    header_t h={.magic=MAGIC,.version=4,.bytes=sizeof(*c),
        .crc=rec_watch_crc(c,sizeof(*c)),.sequence=c->sequence,.archive_id=c->archive_id};
    memcpy(s_watch_io,&h,sizeof(h));
    bool ok=fwrite(s_watch_io,1,sizeof(h),f)==sizeof(h);
    const uint8_t *src=(const uint8_t *)c;size_t remaining=sizeof(*c);
    while(ok && remaining) {
        size_t n=remaining<sizeof(s_watch_io)?remaining:sizeof(s_watch_io);
        memcpy(s_watch_io,src,n);
        ok=fwrite(s_watch_io,1,n,f)==n;src+=n;remaining-=n;
        if(pump)pump(context);
    }
    if (fflush(f)!=0) ok=false;
    if (ok && fsync(fileno(f))!=0) ok=false;
    if (fclose(f)!=0) ok=false;
    return ok;
}
bool rec_watch_store(const char *dir, const rec_watch_catalog_t *c, uint64_t free_bytes)
{ return rec_watch_store_pumped(dir,c,free_bytes,NULL,NULL); }

bool rec_watch_restore(const char *dir, rec_watch_catalog_t *c)
{
    if(!dir || !c)return false;
    header_t a={0},b={0};bool va=read_header(dir,0,&a,NULL,NULL),vb=read_header(dir,1,&b,NULL,NULL);
    memset(c,0,sizeof(*c));
    if(!va&&!vb)return false;
    int slot=vb && (!va || b.sequence>a.sequence)?1:0;
    header_t h=slot?b:a;
    char path[256];snprintf(path,sizeof(path),"%s/watch%d.bin",dir,slot);
    FILE *f=fopen(path,"rb");if(!f)return false;
    setvbuf(f,NULL,_IONBF,0);
    bool ok=fseek(f,sizeof(header_t),SEEK_SET)==0;
    const bool legacy=h.bytes!=sizeof(*c);
    uint32_t crc=UINT32_MAX;
    if(!legacy) {
        uint8_t *dst=(uint8_t *)c;size_t remaining=sizeof(*c);
        while(ok && remaining) {
            size_t n=remaining<sizeof(s_watch_io)?remaining:sizeof(s_watch_io);
            ok=fread(s_watch_io,1,n,f)==n;
            if(ok)memcpy(dst,s_watch_io,n);
            dst+=n;remaining-=n;
        }
        if(ok)crc=~rec_watch_crc(c,sizeof(*c));
    } else {
        /* Carry the old layout forward, streaming, so the migration costs one
           512 byte buffer rather than a second catalog. Fields are copied by
           name and not by memcpy: the two structs happen to share a prefix
           today, and a migration that silently depends on that is one that
           breaks the next time somebody reorders a field. */
        ok=ok && fread(s_watch_io,1,24,f)==24;
        if(ok) {
            memcpy(&c->archive_id,s_watch_io,8);
            memcpy(&c->sequence,s_watch_io+8,8);
            memcpy(&c->next_id,s_watch_io+16,4);
            memcpy(&c->rejected,s_watch_io+20,4);
            for(size_t i=0;i<24;i++){crc^=s_watch_io[i];for(int b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
        }
        for(int i=0;ok && i<REC_WATCH_SLOTS;i++) {
            rec_watch_event_v3_t old_event;
            ok=fread(s_watch_io,1,sizeof(old_event),f)==sizeof(old_event);
            if(!ok)break;
            memcpy(&old_event,s_watch_io,sizeof(old_event));
            for(size_t k=0;k<sizeof(old_event);k++){crc^=s_watch_io[k];for(int b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
            rec_watch_event_t *e=&c->record[i].event;
            memset(e,0,sizeof(*e));
            e->id=old_event.id;e->frequency=old_event.frequency;
            e->count=old_event.count;e->first_boot=old_event.first_boot;
            e->last_boot=old_event.last_boot;e->first_ms=old_event.first_ms;
            e->last_ms=old_event.last_ms;e->order=old_event.order;
            e->span_us=old_event.span_us;e->edges=old_event.edges;
            e->pinned=old_event.pinned;e->end_reason=old_event.end_reason;
            e->peak=old_event.peak;e->source=old_event.source;
            /* The modulation fields stay zero: an archive written before they
               existed holds edge timings from a source that never had any. */
            uint8_t *dst=(uint8_t *)c->record[i].pulse;
            size_t remaining=sizeof(c->record[i].pulse);
            while(ok && remaining) {
                size_t n=remaining<sizeof(s_watch_io)?remaining:sizeof(s_watch_io);
                ok=fread(s_watch_io,1,n,f)==n;
                if(!ok)break;
                memcpy(dst,s_watch_io,n);
                for(size_t k=0;k<n;k++){crc^=s_watch_io[k];for(int b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
                dst+=n;remaining-=n;
            }
        }
    }
    fclose(f);
    ok=ok && c->sequence==h.sequence && ~crc==h.crc;
    for(int i=0;ok && i<REC_WATCH_SLOTS;i++) {
        rec_watch_event_t *e=&c->record[i].event;
        /* A source byte that is not a source is not evidence about which
           receiver heard something, so it does not get to name one. This
           began as a version-2 migration - that format used the byte as
           padding - but the reasoning never depended on the version: a byte
           flipped on the card reads back exactly the same way, and with
           three sources there is more room to land on a plausible wrong
           one. Checked on every restore, cheaply, for all of them. */
        if(h.version==2 || e->source>=REC_SOURCE_COUNT) {e->source=REC_SOURCE_RTL;e->reserved=0;}
        if(e->id && (!e->count || e->edges<6 || e->edges>REC_WATCH_EDGES ||
            e->order>c->sequence || e->id>c->next_id || e->source>=REC_SOURCE_COUNT)) ok=false;
    }
    if(!ok)memset(c,0,sizeof(*c));
    return ok;
}
