/* IDF 5.5 NimBLE maps ble_gap_disc/cancel to extended HCI when EXT_ADV
 * is built. Translate only the 1M fallback at the HCI boundary, preserving
 * NimBLE GAP ownership, callbacks, cancellation and connection state. */
#include "ble_link_legacy.h"
#include <stdint.h>
#include <string.h>
static bool legacy_scan;
void ble_link_legacy_scan(bool legacy) { legacy_scan=legacy; }
extern int __real_ble_hs_hci_cmd_tx(uint16_t,const void *,uint8_t,void *,uint8_t);
extern uint64_t ble_hs_hci_get_le_supported_feat(void);
int __wrap_ble_hs_hci_cmd_tx(uint16_t op,const void *cmd,uint8_t len,void *rsp,uint8_t size)
{
    const uint8_t *p=cmd;uint8_t old[25];
    if(legacy_scan && op==0x2041 && len==8 && p[2]==1) {
        old[0]=p[3];memcpy(old+1,p+4,4);old[5]=p[0];old[6]=p[1];
        return __real_ble_hs_hci_cmd_tx(0x200b,old,7,rsp,size);
    }
    if(legacy_scan && op==0x2042 && len==6)
        return __real_ble_hs_hci_cmd_tx(0x200c,p,2,rsp,size);
    /* Legacy-only controllers must still connect to the Flipper on 1M. */
    if(op==0x2043 && !(ble_hs_hci_get_le_supported_feat() & 0x1000u) && len==26 && p[9]==1) {
        memcpy(old,p+10,4);old[4]=p[0];old[5]=p[2];memcpy(old+6,p+3,6);
        old[12]=p[1];memcpy(old+13,p+14,12);
        return __real_ble_hs_hci_cmd_tx(0x200d,old,25,rsp,size);
    }
    return __real_ble_hs_hci_cmd_tx(op,cmd,len,rsp,size);
}
