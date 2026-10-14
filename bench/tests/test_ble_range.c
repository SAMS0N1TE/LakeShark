#include "ls_test.h"
#include "ble_link_scan.h"
#include "ble_link_legacy.h"
#include <string.h>
static uint64_t features;
static uint16_t opcode;
static uint8_t bytes[40], length;
uint64_t ble_hs_hci_get_le_supported_feat(void) {return features;}
int __real_ble_hs_hci_cmd_tx(uint16_t op,const void *cmd,uint8_t len,void *rsp,uint8_t size) {
    (void)rsp;(void)size;opcode=op;length=len;if(len)memcpy(bytes,cmd,len);return 0;
}
int __wrap_ble_hs_hci_cmd_tx(uint16_t,const void *,uint8_t,void *,uint8_t);
LS_CASE(controller_features_and_real_legacy_hci_fallback) {
    LS_CHECK(ble_scan_long_range(0x1800,true,false));
    LS_CHECK(!ble_scan_long_range(0x1000,true,false));
    LS_CHECK(!ble_scan_long_range(0x800,true,false));
    LS_CHECK(!ble_scan_long_range(0x1800,false,false));
    LS_CHECK(!ble_scan_long_range(0x1800,true,true));
    uint8_t scan[]={0,0,1,0,160,0,48,0};
    ble_link_legacy_scan(false);__wrap_ble_hs_hci_cmd_tx(0x2041,scan,8,0,0);
    LS_CHECK(opcode==0x2041 && length==8);
    ble_link_legacy_scan(true);__wrap_ble_hs_hci_cmd_tx(0x2041,scan,8,0,0);
    uint8_t expected[]={0,160,0,48,0,0,0};
    LS_CHECK(opcode==0x200b && length==7 && !memcmp(bytes,expected,7));
    uint8_t enable[]={1,0,0,0,0,0};__wrap_ble_hs_hci_cmd_tx(0x2042,enable,6,0,0);
    LS_CHECK(opcode==0x200c && length==2 && bytes[0]==1 && bytes[1]==0);
    enable[0]=0;__wrap_ble_hs_hci_cmd_tx(0x2042,enable,6,0,0);LS_CHECK(bytes[0]==0);
    uint8_t connect[26];for(unsigned i=0;i<26;i++)connect[i]=i;connect[9]=1;
    features=0;__wrap_ble_hs_hci_cmd_tx(0x2043,connect,26,0,0);
    LS_CHECK(opcode==0x200d && length==25 && bytes[0]==10 && bytes[4]==0 && bytes[12]==1 && bytes[24]==25);
    features=0x1800;__wrap_ble_hs_hci_cmd_tx(0x2043,connect,26,0,0);LS_CHECK(opcode==0x2043);
}
