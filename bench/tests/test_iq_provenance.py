"""Exercise the production FIFO-reset and RTL post-reset epoch binding."""
import pathlib
import unittest
from test_audio_reinit import body, run_c

ROOT = pathlib.Path(__file__).resolve().parents[2]


class IqProvenanceTest(unittest.TestCase):
    def test_fifo_reset_requires_both_complete_control_writes(self):
        source = (ROOT / 'components/lakeshark/radio/librtlsdr.c').read_text()
        fn = body(source, 'int rtlsdr_reset_buffer(')
        run_c(r'''
#include <stdint.h>
#include <stdio.h>
typedef int rtlsdr_dev_t;
enum {USBB=1,USB_EPA_CTL=0x2148};
static int calls,first=2,second=2;
static int rtlsdr_write_reg(rtlsdr_dev_t*d,int b,int a,int v,int n){
 (void)d;(void)b;(void)a;(void)v;(void)n;return ++calls==1?first:second;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
''' + fn + r'''
int main(void){rtlsdr_dev_t d=0;CHECK(rtlsdr_reset_buffer(0)<0);
 for(int n=-1;n<2;n++){calls=0;first=n;second=2;CHECK(rtlsdr_reset_buffer(&d)<0);CHECK(calls==1);}
 first=2;for(int n=-1;n<2;n++){calls=0;second=n;CHECK(rtlsdr_reset_buffer(&d)<0);CHECK(calls==2);}
 calls=0;first=second=2;CHECK(rtlsdr_reset_buffer(&d)==0);CHECK(calls==2);return 0;}
''')

    def test_only_successful_full_reset_start_binds_current_transport_epoch(self):
        source = (ROOT / 'components/lakeshark/radio/rtlsdr_dev.c').read_text()
        fns = '\n'.join(body(source, name) for name in (
            'static void rtl_bind_fifo_epoch(', 'static bool rtl_transport_status(',
            'static ls_radio_err_t rtl_iq_start(', 'static ls_radio_err_t rtl_iq_retune('))
        run_c(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "''' + (ROOT / 'components/lakeshark/radio/ls_radio_hf.h').as_posix() + r'''"
typedef int rtlsdr_dev_t,ls_radio_err_t;
typedef struct {bool known,fifo_reset_known,counters_saturated;uint64_t epoch;} ls_radio_iq_transport_t;
enum {LS_RADIO_OK=0,LS_RADIO_ERR_IO=-1,LS_RADIO_ERR_BUSY=-2,LS_RADIO_ERR_NO_MEMORY=-3,
 LS_RADIO_ERR_INVALID=-4,ESP_LIBUSB_ERR_BUSY=-2,ESP_LIBUSB_ERR_NO_MEM=-3};
static int s_provenance_mux,reset_error,start_error,tune_error,resets,host_resets,starts,stops;
static bool s_read_cancelled,s_adapter_streaming,streaming;
static uint64_t s_fifo_reset_epoch,epoch=1,center=100100000;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
static bool esp_libusb_stream_evidence(ls_radio_iq_transport_t*out){*out=(ls_radio_iq_transport_t){.known=streaming,.epoch=epoch};return streaming;}
static int rtlsdr_reset_buffer(rtlsdr_dev_t*d){(void)d;++resets;return reset_error;}
static void rtlsdr_stream_reset(void){++host_resets;}
static int rtlsdr_stream_start(rtlsdr_dev_t*d){(void)d;++starts;if(start_error)return start_error;streaming=true;++epoch;return 0;}
static void rtlsdr_stream_stop_for(rtlsdr_dev_t*d){(void)d;++stops;streaming=false;}
static int rtlsdr_set_center_freq(rtlsdr_dev_t*d,uint32_t hz){(void)d;center=hz;return tune_error;}
static uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t*d){(void)d;return (uint32_t)center;}
static int rtl_error(int error){return error?LS_RADIO_ERR_IO:LS_RADIO_OK;}
/* HF routing (direct sampling below 24 MHz); these tunes stay on the tuner. */
static int routes;
static int rtl_route(rtlsdr_dev_t*d,uint64_t hz){(void)d;(void)hz;++routes;return 0;}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
''' + fns + r'''
int main(void){rtlsdr_dev_t d=0;ls_radio_iq_transport_t p;uint64_t actual;
 reset_error=-1;CHECK(rtl_iq_start(&d)==LS_RADIO_ERR_IO);CHECK(!starts&&!s_fifo_reset_epoch);
 reset_error=0;CHECK(rtl_iq_start(&d)==0);CHECK(resets==2&&host_resets==1&&starts==1);
 CHECK(rtl_transport_status(&d,&p)&&p.fifo_reset_known);uint64_t old=p.epoch;
 CHECK(rtl_iq_retune(&d,100300000,true,&actual)==0);CHECK(stops==1&&resets==2&&routes==1);
 CHECK(rtl_transport_status(&d,&p)&&!p.fifo_reset_known&&p.epoch>old);
 CHECK(rtl_iq_retune(&d,100100000,false,&actual)==0);CHECK(resets==3);
 CHECK(rtl_transport_status(&d,&p)&&p.fifo_reset_known);
 ++epoch;CHECK(rtl_transport_status(&d,&p)&&!p.fifo_reset_known); /* pipe reprime */
 tune_error=-1;CHECK(rtl_iq_retune(&d,100300000,false,&actual)==LS_RADIO_ERR_IO);
 CHECK(!rtl_transport_status(&d,&p));CHECK(!s_fifo_reset_epoch);
 tune_error=0;start_error=ESP_LIBUSB_ERR_BUSY;CHECK(rtl_iq_start(&d)==LS_RADIO_ERR_BUSY);
 CHECK(!s_fifo_reset_epoch);return 0;}
''')


if __name__ == '__main__':
    unittest.main()
