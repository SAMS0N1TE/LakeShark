#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*LS-602*/
#define LS_HUB_RADIO   0x01u
#define LS_HUB_TUNE    0x02u
#define LS_HUB_SIGNAL  0x04u
#define LS_HUB_AUDIO   0x08u
#define LS_HUB_EVENT   0x10u
#define LS_HUB_ALL     0x1Fu

#define LS_HUB_LINE_MAX 48

/*LS-602*/
typedef struct {
    bool     rtl_ready;
    bool     parked;
    char     mode[8];
    char     target_app[8];
    uint32_t freq_hz;

    int      sig_pct;
    bool     active;
    uint32_t iq_bytes_sec;
    int      contacts;
    char     detail[40];

    int      volume;
    bool     muted;

    bool     sd_present;
    int      c6_state;
    int      batt_pct;
} ls_hub_state_t;

typedef void (*ls_hub_fn)(const ls_hub_state_t *s, uint32_t dirty, void *ud);

/*LS-602*/
void ls_hub_start(void);
int  ls_hub_subscribe(ls_hub_fn fn, void *ud);
void ls_hub_unsubscribe(int id);
const ls_hub_state_t *ls_hub_state(void);

/*LS-602*/
bool ls_hub_last_line(char *dst, int cap);

/*LS-602*/
void ls_hub_set_sd(bool present);
void ls_hub_set_c6(int state);

#ifdef __cplusplus
}
#endif
