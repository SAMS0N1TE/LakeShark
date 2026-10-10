#include "ls_test.h"
#include "../../components/apps/tui/ls_df_sources.c"
static ls_ble_heard_t received;
static uint8_t looked_up[6];
int ls_wifi_scan_cached(ls_wifi_scan_ap_t *out,int cap,int64_t *us) {
    memset(out,0,sizeof(*out));memcpy(out->bssid,looked_up,6);out->rssi=-62;*us=10000000;return 1;
}
void ls_wireless_get(ls_wireless_snapshot_t *out) {memset(out,0,sizeof(*out));}
bool ls_ble_heard_find(const uint8_t addr[6],ls_ble_heard_t *out) {memcpy(looked_up,addr,6);*out=received;return true;}
LS_CASE(address_handoff_reads_target_by_address_after_heard_list_churn) {
    const uint8_t target[6]={7,6,5,4,3,2};
    s.slot[0].source=LS_DFS_BLE;s.slot[0].active=true;
    LS_CHECK(ls_dfs_target_address(target,"SWEEP T07"));
    LS_CHECK(s.slot[0].target>=0 && !memcmp(s.slot[0].target_key,target,6));
    ls_shim_time_set(10000000);
    received.adverts=1;received.last_us=10000000;received.rssi=-58;
    memset(s_ble,0,sizeof(s_ble)); /* target is no longer at the old index */
    step_ble(0,(uint8_t *)s.slot[0].target_key,true);
    LS_CHECK(!memcmp(looked_up,target,6));LS_EQ_UINT(s.slot[0].updates,1);LS_CHECK(s.slot[0].level==-58);
    step_ble(0,(uint8_t *)s.slot[0].target_key,true);LS_EQ_UINT(s.slot[0].updates,1);
    received.adverts++;received.rssi=-52;step_ble(0,(uint8_t *)s.slot[0].target_key,true);LS_EQ_UINT(s.slot[0].updates,2);
    s.slot[0].source=LS_DFS_WIFI;LS_CHECK(ls_dfs_target_address(target,"SWEEP camera"));
    step_wifi_address(0,(uint8_t *)s.slot[0].target_key);LS_EQ_UINT(s.slot[0].updates,3);LS_CHECK(s.slot[0].level==-62);
    step_wifi_address(0,(uint8_t *)s.slot[0].target_key);LS_EQ_UINT(s.slot[0].updates,3);
    s.slot[0].source=LS_DFS_MESH;LS_CHECK(!ls_dfs_target_address(target,"wrong radio"));
}

