/* The shared LR session's calls, for host builds that compile an adapter
   with ESP_PLATFORM and leave WIDE I/Q off: its narrow path is what they
   test. test_pager_iq_wide fakes the session for real. */
#include "lr_wf_session.h"
#include <string.h>

bool lr_wf_wide_on(void) { return false; }
void lr_wf_wide_get(lr_wf_wide_status *out) { memset(out, 0, sizeof(*out)); }
esp_err_t lr_wf_start_consumer(uint32_t hz, lr_wf_consumer_t consume, void *ctx)
{
    (void)hz; (void)consume; (void)ctx; return ESP_ERR_NOT_SUPPORTED;
}
void lr_wf_get_stats(lr_wf_stats *out) { memset(out, 0, sizeof(*out)); }
void lr_wf_stop_stats(lr_wf_stats *out) { if (out) memset(out, 0, sizeof(*out)); }
