#include "ls_test.h"
#include "ls_map_motion.h"
LS_CASE(aircraft_eases_between_fixes_without_extrapolating)
{
    ls_map_motion_t t={0}; double lat,lon;
    ls_map_motion_position(&t,1,1000000,42,-71,1000000,&lat,&lon);
    ls_map_motion_position(&t,1,2000000,43,-70,2000000,&lat,&lon);
    LS_NEAR(lat,42,1e-8);
    ls_map_motion_position(&t,1,2000000,43,-70,2375000,&lat,&lon);
    LS_NEAR(lat,42.5,1e-8);
    ls_map_motion_position(&t,1,2000000,43,-70,9000000,&lat,&lon);
    LS_NEAR(lat,43,1e-8);
}
LS_CASE(aircraft_crosses_dateline_and_new_slot_does_not_inherit_motion)
{
    ls_map_motion_t t={0}; double lat,lon;
    ls_map_motion_position(&t,1,1000000,42,179.9,1000000,&lat,&lon);
    ls_map_motion_position(&t,1,2000000,42,-179.9,2000000,&lat,&lon);
    ls_map_motion_position(&t,1,2000000,42,-179.9,2375000,&lat,&lon);
    LS_NEAR(fabs(lon),180,1e-8);
    ls_map_motion_position(&t,2,2000000,51,0,2375000,&lat,&lon);
    LS_NEAR(lat,51,1e-8); LS_NEAR(lon,0,1e-8);
}
