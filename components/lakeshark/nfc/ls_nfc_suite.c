#include "ls_nfc_suite.h"
#include "nfc_classic.h"
#include "ls_board.h"
#include <string.h>
#include <stdio.h>
#ifdef LS_BOARD_MIX_CC_CS
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <sys/stat.h>
static portMUX_TYPE guard=portMUX_INITIALIZER_UNLOCKED;
static ls_nfc_suite_status_t s;
EXT_RAM_BSS_ATTR static uint8_t memory[256][16];
static uint16_t known[256];
static uint8_t keys[2][6]={{255,255,255,255,255,255},{255,255,255,255,255,255}};
static uint8_t sector_key[40][2][6],sector_key_set[40];
static unsigned request,sector;
static bool cancel;
static void message(const char *text){portENTER_CRITICAL(&guard);snprintf(s.status,sizeof(s.status),"%s",text);portEXIT_CRITICAL(&guard);}
bool ls_nfc_suite_start(bool read){portENTER_CRITICAL(&guard);bool ok=!s.busy;if(ok){request=read?2:1;s.busy=true;cancel=false;}portEXIT_CRITICAL(&guard);return ok;}
void ls_nfc_suite_stop(void){portENTER_CRITICAL(&guard);cancel=true;portEXIT_CRITICAL(&guard);}
bool ls_nfc_suite_busy(void){portENTER_CRITICAL(&guard);bool b=s.busy;portEXIT_CRITICAL(&guard);return b;}
void ls_nfc_suite_snapshot(ls_nfc_suite_status_t *out){if(out){portENTER_CRITICAL(&guard);*out=s;portEXIT_CRITICAL(&guard);}}
bool ls_nfc_suite_block(unsigned b,uint8_t out[16],uint16_t *mask){if(b>=256 || !out || !mask)return false;portENTER_CRITICAL(&guard);bool ok=b<s.blocks;if(ok){memcpy(out,memory[b],16);*mask=known[b];}portEXIT_CRITICAL(&guard);return ok;}
static int hex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
bool ls_nfc_suite_key(bool b,const char *text){if(!text || strlen(text)!=12)return false;uint8_t k[6];for(unsigned i=0;i<6;i++){int a=hex(text[i*2]),v=hex(text[i*2+1]);if(a<0||v<0)return false;k[i]=a*16+v;}portENTER_CRITICAL(&guard);bool ok=!s.busy;if(ok)memcpy(keys[b],k,6);portEXIT_CRITICAL(&guard);return ok;}
bool ls_nfc_suite_save(void){portENTER_CRITICAL(&guard);bool ok=!s.busy&&s.identified;if(ok){request=3;s.busy=true;cancel=false;}portEXIT_CRITICAL(&guard);return ok;}
bool ls_nfc_suite_sector_key(unsigned sector,bool b,const char *text){
 if(sector>=40 || !text || strlen(text)!=12)return false;
 uint8_t k[6];for(unsigned i=0;i<6;i++){int a=hex(text[i*2]),v=hex(text[i*2+1]);if(a<0||v<0)return false;k[i]=a*16+v;}
 portENTER_CRITICAL(&guard);bool ok=!s.busy&&s.identified&&sector<s.sectors;if(ok){memcpy(sector_key[sector][b],k,6);sector_key_set[sector]|=1u<<b;}portEXIT_CRITICAL(&guard);return ok;
}
static uint32_t be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void append_crc(uint8_t *p,unsigned n){uint16_t c=ls_nfc_crc(p,n);p[n]=c;p[n+1]=c>>8;}
static bool crc_ok(const uint8_t *p,unsigned n){return n>=2 && ls_nfc_crc(p,n-2)==(p[n-2]|p[n-1]<<8);}
static bool selected(const ls_nfc_suite_io_t *io,uint8_t uid[10],uint8_t *len,uint8_t *sak,uint16_t *atqa){
 *len=0;
 for(unsigned attempt=0;attempt<3;attempt++){
  io->end();if(!io->begin())return false;
  uint8_t tx[9]={0x26},rx[24];unsigned bits=0;
  if(io->exchange(tx,7,rx,sizeof(rx),&bits,false) || bits!=16)continue;
  *atqa=rx[0]|rx[1]<<8;
  bool ok=true;
  for(unsigned level=0;level<3;level++){
   tx[0]=0x93+level*2;tx[1]=0x20;
   if(io->exchange(tx,16,rx,sizeof(rx),&bits,false)||bits!=40 || (rx[0]^rx[1]^rx[2]^rx[3])!=rx[4]){ok=false;break;}
   bool cascade=rx[0]==0x88;memcpy(tx+2,rx,5);tx[1]=0x70;append_crc(tx,7);
   if(io->exchange(tx,72,rx,sizeof(rx),&bits,false)||bits!=24||!crc_ok(rx,3)||cascade!=!!(rx[0]&4)){ok=false;break;}
   *sak=rx[0];unsigned count=cascade?3:4;if(*len+count>10){ok=false;break;}
   memcpy(uid+*len,tx+2+(cascade?1:0),count);*len+=count;
   if(!cascade)return true;
  }
  (void)ok;*len=0;
 }
 return false;
}
static void putbit(uint8_t *p,unsigned n,unsigned value){if(value&1)p[n/8]|=1u<<(n%8);}
static unsigned getbit(const uint8_t *p,unsigned n){return (p[n/8]>>(n%8))&1;}
static unsigned encrypt(ls_crypto1_t *c,const uint8_t *plain,unsigned n,uint8_t *out,bool feed){
 memset(out,0,(n*9+7)/8);
 for(unsigned i=0;i<n;i++){uint8_t v=plain[i]^ls_crypto1_byte(c,feed?plain[i]:0);for(unsigned b=0;b<8;b++)putbit(out,i*9+b,v>>b);putbit(out,i*9+8,ls_crypto1_filter(c->odd)^ls_nfc_odd_parity(plain[i]));}
 return n*9;
}
static bool decrypt(ls_crypto1_t *c,const uint8_t *in,unsigned bits,uint8_t *out,unsigned n){
 if(bits!=n*9)return false;
 bool ok=true;
 for(unsigned i=0;i<n;i++){uint8_t v=0;for(unsigned b=0;b<8;b++)v|=getbit(in,i*9+b)<<b;out[i]=v^ls_crypto1_byte(c,0);ok&=getbit(in,i*9+8)==(ls_crypto1_filter(c->odd)^ls_nfc_odd_parity(out[i]));}return ok;
}
static bool authenticate(const ls_nfc_suite_io_t *io,unsigned block,unsigned key_b,ls_crypto1_t *c){
 uint8_t uid[10],len,sak;uint16_t atqa;
 if(!selected(io,uid,&len,&sak,&atqa))return false;
 if(len!=s.uid_len || memcmp(uid,s.uid,len) || sak!=s.sak){message("Card changed or selection failed; retained prior data");return false;}
 portENTER_CRITICAL(&guard);s.attempts++;portEXIT_CRITICAL(&guard);
 uint8_t tx[20]={key_b?0x61:0x60,(uint8_t)block},rx[32];unsigned bits=0;append_crc(tx,2);
 if(io->exchange(tx,32,rx,sizeof(rx),&bits,false)||bits!=32)return false;
 uint32_t nt=be32(rx);unsigned sec=ls_classic_sector(block);const uint8_t *key_bytes=(sector_key_set[sec]&(1u<<key_b))?sector_key[sec][key_b]:keys[key_b];
 uint64_t key=0;for(unsigned i=0;i<6;i++)key=(key<<8)|key_bytes[i];
 ls_crypto1_init(c,key);ls_crypto1_word(c,nt^be32(uid+len-4));
 uint8_t nr[4],ar[4];esp_fill_random(nr,4);
 uint32_t prng=ls_crypto1_prng(nt,32);for(unsigned i=0;i<4;i++){prng=ls_crypto1_prng(prng,8);ar[i]=prng;}
 encrypt(c,nr,4,tx,true);uint8_t tail[5];encrypt(c,ar,4,tail,false);
 for(unsigned i=0;i<36;i++)putbit(tx,36+i,getbit(tail,i));
 if(io->exchange(tx,72,rx,sizeof(rx),&bits,true))return false;
 uint8_t answer[4];if(!decrypt(c,rx,bits,answer,4))return false;
 return be32(answer)==ls_crypto1_prng(nt,96);
}
static int read_block(const ls_nfc_suite_io_t *io,ls_crypto1_t *c,unsigned block,uint8_t out[16]){
 uint8_t tx[5],plain[18]={0x30,(uint8_t)block},rx[24];unsigned bits=0;append_crc(plain,2);
 encrypt(c,plain,4,tx,false);
 if(io->exchange(tx,36,rx,sizeof(rx),&bits,true))return 1;
 if(bits==4){unsigned nak=(rx[0]^ls_crypto1_byte(c,0))&15;if(nak==0||nak==1||nak==4||nak==5)return 2;return 1;}
 if(!decrypt(c,rx,bits,plain,18)||!crc_ok(plain,18))return 1;
 memcpy(out,plain,16);return 0;
}
static void export_card(void){
 char path[96];mkdir("/sdcard/nfc",0775);
 snprintf(path,sizeof(path),"/sdcard/nfc/card-%08lx-%08lx.txt",(unsigned long)esp_random(),(unsigned long)esp_random());
 FILE *f=fopen(path,"wx");if(!f){message("SD export failed; card data remains in memory");return;}
 fprintf(f,"LakeShark NFC capture v1\nUID:");for(unsigned i=0;i<s.uid_len;i++)fprintf(f," %02X",s.uid[i]);
 fprintf(f,"\nATQA: %04X\nSAK: %02X\nBlocks: %u\nRead: %u\nUnknown bytes: ?? (Key A is never readable from the card)\n",s.atqa,s.sak,s.blocks,s.read_blocks);
 for(unsigned b=0;b<s.blocks;b++){fprintf(f,"%03u [%u] ",b,s.block_state[b]);for(unsigned i=0;i<16;i++){if(known[b]&(1u<<i))fprintf(f,"%02X ",memory[b][i]);else fputs("?? ",f);}fputc('\n',f);}
 bool ok=!ferror(f);if(fclose(f))ok=false;message(ok?path:"SD export incomplete; retry after checking card");
}
void ls_nfc_suite_step(const ls_nfc_suite_io_t *io){
 portENTER_CRITICAL(&guard);unsigned req=request;request=0;bool stop=cancel;portEXIT_CRITICAL(&guard);
 if(stop){io->end();message("Stopped; verified blocks retained");portENTER_CRITICAL(&guard);s.busy=false;s.reading=false;for(unsigned b=0;b<s.blocks;b++)if(s.block_state[b]==LS_CARD_ACTIVE)s.block_state[b]=LS_CARD_UNREAD;portEXIT_CRITICAL(&guard);return;}
 if(req==3){export_card();portENTER_CRITICAL(&guard);s.busy=false;portEXIT_CRITICAL(&guard);return;}
 if(req){
  portENTER_CRITICAL(&guard);s.busy=true;s.reading=req==2;portEXIT_CRITICAL(&guard);
  message("Selecting card: validating UID, BCC and SAK CRC");
  uint8_t uid[10],len,sak;uint16_t atqa;
  if(!selected(io,uid,&len,&sak,&atqa)){io->end();message("No valid selection; previous capture retained");portENTER_CRITICAL(&guard);s.busy=false;s.reading=false;portEXIT_CRITICAL(&guard);return;}
  portENTER_CRITICAL(&guard);
  if(s.uid_len!=len || memcmp(s.uid,uid,len))memset(sector_key_set,0,sizeof(sector_key_set));
  memset(&s,0,sizeof(s));memset(known,0,sizeof(known));s.reading=req==2;s.identified=true;memcpy(s.uid,uid,len);s.uid_len=len;s.sak=sak;s.atqa=atqa;
  s.blocks=sak==8?64:sak==0x18?256:sak==9?20:0;s.sectors=sak==8?16:sak==0x18?40:sak==9?5:0;sector=0;
  bool read=s.reading&&s.blocks;s.busy=read;s.reading=read;portEXIT_CRITICAL(&guard);io->end();
  message(read?"Reading sectors with configured Key A/B":s.blocks?"Identity verified; memory is unread":"Identity verified; this memory protocol is not supported yet");return;
 }
 if(!s.busy)return;
 unsigned first=ls_classic_first(sector),count=ls_classic_count(sector);bool authenticated=false;
 for(unsigned k=0;k<2;k++){
  portENTER_CRITICAL(&guard);bool abort=cancel;s.current=first;if(s.block_state[first]!=LS_CARD_READ)s.block_state[first]=LS_CARD_ACTIVE;portEXIT_CRITICAL(&guard);if(abort)break;
  char text[96];snprintf(text,sizeof(text),"Sector %u/%u: authenticating Key %c",sector+1,s.sectors,'A'+k);message(text);
  ls_crypto1_t crypto;
  bool auth=false;
  for(unsigned retry=0;retry<2 && !auth;retry++){
   portENTER_CRITICAL(&guard);bool abort=cancel;portEXIT_CRITICAL(&guard);if(abort)break;
   auth=authenticate(io,first,k,&crypto);
  }
  if(!auth)continue;
  authenticated=true;portENTER_CRITICAL(&guard);s.auth_ok++;s.sector_keys[sector]|=1u<<k;portEXIT_CRITICAL(&guard);
  for(unsigned b=first;b<first+count;b++){
   portENTER_CRITICAL(&guard);s.current=b;if(s.block_state[b]!=LS_CARD_READ)s.block_state[b]=LS_CARD_ACTIVE;bool abort=cancel;portEXIT_CRITICAL(&guard);if(abort)break;
   uint8_t data[16];int result=read_block(io,&crypto,b,data);bool ok=result==0;
   portENTER_CRITICAL(&guard);if(ok){if(s.block_state[b]!=LS_CARD_READ)s.read_blocks++;uint16_t mask=b==first+count-1?ls_classic_trailer_mask(data,k):0xffff;
    for(unsigned i=0;i<16;i++){if(mask&(1u<<i))memory[b][i]=data[i];}
    known[b]|=mask;s.block_state[b]=LS_CARD_READ;}else{if(result==2)s.naks++;else s.errors++;if(s.block_state[b]!=LS_CARD_READ)s.block_state[b]=result==2?LS_CARD_NAK:LS_CARD_ERROR;}portEXIT_CRITICAL(&guard);
   if(!ok)break;
  }
  io->end();
 }
 io->end();portENTER_CRITICAL(&guard);
 for(unsigned b=first;b<first+count;b++)if(s.block_state[b]==LS_CARD_ACTIVE || (!authenticated && s.block_state[b]==LS_CARD_UNREAD))s.block_state[b]=authenticated?LS_CARD_UNREAD:LS_CARD_AUTH_FAILED;
 sector++;bool done=sector>=s.sectors;if(done){s.busy=false;s.reading=false;}portEXIT_CRITICAL(&guard);
 if(done){char text[96];snprintf(text,sizeof(text),"Read finished: %u/%u blocks verified",s.read_blocks,s.blocks);message(text);}
}
#else
bool ls_nfc_suite_start(bool read){(void)read;return false;}
void ls_nfc_suite_stop(void){}
bool ls_nfc_suite_busy(void){return false;}
void ls_nfc_suite_snapshot(ls_nfc_suite_status_t *out){if(out){memset(out,0,sizeof(*out));snprintf(out->status,sizeof(out->status),"Connect NFC reader for live card data");}}
bool ls_nfc_suite_block(unsigned b,uint8_t out[16],uint16_t *mask){(void)b;(void)out;(void)mask;return false;}
bool ls_nfc_suite_key(bool b,const char *hex){(void)b;(void)hex;return false;}
bool ls_nfc_suite_save(void){return false;}
bool ls_nfc_suite_sector_key(unsigned sector,bool b,const char *hex){(void)sector;(void)b;(void)hex;return false;}
void ls_nfc_suite_step(const ls_nfc_suite_io_t *io){(void)io;}
#endif
