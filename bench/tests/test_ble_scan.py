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
#include "BLE_SCAN_HEADER"
struct ble_gap_event {int type;};
struct ble_npl_event {int unused;};
struct ble_gap_disc_params {int passive,itvl,window,filter_duplicates;};
enum {BLE_HS_CONN_HANDLE_NONE=65535,BLE_LINK_SYNCING,BLE_LINK_SCANNING,BLE_LINK_CONNECTING,BLE_LINK_DISCOVERING,BLE_LINK_READY,BLE_OWN_ADDR_PUBLIC,BLE_HS_FOREVER=-1,BLE_HS_EALREADY=10};
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static struct ble_npl_event s_scan_event;
static bool s_scan_event_ready=true,s_run=true,s_passive,s_scanning,s_scan_connected,active,synced=true,queued;
static int s_conn=BLE_HS_CONN_HANDLE_NONE,s_state=BLE_LINK_SYNCING,starts,cancels,fail;
static int64_t s_rescan_at_us,now;
static struct {int listener_count;} s_adverts={3};
static ble_scan_mode_t s_scan_mode;
static struct ble_gap_disc_params params;
static bool ble_hs_synced(void){return synced;}
static int64_t esp_timer_get_time(void){return now;}
static bool ble_gap_disc_active(void){return active;}
static int ble_gap_disc_cancel(void){cancels++;active=false;return 0;}
static int gap_event(struct ble_gap_event*e,void*p){(void)e;(void)p;return 0;}
static int ble_gap_disc(int addr,int duration,struct ble_gap_disc_params*p,int(*cb)(struct ble_gap_event*,void*),void*arg){
 (void)addr;(void)duration;(void)cb;(void)arg;starts++;params=*p;if(fail)return fail;active=true;return 0;
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
 active=false;fail=5;start_scan();pump();CHECK(!active && !s_scanning && s_state==BLE_LINK_READY);
 fail=0;start_scan();pump();CHECK(active && s_scanning && starts==5);
 s_conn=BLE_HS_CONN_HANDLE_NONE;s_state=BLE_LINK_SCANNING;s_rescan_at_us=100;now=50;start_scan();pump();CHECK(params.passive && !s_scan_connected && starts==5);
 now=100;start_scan();pump();CHECK(!s_rescan_at_us && active && !params.passive && starts==6 && cancels==2);
 s_run=false;start_scan();pump();CHECK(!active && !s_scanning);
 s_run=true;s_conn=1;s_state=BLE_LINK_READY;s_adverts.listener_count=0;start_scan();pump();CHECK(!active);
 synced=false;s_adverts.listener_count=3;start_scan();pump();CHECK(!active);
 return 0;
}
'''
        harness = harness.replace('BLE_SCAN_HEADER', (ROOT / 'main/ble_link_scan.h').as_posix())
        run_c(harness)

if __name__ == '__main__':
    unittest.main()
