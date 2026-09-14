/* LS_TEST_SOURCES: ${APP}/cell/cell_report_core.c */
/* LS_TEST_INCLUDE: ${APP}/cell */
#include "ls_test.h"
#include "cell_report.h"
#include <string.h>
LS_CASE(report_wire_is_bounded_and_has_known_encoding)
{
    cell_report_record_t r={.pci=-1,.lat_e5=INT32_MIN,.lon_e5=INT32_MIN,.kind='T'};
    char b[50];memset(b,'!',sizeof(b));
    LS_CHECK(cell_report_encode(&r,b,49));
    LS_CHECK(!strcmp(b,"CW1:AAAAAAAAAAAAAAAAAAAAAAAAAIAAAACAAAD//wAAVAA="));
    LS_EQ_INT(b[49],'!');
    LS_CHECK(!cell_report_encode(&r,b,48));
    r.pci=504;LS_CHECK(!cell_report_encode(&r,b,sizeof(b)));
    r.pci=-1;r.kind='X';LS_CHECK(!cell_report_encode(&r,b,sizeof(b)));
}
