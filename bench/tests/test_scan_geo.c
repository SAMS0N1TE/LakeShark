#include "ls_test.h"
#include "scan_geo.h"
#include "scan_import.h"
#include <math.h>
#include <string.h>

LS_CASE(geographic_filter_handles_range_hysteresis_and_fix_loss)
{
    scan_channel_t c = {.radius_m=10000};
    scan_geo_t s = {0};
    scan_geo_update(&s, &c, 1, true, 0, .08, 1000000, 1000000);
    LS_CHECK(scan_geo_admits(&s,0,1000000));
    scan_geo_update(&s, &c, 1, true, 0, .095, 2000000, 2000000);
    LS_CHECK(scan_geo_admits(&s,0,2000000));
    scan_geo_update(&s, &c, 1, true, 0, .11, 3000000, 3000000);
    LS_CHECK(!scan_geo_admits(&s,0,3000000));
    scan_geo_update(&s, &c, 1, true, 0, .095, 4000000, 4000000);
    LS_CHECK(!scan_geo_admits(&s,0,4000000));
    scan_geo_update(&s, &c, 1, true, 0, 0, 5000000, 5000000);
    scan_geo_update(&s, &c, 1, false, 50, 50, 6000000, 6000000);
    LS_CHECK(scan_geo_admits(&s,0,30000000));
    LS_CHECK(!scan_geo_admits(&s,0,36000000));
}
LS_CASE(geographic_filter_crosses_dateline_and_rejects_stale_or_invalid_fixes)
{
    scan_channel_t c[2] = {{.lon_e7=1799900000,.radius_m=5000},{0}};
    scan_geo_t s = {0};
    scan_geo_update(&s,c,2,true,0,-179.99,1000000,1000000);
    LS_CHECK(scan_geo_admits(&s,0,1000000));
    LS_CHECK(!scan_geo_admits(&s,1,1000000));
    scan_geo_update(&s,c,2,true,NAN,0,100000000,100000000);
    LS_CHECK(!scan_geo_ready(&s,100000000));
    scan_geo_update(&s,c,2,true,0,0,1000000,100000000);
    LS_CHECK(!scan_geo_ready(&s,100000000));
}
LS_CASE(import_line_validates_before_changing_destination)
{
    scan_channel_t c = {0};
    LS_CHECK(scan_import_line("County Dispatch|154785000|P25|1|43.4|-71.6|30|1", &c));
    LS_EQ_INT(c.mode,SCAN_MODE_P25); LS_EQ_INT(c.radius_m,30000);
    scan_channel_t before=c;
    const char *bad[]={"Bad|154785000|DMR|1|43|-71|30|0", "Bad|nan|NFM|1|43|-71|30|0",
        "Bad|154785000|P25|1|91|-71|30|0", "Bad|154785000|P25|1|43|-71|-1|0",
        "Bad|154785000|P25|1|43|-71|30|0|extra", "Bad|154785000|P25||43|-71|30|0"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        LS_CHECK(!scan_import_line(bad[i],&c)); LS_CHECK(!memcmp(&before,&c,sizeof(c)));
    }
}
