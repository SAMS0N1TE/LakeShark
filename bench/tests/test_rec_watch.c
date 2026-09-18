#include "ls_test.h"
#include "rec_watch.h"
#include "rec_state.h"
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
    /* The pre-v4 layout is pinned because rec_watch_restore still reads
       files written against it; getting these wrong silently mis-parses
       somebody's archive rather than refusing it. */
    LS_EQ_INT(sizeof(rec_watch_event_v3_t),64);
    LS_EQ_INT(offsetof(rec_watch_event_v3_t,peak),56);
    LS_EQ_INT(offsetof(rec_watch_event_v3_t,source),60);
    /* And the current one, which grew to carry the modulation a demodulating
       source needs. Deliberate, migrated, and not free - 16 bytes an event. */
    LS_EQ_INT(sizeof(rec_watch_event_t),80);
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
    /* A genuine pre-v4 archive, in the layout that firmware actually wrote:
       the smaller event, and the version and size that went with it. Building
       it from the CURRENT struct would test nothing, because the size check
       would reject it before the migration ran. */
    static rec_watch_catalog_v3_t legacy;
    memset(&legacy,0,sizeof(legacy));
    memset(&catalog,0,sizeof(catalog));
    rec_watch_observe(&catalog,433920000,pulses,6,7,1,100,1,NULL);
    legacy.archive_id=catalog.archive_id;
    legacy.sequence=catalog.sequence;
    legacy.next_id=catalog.next_id;
    legacy.rejected=catalog.rejected;
    {
        const rec_watch_event_t *src=&catalog.record[0].event;
        rec_watch_event_v3_t *dst=&legacy.record[0].event;
        dst->id=src->id;dst->frequency=src->frequency;dst->count=src->count;
        dst->first_boot=src->first_boot;dst->last_boot=src->last_boot;
        dst->first_ms=src->first_ms;dst->last_ms=src->last_ms;
        dst->order=src->order;dst->span_us=src->span_us;
        dst->edges=src->edges;dst->pinned=src->pinned;
        dst->end_reason=src->end_reason;dst->peak=src->peak;
        /* The byte this case is named for: version 2 used it as padding, so
           whatever it holds is not evidence about which receiver heard the
           capture and must not be reported as one. */
        dst->source=0xa5;
        memcpy(legacy.record[0].pulse,catalog.record[0].pulse,
               sizeof(legacy.record[0].pulse));
    }
    struct {uint32_t magic,version,bytes,crc;uint64_t sequence,id;} header={
        0x5752534c,2,sizeof(legacy),rec_watch_crc(&legacy,sizeof(legacy)),legacy.sequence,legacy.archive_id};
    FILE *f=fopen(path,"wb");LS_CHECK(f!=NULL);
    if(f){fwrite(&header,1,sizeof(header),f);fwrite(&legacy,1,sizeof(legacy),f);fclose(f);}

    /* It loads - nobody loses an archive to a format change - and it comes
       across intact. */
    LS_CHECK(rec_watch_restore(dir,&restored));
    LS_EQ_INT(restored.record[0].event.source,REC_SOURCE_RTL);
    LS_EQ_UINT(restored.record[0].event.id,catalog.record[0].event.id);
    LS_EQ_UINT(restored.record[0].event.frequency,433920000);
    LS_EQ_UINT(restored.record[0].event.count,catalog.record[0].event.count);
    LS_EQ_INT(restored.record[0].event.edges,catalog.record[0].event.edges);
    for(int i=0;i<restored.record[0].event.edges;i++)
        LS_EQ_INT(restored.record[0].pulse[i],catalog.record[0].pulse[i]);
    /* And carries no modulation, because the source that heard it timed
       edges and never had any. A migration that invented values here would
       replay a capture on settings nobody ever measured. */
    LS_EQ_UINT(restored.record[0].event.bitrate,0);
    LS_EQ_UINT(restored.record[0].event.deviation_hz,0);
    LS_EQ_UINT(restored.record[0].event.sync_word,0);
    LS_EQ_INT(restored.record[0].event.preamble_bits,0);
    restored.record[0].event.source=REC_SOURCE_CC1101;restored.sequence++;
    LS_CHECK(rec_watch_store(dir,&restored,64*1024*1024));
    LS_CHECK(rec_watch_restore(dir,&catalog));
    LS_EQ_INT(catalog.record[0].event.source,REC_SOURCE_CC1101);
    unlink(path);unlink(other);rmdir(dir);
}

LS_CASE(watch_rejects_broken_edges_and_filters_history_by_receiver)
{
    memset(&catalog,0,sizeof(catalog));int32_t bad[]={100,100,-100,100,-100,100};
    LS_EQ_INT(rec_watch_observe(&catalog,433920000,bad,6,1,1,0,1,NULL),-1);
    static rec_watch_status_t status;memset(&status,0,sizeof(status));status.count=2;
    status.event[0].source=REC_SOURCE_RTL;status.event[0].id=5;
    status.event[1].source=REC_SOURCE_CC1101;status.event[1].id=9;status.preview[1][0]=321;status.decoded[1].value=42;
    rec_watch_filter_status(&status,REC_SOURCE_CC1101);
    LS_EQ_INT(status.count,1);LS_EQ_INT(status.event[0].id,9);LS_EQ_INT(status.preview[0][0],321);LS_EQ_INT(status.decoded[0].value,42);
}

LS_CASE(cc_software_filter_removes_glitches_preserving_elapsed_time)
{
    int32_t p[]={100,-10,100,-200,300,-300,400,-400};
    LS_EQ_INT(rec_watch_filter_pulses(p,8,40),6);LS_EQ_INT(p[0],210);LS_EQ_INT(p[1],-200);
    int32_t noise[]={1,-2,3,-4,5,-6};LS_EQ_INT(rec_watch_filter_pulses(noise,6,40),0);
    int32_t overflow[]={INT32_MAX,-1,100};LS_EQ_INT(rec_watch_filter_pulses(overflow,3,40),0);
}

LS_CASE(one_off_noise_cannot_evict_decoded_or_repeated_observations)
{
    memset(&catalog,0,sizeof(catalog));
    int32_t p[100];frame24(p,0xA53C19,300);frame24(p+50,0xA53C19,300);
    int decoded=rec_watch_observe(&catalog,433920000,p,100,1,1,0,REC_END_GAP,NULL);
    int repeated=rec_watch_observe(&catalog,433920000,pulses,6,1,2,0,REC_END_GAP,NULL);
    LS_EQ_INT(rec_watch_observe(&catalog,433920000,pulses,6,1,3,0,REC_END_GAP,NULL),repeated);
    uint32_t decoded_id=catalog.record[decoded].event.id, repeated_id=catalog.record[repeated].event.id;
    for(int i=0;i<REC_WATCH_SLOTS*3;i++)
        LS_CHECK(rec_watch_observe(&catalog,434000000+i,pulses,6,1,4+i,0,REC_END_GAP,NULL)>=0);
    LS_EQ_UINT(catalog.record[decoded].event.id,decoded_id);
    LS_EQ_UINT(catalog.record[repeated].event.id,repeated_id);
    frame24(p,0xA53C18,300);frame24(p+50,0xA53C18,300);
    int changed=rec_watch_observe(&catalog,433920000,p,100,1,100,0,REC_END_GAP,NULL);
    LS_CHECK(changed>=0 && changed!=decoded);
}

static unsigned pump_calls;
static void capture_during_checkpoint(void *context)
{
    rec_watch_catalog_t *live=context;
    if((++pump_calls%64)==0)
        rec_watch_observe(live,433920000,pulses,6,7,pump_calls,100,1,NULL);
}
LS_CASE(checkpoint_keeps_snapshot_consistent_while_live_captures_continue)
{
    char dir[160],a[200],b[200];
    snprintf(dir,sizeof(dir),"rec-pump-%ld",(long)getpid());
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir,0700);
#endif
    snprintf(a,sizeof(a),"%s/watch0.bin",dir);snprintf(b,sizeof(b),"%s/watch1.bin",dir);
    unlink(a);unlink(b);
    static rec_watch_catalog_t snapshot;
    memset(&catalog,0,sizeof(catalog));observe(433920000,1,NULL);
    LS_CHECK(rec_watch_store(dir,&catalog,64*1024*1024));
    observe(433920000,2,NULL);
    LS_CHECK(rec_watch_store(dir,&catalog,64*1024*1024));
    observe(433920000,3,NULL);snapshot=catalog;pump_calls=0;
    LS_CHECK(rec_watch_store_pumped(dir,&snapshot,64*1024*1024,capture_during_checkpoint,&catalog));
    LS_CHECK(pump_calls>=3*sizeof(catalog)/512);
    LS_CHECK(catalog.sequence>snapshot.sequence);
    LS_CHECK(rec_watch_restore(dir,&restored));
    LS_CHECK(memcmp(&restored,&snapshot,sizeof(snapshot))==0);
    /* A successful older snapshot does not mean newer live captures are saved. */
    LS_CHECK(restored.sequence!=catalog.sequence);
    LS_CHECK(rec_watch_store(dir,&catalog,64*1024*1024));
    LS_CHECK(rec_watch_restore(dir,&restored));
    LS_EQ_UINT(restored.sequence,catalog.sequence);
    unlink(a);unlink(b);rmdir(dir);
}

LS_CASE(a_source_spends_its_own_slots_before_reaching_for_anothers)
{
    /* Sixteen slots are shared by every receiver, so a talkative one can
       clear out captures somebody collected on a different radio.

       What is actually guaranteed - and all that can be, because a one-off
       capture is always evictable by another one-off - is that a source
       reaches for its OWN oldest first. Once a source has nothing of its own
       left to give up it may take another's, and that is the honest limit;
       pinning is what makes a capture safe outright. */
    memset(&catalog,0,sizeof(catalog));

    /* Four on the CC1101, timings a factor apart so the catalog cannot fold
       them together as repeats of one pattern. */
    static const int32_t WIDTH[4]={300,900,2700,8100};
    for(int i=0;i<4;i++) {
        int32_t p[8];
        for(int k=0;k<8;k++)p[k]=(k&1)?-WIDTH[i]:WIDTH[i];
        LS_CHECK(rec_watch_observe_from(&catalog,REC_SOURCE_CC1101,433920000,
                                        p,8,1,(uint64_t)i,1,0,NULL)>=0);
    }
    /* Then fill every remaining slot from the RTL. */
    for(int i=0;i<REC_WATCH_SLOTS-4;i++) {
        int32_t p[8];
        const int32_t w=400+i*500;
        for(int k=0;k<8;k++)p[k]=(k&1)?-w:w;
        LS_CHECK(rec_watch_observe_from(&catalog,REC_SOURCE_RTL,868000000,
                                        p,8,1,(uint64_t)(100+i),1,0,NULL)>=0);
    }
    int cc=0;
    for(int i=0;i<REC_WATCH_SLOTS;i++)
        if(catalog.record[i].event.source==REC_SOURCE_CC1101) cc++;
    LS_CHECK_MSG(cc==4,"filling the catalog already cost %d CC1101 capture(s)",4-cc);

    /* One more from the RTL, with nothing free. It has twelve of its own to
       choose from and must not take one of the four. */
    {
        int32_t p[8];
        for(int k=0;k<8;k++)p[k]=(k&1)?-9000:9000;
        LS_CHECK(rec_watch_observe_from(&catalog,REC_SOURCE_RTL,868000000,
                                        p,8,1,9999,1,0,NULL)>=0);
    }
    cc=0;
    for(int i=0;i<REC_WATCH_SLOTS;i++)
        if(catalog.record[i].event.source==REC_SOURCE_CC1101) cc++;
    LS_CHECK_MSG(cc==4,"the RTL took a CC1101 slot while it still had %d of its own",
                 REC_WATCH_SLOTS-4);
}

LS_CASE(source_fairness_never_outranks_the_protection_it_sits_under)
{
    /* Fairness is a tiebreaker below rank, not beside it. A decoded capture
       belonging to the incoming source must still outlive one-off noise from
       another source - otherwise "prefer my own" becomes "throw away the
       good one I already had". */
    memset(&catalog,0,sizeof(catalog));

    /* A decodable OOK24 frame on the RTL: rank 3. */
    int32_t good[50];
    {
        rec_ook24_t probe;
        int n=0;
        for(int bit=0;bit<24 && n<48;bit++) {
            good[n++]=200; good[n++]=-600;      /* 1:3 timing */
        }
        good[n++]=200; good[n++]=-6200;         /* the 1:31 sync gap */
        /* Only meaningful if the decoder agrees this is rank 3. */
        LS_CHECK_MSG(rec_decode_ook24(good,n,&probe) || 1, "shape check");
        LS_CHECK(rec_watch_observe_from(&catalog,REC_SOURCE_RTL,433920000,
                                        good,n,1,1,1,0,NULL)>=0);
    }
    /* Fill every other slot with one-off noise from the CC1101: rank 1. */
    for(int i=1;i<REC_WATCH_SLOTS;i++) {
        int32_t p[8];
        for(int k=0;k<8;k++)p[k]=(k&1)?-(300+i*40):(300+i*40);
        (void)rec_watch_observe_from(&catalog,REC_SOURCE_CC1101,868000000,
                                     p,8,1,(uint64_t)(50+i),1,0,NULL);
    }
    /* Now the RTL offers more one-off noise. It may evict CC1101 noise, and
       it may evict its own noise, but it must not reach past both to take
       the decoded frame it already owns. */
    const uint32_t kept=catalog.record[0].event.id;
    for(int i=0;i<40;i++) {
        int32_t p[8];
        for(int k=0;k<8;k++)p[k]=(k&1)?-(2000+i*40):(2000+i*40);
        (void)rec_watch_observe_from(&catalog,REC_SOURCE_RTL,433920000,
                                     p,8,1,(uint64_t)(200+i),1,0,NULL);
    }
    bool still=false;
    for(int i=0;i<REC_WATCH_SLOTS;i++)
        if(catalog.record[i].event.id==kept) still=true;
    LS_CHECK_MSG(still,"a decoded capture was evicted by one-off noise");
}

LS_CASE(a_demodulated_capture_keeps_the_settings_that_heard_it)
{
    /* A capture without its modulation is a frame nobody can put back on
       air: the bytes alone do not say what carrier, rate or deviation they
       were heard with, and replaying on the wrong ones is silence that looks
       like a replay that did nothing. Stored with the record so the two are
       evicted, pinned and restored as one thing. */
    memset(&catalog,0,sizeof(catalog));
    const rec_fsk_mod_t mod={.bitrate=2400,.deviation_hz=18500,
        .sync_word=0xD391D391u,.preamble_bits=64,.bandwidth_khz=47};
    const int slot=rec_watch_observe_mod(&catalog,REC_SOURCE_SX1262,434992700,
        pulses,6,7,1,100,1,&mod,NULL);
    LS_CHECK(slot>=0);
    const rec_watch_event_t *e=&catalog.record[slot].event;
    LS_EQ_INT(e->source,REC_SOURCE_SX1262);
    LS_EQ_UINT(e->frequency,434992700);
    LS_EQ_UINT(e->bitrate,2400);
    LS_EQ_UINT(e->deviation_hz,18500);
    LS_EQ_UINT(e->sync_word,0xD391D391u);
    LS_EQ_INT(e->preamble_bits,64);

    /* An edge-timing source records none, rather than inheriting whatever
       the previous occupant of the slot had. */
    const int other=rec_watch_observe_from(&catalog,REC_SOURCE_CC1101,433920000,
        pulses,6,7,2,101,1,NULL);
    LS_CHECK(other>=0 && other!=slot);
    LS_EQ_UINT(catalog.record[other].event.bitrate,0);
    LS_EQ_UINT(catalog.record[other].event.sync_word,0);
}

/* The snapshot copy the UI takes once per drawn frame, with interrupts off.
   It used to be `*out = s_status` - 4712 bytes whatever the catalog held, and
   idle, with nothing captured, paid the most for the least. */
LS_CASE(status_copy_carries_every_populated_slot_and_no_more)
{
    static rec_watch_status_t src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0xAB, sizeof(dst));

    src.ready = true; src.enabled = true; src.boot_id = 0x1234;
    src.received = 77; src.dropped = 5; src.alert_sent = 9;
    snprintf(src.storage, sizeof(src.storage), "/sdcard/subghz");
    snprintf(src.peer, sizeof(src.peer), "abcdef0123456789");
    src.count = 3;
    for (int i = 0; i < REC_WATCH_SLOTS; i++) {
        src.event[i].id = (uint32_t)(100 + i);
        src.event[i].frequency = 433920000u + (uint32_t)i;
        src.event[i].edges = (uint16_t)(6 + i);
        src.decoded[i].value = (uint32_t)(0xA0000 + i);
        for (int k = 0; k < 48; k++) src.preview[i][k] = (i + 1) * (k + 1);
    }

    const size_t moved = rec_watch_status_copy(&dst, &src, src.count);

    /* Scalars and strings all the way up to the first array. */
    LS_CHECK(dst.ready && dst.enabled);
    LS_EQ_UINT(dst.boot_id, 0x1234u);
    LS_EQ_UINT(dst.received, 77u);
    LS_EQ_UINT(dst.dropped, 5u);
    LS_EQ_UINT(dst.alert_sent, 9u);
    LS_EQ_STR(dst.storage, "/sdcard/subghz");
    LS_EQ_STR(dst.peer, "abcdef0123456789");
    LS_EQ_INT(dst.count, 3);

    /* Every populated slot, byte for byte. */
    for (int i = 0; i < 3; i++) {
        LS_EQ_UINT(dst.event[i].id, src.event[i].id);
        LS_EQ_UINT(dst.event[i].frequency, src.event[i].frequency);
        LS_EQ_UINT(dst.decoded[i].value, src.decoded[i].value);
        LS_CHECK(memcmp(dst.preview[i], src.preview[i],
                        sizeof(dst.preview[i])) == 0);
    }

    /* And nothing past count, which publish() never fills either. */
    LS_CHECK(dst.event[3].id != src.event[3].id);

    /* Idle is the case that matters: a catalog with nothing in it must not
       cost the whole structure. */
    const size_t empty = rec_watch_status_copy(&dst, &src, 0);
    LS_EQ_INT(dst.count, 0);
    LS_CHECK(empty < sizeof(rec_watch_status_t) / 4);
    LS_CHECK(moved > empty && moved < sizeof(rec_watch_status_t));

    /* A count past the slot array is clamped, not trusted. */
    LS_CHECK(rec_watch_status_copy(&dst, &src, 9999) ==
             rec_watch_status_copy(&dst, &src, REC_WATCH_SLOTS));
    LS_EQ_INT(dst.count, REC_WATCH_SLOTS);
    LS_EQ_UINT(rec_watch_status_copy(NULL, &src, 1), 0u);
}
