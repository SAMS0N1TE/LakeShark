#ifndef DISPLAY_CTL_H
#define DISPLAY_CTL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_ctl_init(void);

void display_ctl_set_user(int pct);
int  display_ctl_get_user(void);

void display_ctl_set_autodim(bool enabled);
bool display_ctl_autodim_enabled(void);

void display_ctl_set_autodim_timeout(int seconds);
int  display_ctl_autodim_timeout(void);

#ifdef __cplusplus
}
#endif

#endif
