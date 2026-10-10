#ifndef FM_RECEIVE_H
#define FM_RECEIVE_H

#include <string.h>
#include "radio_endpoint.h"

/* Return to controls even if the driver times out or makes tiny progress. */
static inline ls_radio_err_t fm_receive_block(ls_radio_session_t *session,
    uint8_t *iq, size_t bytes, bool (*active)(void), int64_t (*now)(void))
{
    const int64_t deadline = now() + 100000;
    size_t got = 0;
    for (unsigned reads = 0; reads < 64 && now() < deadline; ++reads) {
        if (!active()) return LS_RADIO_ERR_STOPPED;
        size_t part = 0;
        ls_radio_err_t error = ls_radio_iq_read(session, iq + got,
                                               bytes - got, 20, &part);
        if (error != LS_RADIO_OK && error != LS_RADIO_ERR_TIMEOUT) return error;
        if (error == LS_RADIO_OK) {
            if (part > bytes - got) return LS_RADIO_ERR_IO;
            got += part;
            if (got == bytes) return LS_RADIO_OK;
        }
    }
    return LS_RADIO_ERR_TIMEOUT;
}

static inline bool fm_receive_switch(ls_radio_session_t **session,
                                     const char *held, const char *wanted)
{
    if (!*session || !wanted || (held && !strcmp(held, wanted))) return false;
    ls_radio_release(*session);
    *session = NULL;
    return true;
}

#endif
