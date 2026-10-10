"""Inject detach at setup boundaries in the production RTL/HackRF workers."""
import pathlib
import unittest
from test_audio_reinit import body, run_c

ROOT = pathlib.Path(__file__).resolve().parents[2]


class UsbAttachTest(unittest.TestCase):
    def test_host_start_oom_uninstalls_and_can_retry_without_bsp_abort_macro(self):
        source = (ROOT / 'managed_components/waveshare__esp32_p4_wifi6_touch_lcd_4b/esp32_p4_wifi6_touch_lcd_4b.c').read_text()
        start = body(source, 'esp_err_t bsp_usb_host_start(')
        harness = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t,bsp_usb_host_power_mode_t;
enum {ESP_OK=0,ESP_ERR_NO_MEM=1,pdTRUE=1,ESP_INTR_FLAG_LEVEL1=1};
typedef struct {bool skip_phy_setup;int intr_flags;} usb_host_config_t;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
/* This board's configured BSP macro aborts on any error. */
#define BSP_ERROR_CHECK_RETURN_ERR(x) do {if((x)!=ESP_OK)abort();}while(0)
static void *usb_host_task;
static int stage,installs,uninstalls;
static bool installed;
static int usb_host_install(const usb_host_config_t*c){(void)c;++installs;if(stage==1)return ESP_ERR_NO_MEM;installed=true;return ESP_OK;}
static int usb_host_uninstall(void){installed=false;++uninstalls;return ESP_OK;}
static void usb_lib_task(void*p){(void)p;}
static int xTaskCreatePinnedToCore(void(*fn)(void*),const char*n,int bytes,void*arg,int pri,void**task,int core){
 (void)fn;(void)n;(void)bytes;(void)arg;(void)pri;(void)core;
 if(stage==2)return 0;*task=(void*)1;return pdTRUE;
}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
''' + start + r'''
int main(void){
 stage=1;CHECK(bsp_usb_host_start(0,false)==ESP_ERR_NO_MEM);CHECK(!installed && !usb_host_task && !uninstalls);
 stage=2;CHECK(bsp_usb_host_start(0,false)==ESP_ERR_NO_MEM);CHECK(!installed && !usb_host_task && uninstalls==1);
 stage=0;CHECK(bsp_usb_host_start(0,false)==ESP_OK);CHECK(installed && usb_host_task);
 int before=installs;CHECK(bsp_usb_host_start(0,false)==ESP_OK);CHECK(installs==before);
 return 0;
}
'''
        run_c(harness)

    def test_private_setup_ownership_cancellation_and_replacement_probe(self):
        for adapter in ('rtl', 'hackrf'):
            with self.subTest(adapter=adapter):
                rtl = adapter == 'rtl'
                file = 'rtlsdr_dev.c' if rtl else 'hackrf_dev.c'
                source = (ROOT / 'components/lakeshark/radio' / file).read_text()
                setup_name = 'rtlsdr_setup_task' if rtl else 'setup_task'
                dev_type = 'rtlsdr_dev_t' if rtl else 'hackrf_dev_t'
                arg_type = 'setup_arg_t' if rtl else 'hackrf_setup_arg_t'
                declarations = source[source.index('static portMUX_TYPE s_lifecycle'):source.index('/* Keep at most')]
                helper = body(source, 'static void finish_lifecycle(bool setup)')
                removed = body(source, 'bool ' + adapter + '_adapter_note_removed(')
                setup = body(source, 'static void ' + setup_name + '(void *arg)')
                harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int portMUX_TYPE,ls_radio_err_t,esp_err_t;
typedef void *usb_device_handle_t;typedef void *usb_host_client_handle_t;
enum {ESP_OK=0,ESP_ERR_NO_MEM=1,LS_RADIO_OK=0,LS_RADIO_ERR_BUSY=1,LS_RADIO_ERR_IO=2};
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define LS_RADIO_ENDPOINT_HACKRF_USB "hackrf"
typedef struct {void *client_hdl;uint8_t dev_addr;void *dev_hdl;} usb_t;
typedef struct {usb_t usb;uint16_t usb_api;int ppm;} device_t;
typedef device_t rtlsdr_dev_t,hackrf_dev_t;
typedef struct {unsigned generation;uint8_t dev_addr;void *client;} setup_arg_t,hackrf_setup_arg_t;
typedef struct {uint16_t idVendor,idProduct,bcdDevice;} usb_device_desc_t;
static usb_device_desc_t descriptor={1,2,3};
static device_t *s_dev;
static bool s_adapter_streaming;
static unsigned s_alloc_fail_count;
static int scenario,closes,unregisters,probes,retired,registry_calls;
static bool present,injected;
static void *const device=(void*)0x1234;
static bool ADAPTER_adapter_note_removed(void *dev);
static void ADAPTER_adapter_probe_async(uint8_t addr,void *client){(void)client;if(addr!=22)abort();++probes;}
''' .replace('ADAPTER', adapter) + declarations + helper + r'''
static void inject(int boundary){
 if(scenario==boundary && !injected){
  injected=true;
  if(ADAPTER_adapter_note_removed(device) || closes)abort();
  s_probe_pending=true;s_probe_generation=s_remove_generation;s_probe_addr=22;
 }
}
static void esp_libusb_note_device_gone(void*h){(void)h;}
static void rtlsdr_stream_stop_for(device_t*d){(void)d;}
static void rtl_unregister_endpoint(void){present=false;++unregisters;}
static int ls_radio_endpoint_unregister(const char*id){(void)id;present=false;++unregisters;return 0;}
static void *rtlsdr_usb_device_handle(device_t*d){return d->usb.dev_hdl;}
static void rtlsdr_close(device_t*d){++closes;free(d);}
static void close_device(device_t*d,bool gone){(void)gone;++closes;free(d);}
static void vPortFree(void*p){(void)p;}
static void ls_task_retire_self(void){++retired;}
static void vTaskDelete(void*p){(void)p;++retired;}
static void vTaskDelay(int ticks){(void)ticks;inject(4);}
static void event_bus_publish_simple(int e,const char*n){(void)e;(void)n;}
#define EVT_TUNER_LOCKED 1
static int rtlsdr_open(device_t**out,uint8_t addr,void*client){
 (void)addr;(void)client;*out=calloc(1,sizeof(device_t));(*out)->usb.dev_hdl=device;inject(1);return 0;
}
static void rtlsdr_set_freq_correction(device_t*d,int ppm){(void)d;(void)ppm;inject(2);}
static void rtlsdr_reset_buffer(device_t*d){(void)d;}
static int register_endpoint(device_t*d){
 (void)d;++registry_calls;inject(3);
 if(scenario==4 && !injected)return LS_RADIO_ERR_BUSY;
 present=true;return LS_RADIO_OK;
}
static int rtl_register_endpoint(device_t*d){return register_endpoint(d);}
static int usb_host_device_open(void*client,uint8_t addr,void**out){(void)client;(void)addr;*out=device;inject(1);return ESP_OK;}
static int usb_host_get_device_descriptor(void*d,const usb_device_desc_t**out){(void)d;*out=&descriptor;return ESP_OK;}
static bool hackrf_adapter_matches(int vid,int pid){(void)vid;(void)pid;return true;}
static int usb_host_interface_claim(void*c,void*d,int i,int a){(void)c;(void)d;(void)i;(void)a;inject(2);return ESP_OK;}
static int usb_host_device_close(void*c,void*d){(void)c;(void)d;return ESP_OK;}
static int init_adsb_dev(void){return ESP_OK;}
static int settings_hackrf_ppm_get(void){return 0;}
'''.replace('ADAPTER', adapter) + removed + '\n' + setup + r'''
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d scenario %d: %s\n",__LINE__,scenario,#x);return 1;}}while(0)
int main(void){
 for(scenario=1;scenario<=4;++scenario){
  setup_arg_t setup={.generation=s_remove_generation};
  s_setup_active=true;closes=unregisters=probes=registry_calls=0;injected=false;
  SETUP(&setup);
  CHECK(injected && !s_dev && !s_setup_active && !present && closes==1 && probes==1);
 }
 scenario=0;closes=probes=0;injected=false;
 setup_arg_t setup={.generation=s_remove_generation};s_setup_active=true;SETUP(&setup);
 CHECK(s_dev && present && !s_setup_active && closes==0);
 CHECK(ADAPTER_adapter_note_removed(device));
 CHECK(!s_dev && !present && closes==1);
 CHECK(!ADAPTER_adapter_note_removed(device));CHECK(closes==1);
 return 0;
}
'''.replace('SETUP', setup_name).replace('ADAPTER', adapter)
                run_c(harness)


if __name__ == '__main__':
    unittest.main()
