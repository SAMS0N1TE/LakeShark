#include "ls_test.h"
#include "../../components/apps/tui/ls_df_sources.c"
static ls_ble_heard_t received;
static uint8_t looked_up[6];
static int requests;
static bool wireless_busy,request_ok=true;
static int64_t cache_us=10000000;
static int cache_result=1;
bool ls_wireless_request(ls_wireless_op_t op,const char *ssid,const char *pass) {LS_CHECK(op==LS_WIRELESS_SCAN && !ssid && !pass);requests++;return request_ok;}
int ls_wifi_scan_cached(ls_wifi_scan_ap_t *out,int cap,int64_t *us) {
    memset(out,0,sizeof(*out));memcpy(out->bssid,looked_up,6);out->rssi=-62;*us=cache_us;return cache_result;
}
void ls_wireless_get(ls_wireless_snapshot_t *out) {memset(out,0,sizeof(*out));out->busy=wireless_busy;}
bool ls_ble_heard_find(const uint8_t addr[6],ls_ble_heard_t *out) {memcpy(looked_up,addr,6);*out=received;return true;}
LS_CASE(address_handoff_reads_target_by_address_after_heard_list_churn) {
    memset(&s,0,sizeof(s));memset(w,0,sizeof(w));s_wifi_scan_us=0;
    cache_us=10000000;cache_result=1;wireless_busy=false;request_ok=true;
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


LS_CASE(wifi_address_find_requests_bounded_scans_and_reports_waiting) {
    uint8_t target[6]={7,6,5,4,3,2};memcpy(looked_up,target,6);
    memset(&w[0],0,sizeof(w[0]));s_wifi_scan_us=0;requests=0;
    s.slot[0].source=LS_DFS_WIFI;s.slot[0].updates=0;
    ls_shim_time_set(10000000);cache_us=10000000;step_wifi_address(0,target);
    LS_EQ_INT(requests,1);LS_EQ_UINT(s.slot[0].updates,1);
    step_wifi_address(0,target);LS_EQ_INT(requests,1);LS_EQ_UINT(s.slot[0].updates,1);
    ls_shim_time_set(14000000);wireless_busy=true;step_wifi_address(0,target);LS_EQ_INT(requests,1);
    LS_CHECK(strstr(s.slot[0].status,"busy"));
    wireless_busy=false;request_ok=false;step_wifi_address(0,target);LS_EQ_INT(requests,2);
    LS_CHECK(strstr(s.slot[0].status,"unavailable"));
    step_wifi_address(0,target);LS_EQ_INT(requests,2);
    ls_shim_time_set(18000000);request_ok=true;cache_result=-1;step_wifi_address(0,target);LS_EQ_INT(requests,3);LS_EQ_UINT(s.slot[0].updates,1);
    cache_result=1;cache_us=18000000;step_wifi_address(0,target);LS_EQ_UINT(s.slot[0].updates,2);
    ls_shim_time_set(24000000);step_wifi_address(0,target);LS_EQ_INT(requests,4);LS_EQ_UINT(s.slot[0].updates,2);
}
