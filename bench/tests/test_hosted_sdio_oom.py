"""Fault-inject production SDIO buffer growth and the receive OOM exit."""
import pathlib
import unittest
from test_audio_reinit import body, run_c

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c'


class HostedSdioOomTest(unittest.TestCase):
    def test_resize_preserves_old_buffer_and_capacity_on_oom(self):
        source = SOURCE.read_text()
        resize = body(source[source.index('// SDIO streaming mode'):], 'static uint8_t * sdio_rx_get_buffer(')
        harness = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#define H_SDIO_RX_BLOCK_ONLY_XFER 0
#define HOSTED_MEM_ALIGNMENT_64 64
#define ESP_LOGD(...) ((void)0)
static bool fail;
static int allocations;
static void *allocate(size_t len,int alignment){(void)alignment;if(fail)return NULL;void*p=malloc(len);if(p)++allocations;return p;}
static void release(void*p){if(p){--allocations;free(p);}}
static struct {void *(*_h_malloc_align)(size_t,int);void (*_h_free_align)(void*);} funcs={allocate,release};
static struct {typeof(funcs)*funcs;} g_h={&funcs};
static struct {int write_index;struct {uint8_t*buf;uint32_t buf_size;} buffer[2];} double_buf;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
''' .replace('typeof(funcs)', '__typeof__(funcs)') + resize + r'''
int main(void){
 uint8_t*old=sdio_rx_get_buffer(32);CHECK(old && allocations==1);old[0]=0x5a;
 fail=true;CHECK(!sdio_rx_get_buffer(64));CHECK(allocations==1);
 CHECK(double_buf.buffer[0].buf==old && double_buf.buffer[0].buf_size==32 && old[0]==0x5a);
 fail=false;CHECK(sdio_rx_get_buffer(64));CHECK(allocations==1 && double_buf.buffer[0].buf_size==64);
 release(double_buf.buffer[0].buf);CHECK(!allocations);return 0;
}
'''
        run_c(harness)

    def test_rx_oom_releases_bus_before_waiting_and_retrying(self):
        branch = body(SOURCE.read_text(), 'if (!rxbuff) {')
        harness = r'''
#include <stdio.h>
static int held=1,delays;
static void unlock(void){held=0;}
static void sleep_ms(int ms){if(held || ms!=100)__builtin_abort();++delays;}
static struct {void(*_h_msleep)(int);} funcs={sleep_ms};
static struct {__typeof__(funcs)*funcs;} g_h={&funcs};
#define SDIO_DRV_UNLOCK() unlock()
int main(void){
 void*rxbuff=0;
 for(int i=0;i<1;++i){
''' + branch + r'''
  return 2;
 }
 if(held || delays!=1)return 1;return 0;
}
'''
        run_c(harness)


if __name__ == '__main__':
    unittest.main()
