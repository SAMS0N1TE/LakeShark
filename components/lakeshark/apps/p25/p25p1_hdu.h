
#ifndef P25P1_HDU_H_9f95c3a5072842e8aaf94444e1452d20
#define P25P1_HDU_H_9f95c3a5072842e8aaf94444e1452d20

#include "p25p1_heuristics.h"

int read_dibit (dsd_opts* opts, dsd_state* state, char* output, int* status_count, int* analog_signal,
        int* did_read_status);

void read_dibit_update_analog_data (dsd_opts* opts, dsd_state* state, char* buffer, unsigned int count,
        int* status_count, AnalogSignal* analog_signal_array, int* analog_signal_index);

void read_word (dsd_opts* opts, dsd_state* state, char* word, unsigned int length, int* status_count,
        AnalogSignal* analog_signal_array, int* analog_signal_index);

void read_hamm_parity (dsd_opts* opts, dsd_state* state, char* parity, int* status_count,
        AnalogSignal* analog_signal_array, int* analog_signal_index);

void read_golay24_parity (dsd_opts* opts, dsd_state* state, char* parity, int* status_count, AnalogSignal* analog_signal_array, int* analog_signal_index);

void read_zeros(dsd_opts* opts, dsd_state* state, AnalogSignal* analog_signal_array, unsigned int length,
        int* status_count, int new_sequence);

#endif
