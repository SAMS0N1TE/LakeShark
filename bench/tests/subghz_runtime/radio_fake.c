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
