/* Simulator has no USB/socket transport. Never invent detections. */
#include "apps/rec/rec_catch.h"
#include <string.h>
ls_radio_err_t rec_catch_start(uint32_t hz){(void)hz;return LS_RADIO_ERR_UNAVAILABLE;}
void rec_catch_stop(void){}
void rec_catch_status(catch_status_t *s){memset(s,0,sizeof(*s));strcpy(s->rtl_status,"RTL: unavailable in simulator");strcpy(s->lr_status,"LR2021: unavailable in simulator");}
bool rec_catch_result(int i,catch_result_t *r){(void)i;(void)r;return false;}
bool rec_catch_save(int i){(void)i;return false;}
