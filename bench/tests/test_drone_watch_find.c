#include "ls_test.h"
#include "../../components/apps/tui/ls_df_sources.c"
static ls_drone_watch_entry_t entry;
bool ls_drone_watch_read(uint64_t id,ls_drone_watch_entry_t *out) {
    if(id!=entry.id)return false;*out=entry;return true;
}
LS_CASE(cached_feed_preserves_58ghz_timestamp_units_and_freshness) {
    memset(&s,0,sizeof(s));memset(w,0,sizeof(w));
    entry=(ls_drone_watch_entry_t){.id=7,.centre_hz=5806000000ULL,.rssi_dbm=-30,.last_us=10000000,.calibrated=false};
    ls_shim_time_set(10001000);step_drone(0,7);
    ls_dfs_reading_t reading;LS_EQ_INT(ls_dfs_take(&reading,1),1);
    LS_CHECK(reading.centre_hz==5806000000ULL && reading.freq_hz==0 && reading.us==10000000 && reading.level==-30);
    LS_EQ_STR(s.slot[0].unit,"dB");
    step_drone(0,7);LS_EQ_INT(ls_dfs_take(&reading,1),0);
    ls_shim_time_set(13000000);entry.last_us=11000000;step_drone(0,7);LS_EQ_INT(ls_dfs_take(&reading,1),0);
    entry.last_us=14000000;step_drone(0,7);LS_EQ_INT(ls_dfs_take(&reading,1),0); /* future */
    entry.last_us=13000000;entry.centre_hz=915000000;entry.calibrated=true;entry.rssi_dbm=-62;
    step_drone(0,7);LS_EQ_INT(ls_dfs_take(&reading,1),1);LS_CHECK(reading.freq_hz==915000000 && reading.level==-62);
    LS_EQ_STR(s.slot[0].unit,"dBm");step_drone(0,8);LS_EQ_INT(ls_dfs_take(&reading,1),0);
}
