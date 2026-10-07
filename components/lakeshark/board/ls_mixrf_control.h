#ifndef LS_MIXRF_CONTROL_H
#define LS_MIXRF_CONTROL_H
#include "ls_mixrf.h"
/* Called under the radio lock: a stop can only release the caller's claim. */
static inline bool ls_mixrf_claim(ls_mixrf_owner_t *holder,ls_mixrf_owner_t owner,bool on)
{
    if(owner==LS_MIXRF_OWNER_NONE)return false;
    if(*holder!=LS_MIXRF_OWNER_NONE && *holder!=owner)return false;
    *holder=on?owner:LS_MIXRF_OWNER_NONE;
    return true;
}
static inline bool ls_mixrf_cc_request(ls_mixrf_status_t *s,ls_mixrf_owner_t owner,
                                      bool on,uint32_t hz,bool tx)
{
    if(on && (!s->cc || s->busy || tx ||
        !((hz>=300000000 && hz<=348000000)||(hz>=387000000 && hz<=464000000)||
          (hz>=779000000 && hz<=928000000))))return false;
    if(!ls_mixrf_claim(&s->cc_owner,owner,on))return false;
    s->receive_requested=on;s->capture_requested=on && owner==LS_MIXRF_OWNER_REC;
    return true;
}
static inline const char *ls_mixrf_cc_refusal(const ls_mixrf_status_t *s)
{
    if(s->cc_owner==LS_MIXRF_OWNER_MONITOR)return "CC1101 in use by MIX-RF MONITOR";
    if(s->cc_owner==LS_MIXRF_OWNER_REC)return "CC1101 in use by REC/SUB-GHZ WATCH";
    if(s->cc_owner!=LS_MIXRF_OWNER_NONE)return "CC1101 in use by COMPASS FIND";
    if(!s->keyboard)return "Keyboard absent; use PROBE after reconnecting";
    if(s->busy)return "CC1101 probe in progress";
    if(!s->cc)return "CC1101 absent; try PROBE in MIX-RF";
    return "CC1101 busy; try again when idle";
}
static inline void ls_mixrf_detached(ls_mixrf_status_t *s)
{
    s->keyboard=s->power=s->cc=s->nrf=s->nfc=false;
    s->receive_requested=s->capture_requested=s->scan_requested=false;
    s->receiving=s->capturing=s->scanning=false;
    s->cc_owner=s->scan_owner=LS_MIXRF_OWNER_NONE;
}
/* Every finished attempt establishes presence, including early failures. */
static inline void ls_mixrf_probe_complete(ls_mixrf_status_t *s)
{s->ready=true;s->busy=false;}
#endif
