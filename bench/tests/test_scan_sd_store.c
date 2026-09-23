#include "ls_test.h"
#define LS_SCAN_SD_ROOT "scan_sd_fixture"
#include "../../components/lakeshark/core/scan_sd_store.c"
LS_CASE(sd_profiles_preserve_large_lists_and_recover_backup)
{
    ls_test_mkdir(LS_SCAN_SD_ROOT);
    static scan_channel_t rows[SCAN_MAX_CHANNELS], loaded[SCAN_MAX_CHANNELS];
    for(int i=0;i<SCAN_MAX_CHANNELS;i++) rows[i]=(scan_channel_t){.name="Channel",.freq_hz=154785000+i,.flags=SCAN_FLAG_ENABLED};
    LS_CHECK(scan_sd_write("test",rows,SCAN_MAX_CHANNELS));
    LS_EQ_INT(scan_sd_read("test",loaded),SCAN_MAX_CHANNELS);
    LS_EQ_INT(loaded[SCAN_MAX_CHANNELS-1].freq_hz,rows[SCAN_MAX_CHANNELS-1].freq_hz);
    LS_CHECK(!scan_sd_write("../escape",rows,1));
    LS_CHECK(scan_sd_write("test",rows,2));
    FILE *f=fopen(LS_SCAN_SD_ROOT "/test.bin","wb"); fputs("broken",f); fclose(f);
    LS_EQ_INT(scan_sd_read("test",loaded),SCAN_MAX_CHANNELS);
    remove(LS_SCAN_SD_ROOT "/test.bin"); remove(LS_SCAN_SD_ROOT "/test.bak");
    ls_test_rmdir(LS_SCAN_SD_ROOT);
}
