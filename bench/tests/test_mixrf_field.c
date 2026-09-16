/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/board */
#include "ls_test.h"
#include "ls_mixrf.h"
LS_CASE(startup_is_not_an_arrival)
{
    ls_mixrf_field_edge_t edge={0};
    LS_CHECK(!ls_mixrf_field_arrival(&edge,true));
    LS_CHECK(edge.present);
    LS_CHECK(!ls_mixrf_field_arrival(&edge,false));
    LS_CHECK(ls_mixrf_field_arrival(&edge,true));
    LS_CHECK(!ls_mixrf_field_arrival(&edge,true));
}
LS_CASE(distinct_field_cycles)
{
    ls_mixrf_field_edge_t edge={0};
    LS_CHECK(!ls_mixrf_field_arrival(&edge,false));
    LS_CHECK(ls_mixrf_field_arrival(&edge,true));
    LS_CHECK(!ls_mixrf_field_arrival(&edge,false));
    LS_CHECK(ls_mixrf_field_arrival(&edge,true));
    edge=(ls_mixrf_field_edge_t){0};
    LS_CHECK(!ls_mixrf_field_arrival(&edge,true));
}
