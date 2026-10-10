#include "ls_test.h"
/* Exercise the numpad callback with the production screen's narrowing path. */
#include "../../components/apps/tui/screens/ext/scr_labs.c"

static unsigned fsk_requests, lora_requests;
static ls_fsk_cfg_t accepted_fsk;
static uint32_t radio_caps;
uint32_t ls_lora_caps(void) { return radio_caps; }
bool ls_field_configure_fsk(const ls_fsk_cfg_t *cfg)
{
    fsk_requests++;
    accepted_fsk = *cfg;
    return true;
}
bool ls_field_configure(const ls_lora_cfg_t *cfg)
{
    (void)cfg; lora_requests++; return true;
}

LS_CASE(payload_and_integer_fields_cannot_wrap_or_round_before_validation)
{
    fsk_requests = 0;
    s_fsk_setting = 5;
    set_fsk_number(257);
    set_fsk_number(256);
    set_fsk_number(-1);
    set_fsk_number(1.5);
    LS_EQ_UINT(fsk_requests, 0);
    LS_CHECK(strstr(s_feedback, "Not accepted") != NULL);
    set_fsk_number(255);
    LS_EQ_UINT(fsk_requests, 1); LS_EQ_UINT(accepted_fsk.payload_bytes, 255);
    for (int setting = 1; setting <= 4; setting++) {
        s_fsk_setting = setting;
        set_fsk_number(4294968496.0);
        set_fsk_number(-1);
        set_fsk_number(1200.5);
        set_fsk_number(NAN);
        set_fsk_number(INFINITY);
    }
    LS_EQ_UINT(fsk_requests, 1);
    s_fsk_setting = 4;
    set_fsk_number(UINT32_MAX);
    LS_EQ_UINT(fsk_requests, 2); LS_EQ_UINT(accepted_fsk.sync_word, UINT32_MAX);
}

LS_CASE(fsk_frequency_keeps_fractional_mhz_entry)
{
    lora_requests = 0; s_fsk_setting = 0;
    set_fsk_number(152.6);
    LS_EQ_UINT(lora_requests, 1);
    set_fsk_number(1e100);
    LS_EQ_UINT(lora_requests, 1);
}

LS_CASE(hf_frequency_entry_is_lr2021_receive_only)
{
    lora_requests = 0; s_setting = 0; radio_caps = LS_LORA_CAP_BAND_1G5_2G5;
    set_number(2400); set_number(2500); set_number(1900); set_number(2200);
    LS_EQ_UINT(lora_requests, 4);
    set_number(2300); set_number(1899); set_number(2500.1); set_number(1e100);
    LS_EQ_UINT(lora_requests, 4);
    radio_caps = 0;
    set_number(2400); set_number(959.1);
    LS_EQ_UINT(lora_requests, 4);
    set_number(959); LS_EQ_UINT(lora_requests, 5);
    radio_caps = LS_LORA_CAP_BAND_1G5_2G5; s_fsk_setting = 0;
    set_fsk_number(2440); LS_EQ_UINT(lora_requests, 6);
    radio_caps = 0;
}
