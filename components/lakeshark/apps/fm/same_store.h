#ifndef LS_SAME_STORE_H
#define LS_SAME_STORE_H
#include "same.h"
void same_store_receive(const same_alert_t *a, void *user);
int same_store_count(void);
bool same_store_get(int newest_index, same_alert_t *out);
bool same_store_notice(same_alert_t *out);
void same_options_get(same_options_t *out);
void same_options_set(const same_options_t *options);
void same_options_load(void);
#endif
