#ifndef LS_ADSB_APP_H
#define LS_ADSB_APP_H

#include "adsb_source.h"

#ifdef __cplusplus
extern "C" {
#endif

int adsb_app_register(void);
void adsb_request_gain(int gain_tenths_db);
int adsb_requested_gain(void);

/* Which receiver is feeding the decoder now: an IQ receiver, the LoRa
   socket's chip, or none. The name is for the status line. */
adsb_source_t adsb_active_source(void);
const char *adsb_active_source_name(void);

#ifdef __cplusplus
}
#endif

#endif
