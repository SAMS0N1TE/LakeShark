/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
/* SDR front ends: the analog end of the RTL-SDR and the HackRF - bias-T,
   reference correction, AGC, amp and gain stages - set one way whoever holds
   the radio, from OPTIONS > FRONT END in any app on that radio, or the
   console's `bias` and `rffe`.

   What is saved and what is not:
   - The bias-T is never saved on. It is off at every plug-in and every boot,
     and drops with the dongle, unless the user pins it ON AT PLUG-IN for
     that radio; then the driver turns it on at attach and says so.
   - PPM is saved per RTL-SDR dongle (keyed by its USB strings, four
     dongles remembered) and once for the HackRF.
   - Digital AGC, offset tuning, the HackRF amp and fixed LNA/VGA are saved.
   - Direct sampling is not saved: HF on a VHF app after a reboot would read
     as a dead radio. The HF work (feature/ovh-hf-rtl-20261028) answers it
     in the RTL driver; until then it is UNSUPPORTED.

   Switching DC onto the antenna port or forcing the amp on leaves a notice
   (ls_rffe_take_notice) that the TUI shows as a banner. */
#ifndef LS_RADIO_FRONTEND_H
#define LS_RADIO_FRONTEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LS_RFFE_RTL = 0, LS_RFFE_HACKRF, LS_RFFE_DEVS } ls_rffe_dev_t;

/* "rtl-sdr.usb", "hackrf.usb"; "RTL-SDR", "HackRF". Never NULL. */
const char *ls_rffe_endpoint(ls_rffe_dev_t d);
const char *ls_rffe_name(ls_rffe_dev_t d);
/* From an endpoint id, or a console word: "rtl", "rtlsdr", "hackrf". */
bool ls_rffe_from_endpoint(const char *endpoint_id, ls_rffe_dev_t *out);
bool ls_rffe_from_word(const char *word, ls_rffe_dev_t *out);

/* ---------------------------------------------------------- bias-T -- */

#define LS_RFFE_BIAS_WARNING "DC on the antenna port: only with an LNA or active antenna"

/* The device is attached and its driver can switch the port. */
bool ls_radio_bias_t_supported(const char *endpoint_id);
/* Switch it, now, on the caller's task (USB control transfers: never the UI
   task - the UI uses ls_rffe_request). On leaves the warning notice. */
ls_radio_err_t ls_radio_bias_t_set(const char *endpoint_id, bool on);
/* What the device says: the RTL-SDR's GPIO registers read back; the HackRF
   has no read for it, so what it was last told. Blocks like set. */
ls_radio_err_t ls_radio_bias_t_get(const char *endpoint_id, bool *on);
/* The state last applied, from RAM: for a screen. */
bool ls_radio_bias_t_state(const char *endpoint_id);
/* ON AT PLUG-IN: the one way it comes back on by itself. Saved. */
void ls_radio_bias_t_pin(const char *endpoint_id, bool pinned);
bool ls_radio_bias_t_pinned(const char *endpoint_id);

/* ------------------------------------------------- every control -- */

/* Whether the device's driver answers ctl now (attached, and e.g. offset
   tuning only on an E4000). RAM only, never blocks: safe on the UI task. */
bool ls_rffe_supported(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl);
/* Whether this kind of device has the control at all, attached or not. */
bool ls_rffe_has(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl);
/* NULL when value is one ctl takes, else why not. */
const char *ls_rffe_check(ls_radio_fe_ctl_t ctl, int value);
/* The value in force, or the saved one while the device is away. RAM. */
int ls_rffe_value(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl);

/* Validate, save what is saved, apply on this task. A device that is not
   attached keeps a saved setting for its next attach and answers
   UNAVAILABLE; the bias-T and direct sampling are refused then. */
ls_radio_err_t ls_rffe_set(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl, int value);
/* The same without blocking: save now, apply on the front-end worker. For
   the UI. False when the value is refused (ls_rffe_check says why). */
bool ls_rffe_request(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl, int value);

/* The tuner's gain steps, tenths of a dB. Blocks. */
int ls_rffe_gains(ls_rffe_dev_t d, int *tenths, int max);

/* A warning waiting to be shown, taken once. */
bool ls_rffe_take_notice(char *title, size_t title_n, char *body, size_t body_n);

/* ----------------------------------------------- for the drivers -- */

/* The saved setting a driver applies at attach. RAM only, any task. For
   LS_RADIO_FE_PPM on the RTL-SDR it is the dongle's own (see attach). */
int  ls_rffe_saved(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl);
/* The RTL-SDR in the socket, by ls_rffe_dongle_key: its PPM is looked up
   and later saved under that key. */
void ls_rffe_rtl_attach(uint32_t key);
/* An RTL-SDR has been attached this boot, so its PPM has a key. */
bool ls_rffe_rtl_known(void);
/* A driver reports a state it set itself: the bias-T at attach and detach.
   pinned_on: it came on at attach because it is pinned (a notice is left). */
void ls_rffe_note(ls_rffe_dev_t d, ls_radio_fe_ctl_t ctl, int value, bool pinned_on);

/* ------------------------------------------------- stored forms -- */

typedef struct {
    uint8_t dagc;                  /* 0 follow gain, 1 on, 2 off   */
    bool    offset;                /* RTL offset tuning            */
    bool    pin[LS_RFFE_DEVS];     /* bias-T on at plug-in         */
    uint8_t amp;                   /* 0 by gain, 1 off, 2 on       */
    int8_t  lna_db;                /* -1 by gain, 0..40 step 8     */
    int8_t  vga_db;                /* -1 by gain, 0..62 step 2     */
} ls_rffe_cfg_t;

/* One word, version in the top nibble; 0 or anything malformed unpacks to
   the defaults (false), field by field. Pure, for the bench. */
void     ls_rffe_cfg_default(ls_rffe_cfg_t *c);
uint32_t ls_rffe_pack(const ls_rffe_cfg_t *c);
bool     ls_rffe_unpack(uint32_t word, ls_rffe_cfg_t *c);

/* RTL-SDR PPM by dongle: four 16-bit slots, newest first, each a key byte
   (1..255, 0 empty) and a signed ppm byte (-100..100). */
#define LS_RFFE_RTL_PPM_MAX 100
uint32_t ls_rffe_dongle_key(const char *manufact, const char *product, const char *serial);
uint64_t ls_rffe_ppm_put(uint64_t table, uint32_t key, int ppm);
bool     ls_rffe_ppm_get(uint64_t table, uint32_t key, int *ppm);

/* Console: `bias [rtl|hackrf] on|off|status|pin|unpin` and
   `rffe [rtl|hackrf] [ppm N|dagc follow|on|off|offset on|off|amp auto|on|off|
   lna auto|N|vga auto|N|gains]`. */
int ls_bias_command(int argc, char **argv);
int ls_rffe_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
