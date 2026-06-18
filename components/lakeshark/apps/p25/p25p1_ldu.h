
#ifndef P25P1_LDU_H_a3c417fcb7804991b0e6d828066bd133
#define P25P1_LDU_H_a3c417fcb7804991b0e6d828066bd133

#include "dsd.h"

#include "p25p1_hdu.h"
#include "p25p1_const.h"
#include "p25p1_heuristics.h"

void process_IMBE (dsd_opts* opts, dsd_state* state, int* status_count);

void read_and_correct_hex_word (dsd_opts* opts, dsd_state* state, char* hex, int* status_count,
        AnalogSignal* analog_signal_array, int* analog_signal_index);

void correct_hamming_dibits(char* hex_data, int hex_count, AnalogSignal* analog_signal_array);

void debug_ldu_header(dsd_state* state);

#endif
