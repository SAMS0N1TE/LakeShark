#ifndef LS_ADSB_APP_H
#define LS_ADSB_APP_H

#ifdef __cplusplus
extern "C" {
#endif

int adsb_app_register(void);
void adsb_request_gain(int gain_tenths_db);
int adsb_requested_gain(void);

#ifdef __cplusplus
}
#endif

#endif
