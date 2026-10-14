"""Execute production host-queue scan reconciliation with a fake GAP controller."""
import unittest
from test_audio_reinit import ROOT, body, run_c

class BleScanTest(unittest.TestCase):
    def test_start_connection_restart_backoff_and_stop(self):
        source = (ROOT / 'main/ble_link.c').read_text()
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MYNEWT_VAL(x) 1
#define BLE_HS_ENOTSUP 55
static bool s_scan_coded,s_ext_rejected;
static bool s_c6_inhibit;
static uint64_t s_le_features;
#include "BLE_SCAN_HEADER"
struct ble_gap_event {int type;};
struct ble_npl_event {int unused;};
struct ble_gap_disc_params {int passive,itvl,window,filter_duplicates;};
enum {BLE_HS_CONN_HANDLE_NONE=65535,BLE_LINK_OFF=0,BLE_LINK_SYNCING,BLE_LINK_SCANNING,BLE_LINK_CONNECTING,BLE_LINK_DISCOVERING,BLE_LINK_READY,BLE_OWN_ADDR_PUBLIC,BLE_HS_FOREVER=-1,BLE_HS_EALREADY=10};
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static struct ble_npl_event s_scan_event;
static bool s_disconnect_requested,s_cancel_connect_requested;
static bool s_scan_event_ready=true,s_run=true,s_passive,s_scanning,s_scan_connected,s_scan_failed,active,synced=true,queued;
static int s_conn=BLE_HS_CONN_HANDLE_NONE,s_state=BLE_LINK_SYNCING,starts,cancels,fail;
static int64_t s_rescan_at_us,now;
static struct {int listener_count; char chains[4];int64_t chain_quarantine;} s_adverts={3};
static ble_scan_mode_t s_scan_mode;
static struct ble_gap_disc_params params;
static bool ble_hs_synced(void){return synced;}
static int64_t esp_timer_get_time(void){return now;}
static bool ble_gap_disc_active(void){return active;}
static int ble_gap_disc_cancel(void){cancels++;active=false;return 0;}
#define BLE_ERR_REM_USER_CONN_TERM 0
static void ble_gap_terminate(int conn,int why){(void)conn;(void)why;}
static void ble_gap_conn_cancel(void){}
static int gap_event(struct ble_gap_event*e,void*p){(void)e;(void)p;return 0;}
static int ble_gap_disc(int addr,int duration,struct ble_gap_disc_params*p,int(*cb)(struct ble_gap_event*,void*),void*arg){
 (void)addr;(void)duration;(void)cb;(void)arg;starts++;params=*p;if(fail)return fail;active=true;return 0;
}
struct ble_gap_ext_disc_params {uint16_t itvl,window;bool passive;};
static int ext_starts,ext_fail;static bool legacy_mode;
static void ble_link_legacy_scan(bool legacy){legacy_mode=legacy;}
static int ble_gap_ext_disc(int own,int duration,int period,int dup,int filter,int limited,
 const struct ble_gap_ext_disc_params *one,const struct ble_gap_ext_disc_params *coded,
 int(*cb)(struct ble_gap_event*,void*),void *arg){
 (void)own;(void)cb;(void)arg;ext_starts++;
 if(duration || period || dup || filter || limited || !coded || !coded->passive || one->itvl!=(one->passive?160:48) ||
    one->window!=24 || coded->window!=24)__builtin_trap();
 if(ext_fail)return ext_fail;active=true;return 0;
}
static void* nimble_port_get_dflt_eventq(void){return NULL;}
static void ble_npl_eventq_put(void*q,struct ble_npl_event*e){(void)q;(void)e;queued=true;}
''' + body(source, 'static void scan_refresh(') + '\n' + body(source, 'static void start_scan(void)\n{') + r'''
static void pump(void){if(queued){queued=false;scan_refresh(&s_scan_event);}}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 start_scan();start_scan();CHECK(queued && !starts);pump();CHECK(starts==1 && active && !params.passive && s_state==BLE_LINK_SCANNING);
 start_scan();pump();CHECK(starts==1);
 s_state=BLE_LINK_CONNECTING;start_scan();pump();CHECK(!active && cancels==1);
 s_conn=1;s_state=BLE_LINK_DISCOVERING;start_scan();pump();CHECK(active && s_scan_connected && s_scanning && s_state==BLE_LINK_DISCOVERING);
 CHECK(params.passive && params.itvl==160 && params.window==48 && !params.filter_duplicates);
 s_state=BLE_LINK_READY;start_scan();pump();CHECK(s_state==BLE_LINK_READY && starts==2);
 active=false;start_scan();pump();CHECK(active && s_state==BLE_LINK_READY && starts==3);
 active=false;fail=5;start_scan();pump();CHECK(!active && !s_scanning && s_scan_failed && s_state==BLE_LINK_READY);
 fail=0;start_scan();pump();CHECK(active && s_scanning && !s_scan_failed && starts==5);
 s_conn=BLE_HS_CONN_HANDLE_NONE;s_state=BLE_LINK_SCANNING;s_rescan_at_us=100;now=50;start_scan();pump();CHECK(params.passive && !s_scan_connected && starts==5);
 now=100;start_scan();pump();CHECK(!s_rescan_at_us && active && !params.passive && starts==6 && cancels==2);
 s_run=false;start_scan();pump();CHECK(!active && !s_scanning);
 s_run=true;s_conn=1;s_state=BLE_LINK_READY;s_adverts.listener_count=0;start_scan();pump();CHECK(!active);
 synced=false;s_adverts.listener_count=3;start_scan();pump();CHECK(!active);
 synced=true;s_le_features=0x1800;start_scan();pump();CHECK(active && s_scan_coded && !legacy_mode && ext_starts==1);
 active=false;ext_fail=5;start_scan();pump();CHECK(active && !s_scan_coded && legacy_mode && s_ext_rejected && ext_starts==2);
 active=false;ext_fail=0;start_scan();pump();CHECK(active && !s_scan_coded && ext_starts==2);
 s_ext_rejected=false;s_le_features=0x1000;active=false;start_scan();pump();CHECK(active && !s_scan_coded && ext_starts==2);
 s_le_features=0x1800;s_conn=BLE_HS_CONN_HANDLE_NONE;s_state=BLE_LINK_SCANNING;
 start_scan();pump();CHECK(active && s_scan_coded && ext_starts==3);
 return 0;
}
'''
        harness = harness.replace('BLE_SCAN_HEADER', (ROOT / 'main/ble_link_scan.h').as_posix())
        run_c(harness)

    def test_legacy_reports_in_extended_host_preserve_descriptor(self):
        source = (ROOT / 'main/ble_link.c').read_text()
        harness = r'''
#include <stdint.h>
#include <string.h>
#define MYNEWT_VAL(x) 1
#define BLE_HCI_ADV_LEGACY_MASK 16
#define BLE_HCI_ADV_SCAN_RSP_MASK 8
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP 4
#define BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE 0
struct addr {uint8_t type,val[6];};
struct ble_gap_disc_desc {uint8_t event_type;struct addr addr;int8_t rssi;uint8_t length_data;const uint8_t *data;};
struct ble_gap_ext_disc_desc {uint8_t props,data_status,legacy_event_type;struct addr addr;int8_t rssi;const uint8_t *data;uint16_t length_data;};
static struct ble_gap_ext_disc_desc got;
void ble_gap_rx_ext_adv_report(struct ble_gap_ext_disc_desc *d){got=*d;}
''' + body(source, 'void __wrap_ble_gap_rx_adv_report(') + r'''
int main(void){
 uint8_t data[]={2,1,6};
 struct ble_gap_disc_desc d={.event_type=4,.addr={1,{1,2,3,4,5,6}},.rssi=-55,.length_data=3,.data=data};
 __wrap_ble_gap_rx_adv_report(&d);
 return got.props!=24 || got.rssi!=-55 || got.length_data!=3 || got.data!=data ||
        got.legacy_event_type!=4 || got.data_status || memcmp(&got.addr,&d.addr,sizeof(d.addr));
}
'''
        run_c(harness)

    def test_sweep_lease_survives_primary_stop_and_allows_link_upgrade(self):
        source = (ROOT / 'main/ble_link.c').read_text()
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MYNEWT_VAL(x) 1
#define BLE_HS_ENOTSUP 55
static bool s_scan_coded,s_ext_rejected;
static bool s_c6_inhibit;
static uint64_t s_le_features;
#include "BLE_SCAN_HEADER"
#include "BLE_CORE_HEADER"
typedef int esp_err_t;
enum {ESP_OK,ESP_FAIL,ESP_ERR_INVALID_STATE,ESP_ERR_NO_MEM,BLE_HS_CONN_HANDLE_NONE=65535,BLE_ERR_REM_USER_CONN_TERM,BLE_OWN_ADDR_PUBLIC,BLE_HS_FOREVER=-1,BLE_HS_EALREADY=10};
#define portENTER_CRITICAL(p) (++locked)
#define portEXIT_CRITICAL(p) (--locked)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
struct ble_gap_event {int type;};
struct ble_npl_event {int unused;};
struct ble_gap_disc_params {int passive,itvl,window,filter_duplicates;};
static struct ble_npl_event s_scan_event;
static bool s_scan_event_ready=true,queued,active;
static int locked,s_start_mux,s_conn=BLE_HS_CONN_HANDLE_NONE;
static ble_scan_mode_t s_scan_mode;
static ble_link_state_t s_state;
static bool s_starting,s_primary,s_sweep_observer,s_rid_observer,s_find_observer,s_run,s_passive,s_scanning,s_scan_connected,s_scan_failed;
static bool s_disconnect_requested,s_cancel_connect_requested,s_stack_up=true;
static int64_t s_rescan_at_us;
static int s_backoff_ms;
static struct {int listener_count; char chains[4];int64_t chain_quarantine;} s_adverts={3};
static int starts,refreshes,terminates,cancels,connect_cancels,fail;
static bool ble_hs_synced(void){return true;}
static int64_t esp_timer_get_time(void){return 1000;}
static bool ble_gap_disc_active(void){return active;}
static void ble_gap_terminate(int conn,int why){(void)conn;(void)why;if(locked)__builtin_trap();terminates++;s_conn=BLE_HS_CONN_HANDLE_NONE;}
static void ble_gap_conn_cancel(void){if(locked)__builtin_trap();connect_cancels++;}
static int ble_gap_disc_cancel(void){if(locked)__builtin_trap();cancels++;active=false;return 0;}
static int gap_event(struct ble_gap_event*e,void*p){(void)e;(void)p;return 0;}
static int ble_gap_disc(int addr,int duration,struct ble_gap_disc_params*p,int(*cb)(struct ble_gap_event*,void*),void*arg){
 (void)addr;(void)duration;(void)p;(void)cb;(void)arg;if(locked)__builtin_trap();active=true;return 0;
}
struct ble_gap_ext_disc_params {uint16_t itvl,window;bool passive;};
static int ext_starts,ext_fail;static bool legacy_mode;
static void ble_link_legacy_scan(bool legacy){legacy_mode=legacy;}
static int ble_gap_ext_disc(int own,int duration,int period,int dup,int filter,int limited,
 const struct ble_gap_ext_disc_params *one,const struct ble_gap_ext_disc_params *coded,
 int(*cb)(struct ble_gap_event*,void*),void *arg){
 (void)own;(void)cb;(void)arg;ext_starts++;
 if(duration || period || dup || filter || limited || !coded || !coded->passive || one->itvl!=(one->passive?160:48) ||
    one->window!=24 || coded->window!=24)__builtin_trap();
 if(ext_fail)return ext_fail;active=true;return 0;
}
static void* nimble_port_get_dflt_eventq(void){return NULL;}
static void ble_npl_eventq_put(void*q,struct ble_npl_event*e){(void)q;(void)e;if(locked)__builtin_trap();queued=true;refreshes++;}
static bool workers_start(void){return !fail;}
ble_link_state_t ble_link_state_step(ble_link_state_t state,ble_link_event_t event){(void)state;(void)event;return BLE_LINK_SCANNING;}
static void start_scan(void);
''' + body(source, 'static void scan_refresh(') + '\n' + body(source, 'static void start_scan(void)\n{') + '\nstatic int link_start(void){starts++;\n' + body(source, 'if (s_stack_up) {') + '\nreturn ESP_FAIL;}\n' + body(source, 'static void on_reset(') + '\n' + body(source, 'void ble_link_rescan(void)') + '\n' + body(source, 'static void stop_mode(') + '\n' + body(source, 'void ble_link_stop(void)') + '\n' + body(source, 'static esp_err_t start_mode(') + '\n' + body(source, 'esp_err_t ble_link_start(void)') + '\n' + body(source, 'esp_err_t ble_link_listen(void)') + '\n' + body(source, 'esp_err_t ble_link_sweep_observer(') + '\n' + body(source, 'esp_err_t ble_link_find_observer(') + '\n' + body(source, 'esp_err_t ble_link_rid_observer(') + '\nstatic int late_connect(int handle){struct {struct {int conn_handle;} connect;} report={{handle}},*event=&report;\n' + body(source, 'if (!s_run || s_passive) {') + '\nreturn 1;}\n' + r'''
static void pump(void){if(queued){queued=false;scan_refresh(&s_scan_event);}}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 CHECK(ble_link_sweep_observer(true)==ESP_OK && s_sweep_observer && s_passive && starts==1);pump();
 CHECK(s_scan_mode==BLE_SCAN_OBSERVER);
 CHECK(ble_link_start()==ESP_OK && s_primary && !s_passive && starts==1);pump();
 CHECK(s_scan_mode==BLE_SCAN_HEAD);
 s_state=BLE_LINK_CONNECTING;ble_link_stop();CHECK(s_run && s_passive && !s_primary && !connect_cancels);pump();
 CHECK(connect_cancels==1 && s_state==BLE_LINK_SCANNING && s_scan_mode==BLE_SCAN_OBSERVER);
 CHECK(ble_link_start()==ESP_OK && s_primary && !s_passive && starts==1);pump();
 s_conn=1;CHECK(ble_link_sweep_observer(false)==ESP_OK && s_run && s_primary && !terminates);
 CHECK(ble_link_sweep_observer(true)==ESP_OK);pump();ble_link_stop();CHECK(s_run && s_passive && !terminates);pump();
 CHECK(terminates==1 && s_scan_mode==BLE_SCAN_OBSERVER);
 CHECK(ble_link_sweep_observer(false)==ESP_OK && !s_run);pump();CHECK(!active && s_state==BLE_LINK_OFF);
 on_reset(7);CHECK(s_state==BLE_LINK_OFF);
 fail=1;CHECK(ble_link_sweep_observer(true)==ESP_ERR_NO_MEM && !s_sweep_observer && !s_starting && !s_run && s_state==BLE_LINK_OFF);
 fail=0;CHECK(ble_link_sweep_observer(true)==ESP_OK && s_run && s_passive && !s_primary);pump();
 on_reset(7);CHECK(s_state==BLE_LINK_SYNCING);active=false;start_scan();pump();CHECK(active);
 CHECK(late_connect(2)==0 && terminates==2);pump();
 CHECK(ble_link_start()==ESP_OK && !s_passive);pump();CHECK(ble_link_sweep_observer(false)==ESP_OK && s_run);
 ble_link_stop();pump();CHECK(!s_run && !locked && refreshes>=4 && !active);
 CHECK(ble_link_listen()==ESP_OK && s_primary && s_passive);pump();
 CHECK(ble_link_start()==ESP_ERR_INVALID_STATE && s_primary && s_passive);
 ble_link_stop();pump();CHECK(!s_run);
 CHECK(ble_link_rid_observer(true)==ESP_OK && s_rid_observer && s_passive);pump();
 CHECK(ble_link_sweep_observer(true)==ESP_OK && ble_link_start()==ESP_OK);pump();
 s_conn=3;int before=terminates;
 CHECK(ble_link_find_observer(true)==ESP_OK && s_find_observer);pump();
 CHECK(terminates==before && s_conn==3);
 CHECK(ble_link_find_observer(true)==ESP_OK);pump();CHECK(terminates==before && s_conn==3);
 CHECK(ble_link_find_observer(false)==ESP_OK && !s_find_observer);pump();CHECK(terminates==before && s_conn==3);
 CHECK(ble_link_rid_observer(false)==ESP_OK && s_run && s_sweep_observer && s_primary && !s_rid_observer);
 pump();CHECK(terminates==before && s_conn==3);
 CHECK(ble_link_rid_observer(true)==ESP_OK);pump();
 CHECK(ble_link_sweep_observer(false)==ESP_OK && s_run && s_rid_observer && s_primary);
 ble_link_stop();pump();CHECK(s_run && s_rid_observer && !s_primary && s_passive && terminates==before+1);
 CHECK(ble_link_rid_observer(false)==ESP_OK);pump();CHECK(!s_run && !active);
 fail=1;CHECK(ble_link_rid_observer(true)==ESP_ERR_NO_MEM && !s_rid_observer && !s_run && !s_starting);
 fail=0;CHECK(ble_link_rid_observer(true)==ESP_OK && s_rid_observer);pump();
 CHECK(ble_link_rid_observer(false)==ESP_OK);pump();CHECK(!s_run && !locked);
 return 0;
}
'''
        harness = harness.replace('BLE_SCAN_HEADER', (ROOT / 'main/ble_link_scan.h').as_posix())
        harness = harness.replace('BLE_CORE_HEADER', (ROOT / 'main/ble_link_core.h').as_posix())
        run_c(harness)

if __name__ == '__main__':
    unittest.main()
