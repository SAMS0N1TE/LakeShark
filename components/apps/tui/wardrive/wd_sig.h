/* The signature table moved into the firmware as a shared service
   (tui/ls_sigs.h) so SWEEP and other apps classify with the same rows.
   WARDRIVE keeps including this name and calling wd_sig_* as before. */
#ifndef WD_SIG_FWD_H
#define WD_SIG_FWD_H
#include "../ls_sigs.h"
#endif
