#include "ls_mixrf.h"
#include "ls_nfc_suite.h"
#include "ls_board.h"
#include "ls_keypad.h"
#include "ls_spi.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef LS_BOARD_MIX_CC_CS
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static ls_mixrf_status_t state;
static bool started,want,want_scan,want_nfc,want_card,reprobe;
static uint32_t requested=433920000;
static spi_device_handle_t cc_dev,nrf_dev,nfc_dev;
static DRAM_ATTR spi_transaction_t transaction;
static DRAM_ATTR uint8_t nfc_tx[65],nfc_rx[65];
static esp_err_t transfer_error;

static bool cc_ready(void)
{
    int64_t deadline=esp_timer_get_time()+2000;
    while(gpio_get_level(LS_BOARD_SPI_MISO_GPIO)) {
        if(esp_timer_get_time()>=deadline)return false;
        esp_rom_delay_us(2);
    }
    return true;
}
static bool transfer(spi_device_handle_t dev,int cs,uint8_t command,uint8_t data,uint8_t *out,bool ready)
{
    if(!dev || ls_spi_hold(dev)!=ESP_OK)return false;
    gpio_set_level(cs,0);
    bool ok=!ready || cc_ready();
    if(!ok)transfer_error=ESP_ERR_TIMEOUT;
    transaction=(spi_transaction_t){.flags=SPI_TRANS_USE_TXDATA|SPI_TRANS_USE_RXDATA,.length=out?16:8};
    transaction.tx_data[0]=command;transaction.tx_data[1]=data;
    if(ok){esp_err_t err=spi_device_polling_transmit(dev,&transaction);if(err!=ESP_OK)transfer_error=err;ok=err==ESP_OK;}
    gpio_set_level(cs,1);spi_device_release_bus(dev);
    if(ok && out)*out=transaction.rx_data[1];
    return ok;
}
static bool cc_read(uint8_t reg,uint8_t *v)
{return transfer(cc_dev,LS_BOARD_MIX_CC_CS,reg|0xc0,0,v,true);}
static bool cc_write(uint8_t reg,uint8_t v)
{uint8_t ignored;return transfer(cc_dev,LS_BOARD_MIX_CC_CS,reg,v,&ignored,true);}
static bool cc_strobe(uint8_t strobe)
{return transfer(cc_dev,LS_BOARD_MIX_CC_CS,strobe,0,NULL,true);}
static bool nrf_write(uint8_t reg,uint8_t value)
{uint8_t ignored;return transfer(nrf_dev,LS_BOARD_MIX_NRF_CS,0x20|reg,value,&ignored,false);}
static bool scan_start(void)
{
    gpio_set_level(LS_BOARD_MIX_NRF_CE,0);
    if(!nrf_dev && cc_dev)nrf_dev=cc_dev;
    if(!nrf_dev && ls_spi_device(LS_SPI_RADIO,-1,0,1000000,1,&nrf_dev)!=ESP_OK)return false;
    /* No automatic acknowledgements or enabled packet pipes: energy only. */
    bool ok=nrf_write(1,0) && nrf_write(2,0) && nrf_write(6,7) && nrf_write(0,0x0f);
    uint8_t config=0,ack=255;
    ok=ok && transfer(nrf_dev,LS_BOARD_MIX_NRF_CS,0,0,&config,false) &&
        transfer(nrf_dev,LS_BOARD_MIX_NRF_CS,1,0,&ack,false) && (config&3)==3 && ack==0;
    vTaskDelay(pdMS_TO_TICKS(5));
    return ok;
}
static void scan_stop(void)
{
    gpio_set_level(LS_BOARD_MIX_NRF_CE,0);
    if(nrf_dev) {
        nrf_write(0,0x0c);
        if(nrf_dev==cc_dev || ls_spi_remove(LS_SPI_RADIO,nrf_dev)==ESP_OK)nrf_dev=NULL;
    }
}
static bool scan_channel(uint8_t channel,bool *hit)
{
    uint8_t rpd=0;
    bool ok=nrf_write(5,channel);
    if(ok) {
        gpio_set_level(LS_BOARD_MIX_NRF_CE,1);
        /* 130 us RX settling plus at least 40 us for the power detector. */
        esp_rom_delay_us(200);
        ok=transfer(nrf_dev,LS_BOARD_MIX_NRF_CS,9,0,&rpd,false) && rpd<=1;
        gpio_set_level(LS_BOARD_MIX_NRF_CE,0);
    }
    *hit=rpd!=0;
    return ok;
}
static bool cc_reset(void)
{
    if(!cc_dev || ls_spi_hold(cc_dev)!=ESP_OK)return false;
    /* Synchronize CS before SRES, then wait for reset completion with CS low. */
    gpio_set_level(LS_BOARD_MIX_CC_CS,1);esp_rom_delay_us(5);
    gpio_set_level(LS_BOARD_MIX_CC_CS,0);esp_rom_delay_us(10);
    gpio_set_level(LS_BOARD_MIX_CC_CS,1);esp_rom_delay_us(50);
    gpio_set_level(LS_BOARD_MIX_CC_CS,0);
    transaction=(spi_transaction_t){.flags=SPI_TRANS_USE_TXDATA|SPI_TRANS_USE_RXDATA,.length=8};
    transaction.tx_data[0]=0x30;
    bool ready=cc_ready();
    esp_err_t err=ready?spi_device_polling_transmit(cc_dev,&transaction):ESP_ERR_TIMEOUT;
    if(err!=ESP_OK)transfer_error=err;
    bool ok=err==ESP_OK && cc_ready();
    gpio_set_level(LS_BOARD_MIX_CC_CS,1);
    spi_device_release_bus(cc_dev);
    esp_rom_delay_us(100);
    return ok;
}
static bool nfc_watch_start(void)
{
    if(!nfc_dev && ls_spi_device(LS_SPI_RADIO,-1,1,1000000,1,&nfc_dev)!=ESP_OK)return false;
    uint8_t ignored,control=0;
    /* Peer-field detector only: oscillator, receiver and transmitter stay off. */
    return transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xc1,0,NULL,false) &&
        transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,2,2,&ignored,false) &&
        transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x42,0,&control,false) && control==2;
}
static void nfc_watch_stop(void)
{
    if(nfc_dev) {
        uint8_t ignored;transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,2,0,&ignored,false);
        if(ls_spi_remove(LS_SPI_RADIO,nfc_dev)==ESP_OK)nfc_dev=NULL;
    }
}
static bool nfc_write(uint8_t reg,uint8_t value)
{uint8_t ignored;return transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,reg,value,&ignored,false);}
static bool nfc_bytes(const uint8_t *tx,uint8_t *rx,unsigned n)
{
    if(!nfc_dev || n>sizeof(nfc_tx) || ls_spi_hold(nfc_dev)!=ESP_OK)return false;
    transaction=(spi_transaction_t){.flags=SPI_TRANS_USE_TXDATA|SPI_TRANS_USE_RXDATA,.length=n*8};
    if(n<=4)memcpy(transaction.tx_data,tx,n);
    else {memcpy(nfc_tx,tx,n);transaction.flags=0;transaction.tx_buffer=nfc_tx;transaction.rx_buffer=nfc_rx;}
    gpio_set_level(LS_BOARD_MIX_NFC_CS,0);
    esp_err_t err=spi_device_polling_transmit(nfc_dev,&transaction);
    gpio_set_level(LS_BOARD_MIX_NFC_CS,1);spi_device_release_bus(nfc_dev);
    if(err!=ESP_OK){transfer_error=err;return false;}
    if(rx)memcpy(rx,n<=4?transaction.rx_data:nfc_rx,n);
    return true;
}
static bool card_start(void)
{
    if(!nfc_dev && ls_spi_device(LS_SPI_RADIO,-1,1,1000000,1,&nfc_dev)!=ESP_OK)return false;
    /* NFC-A 106 kbit/s, OOK, parity checked, ATQA has no CRC. */
    if(!transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xc1,0,NULL,false) ||
       !nfc_write(2,0x80))return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t aux=0;
    if(!transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x71,0,&aux,false) || !(aux&0x10))return false;
    if(!transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xd6,0,NULL,false))return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    const uint8_t corr1[]={0xfb,0x0c,0x51},corr2[]={0xfb,0x0d,0};
    return nfc_write(3,0x08) && nfc_write(4,0) && nfc_write(5,1) &&
        nfc_write(0x0a,0x80) && nfc_write(0x0b,0x08) && nfc_write(0x0c,0x2d) &&
        nfc_write(0x0d,0) && nfc_write(0x0e,0) &&
        /* NFC-A minimum FDT 1172/fc minus 276/fc receiver/timer compensation. */
        nfc_write(0x0f,14) && nfc_write(0x10,0) && nfc_write(0x11,200) && nfc_write(0x12,0) &&
        nfc_bytes(corr1,NULL,3) && nfc_bytes(corr2,NULL,3);
}
static bool card_poll(uint16_t *atqa,bool *sent,bool *frame_error,uint8_t *level)
{
    *atqa=0;*sent=false;*frame_error=false;*level=0;
    bool ok=nfc_write(2,0xc8);
    vTaskDelay(pdMS_TO_TICKS(6));
    ok=ok && transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xc2,0,NULL,false) &&
        transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xd5,0,NULL,false) &&
        transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xda,0,NULL,false) && nfc_write(0x23,0);
    /* Field startup can leave receive flags pending. Drain them before REQA. */
    uint8_t discarded=0;
    for(uint8_t reg=0x5a;ok && reg<=0x5d;reg++)
        ok=transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,reg,0,&discarded,false);
    ok=ok && transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xc6,0,NULL,false);
    uint8_t irq=0,errors=0;bool received=false;
    int64_t deadline=esp_timer_get_time()+12000;
    while(ok && esp_timer_get_time()<deadline) {
        uint8_t value=0;
        ok=transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x5a,0,&value,false);irq|=value;
        if(irq&0x14){received=(irq&0x10)!=0;break;}
        vTaskDelay(1);
    }
    *sent=(irq&8)!=0;
    ok=ok && transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x5c,0,&errors,false);
    *frame_error=(errors&0x70)!=0 || (irq&4)!=0;
    uint8_t count=0,flags=0,rx[3]={0},command[]={0x9f,0,0};
    if(ok) {
        ok=transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x5e,0,&count,false) &&
           transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x5f,0,&flags,false);
        if(ok && count>=2)ok=nfc_bytes(command,rx,3);
        if(ok && received && !*frame_error && count==2 && !(flags&0xfe)) {
            uint16_t value=rx[1]|((uint16_t)rx[2]<<8);
            /* NFC-A bit-frame anticollision advertises exactly one low bit;
               reject RF noise even if it happened to form two whole bytes. */
            unsigned anticollision=value&31;
            if(anticollision && !(anticollision&(anticollision-1)) && !(value&0xf020))*atqa=value;
            else *frame_error=true;
        } else if(received) *frame_error=true;
    }
    uint8_t rssi=0;
    if(ok)ok=transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x6d,0,&rssi,false);
    *level=rssi>>4;
    static unsigned diagnostic_poll;
    if(++diagnostic_poll%10==0 || *atqa)
        printf("nfc probe: irq=%02x err=%02x fifo=%u/%02x raw=%02x%02x rssi=%02x valid=%04x\n",irq,errors,count,flags,rx[1],rx[2],rssi,*atqa);
    /* Each short probe ends with the field off, including transport failures. */
    bool off=nfc_write(2,0x80);
    return ok && off;
}
static bool suite_begin(void)
{
    if(!card_start() || !nfc_write(2,0xc8))return false;
    vTaskDelay(pdMS_TO_TICKS(6));return true;
}
static void suite_end(void){if(nfc_dev)nfc_write(2,0x80);}
static int suite_exchange(const uint8_t *tx,unsigned bits,uint8_t *rx,unsigned cap,unsigned *received,bool raw)
{
    *received=0;if(!bits || bits>504 || cap>64)return 1;
    /* NFC-A poll spacing: 6780 carrier cycles = 500 us. */
    esp_rom_delay_us(500);
    uint8_t ignored=0;
    bool ok=nfc_write(5,raw?0xc0:0) && transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xc2,0,NULL,false) &&
        transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xd5,0,NULL,false);
    for(uint8_t r=0x5a;ok && r<=0x5d;r++)ok=transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,r,0,&ignored,false);
    if(bits==7 && tx[0]==0x26)ok=ok && nfc_write(0x23,0) && transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xc6,0,NULL,false);
    else {
        uint8_t fifo[65]={0x80};memcpy(fifo+1,tx,(bits+7)/8);
        ok=ok && nfc_write(0x22,bits>>8) && nfc_write(0x23,bits&255) &&
            nfc_bytes(fifo,NULL,1+(bits+7)/8) && transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0xc5,0,NULL,false);
    }
    uint8_t irq=0,timers=0,error=0,count=0,flags=0;
    int64_t end=esp_timer_get_time()+8000;
    while(ok && esp_timer_get_time()<end){
        uint8_t v=0;ok=transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x5a,0,&v,false);irq|=v;
        ok=ok && transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x5b,0,&v,false);timers|=v;
        if((irq&0x14) || ((timers&0x40) && !(irq&0x20)))break;
        esp_rom_delay_us(80);
    }
    ok=ok && transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x5c,0,&error,false) &&
        transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x5e,0,&count,false) && transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x5f,0,&flags,false);
    if(!ok)return 1;
    if(!(irq&0x10))return 2;
    if((irq&4)||(error&(raw?0x30:0x70))||(flags&0xf0)||count>cap||!count)return 3;
    uint8_t fifo[65]={0x9f},result[65];if(!nfc_bytes(fifo,result,count+1))return 1;
    memcpy(rx,result+1,count);unsigned last=(flags>>1)&7;*received=(count-(last?1:0))*8+last;return 0;
}
static const ls_nfc_suite_io_t suite_io={suite_begin,suite_end,suite_exchange};
static void status_text(const char *text)
{portENTER_CRITICAL(&lock);snprintf(state.status,sizeof(state.status),"%s",text);portEXIT_CRITICAL(&lock);}
static bool probe_radios(void)
{
    transfer_error=ESP_OK;
    extern bool flipper_link_running(void) __attribute__((weak));
    if(flipper_link_running && flipper_link_running()) {status_text("Stop wired Flipper link before radio probe");return false;}
    if(!ls_keypad_present()) {status_text("Keyboard not detected");return false;}
    /* Deassert all SPI selects and nRF CE before enabling expansion power. */
    const int pins[]={LS_BOARD_MIX_CC_CS,LS_BOARD_MIX_NRF_CS,LS_BOARD_MIX_NFC_CS,LS_BOARD_MIX_NRF_CE};
    uint64_t pin_mask=0;
    for(int i=0;i<4;i++){gpio_set_level(pins[i],i<3);pin_mask|=1ULL<<pins[i];}
    /* Direction alone leaves these pads in their reset IOMUX function. */
    gpio_config_t outputs={.pin_bit_mask=pin_mask,.mode=GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    if(gpio_config(&outputs)!=ESP_OK){status_text("Radio GPIO configuration failed");return false;}
    for(int i=0;i<4;i++)gpio_set_level(pins[i],i<3);
    bool power=ls_keypad_expander_update(2,1,0) && ls_keypad_expander_update(6,1,0);
    vTaskDelay(pdMS_TO_TICKS(20));
    power=power && ls_keypad_expander_update(2,1,1);
    uint8_t output=255,direction=255,input=255;
    bool checked=ls_keypad_expander_read(2,&output) && ls_keypad_expander_read(6,&direction) && ls_keypad_expander_read(0,&input);
    printf("mixrf power: ack=%d read=%d output=%02x direction=%02x input=%02x\n",power,checked,output,direction,input);
    power=power && checked && (output&1) && !(direction&1);
    portENTER_CRITICAL(&lock);state.keyboard=true;state.power=power;portEXIT_CRITICAL(&lock);
    if(!power){status_text("Keyboard expander did not enable radio power");return false;}
    /* Allow the nRF24 power-on reset window before reading any chip IDs. */
    vTaskDelay(pdMS_TO_TICKS(100));
    if(!cc_dev)ls_spi_device(LS_SPI_RADIO,-1,0,1000000,1,&cc_dev);
    uint8_t part=255,version=255,nfc=255,aw=255,channel=255;
    bool cc=cc_reset() && cc_read(0x30,&part) && cc_read(0x31,&version) &&
        part==0 && (version==4 || version==0x14);
    if(cc)cc_strobe(0x36);
    if(!cc && cc_dev && ls_spi_remove(LS_SPI_RADIO,cc_dev)==ESP_OK)cc_dev=NULL;
    if(!nrf_dev && cc_dev)nrf_dev=cc_dev;
    if(!nrf_dev)ls_spi_device(LS_SPI_RADIO,-1,0,1000000,1,&nrf_dev);
    bool nr=transfer(nrf_dev,LS_BOARD_MIX_NRF_CS,3,0,&aw,false) &&
        transfer(nrf_dev,LS_BOARD_MIX_NRF_CS,5,0,&channel,false) && aw>=1 && aw<=3 && channel<=125;
    if(nr) {
        uint8_t ignored,check=0,test=aw==3?2:3;
        bool wrote=transfer(nrf_dev,LS_BOARD_MIX_NRF_CS,0x23,test,&ignored,false);
        bool read=wrote && transfer(nrf_dev,LS_BOARD_MIX_NRF_CS,3,0,&check,false);
        bool restored=transfer(nrf_dev,LS_BOARD_MIX_NRF_CS,0x23,aw,&ignored,false);
        nr=read && check==test && restored;
    }
    /* Probe-only devices must not retain the DMA buffers needed by RTL. */
    if(nrf_dev && (nrf_dev==cc_dev || ls_spi_remove(LS_SPI_RADIO,nrf_dev)==ESP_OK))nrf_dev=NULL;
    if(!nfc_dev)ls_spi_device(LS_SPI_RADIO,-1,1,1000000,1,&nfc_dev);
    bool nf=transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x7f,0,&nfc,false) && (nfc&0xf8)==0x28;
    if(nfc_dev && ls_spi_remove(LS_SPI_RADIO,nfc_dev)==ESP_OK)nfc_dev=NULL;
    portENTER_CRITICAL(&lock);
    state.cc=cc;state.nrf=nr;state.nfc=nf;state.cc_version=version;
    state.nfc_identity=nfc;state.nrf_address_width=aw;state.ready=true;
    portEXIT_CRITICAL(&lock);
    printf("mixrf IDs: CC part=%02x version=%02x NRF AW=%02x channel=%02x NFC=%02x\n",part,version,aw,channel,nfc);
    if(transfer_error==ESP_ERR_NO_MEM)status_text("SPI needs DMA memory; radio probe incomplete");
    else if(transfer_error!=ESP_OK) {
        char error[80];snprintf(error,sizeof(error),"Radio probe: %s",esp_err_to_name(transfer_error));status_text(error);
    } else status_text(cc?"Probed; CC1101 receive monitor is off":"Radio IDs unavailable; check power / connection");
    return true;
}
static bool tune(uint32_t hz)
{
    uint8_t route=hz<400000000?4:hz<500000000?6:2;
    if(!cc_strobe(0x36) || !ls_keypad_expander_update(2,6,route) ||
       !ls_keypad_expander_update(6,6,0))return false;
    uint32_t word=(uint32_t)(((uint64_t)hz*65536+13000000)/26000000);
    return cc_write(0x0d,word>>16) && cc_write(0x0e,word>>8) && cc_write(0x0f,word) &&
        /* Continuous asynchronous RX keeps packet completion and FIFO traffic
           from stopping or freezing this energy-only monitor. */
        cc_write(0x02,0x2e) && cc_write(0x08,0x32) && cc_write(0x12,0) &&
        cc_write(0x17,0x3c) && cc_write(0x18,0x18) && cc_strobe(0x34);
}
static void worker(void *arg)
{
    (void)arg;
    probe_radios();
    portENTER_CRITICAL(&lock);state.busy=false;portEXIT_CRITICAL(&lock);
    bool running=false,scanning=false,nfc_watching=false,previous_field=false;
    bool card_scanning=false;
    int64_t last_card_poll=0,last_card_seen=0;
    uint16_t previous_atqa=0;
    uint8_t channel=0;
    unsigned tick=0;
    uint32_t hz=0;
    for(;;) {
        portENTER_CRITICAL(&lock);bool retry=reprobe;reprobe=false;portEXIT_CRITICAL(&lock);
        if(retry) {probe_radios();portENTER_CRITICAL(&lock);state.busy=false;portEXIT_CRITICAL(&lock);}
        portENTER_CRITICAL(&lock);bool on=want && state.cc,scan=want_scan && state.nrf,nfc_on=want_nfc && state.nfc,card_on=want_card && state.nfc;uint32_t next=requested;portEXIT_CRITICAL(&lock);
        if(!ls_keypad_present()) {
            on=scan=nfc_on=card_on=false;running=false;
            portENTER_CRITICAL(&lock);state.keyboard=state.power=false;state.cc=state.nrf=state.nfc=false;want=want_scan=want_nfc=want_card=false;portEXIT_CRITICAL(&lock);
            status_text("Keyboard absent; use PROBE after reconnecting");
        }
        if(ls_nfc_suite_busy()) {
            if(card_scanning || nfc_watching){nfc_watch_stop();card_scanning=nfc_watching=false;}
            portENTER_CRITICAL(&lock);want_card=want_nfc=false;state.card_scanning=state.card_present=state.nfc_watching=false;portEXIT_CRITICAL(&lock);
            if(!state.nfc)ls_nfc_suite_stop();
            ls_nfc_suite_step(&suite_io);
            vTaskDelay(pdMS_TO_TICKS(10));continue;
        }
        if(scan && !scanning) {
            scanning=scan_start();
            if(!scanning){scan_stop();portENTER_CRITICAL(&lock);want_scan=false;portEXIT_CRITICAL(&lock);status_text("2.4 GHz receive setup failed");}
        } else if(!scan && scanning){scan_stop();scanning=false;}
        if(scanning) {
            bool hit=false;
            if(scan_channel(channel,&hit)) {
                portENTER_CRITICAL(&lock);
                unsigned old=state.occupancy[channel];
                state.occupancy[channel]=(uint8_t)(hit?old+(255-old+7)/8:old*7/8);
                state.channel=channel;state.energy_hits+=hit;
                if(channel==LS_MIXRF_CHANNELS-1)state.sweeps++;
                portEXIT_CRITICAL(&lock);
                channel=(channel+1)%LS_MIXRF_CHANNELS;
            } else {scan_stop();scanning=false;portENTER_CRITICAL(&lock);want_scan=false;portEXIT_CRITICAL(&lock);status_text("2.4 GHz register read failed");}
        }
        portENTER_CRITICAL(&lock);state.scanning=scanning;portEXIT_CRITICAL(&lock);
        if(!card_on && card_scanning){nfc_watch_stop();card_scanning=false;last_card_seen=0;}
        if(card_on && nfc_watching){nfc_watch_stop();nfc_watching=false;}
        if(card_on && !card_scanning) {
            previous_atqa=0;last_card_seen=0;
            card_scanning=card_start();
            if(!card_scanning){nfc_watch_stop();portENTER_CRITICAL(&lock);want_card=false;portEXIT_CRITICAL(&lock);status_text("NFC card detector setup failed");}
            else status_text("NFC-A detector ready; hold one card flat over antenna");
        }
        if(card_scanning && esp_timer_get_time()-last_card_poll>=500000) {
            uint16_t atqa=0;bool sent=false,frame_error=false;uint8_t level=0;
            bool ok=card_poll(&atqa,&sent,&frame_error,&level);last_card_poll=esp_timer_get_time();
            portENTER_CRITICAL(&lock);
            state.card_polls++;state.card_tx+=sent;state.card_errors+=frame_error || !ok;
            state.card_level[state.card_head]=level;
            state.card_result[state.card_head]=atqa?(atqa==previous_atqa?3:2):(frame_error || !ok?1:0);
            state.card_head=(state.card_head+1)%LS_MIXRF_CARD_HISTORY;
            if(state.card_count<LS_MIXRF_CARD_HISTORY)state.card_count++;
            if(atqa){state.card_hits++;if(atqa==previous_atqa){state.card_atqa=atqa;last_card_seen=last_card_poll;}}
            previous_atqa=atqa;
            portEXIT_CRITICAL(&lock);
            if(!ok){nfc_watch_stop();card_scanning=false;portENTER_CRITICAL(&lock);want_card=false;portEXIT_CRITICAL(&lock);status_text("NFC card detector SPI failed; stopped");}
        }
        portENTER_CRITICAL(&lock);state.card_scanning=card_scanning;state.card_present=card_scanning && last_card_seen && esp_timer_get_time()-last_card_seen<1200000;portEXIT_CRITICAL(&lock);
        if(nfc_on && !nfc_watching) {
            nfc_watching=nfc_watch_start();previous_field=false;
            if(!nfc_watching){nfc_watch_stop();portENTER_CRITICAL(&lock);want_nfc=false;portEXIT_CRITICAL(&lock);status_text("NFC field detector setup failed");}
        } else if(!nfc_on && nfc_watching){nfc_watch_stop();nfc_watching=false;}
        bool field=false;
        if(nfc_watching) {
            uint8_t aux=0;
            bool valid=transfer(nfc_dev,LS_BOARD_MIX_NFC_CS,0x71,0,&aux,false) && (aux&3)==2 && !(aux&0x20);
            if(valid) {
                field=(aux&0x40)!=0;
                portENTER_CRITICAL(&lock);state.nfc_samples++;if(field && !previous_field)state.nfc_events++;portEXIT_CRITICAL(&lock);
                previous_field=field;
            } else {nfc_watch_stop();nfc_watching=false;portENTER_CRITICAL(&lock);want_nfc=false;portEXIT_CRITICAL(&lock);status_text("NFC field detector read failed");}
        }
        portENTER_CRITICAL(&lock);state.nfc_watching=nfc_watching;state.nfc_field=field;portEXIT_CRITICAL(&lock);
        if(++tick%10){vTaskDelay(pdMS_TO_TICKS(10));continue;}
        if(on && (!running || hz!=next)) {
            running=tune(next);hz=next;
            if(running){vTaskDelay(pdMS_TO_TICKS(10));status_text("CC1101 channel energy; no packet decoding");}
            else {status_text("CC1101 receive setup failed");portENTER_CRITICAL(&lock);want=false;portEXIT_CRITICAL(&lock);}
        } else if(!on && running) {cc_strobe(0x36);running=false;status_text("CC1101 receive monitor stopped");}
        uint8_t raw=0,marc=0;
        bool valid=running && cc_read(0x35,&marc) && (marc&31)==13 && cc_read(0x34,&raw);
        /* Clear a packet FIFO overflow; this monitor reads energy, not payloads. */
        if(running && (marc&31)==17){cc_strobe(0x36);cc_strobe(0x3a);cc_strobe(0x34);}
        portENTER_CRITICAL(&lock);
        state.receiving=valid;state.frequency=hz;
        state.rssi=valid?(float)(int8_t)raw/2-74:NAN;
        if(valid)state.samples++;
        portEXIT_CRITICAL(&lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
bool ls_mixrf_start(void)
{
    portENTER_CRITICAL(&lock);
    if(started){if(!ls_nfc_suite_busy() && !want && !want_scan && !want_nfc && !want_card && !state.scanning && !state.nfc_watching && !state.card_scanning){reprobe=true;state.busy=true;}portEXIT_CRITICAL(&lock);return true;}
    started=true;state.busy=true;state.rssi=NAN;
    portEXIT_CRITICAL(&lock);
    bool ok=xTaskCreatePinnedToCoreWithCaps(worker,"mixrf",4096,NULL,1,NULL,0,
        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS;
    if(!ok){portENTER_CRITICAL(&lock);started=false;state.busy=false;portEXIT_CRITICAL(&lock);status_text("Radio worker allocation failed");}
    return ok;
}
void ls_mixrf_snapshot(ls_mixrf_status_t *out)
{if(out){portENTER_CRITICAL(&lock);*out=state;out->scan_requested=want_scan;out->receive_requested=want;out->nfc_requested=want_nfc;out->card_requested=want_card;portEXIT_CRITICAL(&lock);}}
bool ls_mixrf_receive(bool on,uint32_t hz)
{
    if(on && !((hz>=300000000 && hz<=348000000) || (hz>=387000000 && hz<=464000000) || (hz>=779000000 && hz<=928000000)))return false;
    portENTER_CRITICAL(&lock);bool ok=!on || state.cc;
    if(ok){want=on;requested=hz;}portEXIT_CRITICAL(&lock);return ok;
}
bool ls_mixrf_scan(bool on)
{
    portENTER_CRITICAL(&lock);bool ok=!on || (state.nrf && !state.busy);
    if(ok)want_scan=on;
    portEXIT_CRITICAL(&lock);return ok;
}
bool ls_mixrf_nfc_watch(bool on)
{
    portENTER_CRITICAL(&lock);bool ok=!on || (state.nfc && !state.busy);
    if(ok){want_nfc=on;if(on)want_card=false;}
    portEXIT_CRITICAL(&lock);return ok;
}
bool ls_mixrf_card_scan(bool on)
{
    portENTER_CRITICAL(&lock);bool ok=!on || (state.nfc && !state.busy);
    if(ok){want_card=on;if(on)want_nfc=false;}
    portEXIT_CRITICAL(&lock);return ok;
}
#else
bool ls_mixrf_start(void){return false;}
void ls_mixrf_snapshot(ls_mixrf_status_t *out){if(out){memset(out,0,sizeof(*out));snprintf(out->status,sizeof(out->status),"No keyboard radio wiring for this board");}}
bool ls_mixrf_receive(bool on,uint32_t hz){(void)on;(void)hz;return false;}
bool ls_mixrf_scan(bool on){(void)on;return false;}
bool ls_mixrf_nfc_watch(bool on){(void)on;return false;}
bool ls_mixrf_card_scan(bool on){(void)on;return false;}
#endif
void ls_mixrf_diagnostics(void)
{
    ls_mixrf_status_t s;ls_mixrf_snapshot(&s);
    printf("mixrf: keyboard=%d power=%d CC1101=%d version=%02x nRF24=%d AW=%u NFC=%d ID=%02x\n",
        s.keyboard,s.power,s.cc,s.cc_version,s.nrf,s.nrf_address_width,s.nfc,s.nfc_identity);
    printf("mixrf: RX=%d %.4f MHz RSSI=%.1f samples=%lu; %s\n",s.receiving,s.frequency/1e6,s.rssi,(unsigned long)s.samples,s.status);
    printf("mixrf: scan=%d sweeps=%lu hits=%lu channel=%u; NFC watch=%d field=%d samples=%lu arrivals=%lu\n",
        s.scanning,(unsigned long)s.sweeps,(unsigned long)s.energy_hits,s.channel,
        s.nfc_watching,s.nfc_field,(unsigned long)s.nfc_samples,(unsigned long)s.nfc_events);
    printf("mixrf: cards=%d present=%d ATQA=%04x polls=%lu tx=%lu hits=%lu errors=%lu\n",s.card_scanning,s.card_present,s.card_atqa,(unsigned long)s.card_polls,(unsigned long)s.card_tx,(unsigned long)s.card_hits,(unsigned long)s.card_errors);
}
