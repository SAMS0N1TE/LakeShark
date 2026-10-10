/* The receiver uses these same bounded collection and lease handoff paths. */
#include "ls_test.h"
#include "fm_receive.h"
#include "fm_mode_handoff.h"

static int64_t s_now;
static unsigned s_reads, s_releases;
static size_t s_part;
static ls_radio_err_t s_error;
static bool s_active;
static fm_mode_handoff_t s_handoff;

static int64_t now(void) { return s_now; }
static bool active(void) { return s_active; }
ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *session, void *dst,
                               size_t bytes, uint32_t timeout, size_t *read)
{
    (void)session; (void)dst;
    LS_EQ_UINT(timeout, 20);
    ++s_reads; s_now += 20000;
    *read = s_part < bytes ? s_part : bytes;
    fm_mode_handoff_request(&s_handoff, FM_MODE_FLEX);
    return s_error;
}
void ls_radio_release(ls_radio_session_t *session)
{ LS_CHECK(session != NULL); ++s_releases; }

LS_CASE(fm_timeout_and_partial_reads_return_to_pending_controls)
{
    uint8_t iq[128];
    for (int partial = 0; partial < 3; ++partial) {
        s_now = 0; s_reads = 0; s_active = true;
        s_part = partial == 2 ? 0 : 1;
        s_error = partial ? LS_RADIO_OK : LS_RADIO_ERR_TIMEOUT;
        s_handoff = (fm_mode_handoff_t)FM_MODE_HANDOFF_INITIALIZER;
        LS_EQ_INT(fm_receive_block((ls_radio_session_t *)iq, iq, sizeof(iq), active, now), LS_RADIO_ERR_TIMEOUT);
        LS_CHECK(s_reads <= 5);
        fm_mode_t mode;
        LS_CHECK(fm_mode_handoff_take(&s_handoff, &mode));
        LS_EQ_INT(mode, FM_MODE_FLEX);
        /* The next iteration can collect a fresh block after recovery. */
        s_part = sizeof(iq); s_error = LS_RADIO_OK;
        LS_EQ_INT(fm_receive_block((ls_radio_session_t *)iq, iq, sizeof(iq), active, now), LS_RADIO_OK);
    }
    s_active = false;
    LS_EQ_INT(fm_receive_block((ls_radio_session_t *)iq, iq, sizeof(iq), active, now), LS_RADIO_ERR_STOPPED);
}

LS_CASE(fm_immediate_io_error_is_returned_for_backoff)
{
    uint8_t iq[128];
    s_active = true; s_now = 0; s_reads = 0; s_error = LS_RADIO_ERR_IO;
    LS_EQ_INT(fm_receive_block((ls_radio_session_t *)iq, iq, sizeof(iq), active, now), LS_RADIO_ERR_IO);
    LS_EQ_UINT(s_reads, 1);
}

LS_CASE(fm_mode_endpoint_handoff_releases_both_directions_without_stealing)
{
    char token;
    ls_radio_session_t *session = (ls_radio_session_t *)&token;
    s_releases = 0;
    LS_CHECK(!fm_receive_switch(&session, "rtl_usb", "rtl_usb"));
    LS_EQ_UINT(s_releases, 0);
    LS_CHECK(fm_receive_switch(&session, "rtl_usb", "hackrf_usb"));
    LS_CHECK(session == NULL); LS_EQ_UINT(s_releases, 1);
    /* A busy target leaves no session to tune or read; acquisition remains
       the existing owner's job and is retried by the receiver loop. */
    LS_CHECK(!fm_receive_switch(&session, "rtl_usb", "hackrf_usb"));
    session = (ls_radio_session_t *)&token;
    LS_CHECK(fm_receive_switch(&session, "hackrf_usb", "rtl_usb"));
    LS_CHECK(session == NULL); LS_EQ_UINT(s_releases, 2);
}
