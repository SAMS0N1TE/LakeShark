#include "ls_test.h"
#define LS_NFC_SUITE_HOST 1
#include <stdint.h>
#include <sys/stat.h>
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
static uint32_t esp_random(void){return 42;}
#include "../../components/lakeshark/nfc/ls_nfc_suite.c"
static ls_mixrf_status_t radio;
void ls_mixrf_snapshot(ls_mixrf_status_t *out){*out=radio;}
static uint8_t card_uid[4]={1,2,3,4};
static bool begin(void){return true;}
static void end(void){}
static int exchange(const uint8_t *tx,unsigned bits,uint8_t *rx,unsigned cap,unsigned *received,bool raw)
{
    (void)cap;(void)raw;
    if(bits==7){rx[0]=4;rx[1]=0;*received=16;return 0;}
    if(bits==16 && tx[1]==0x20){memcpy(rx,card_uid,4);rx[4]=rx[0]^rx[1]^rx[2]^rx[3];*received=40;return 0;}
    if(bits==72 && tx[1]==0x70){rx[0]=8;append_crc(rx,1);*received=24;return 0;}
    return 1;
}
static const ls_nfc_suite_io_t io={begin,end,exchange};
static void capture(void)
{
    memset(&s,0,sizeof(s));memset(known,0,sizeof(known));memset(sector_key_set,0,sizeof(sector_key_set));
    request=0;cancel=saved=false;radio=(ls_mixrf_status_t){.nfc=true};card_uid[0]=1;
    LS_CHECK(ls_nfc_suite_start(false));ls_nfc_suite_step(&io);
    LS_CHECK(s.identified);s.read_blocks=1;s.block_state[0]=LS_CARD_READ;
    known[0]=0xffff;memset(memory[0],0x5a,16);
}
static void retained(void)
{
    uint8_t bytes[16];uint16_t mask=0;
    LS_CHECK(ls_nfc_suite_block(0,bytes,&mask));LS_EQ_INT(mask,0xffff);
    for(unsigned i=0;i<16;i++)LS_EQ_INT(bytes[i],0x5a);
    LS_EQ_INT(s.read_blocks,1);LS_EQ_INT(s.block_state[0],LS_CARD_READ);
}
LS_CASE(same_card_scan_keeps_verified_bytes)
{
    capture();LS_CHECK(ls_nfc_suite_start(false));ls_nfc_suite_step(&io);
    LS_CHECK(!s.busy);retained();
}
LS_CASE(repeat_read_auth_failure_and_stop_keep_prior_capture)
{
    capture();LS_CHECK(ls_nfc_suite_start(true));ls_nfc_suite_step(&io);
    LS_CHECK(s.busy);ls_nfc_suite_step(&io);retained();
    LS_CHECK(s.attempts>0);ls_nfc_suite_stop();ls_nfc_suite_step(&io);
    LS_CHECK(!s.busy && !s.reading);retained();
}
LS_CASE(different_card_requires_saved_capture)
{
    capture();card_uid[0]=9;LS_CHECK(ls_nfc_suite_start(true));ls_nfc_suite_step(&io);
    LS_CHECK(!s.busy);LS_EQ_INT(s.uid[0],1);retained();
    LS_CHECK(strstr(s.status,"SAVE")!=NULL);
    saved=true;LS_CHECK(ls_nfc_suite_start(false));ls_nfc_suite_step(&io);
    LS_EQ_INT(s.uid[0],9);LS_EQ_INT(s.read_blocks,0);LS_EQ_INT(known[0],0);
}
LS_CASE(active_capture_refuses_scan_read_and_export)
{
    capture();radio.capture_requested=radio.receive_requested=true;
    LS_CHECK(!ls_nfc_suite_start(false));LS_CHECK(!ls_nfc_suite_start(true));
    LS_CHECK(!ls_nfc_suite_save());LS_CHECK(!s.busy);retained();
    LS_CHECK(strstr(s.status,"Stop CC1101")!=NULL);
}

#if LAP_NFC
static uint8_t t2_memory[176];static bool t2_present=true;static unsigned t2_writes;
static int t2_exchange(const uint8_t *tx,unsigned bits,uint8_t *rx,unsigned cap,unsigned *received,bool raw){
 (void)cap;(void)raw;if(!t2_present)return 1;
 if(bits==7){rx[0]=0x44;rx[1]=0;*received=16;return 0;}
 if(bits==16&&tx[1]==0x20){memcpy(rx,card_uid,4);rx[4]=rx[0]^rx[1]^rx[2]^rx[3];*received=40;return 0;}
 if(bits==72&&tx[1]==0x70){rx[0]=0;append_crc(rx,1);*received=24;return 0;}
 if(bits==32&&tx[0]==0x30&&tx[1]*4+16<=176){memcpy(rx,t2_memory+tx[1]*4,16);append_crc(rx,16);*received=144;return 0;}
 if(bits==64&&tx[0]==0xa2&&tx[1]>=4&&tx[1]<=39){memcpy(t2_memory+tx[1]*4,tx+2,4);t2_writes++;rx[0]=10;*received=4;return 0;}return 1;
}
static const ls_nfc_suite_io_t t2_io={begin,end,t2_exchange};
static void t2_setup(void){memset(&s,0,sizeof(s));tag_owner=tag_pending=tag_result_ready=tag_write_pending=false;request=0;cancel=false;radio=(ls_mixrf_status_t){.nfc=true};t2_present=true;t2_writes=0;card_uid[0]=1;memset(t2_memory,0,sizeof(t2_memory));t2_memory[12]=0xe1;t2_memory[13]=0x10;t2_memory[14]=18;t2_memory[16]=3;t2_memory[17]=0;t2_memory[18]=0xfe;t2_memory[167]=0xff;}
LS_CASE(type2_app_reads_uid_and_writes_verified_ndef_without_lock_pages){
 t2_setup();LS_CHECK(ls_nfc_tag_open());LS_CHECK(!ls_nfc_suite_start(true));ls_nfc_suite_step(&t2_io);
 ls_nfc_tag_result_t r;uint8_t bytes[2048];LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),1);LS_EQ_INT(r.uid_len,4);LS_EQ_INT(r.capacity,144);LS_CHECK(r.writable);
 const uint8_t msg[]={0xd1,1,5,'T',2,'e','n','H','i'};LS_EQ_INT(ls_nfc_tag_write(msg,sizeof(msg)),0);ls_nfc_suite_step(&t2_io);LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),1);LS_EQ_INT(r.kind,5);LS_EQ_INT(r.len,sizeof(msg));LS_CHECK(!memcmp(msg,bytes,sizeof(msg)));LS_CHECK(t2_writes>0);LS_EQ_INT(t2_memory[10],0);LS_EQ_INT(t2_memory[11],0);ls_nfc_tag_close();
}
LS_CASE(type2_app_refuses_locked_tags_changed_uid_and_over_capacity){
 t2_setup();t2_memory[10]=1;LS_CHECK(ls_nfc_tag_open());ls_nfc_suite_step(&t2_io);ls_nfc_tag_result_t r;uint8_t bytes[2048]={0};LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),1);LS_CHECK(!r.writable);LS_EQ_INT(ls_nfc_tag_write(bytes,1),0);ls_nfc_suite_step(&t2_io);LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),1);LS_EQ_INT(r.kind,4);LS_EQ_INT(t2_writes,0);ls_nfc_tag_close();
 t2_setup();LS_CHECK(ls_nfc_tag_open());ls_nfc_suite_step(&t2_io);LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),1);card_uid[0]=9;LS_EQ_INT(ls_nfc_tag_write(bytes,1),0);ls_nfc_suite_step(&t2_io);LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),1);LS_EQ_INT(r.kind,4);LS_EQ_INT(t2_writes,0);LS_EQ_INT(ls_nfc_tag_write(bytes,200),0);ls_nfc_suite_step(&t2_io);LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),1);LS_EQ_INT(r.kind,4);LS_EQ_INT(t2_writes,0);ls_nfc_tag_close();
}
LS_CASE(type2_app_removal_and_stop_end_the_field_owner){
 t2_setup();LS_CHECK(ls_nfc_tag_open());ls_nfc_suite_step(&t2_io);ls_nfc_tag_result_t r;uint8_t bytes[2048];LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),1);t2_present=false;ls_nfc_suite_step(&t2_io);LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),1);LS_EQ_INT(r.kind,3);ls_nfc_suite_stop();LS_EQ_INT(ls_nfc_tag_read(&r,bytes,sizeof(bytes)),-26);LS_CHECK(!ls_nfc_suite_busy());
}
#else
LS_CASE(disabled_tag_service_retains_classic_tools) {
    capture();
    LS_CHECK(!ls_nfc_tag_open());
    LS_EQ_INT(ls_nfc_tag_read(NULL,NULL,0),-6);
    LS_EQ_INT(ls_nfc_tag_write(NULL,0),-6);
    ls_nfc_tag_close();
    LS_CHECK(s.identified);
}
#endif
