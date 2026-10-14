#include <stdbool.h>
#include <stdint.h>
extern int subghz_task,subghz_lease,subghz_hold_attempts,subghz_cancel_hold;
extern void rec_watch_scan_stop(void);
bool ls_mesh_radio_hold(bool on)
{
    if(!on){if(subghz_lease==subghz_task)subghz_lease=0;return true;}
    subghz_hold_attempts++;
    if(subghz_cancel_hold){rec_watch_scan_stop();return false;}
    if(subghz_lease && subghz_lease!=subghz_task)return false;
    subghz_lease=subghz_task;return true;
}
bool ls_mesh_radio_held(void){return subghz_lease==subghz_task;}

/* REC's LR2021 OOK path: no stream on the bench. */
#include "ls_lora.h"
#include "lr_wf_session.h"
ls_lora_chip_t ls_lora_chip(void) { return LS_LORA_CHIP_SX126X; }
bool lr20xx_stream_source2_active(void) { return false; }
esp_err_t lr_wf_start_consumer(uint32_t hz, lr_wf_consumer_t consume, void *ctx)
{ (void)hz; (void)consume; (void)ctx; return ESP_ERR_NOT_SUPPORTED; }
void lr_wf_stop(void) {}
