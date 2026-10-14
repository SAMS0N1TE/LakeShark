#include "ble_link_observer.h"
#include <string.h>

bool ble_link_observer_subscribe(ble_link_observer_t *q, ble_link_advert_fn fn, void *ctx)
{
    if (!q || !fn) return false;
    for (unsigned i = 0; i < q->listener_count; i++)
        if (q->listeners[i].fn == fn && q->listeners[i].ctx == ctx) return true;
    if (q->listener_count == BLE_LINK_AD_LISTENERS) return false;
    q->listeners[q->listener_count].fn = fn;
    q->listeners[q->listener_count++].ctx = ctx;
    return true;
}

bool ble_link_observer_push(ble_link_observer_t *q, const uint8_t addr[6],
    uint8_t type, int rssi, bool response, const uint8_t *data, size_t len, int64_t us)
{
    if (!q) return false;
    __atomic_fetch_add(&q->received, 1, __ATOMIC_RELAXED);
    uint32_t w = __atomic_load_n(&q->write, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&q->read, __ATOMIC_ACQUIRE);
    if (!addr || (!data && len) || len > BLE_LINK_AD_MAX) {
        __atomic_fetch_add(&q->invalid, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&q->dropped, 1, __ATOMIC_RELAXED);
        return false;
    }
    if (w - r >= BLE_LINK_AD_DEPTH) {
        __atomic_fetch_add(&q->overflow, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&q->dropped, 1, __ATOMIC_RELAXED);
        return false;
    }
    ble_link_advert_t *a = &q->records[w % BLE_LINK_AD_DEPTH];
    memcpy(a->addr, addr, 6);
    a->addr_type = type; a->rssi = rssi; a->scan_response = response;
    a->len = len; a->seen_us = us;
    if (len) memcpy(a->data, data, len);
    __atomic_store_n(&q->write, w + 1, __ATOMIC_RELEASE);
    uint32_t depth = w + 1 - r;
    if (depth > __atomic_load_n(&q->high_water, __ATOMIC_RELAXED))
        __atomic_store_n(&q->high_water, depth, __ATOMIC_RELAXED);
    return true;
}

unsigned ble_link_observer_dispatch(ble_link_observer_t *q, unsigned budget)
{
    if (!q) return 0;
    unsigned n = 0;
    uint32_t r = __atomic_load_n(&q->read, __ATOMIC_RELAXED);
    while (n < budget && r != __atomic_load_n(&q->write, __ATOMIC_ACQUIRE)) {
        const ble_link_advert_t *a = &q->records[r % BLE_LINK_AD_DEPTH];
        for (unsigned i = 0; i < q->listener_count; i++)
            q->listeners[i].fn(a, q->listeners[i].ctx);
        __atomic_store_n(&q->read, ++r, __ATOMIC_RELEASE);
        __atomic_fetch_add(&q->dispatched, 1, __ATOMIC_RELAXED);
        n++;
    }
    return n;
}

bool ble_link_observer_ext(ble_link_observer_t *q,const uint8_t addr[6],
    uint8_t type,uint8_t sid,int rssi,bool response,uint8_t status,
    const uint8_t *data,size_t len,int64_t us)
{
    if(!q || !addr || (!data && len))return false;
    if(us<q->chain_quarantine) {__atomic_fetch_add(&q->dropped,1,__ATOMIC_RELAXED);return false;}
    int slot=-1,free_slot=-1;
    for(unsigned i=0;i<4;i++) {
        if(q->chains[i].used && us-q->chains[i].at>2000000) {
            /* A late tail has no chain-start marker. Quarantine rather than
             * parse it as a new complete advertisement. */
            memset(q->chains,0,sizeof(q->chains));q->chain_quarantine=us+2000000;
            __atomic_fetch_add(&q->dropped,1,__ATOMIC_RELAXED);return false;
        }
        if(!q->chains[i].used)free_slot=i;
        else if(q->chains[i].type==type && q->chains[i].sid==sid &&
            q->chains[i].response==response && !memcmp(q->chains[i].addr,addr,6))slot=i;
    }
    if(status>=2) {
        if(slot>=0)q->chains[slot].used=false;
        __atomic_fetch_add(&q->invalid,1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&q->dropped,1,__ATOMIC_RELAXED);return false;
    }
    if(slot<0 && status==0)return ble_link_observer_push(q,addr,type,rssi,response,data,len,us);
    if(slot<0) {
        if(free_slot<0) {
            /* Fail closed while tails of untracked chains can still arrive. */
            memset(q->chains,0,sizeof(q->chains));q->chain_quarantine=us+2000000;
            __atomic_fetch_add(&q->dropped,1,__ATOMIC_RELAXED);return false;
        }
        slot=free_slot;memset(&q->chains[slot],0,sizeof(q->chains[slot]));
        q->chains[slot].used=true;q->chains[slot].type=type;q->chains[slot].sid=sid;
        q->chains[slot].response=response;memcpy(q->chains[slot].addr,addr,6);
    }
    q->chains[slot].at=us;
    if(len>(size_t)(BLE_LINK_AD_MAX-q->chains[slot].len))q->chains[slot].discard=true;
    if(!q->chains[slot].discard) {
        if(len)memcpy(q->chains[slot].data+q->chains[slot].len,data,len);
        q->chains[slot].len+=len;
    }
    if(status==1)return false;
    q->chains[slot].used=false;
    if(q->chains[slot].discard) {
        __atomic_fetch_add(&q->invalid,1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&q->dropped,1,__ATOMIC_RELAXED);return false;
    }
    return ble_link_observer_push(q,addr,type,rssi,response,q->chains[slot].data,q->chains[slot].len,us);
}
