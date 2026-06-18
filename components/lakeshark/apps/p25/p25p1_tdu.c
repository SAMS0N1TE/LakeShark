
#include "dsd.h"

#include "p25p1_hdu.h"
#include "p25p1_heuristics.h"

void
processTDU (dsd_opts* opts, dsd_state* state)
{
    AnalogSignal analog_signal_array[14];
    int status_count;

    status_count = 21;

    read_zeros(opts, state, analog_signal_array, 28, &status_count, 1);

    if (status_count != 35) {
        printf("*** SYNC ERROR\n");
    }

    {
        int status;
        status = getDibit (opts, state) + '0';

    }
}
