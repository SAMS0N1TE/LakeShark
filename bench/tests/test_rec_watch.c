#include "ls_test.h"
#include "rec_watch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#endif
static rec_watch_catalog_t catalog, restored;
static const int32_t pulses[]={300,-300,600,-300,300,-600};
static int frame24(int32_t *p, uint32_t value, int unit)
{
    p[0]=unit;p[1]=-31*unit;
    for(int bit=0;bit<24;bit++) {
        bool one=(value>>(23-bit))&1;
        p[2+bit*2]=(one?3:1)*unit;
        p[3+bit*2]=-(one?1:3)*unit;
    }
    return 50;
}
LS_CASE(ook24_requires_matching_complete_frames_and_rejects_bad_timing)
{
    int32_t p[150];rec_ook24_t result;
    frame24(p,0xA53C19,300);frame24(p+50,0xA53C19,310);
    LS_CHECK(rec_decode_ook24(p,100,&result));
    LS_EQ_INT(result.value,0xA53C19);LS_EQ_INT(result.repeats,2);
    memset(&catalog,0,sizeof(catalog));
    int first=rec_watch_observe_from(&catalog,REC_SOURCE_CC1101,433920000,p,100,7,1,0,1,NULL);
    frame24(p+100,0xA53C19,300);
    LS_EQ_INT(rec_watch_observe_from(&catalog,REC_SOURCE_CC1101,433920000,p,150,7,2,0,1,NULL),first);
    LS_EQ_INT(catalog.record[first].event.count,2);
    LS_CHECK(!rec_decode_ook24(p,99,&result));
    frame24(p+50,0xA53C18,300);
    LS_CHECK(!rec_decode_ook24(p,100,&result));
    frame24(p+50,0xA53C19,300);p[54]=1500;
    LS_CHECK(!rec_decode_ook24(p,100,&result));
    p[1]=INT32_MIN;LS_CHECK(!rec_decode_ook24(p,100,&result));
    for(int i=0;i<100;i++)p[i]=(i&1)?-300:300;
    LS_CHECK(!rec_decode_ook24(p,100,&result));
}
LS_CASE(receiver_source_is_preserved_without_changing_legacy_record_layout)
{
    LS_EQ_INT(rec_watch_receiver_want_source(-1,3,true,REC_SOURCE_CC1101),-1);
    LS_EQ_INT(rec_watch_receiver_want_source(3,3,true,REC_SOURCE_CC1101),-1);
    LS_EQ_INT(rec_watch_receiver_want_source(1,3,true,REC_SOURCE_CC1101),1);
    LS_EQ_INT(rec_watch_receiver_want_source(-1,3,true,REC_SOURCE_RTL),3);
    LS_EQ_INT(sizeof(rec_watch_event_t),64);
    LS_EQ_INT(offsetof(rec_watch_event_t,peak),56);
    LS_EQ_INT(offsetof(rec_watch_event_t,source),60);
    memset(&catalog,0,sizeof(catalog));
    int rtl=rec_watch_observe(&catalog,433920000,pulses,6,7,1,100,1,NULL);
    int cc=rec_watch_observe_from(&catalog,REC_SOURCE_CC1101,433920000,pulses,6,7,2,0,1,NULL);
    LS_CHECK(rtl>=0 && cc>=0 && rtl!=cc);
    LS_EQ_INT(catalog.record[rtl].event.source,REC_SOURCE_RTL);
    LS_EQ_INT(catalog.record[cc].event.source,REC_SOURCE_CC1101);
    LS_EQ_INT(rec_watch_observe_from(&catalog,REC_SOURCE_CC1101,433920000,pulses,6,7,3,0,1,NULL),cc);
    LS_EQ_INT(catalog.record[cc].event.count,2);
    LS_EQ_INT(catalog.record[rtl].event.count,1);
}
LS_CASE(manual_export_preserves_pulses_and_refuses_overwrite_or_low_space)
{
    memset(&catalog,0,sizeof(catalog));
    LS_EQ_INT(rec_watch_observe(&catalog,433920000,pulses,6,7,1,100,1,NULL),0);
    char result[112],path[160],text[1024];
    snprintf(path,sizeof(path),"pattern-%016llx-1.sub",(unsigned long long)catalog.archive_id);
    unlink(path);
    LS_CHECK(!rec_watch_export(".",&catalog,1,REC_WATCH_RESERVE,result,sizeof(result)));
    LS_CHECK(access(path,F_OK)!=0);
    LS_CHECK(rec_watch_export(".",&catalog,1,64*1024*1024,result,sizeof(result)));
    FILE *f=fopen(path,"rb");LS_CHECK(f!=NULL);
    if(f) {size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
        LS_CHECK(strstr(text,"Frequency: 433920000")!=NULL);
        LS_CHECK(strstr(text,"RAW_Data: 300 -300 600 -300 300 -600")!=NULL);
    }
    struct stat before,after;LS_EQ_INT(stat(path,&before),0);
    catalog.record[0].pulse[0]=999;
    LS_CHECK(!rec_watch_export(".",&catalog,1,64*1024*1024,result,sizeof(result)));
    LS_EQ_INT(stat(path,&after),0);LS_EQ_INT(before.st_size,after.st_size);
    LS_CHECK(!rec_watch_export(".",&catalog,99,64*1024*1024,result,sizeof(result)));
    unlink(path);
}
LS_CASE(background_watch_holds_the_receiver_but_visible_radio_choices_win)
{
    LS_EQ_INT(rec_watch_receiver_want(-1,3,true),3);
    LS_EQ_INT(rec_watch_receiver_want(-1,3,false),-1);
    LS_EQ_INT(rec_watch_receiver_want(0,3,true),0);
    LS_EQ_INT(rec_watch_receiver_want(3,3,true),3);
}
static int observe(uint32_t hz, uint64_t ms, bool *novel)
{ return rec_watch_observe(&catalog,hz,pulses,6,7,ms,100,1,novel); }
LS_CASE(repeated_patterns_are_counted_without_growing_the_catalog)
{
    memset(&catalog,0,sizeof(catalog));bool novel=false;
    LS_EQ_INT(observe(433920000,1,&novel),0);LS_CHECK(novel);
    int32_t jitter[]={320,-290,640,-310,300,-590};
    for(int i=0;i<100000;i++) {
        LS_EQ_INT(rec_watch_observe(&catalog,433920000,jitter,6,7,i+2,90,1,&novel),0);
        LS_CHECK(!novel);
    }
    LS_EQ_UINT(catalog.next_id,1);LS_EQ_UINT(catalog.record[0].event.count,100001);
    LS_EQ_INT(catalog.record[0].event.peak,100);
}
LS_CASE(pattern_matching_checks_payload_timing_and_frequency)
{
    memset(&catalog,0,sizeof(catalog));observe(433920000,1,NULL);
    int32_t changed[]={300,-300,300,-600,300,-600};
    LS_EQ_INT(rec_watch_observe(&catalog,433920000,changed,6,7,2,90,1,NULL),1);
    LS_EQ_INT(observe(315000000,3,NULL),2);
    LS_EQ_UINT(catalog.next_id,3);
}
LS_CASE(rotation_preserves_pins_and_all_pinned_refuses_new_patterns)
{
    memset(&catalog,0,sizeof(catalog));
    for(int i=0;i<REC_WATCH_SLOTS;i++)LS_EQ_INT(observe(300000000+i,1,NULL),i);
    LS_CHECK(rec_watch_pin(&catalog,1,true));
    LS_EQ_INT(observe(400000000,2,NULL),1);
    LS_EQ_UINT(catalog.record[0].event.id,1);
    for(int i=0;i<REC_WATCH_SLOTS;i++)LS_CHECK(rec_watch_pin(&catalog,catalog.record[i].event.id,true));
    LS_EQ_INT(observe(500000000,3,NULL),-1);LS_EQ_UINT(catalog.rejected,1);
    LS_EQ_INT(observe(300000000,4,NULL),0);
}
LS_CASE(invalid_and_oversized_captures_do_not_change_the_catalog)
{
    memset(&catalog,0,sizeof(catalog));
    int32_t bad[]={INT32_MIN,-300,600,-300,300,-600};
    LS_EQ_INT(rec_watch_observe(&catalog,433920000,bad,6,7,1,90,1,NULL),-1);
    LS_EQ_INT(rec_watch_observe(&catalog,433920000,pulses,4097,7,1,90,1,NULL),-1);
    LS_EQ_UINT(catalog.sequence,0);
}
LS_CASE(two_generations_recover_after_a_torn_write_and_keep_a_fixed_footprint)
{
    char dir[160];snprintf(dir,sizeof(dir),"rec-watch-test-%ld",(long)getpid());
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir,0700);
#endif
    char a[200],b[200];snprintf(a,sizeof(a),"%s/watch0.bin",dir);snprintf(b,sizeof(b),"%s/watch1.bin",dir);
    unlink(a);unlink(b);
    memset(&catalog,0,sizeof(catalog));observe(433920000,1,NULL);
    LS_CHECK(!rec_watch_store(dir,&catalog,UINT64_MAX));
    LS_CHECK(!rec_watch_store(dir,&catalog,REC_WATCH_RESERVE));
    LS_CHECK(rec_watch_store(dir,&catalog,64*1024*1024));
    observe(433920000,2,NULL);
    LS_CHECK(rec_watch_store(dir,&catalog,64*1024*1024));
    LS_CHECK(rec_watch_restore(dir,&restored));LS_EQ_UINT(restored.record[0].event.count,2);
    FILE *f=fopen(b,"wb");LS_CHECK(f!=NULL);if(f){fputs("torn",f);fclose(f);}
    LS_CHECK(rec_watch_restore(dir,&restored));LS_EQ_UINT(restored.record[0].event.count,1);
    for(int i=0;i<20;i++){observe(433920000,3+i,NULL);LS_CHECK(rec_watch_store(dir,&catalog,64*1024*1024));}
    struct stat sa,sb;LS_EQ_INT(stat(a,&sa),0);LS_EQ_INT(stat(b,&sb),0);
    LS_CHECK(sa.st_size+sb.st_size<520*1024);
    LS_CHECK(rec_watch_restore(dir,&restored));LS_EQ_UINT(restored.record[0].event.count,22);
    restored.archive_id++;
    LS_CHECK(!rec_watch_store(dir,&restored,64*1024*1024));
    LS_CHECK(rec_watch_restore(dir,&restored));LS_EQ_UINT(restored.archive_id,catalog.archive_id);
    unlink(a);unlink(b);rmdir(dir);
}

LS_CASE(two_invalid_generations_are_preserved_and_a_clean_archive_starts)
{
    char dir[160];snprintf(dir,sizeof(dir),"rec-watch-bad-%ld",(long)getpid());
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir,0700);
#endif
    char a[200],b[200],qa[200],qb[200];
    snprintf(a,sizeof(a),"%s/watch0.bin",dir);snprintf(b,sizeof(b),"%s/watch1.bin",dir);
    snprintf(qa,sizeof(qa),"%s/watch0.invalid",dir);snprintf(qb,sizeof(qb),"%s/watch1.invalid",dir);
    unlink(a);unlink(b);unlink(qa);unlink(qb);
    FILE *f=fopen(a,"wb");LS_CHECK(f!=NULL);if(f){fputs("bad-a",f);fclose(f);}
    f=fopen(b,"wb");LS_CHECK(f!=NULL);if(f){fputs("bad-b",f);fclose(f);}
    memset(&catalog,0,sizeof(catalog));observe(433920000,1,NULL);
    LS_CHECK(rec_watch_store(dir,&catalog,64*1024*1024));
    struct stat st;
    LS_EQ_INT(stat(qa,&st),0);LS_EQ_INT((int)st.st_size,5);
    LS_EQ_INT(stat(qb,&st),0);LS_EQ_INT((int)st.st_size,5);
    LS_CHECK(rec_watch_restore(dir,&restored));
    LS_EQ_UINT(restored.record[0].event.count,1);
    unlink(a);unlink(b);unlink(qa);unlink(qb);rmdir(dir);
}

LS_CASE(legacy_archive_padding_is_not_interpreted_as_a_receiver_source)
{
    char dir[160],path[200],other[200];
    snprintf(dir,sizeof(dir),"rec-legacy-%ld",(long)getpid());
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir,0700);
#endif
    snprintf(path,sizeof(path),"%s/watch0.bin",dir);
    snprintf(other,sizeof(other),"%s/watch1.bin",dir);unlink(path);unlink(other);
    memset(&catalog,0,sizeof(catalog));
    rec_watch_observe(&catalog,433920000,pulses,6,7,1,100,1,NULL);
    catalog.record[0].event.source=0xa5;
    struct {uint32_t magic,version,bytes,crc;uint64_t sequence,id;} header={
        0x5752534c,2,sizeof(catalog),rec_watch_crc(&catalog,sizeof(catalog)),catalog.sequence,catalog.archive_id};
    FILE *f=fopen(path,"wb");LS_CHECK(f!=NULL);
    if(f){fwrite(&header,1,sizeof(header),f);fwrite(&catalog,1,sizeof(catalog),f);fclose(f);}
    LS_CHECK(rec_watch_restore(dir,&restored));
    LS_EQ_INT(restored.record[0].event.source,REC_SOURCE_RTL);
    restored.record[0].event.source=REC_SOURCE_CC1101;restored.sequence++;
    LS_CHECK(rec_watch_store(dir,&restored,64*1024*1024));
    LS_CHECK(rec_watch_restore(dir,&catalog));
    LS_EQ_INT(catalog.record[0].event.source,REC_SOURCE_CC1101);
    unlink(path);unlink(other);rmdir(dir);
}
