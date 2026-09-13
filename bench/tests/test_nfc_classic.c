/* LS_TEST_SOURCES: ${FW}/components/lakeshark/nfc/nfc_classic.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/nfc */
#include "ls_test.h"
#include "nfc_classic.h"
LS_CASE(classic_4k_sector_boundary){
 LS_EQ_INT(ls_classic_first(31),124);LS_EQ_INT(ls_classic_first(32),128);
 LS_EQ_INT(ls_classic_first(39),240);LS_EQ_INT(ls_classic_count(31),4);
 LS_EQ_INT(ls_classic_count(32),16);LS_EQ_INT(ls_classic_sector(127),31);
 LS_EQ_INT(ls_classic_sector(128),32);LS_EQ_INT(ls_classic_sector(255),39);
 for(unsigned b=0;b<256;b++){unsigned s=ls_classic_sector(b);LS_CHECK(b>=ls_classic_first(s));LS_CHECK(b<ls_classic_first(s)+ls_classic_count(s));}
}
LS_CASE(nfca_halt_crc_wire_vector){const uint8_t halt[]={0x50,0x00};LS_EQ_INT(ls_nfc_crc(halt,2),0xcd57);}
LS_CASE(classic_odd_parity){for(unsigned v=0;v<256;v++){unsigned bits=0;for(unsigned b=0;b<8;b++)bits+=(v>>b)&1;LS_EQ_INT((bits+ls_nfc_odd_parity(v))&1,1);}}
LS_CASE(classic_trailer_key_visibility){
 uint8_t t[16]={0};t[6]=0xff;t[7]=7;t[8]=0x80;
 LS_EQ_INT(ls_classic_trailer_mask(t,0),0xffc0);
 LS_EQ_INT(ls_classic_trailer_mask(t,1),0x03c0);
 t[6]=0x7f;t[8]=0x88;
 LS_EQ_INT(ls_classic_trailer_mask(t,0),0x03c0);
 t[6]=0xfe;t[8]=0x80;
 LS_EQ_INT(ls_classic_trailer_mask(t,0),0x03c0);
}
