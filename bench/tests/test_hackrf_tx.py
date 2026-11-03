"""Execute the production HackRF TX state machine with deferred USB completions."""
import pathlib
import unittest
from test_audio_reinit import body, run_c

ROOT = pathlib.Path(__file__).resolve().parents[2]


class HackrfTransmit(unittest.TestCase):
    def test_build_disabled_backend(self):
        source = (ROOT / 'components/lakeshark/radio/hackrf_dev.c').read_text()
        disabled = source[source.index('#else\nvoid ls_hackrf_tx_cancel'):].split('#endif', 1)[0][6:]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
''' + '#include "' + (ROOT/'app-sdk/include/lakeshark_app.h').as_posix() + '"\n' + disabled + r'''
int main(void) {
    assert(ls_hackrf_tx_start(100000000, 2000000, 0, false, 0, NULL, 0) == LAP_RX_EUNSUPPORTED);
    assert(ls_hackrf_tx_poll() == LAP_RX_EUNSUPPORTED);
    ls_hackrf_tx_cancel();
    ls_hackrf_tx_limits(0, false);
    return 0;
}
'''
        run_c(harness)

    def test_usb_configuration_ownership_faults_and_drain(self):
        source = (ROOT / 'components/lakeshark/radio/hackrf_dev.c').read_text()
        state = source[source.index('static hackrf_dev_t *s_dev;'):source.index('/* Setup owns')]
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#define LAP_HACKRF_TX 1
""" + '#include "' + (ROOT/'app-sdk/include/lakeshark_app.h').as_posix() + '"\n' + r"""
typedef int ls_radio_session_t,ls_radio_err_t;
enum { LS_RADIO_OK,LS_RADIO_ERR_BUSY,LS_RADIO_ERR_STOPPED,LS_RADIO_ERR_IO };
enum { ESP_OK=0,USB_TRANSFER_STATUS_COMPLETED=0,LS_RADIO_RX_IQ_U8=1,LS_TX_HACKRF=2 };
enum { HACKRF_REQ_SET_TXVGA_GAIN=21,HACKRF_REQ_SET_AMP_ENABLE=17,HACKRF_REQ_SET_TRANSCEIVER_MODE=1,HACKRF_MODE_OFF=0,HACKRF_MODE_TRANSMIT=2 };
#define LS_RADIO_ENDPOINT_HACKRF_USB "hackrf.usb"
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
static int s_lifecycle;
typedef struct {void *dev_hdl;} usb_t;
typedef struct {usb_t usb;unsigned shift_hz;bool streaming;} hackrf_dev_t;
typedef struct {unsigned required_caps;const char *preferred_endpoint_id;uint64_t min_hz,max_hz;} ls_radio_requirements_t;
typedef struct usb_transfer usb_transfer_t;
struct usb_transfer {uint8_t *data_buffer;void *device_handle;unsigned bEndpointAddress,num_bytes,actual_num_bytes,timeout_ms;int status;void(*callback)(usb_transfer_t*);};
static hackrf_dev_t device={.usb={(void *)1}};
static int lease_state,protect_error,config_error,mode,amp_setting,gain_setting;
static uint64_t frequency;
static uint32_t sample_rate,charged;
static int frees,flushes,controls;
static bool usb_pending,alloc_fail,submit_fail,task_fail;
#define pdPASS 1
static int xTaskCreate(void(*fn)(void *),const char *name,int stack,void *arg,int priority,void *out){(void)fn;(void)name;(void)stack;(void)arg;(void)priority;(void)out;assert(mode==0);return !task_fail;}
#define pdMS_TO_TICKS(x) (x)
static void vTaskDelay(int ticks){(void)ticks;}
static void vTaskDelete(void *task){(void)task;}
static int64_t time_us=1000000;
static usb_transfer_t *submitted;
static int64_t esp_timer_get_time(void){return time_us;}
static int ls_radio_acquire(const char *owner,const ls_radio_requirements_t *r,ls_radio_session_t **out){
 assert(!strcmp(owner,"app.hackrf.tx")&&!strcmp(r->preferred_endpoint_id,"hackrf.usb"));
 if(lease_state) return LS_RADIO_ERR_BUSY;
 lease_state=1;*out=&lease_state;return LS_RADIO_OK;
}
static void ls_radio_release(ls_radio_session_t *lease){assert(lease==&lease_state);lease_state=0;}
static int ls_tx_protect(unsigned r,int p,bool rx,uint32_t us,uint64_t now){assert(r==LS_TX_HACKRF&&!rx&&p==0&&now);charged=us;return protect_error;}
static int usb_host_transfer_alloc(size_t n,int flags,usb_transfer_t **out){(void)flags;if(alloc_fail)return 1;*out=calloc(1,sizeof(**out));(*out)->data_buffer=malloc(n);return ESP_OK;}
static int usb_host_transfer_free(usb_transfer_t *x){assert(!usb_pending);free(x->data_buffer);free(x);frees++;return 0;}
static int usb_host_transfer_submit(usb_transfer_t *x){submitted=x;if(submit_fail)return 1;usb_pending=true;return 0;}
static void complete(int status,int bytes){usb_pending=false;submitted->status=status;submitted->actual_num_bytes=bytes;submitted->callback(submitted);}
static int usb_host_endpoint_halt(void *d,int ep){assert(d==(void *)1&&ep==2);return 0;}
static int usb_host_endpoint_flush(void *d,int ep){assert(d==(void *)1&&ep==2);flushes++;return 0;}
static int usb_host_endpoint_clear(void *d,int ep){assert(d==(void *)1&&ep==2);return 0;}
static int set_sample_rate(hackrf_dev_t *d,uint32_t rate){assert(d==&device);sample_rate=rate;return config_error;}
static int set_bandwidth(hackrf_dev_t *d,uint32_t bw){assert(d==&device&&bw);return config_error;}
static uint32_t ls_hackrf_filter_bandwidth(uint32_t wanted,uint32_t rate){assert(!wanted);return rate;}
static int set_frequency(hackrf_dev_t *d,uint64_t hz){assert(d==&device&&!d->shift_hz);frequency=hz;return config_error;}
static int control_gain(hackrf_dev_t *d,int request,unsigned gain){assert(d==&device&&request==21);gain_setting=gain;return config_error;}
static int control_out(hackrf_dev_t *d,int request,int value,int index,void *data,int bytes){
 assert(d==&device&&!index&&!data&&!bytes);controls++;
 if(request==1)mode=value;else if(request==17)amp_setting=value;else abort();
 return config_error;
}
static void settings_hackrf_tx_limits_set(unsigned vga,bool amp){(void)vga;(void)amp;}
""" + state + '\n'.join(body(source, name) for name in (
            'static void tx_complete(', 'void ls_hackrf_tx_cancel(',
            'void ls_hackrf_tx_limits(', 'static int tx_release(',
            'int ls_hackrf_tx_start(', 'static int tx_step(', 'static void tx_guard_task(void *arg) {', 'int ls_hackrf_tx_poll(')) + r"""
static int test_poll(void){int e=tx_step();if(e)atomic_store(&s_tx_result,e);return ls_hackrf_tx_poll();}
#define ls_hackrf_tx_poll test_poll
static int start(void){int8_t iq[256];memset(iq,127,sizeof(iq));return ls_hackrf_tx_start(5800000000ULL,2000000,12,true,0,iq,sizeof(iq));}
int main(void){
 s_dev=&device;device.shift_hz=96000;
 lease_state=1;assert(start()==LAP_RX_EBUSY&&!s_tx_dev&&!controls);lease_state=0;
 ls_hackrf_tx_limits(10,false);assert(start()==LAP_IO_EPOWER&&!lease_state&&!controls);
 ls_hackrf_tx_limits(47,true);
 protect_error=LAP_IO_ETHERMAL;assert(start()==LAP_IO_ETHERMAL&&!lease_state&&!s_tx_dev&&!controls);protect_error=0;
 alloc_fail=true;assert(start()==LAP_RX_ENOMEM&&!lease_state&&!s_tx_dev);alloc_fail=false;
 config_error=LS_RADIO_ERR_IO;assert(start()==LAP_RX_EIO&&!s_tx_dev&&mode==0);config_error=0;
 task_fail=true;assert(start()==LAP_RX_ENOMEM&&!s_tx_dev&&mode==0);task_fail=false;
 submit_fail=true;assert(!start());assert(ls_hackrf_tx_poll()==LAP_RX_EIO&&!s_tx_dev&&mode==0);submit_fail=false;
 assert(!start());assert(lease_state&&s_tx_dev==&device&&mode==2&&amp_setting&&gain_setting==12);
 assert(frequency==5800000000ULL&&sample_rate==2000000&&charged==500064);
 assert(submitted->bEndpointAddress==2&&submitted->num_bytes==256&&submitted->data_buffer[0]==127);
 assert(!ls_hackrf_tx_poll());complete(0,256);assert(!ls_hackrf_tx_poll());time_us+=3000;
 assert(ls_hackrf_tx_poll()==1&&!s_tx_dev&&!lease_state&&mode==0&&!amp_setting);
 /* Cancellation turns RF off immediately, but cannot free host-owned DMA. */
 assert(!start());int old_frees=frees;ls_hackrf_tx_cancel();assert(!ls_hackrf_tx_poll());
 assert(mode==0&&flushes&&frees==old_frees&&s_tx_dev);complete(3,0);
 assert(ls_hackrf_tx_poll()==LAP_IO_ECANCELLED&&!s_tx_dev&&frees==old_frees+1);
 /* Unplug retains driver context until the same callback drains. */
 assert(!start());s_dev=NULL;old_frees=frees;assert(!ls_hackrf_tx_poll()&&frees==old_frees);complete(3,0);
 assert(ls_hackrf_tx_poll()==LAP_RX_ENODEV&&!s_tx_dev);s_dev=&device;
 assert(!start());time_us+=300000;assert(!ls_hackrf_tx_poll()&&mode==0);complete(3,0);
 assert(ls_hackrf_tx_poll()==LAP_RX_EIO);
 assert(!start());ls_hackrf_tx_limits(0,false);assert(!ls_hackrf_tx_poll()&&mode==0);complete(3,0);
 assert(ls_hackrf_tx_poll()==LAP_IO_EPOWER);ls_hackrf_tx_limits(47,true);
 assert(!start());complete(0,100);assert(ls_hackrf_tx_poll()==LAP_RX_EIO&&mode==0);
 assert(!start());protect_error=LAP_IO_ETHERMAL;assert(!ls_hackrf_tx_poll()&&mode==0);complete(3,0);
 assert(ls_hackrf_tx_poll()==LAP_IO_ETHERMAL);
 return 0;
}
"""
        try:
            run_c(harness)
        except Exception as error:
            if hasattr(error, 'stderr'):
                self.fail(error.stderr)
            raise


if __name__ == '__main__':
    unittest.main()
