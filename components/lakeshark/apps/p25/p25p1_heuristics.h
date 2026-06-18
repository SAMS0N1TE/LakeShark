
#ifndef P25P1_HEURISTICS_H_030dd3530b7546abbb56f8dd1e66a2f6
#define P25P1_HEURISTICS_H_030dd3530b7546abbb56f8dd1e66a2f6

#define HEURISTICS_SIZE 200
typedef struct
{
  int values[HEURISTICS_SIZE];
  float means[HEURISTICS_SIZE];
  int index;
  int count;
  float sum;
  float var_sum;
} SymbolHeuristics;

typedef struct
{
  unsigned int bit_count;
  unsigned int bit_error_count;
  SymbolHeuristics symbols[4][4];
} P25Heuristics;

typedef struct
{
    int value;
    int dibit;
    int corrected_dibit;
    int sequence_broken;
} AnalogSignal;

#ifdef __cplusplus
extern "C"{
#endif

void initialize_p25_heuristics(P25Heuristics* heuristics);

int estimate_symbol(int rf_mod, P25Heuristics* heuristics, int previous_dibit, int analog_value, int* dibit);

void debug_print_heuristics(P25Heuristics* heuristics);

void contribute_to_heuristics(int rf_mod, P25Heuristics* heuristics, AnalogSignal* analog_signal_array, int count);

void update_error_stats(P25Heuristics* heuristics, int bits, int errors);

float get_P25_BER_estimate(P25Heuristics* heuristics);

#ifdef __cplusplus
}
#endif

#endif
