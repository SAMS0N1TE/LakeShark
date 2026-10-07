/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/ls_route.c */
#include "ls_test.h"
#include "ls_route.h"
#include "ls_track.h"
#include "ls_rlog.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static ls_route_point_t points[LS_ROUTE_CAP+1];
static ls_route_t r;
static bool parse(const char *xml,size_t cap)
{
    ls_route_init(&r,points,cap);
    FILE *f=ls_test_tmpfile(); LS_CHECK(f!=NULL);
    fputs(xml,f);bool ok=ls_route_gpx(&r,f);fclose(f);return ok;
}
static void straight(void)
{
    LS_CHECK(parse("<gpx><rte><rtept lat='0' lon='0'/><rtept lat='0' lon='.01'/><rtept lat='0' lon='.02'/></rte></gpx>",LS_ROUTE_CAP));
}
LS_CASE(gpx_tracks_routes_namespaces_and_optional_elevation)
{
    LS_CHECK(parse("<?xml version='1.0'?><g:gpx xmlns:g='http://www.topografix.com/GPX/1/1'><g:trk><g:name>walk</g:name><g:trkseg><g:trkpt lon='-71' lat='43'><g:ele>12</g:ele></g:trkpt><g:trkpt lat='43.001' lon='-71'/></g:trkseg></g:trk></g:gpx>",LS_ROUTE_CAP));
    LS_EQ_INT(2,r.n);LS_NEAR(111.195,r.length_m,.1);
    LS_CHECK(parse("\xef\xbb\xbf<gpx><!-- metadata > stays text --><rte><rtept lat=\"0\" lon=\"0\"/><rtept lat=\"0\" lon=\".01\"/></rte></gpx>",LS_ROUTE_CAP));
    straight();LS_EQ_INT(3,r.n);LS_NEAR(2223.902,r.length_m,.2);
}
LS_CASE(gpx_rejects_garbage_truncation_nonfinite_coordinates_and_dtd)
{
    const char *bad[]={
        "garbage", "<gpx><rte><rtept lat='0' lon='0'/><rtept lat='0' lon='1'/></gpx>",
        "<gpx><rte><rtept lat='0' lon='0'/><rtept lat='nan' lon='1'/></rte></gpx>",
        "<gpx><rte><rtept lat='0' lon='0'/><rtept lat='91' lon='1'/></rte></gpx>",
        "<gpx><rte><rtept lat='0' lon='0'/><rtept lon='1'/></rte></gpx>",
        "<gpx><rte><rtept lat='0' lon='0'/><rtept lat='0x' lon='1'/></rte></gpx>",
        "<gpx><rte><rtept lat='0' lon='0'/><rtept lat='0' lat='1' lon='1'/></rte></gpx>",
        "<gpx><rte><rtept lat=0 lon='0'/><rtept lat='0' lon='1'/></rte></gpx>",
        "<!DOCTYPE gpx><gpx><rte><rtept lat='0' lon='0'/><rtept lat='0' lon='1'/></rte></gpx>",
        "<gpx><rte><rtept lat='0' lon='0'/><rtept lat='0' lon='1'/></rte></gpx>trash",
        "<gpx/><gpx/>",
        "<gpx><trkpt lat='0' lon='0'/><trkpt lat='0' lon='1'/></gpx>",
        "<gpx><rte><rtept lat='0' lon='0'/><rtept lat='0x1' lon='1'/></rte></gpx>"
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        LS_CHECK(!parse(bad[i],LS_ROUTE_CAP));LS_CHECK(!r.valid);LS_EQ_INT(0,r.n);
    }
}
LS_CASE(gpx_cap_uniform_decimation_and_endpoints)
{
    ls_route_init(&r,points,LS_ROUTE_CAP+1);
    points[LS_ROUTE_CAP].lat=123;
    FILE *f=ls_test_tmpfile();LS_CHECK(f!=NULL);
    fputs("<gpx><trk><trkseg>",f);
    for(int i=0;i<10001;i++) fprintf(f,"<trkpt lat='0' lon='%.7f'/>",i/100000.0);
    fputs("</trkseg></trk></gpx>",f);
    LS_CHECK(ls_route_gpx(&r,f));fclose(f);
    LS_EQ_INT(LS_ROUTE_CAP,r.n);LS_NEAR(0,points[0].lon,1e-9);LS_NEAR(.1,points[r.n-1].lon,1e-9);
    LS_NEAR(123,points[LS_ROUTE_CAP].lat,0);
    for(size_t i=1;i<r.n;i++) LS_CHECK(points[i].lon>points[i-1].lon);
    LS_CHECK(parse("<gpx><rte><rtept lat='0' lon='0'/><rtept lat='0' lon='1'/><rtept lat='0' lon='2'/></rte></gpx>",2));
    LS_EQ_INT(2,r.n);LS_NEAR(2,points[1].lon,0);
}
LS_CASE(gpx_bounds_tags_depth_and_bytes)
{
    ls_route_init(&r,points,LS_ROUTE_CAP);
    FILE *f=ls_test_tmpfile();LS_CHECK(f!=NULL);
    fputs("<gpx name='",f);for(int i=0;i<500;i++) fputc('x',f);fputs("'/>",f);
    LS_CHECK(!ls_route_gpx(&r,f));fclose(f);
    f=ls_test_tmpfile();fputs("<gpx>",f);for(int i=0;i<20;i++) fputs("<x>",f);
    LS_CHECK(!ls_route_gpx(&r,f));fclose(f);
    f=ls_test_tmpfile();fputs("<gpx>",f);for(size_t i=0;i<LS_ROUTE_FILE_MAX;i++) fputc(' ',f);fputs("</gpx>",f);
    LS_CHECK(!ls_route_gpx(&r,f));fclose(f);
}
LS_CASE(segment_gaps_are_not_connected_and_reverse_preserves_them)
{
    LS_CHECK(parse("<gpx><trk><trkseg><trkpt lat='0' lon='0'/><trkpt lat='0' lon='.01'/></trkseg><trkseg><trkpt lat='1' lon='1'/><trkpt lat='1' lon='1.01'/></trkseg></trk></gpx>",LS_ROUTE_CAP));
    LS_CHECK(points[2].start);LS_CHECK(r.length_m<2224);
    double length=r.length_m;ls_route_reverse(&r);LS_CHECK(points[2].start);LS_CHECK(!points[1].start);
    LS_NEAR(length,r.length_m,.01);LS_NEAR(1.01,points[0].lon,1e-9);
    ls_route_reverse(&r);LS_NEAR(0,points[0].lon,0);LS_CHECK(points[2].start);
}
LS_CASE(cross_track_progress_remaining_and_next_bearing)
{
    straight();LS_EQ_INT(LS_ROUTE_NONE,ls_route_update(&r,true,.001,.005));
    LS_NEAR(111.195,r.cross_m,.1);LS_NEAR(555.975,r.progress_m,.1);
    LS_NEAR(1667.926,r.remaining_m,.2);LS_EQ_INT(1,r.next);
    LS_CHECK(r.bearing>90 && r.bearing<110);
    ls_route_update(&r,true,0,.012);LS_NEAR(1334.341,r.progress_m,.2);LS_EQ_INT(2,r.next);LS_NEAR(90,r.bearing,.01);
    ls_route_update(&r,true,0,-.001);LS_NEAR(0,r.progress_m,.01);LS_NEAR(111.195,r.cross_m,.1);
    ls_route_update(&r,true,0,.025);LS_NEAR(0,r.remaining_m,.01);LS_NEAR(555.975,r.cross_m,.1);LS_CHECK(!r.arrived);
}
LS_CASE(dateline_and_duplicate_points_have_finite_geometry)
{
    LS_CHECK(parse("<gpx><rte><rtept lat='0' lon='179.99'/><rtept lat='0' lon='179.99'/><rtept lat='0' lon='-179.99'/></rte></gpx>",LS_ROUTE_CAP));
    ls_route_update(&r,true,0,180);LS_NEAR(1111.951,r.progress_m,.2);LS_NEAR(0,r.cross_m,.01);LS_NEAR(90,r.bearing,.01);
}
LS_CASE(off_route_hysteresis_uses_distinct_samples_and_stale_fixes_reset_dwell)
{
    straight();r.off_m=40;
    LS_EQ_INT(LS_ROUTE_NONE,ls_route_update(&r,true,.001,.005));
    LS_EQ_INT(LS_ROUTE_NONE,ls_route_update(&r,true,.001,.005));
    ls_route_update(&r,false,0,.005);LS_CHECK(!r.located);LS_CHECK(isnan(r.bearing));
    for(int i=0;i<2;i++) LS_EQ_INT(LS_ROUTE_NONE,ls_route_update(&r,true,.001,.005));
    LS_EQ_INT(LS_ROUTE_OFF,ls_route_update(&r,true,.001,.005));LS_CHECK(r.off_route);
    for(int i=0;i<5;i++) LS_EQ_INT(LS_ROUTE_NONE,ls_route_update(&r,true,.0003,.005));
    LS_CHECK(r.off_route);
    LS_EQ_INT(LS_ROUTE_NONE,ls_route_update(&r,true,0,.005));
    LS_EQ_INT(LS_ROUTE_REJOIN,ls_route_update(&r,true,0,.005));LS_CHECK(!r.off_route);
}
LS_CASE(arrival_requires_end_distance_and_route_progress_and_alerts_once)
{
    straight();LS_EQ_INT(LS_ROUTE_NONE,ls_route_update(&r,true,0,.01995));
    LS_EQ_INT(LS_ROUTE_ARRIVE,ls_route_update(&r,true,0,.01995));LS_CHECK(r.arrived);LS_CHECK(isnan(r.bearing));
    LS_EQ_INT(LS_ROUTE_NONE,ls_route_update(&r,true,0,.02));
    ls_route_reverse(&r);LS_CHECK(!r.arrived);LS_CHECK(!r.located);
    LS_NEAR(.02,points[0].lon,1e-9);
    LS_CHECK(parse("<gpx><rte><rtept lat='0' lon='0'/><rtept lat='0' lon='.01'/><rtept lat='.01' lon='.01'/><rtept lat='0' lon='0'/></rte></gpx>",LS_ROUTE_CAP));
    ls_route_update(&r,true,0,0);ls_route_update(&r,true,0,0);LS_CHECK(!r.arrived);
}
LS_CASE(backtrack_reads_ring_in_reverse_oldest_to_newest_and_validates_header)
{
    ls_route_init(&r,points,2);
    FILE *f=ls_test_tmpfile();LS_CHECK(f!=NULL);
    ls_rlog_header_t h={ LS_RLOG_MAGIC,LS_RLOG_VERSION,sizeof(ls_track_pt_t),LS_TRACK_CAPACITY,3,1 };
    fwrite(&h,sizeof(h),1,f);
    ls_track_pt_t p={0};
    /* Wrapped ring: oldest at capacity-2, newest at slot zero. */
    for(int i=0;i<3;i++) {
        unsigned slot=(h.head+h.capacity-h.count+i)%h.capacity;p.lon_e7=i*100000;
        fseek(f,sizeof(h)+slot*sizeof(p),SEEK_SET);fwrite(&p,sizeof(p),1,f);
    }
    LS_CHECK(ls_route_track(&r,f));LS_EQ_INT(2,r.n);LS_CHECK(r.reversed);
    LS_NEAR(.02,points[0].lon,1e-9);LS_NEAR(0,points[1].lon,0);
    h.head=h.capacity;fseek(f,0,SEEK_SET);fwrite(&h,sizeof(h),1,f);
    LS_CHECK(!ls_route_track(&r,f));LS_CHECK(!r.valid);fclose(f);
}

LS_CASE(walked_part_stays_dim_when_retracing_and_off_route_cannot_advance_it)
{
    straight();ls_route_update(&r,true,0,.01);
    double walked=r.walked_m;
    ls_route_update(&r,true,0,.005);LS_CHECK(r.progress_m<walked);LS_NEAR(walked,r.walked_m,0);
    ls_route_update(&r,true,.001,.015);LS_NEAR(walked,r.walked_m,0);
    ls_route_reverse(&r);LS_NEAR(0,r.walked_m,0);
}
