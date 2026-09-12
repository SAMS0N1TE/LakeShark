#include "ls_test.h"
#include "location_pref.h"
LS_CASE(location_defaults_migration_and_explicit_zero)
{
    uint64_t stored = 0; float lat,lon;
    LS_EQ_INT(location_load(false,0,false,0,0),0);
    LS_EQ_INT(location_load(false,0,true,0,0),0);
    LS_CHECK(location_pack(0,0,&stored));
    LS_CHECK(location_unpack(location_load(true,stored,false,0,0),&lat,&lon));
    LS_CHECK(lat==0 && lon==0);
    LS_EQ_INT(location_load(true,0,true,12000000,34000000),0);
    LS_CHECK(location_unpack(location_load(false,0,true,12000000,-34000000),&lat,&lon));
    LS_CHECK(lat==12 && lon==-34);
    LS_EQ_INT(location_load(false,0,true,91000000,0),0);
}
LS_CASE(location_bounds_corruption_and_roundtrip)
{
    uint64_t stored = 0; float lat,lon;
    for (int a=-90;a<=90;a+=90) for (int b=-180;b<=180;b+=180) {
        LS_CHECK(location_pack(a,b,&stored));
        LS_CHECK(location_unpack(stored,&lat,&lon));
        LS_CHECK(lat==a && lon==b);
    }
    LS_CHECK(!location_pack(NAN,0,&stored));
    LS_CHECK(!location_pack(0,INFINITY,&stored));
    LS_CHECK(!location_pack(90.000001,0,&stored));
    LS_CHECK(!location_pack(0,-180.000001,&stored));
    LS_CHECK(!location_unpack(UINT64_MAX,&lat,&lon));
    LS_CHECK(!location_unpack(UINT64_C(1)<<62,&lat,&lon));
    LS_CHECK(!location_unpack((UINT64_C(1)<<63)|360000001,&lat,&lon));
    LS_CHECK(location_pack(-12.123456,34.654321,&stored));
    LS_CHECK(location_unpack(stored,&lat,&lon));
    LS_CHECK(fabs(lat+12.123456)<0.00001 && fabs(lon-34.654321)<0.00001);
}
LS_CASE(location_manual_entry_is_strict)
{
    double value;
    const char *bad[]={"", " ","NaN","inf","1e2","1x","--1",".","1.2.3"," 1","1 "};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) LS_CHECK(!location_parse(bad[i],true,&value));
    LS_CHECK(location_parse("-90",false,&value) && value==-90);
    LS_CHECK(location_parse("+180.000000",true,&value) && value==180);
    LS_CHECK(!location_parse("90.000001",false,&value));
    LS_CHECK(!location_parse("180.000001",true,&value));
}
