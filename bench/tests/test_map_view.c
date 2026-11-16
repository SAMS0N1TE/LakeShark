#include "ls_test.h"
#include "core/ls_map_view.h"

LS_CASE(a_view_survives_packing_to_within_a_metre)
{
    double lat = 0, lon = 0; int z = 0;
    const uint64_t v = ls_map_view_pack(-34.6037220, -58.3815920, 14);
    LS_CHECK(v != 0);
    LS_CHECK(ls_map_view_unpack(v, &lat, &lon, &z));
    LS_NEAR(lat, -34.603722, 1e-6); LS_NEAR(lon, -58.381592, 1e-6); LS_EQ_INT(z, 14);
    LS_CHECK(ls_map_view_unpack(ls_map_view_pack(85, 180, 22), &lat, &lon, &z));
    LS_NEAR(lat, 85, 1e-6); LS_NEAR(lon, 180, 1e-6); LS_EQ_INT(z, 22);
    LS_CHECK(ls_map_view_unpack(ls_map_view_pack(-85, -180, 0), &lat, &lon, &z));
    LS_NEAR(lat, -85, 1e-6); LS_NEAR(lon, -180, 1e-6); LS_EQ_INT(z, 0);
}

LS_CASE(nothing_saved_and_nonsense_are_not_a_view)
{
    double lat = 1, lon = 2; int z = 3;
    LS_CHECK(!ls_map_view_unpack(0, &lat, &lon, &z));
    LS_CHECK(ls_map_view_pack(NAN, 0, 12) == 0);
    LS_CHECK(ls_map_view_pack(86, 0, 12) == 0);
    LS_CHECK(ls_map_view_pack(0, 181, 12) == 0);
    LS_CHECK(ls_map_view_pack(0, 0, 23) == 0);
    LS_CHECK(!ls_map_view_unpack(UINT64_C(1) << 63 | UINT64_C(0x1FFFFFFF) << 29, &lat, &lon, &z));
    LS_NEAR(lat, 1, 0); LS_NEAR(lon, 2, 0); LS_EQ_INT(z, 3);
}

LS_CASE(a_map_never_placed_opens_at_home_keeping_its_zoom)
{
    double lat = 43.4445, lon = -71.6473; int z = 12;
    LS_CHECK(ls_map_view_start(0, true, -36.6734, -60.1133, &lat, &lon, &z));
    LS_NEAR(lat, -36.6734, 1e-9); LS_NEAR(lon, -60.1133, 1e-9); LS_EQ_INT(z, 12);
}

LS_CASE(the_view_it_was_left_at_wins_over_home)
{
    double lat = 43.4445, lon = -71.6473; int z = 12;
    LS_CHECK(ls_map_view_start(ls_map_view_pack(-34.6, -58.38, 15), true, -36.6734, -60.1133, &lat, &lon, &z));
    LS_NEAR(lat, -34.6, 1e-6); LS_NEAR(lon, -58.38, 1e-6); LS_EQ_INT(z, 15);
}

LS_CASE(no_view_and_no_home_leaves_the_map_where_it_is)
{
    double lat = 43.4445, lon = -71.6473; int z = 12;
    LS_CHECK(!ls_map_view_start(0, false, 0, 0, &lat, &lon, &z));
    LS_CHECK(!ls_map_view_start(0, true, 91, 0, &lat, &lon, &z));
    LS_NEAR(lat, 43.4445, 0); LS_NEAR(lon, -71.6473, 0); LS_EQ_INT(z, 12);
}
