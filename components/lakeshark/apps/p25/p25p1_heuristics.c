
#define _USE_MATH_DEFINES
#include <math.h>

#include "dsd.h"

#include "p25p1_heuristics.h"

#define USE_PREVIOUS_DIBIT

#define MIN_ELEMENTS_FOR_HEURISTICS 10

static int use_previous_dibit(int rf_mod)
{

    return (rf_mod == 0)? 1 : 0;
}

static void update_p25_heuristics(P25Heuristics* heuristics, int previous_dibit, int original_dibit, int dibit, int analog_value)
{
    float mean;
    int old_value;
    float old_mean;

    SymbolHeuristics* sh;
    int number_errors;

#ifndef USE_PREVIOUS_DIBIT
    previous_dibit = 0;
#endif

    sh = &(heuristics->symbols[previous_dibit][dibit]);

    old_value = sh->values[sh->index];
    old_mean = sh->means[sh->index];

    number_errors = 0;
    if (original_dibit != dibit) {
        if ((original_dibit == 0 && dibit == 3) || (original_dibit == 3 && dibit == 0) ||
            (original_dibit == 1 && dibit == 2) || (original_dibit == 2 && dibit == 1)) {

            number_errors = 2;
        } else {

            number_errors = 1;
        }
    }
    update_error_stats(heuristics, 2, number_errors);

    if (sh->count >= HEURISTICS_SIZE) {
        sh->sum -= old_value;
        sh->var_sum -= (((float)old_value) - old_mean) * (((float)old_value) - old_mean);
    }
    sh->sum += analog_value;

    sh->values[sh->index] = analog_value;
    if (sh->count < HEURISTICS_SIZE) {
        sh->count++;
    }
    mean = sh->sum / ((float)sh->count);
    sh->means[sh->index] = mean;
    if (sh->index >= (HEURISTICS_SIZE-1)) {
        sh->index = 0;
    } else {
        sh->index++;
    }

    sh->var_sum += (((float)analog_value) - mean) * (((float)analog_value) - mean);
}

void contribute_to_heuristics(int rf_mod, P25Heuristics* heuristics, AnalogSignal* analog_signal_array, int count)
{
    int i;
    int use_prev_dibit;

#ifdef USE_PREVIOUS_DIBIT
    use_prev_dibit = use_previous_dibit(rf_mod);
#else
    use_prev_dibit = 0;
#endif

    for (i=0; i<count; i++) {
        int use;
        int prev_dibit;

        if (use_prev_dibit) {
            if (analog_signal_array[i].sequence_broken) {

                use = 0;
            } else {
                use = 1;

                prev_dibit = analog_signal_array[i-1].corrected_dibit;
            }
        } else {
            use = 1;
            prev_dibit = 0;
        }

        if (use) {
            update_p25_heuristics(heuristics, prev_dibit, analog_signal_array[i].dibit,
                    analog_signal_array[i].corrected_dibit, analog_signal_array[i].value);
        }
    }
}

static void initialize_symbol_heuristics(SymbolHeuristics* sh)
{
    sh->count = 0;
    sh->index = 0;
    sh->sum = 0;
    sh->var_sum = 0;
}

void initialize_p25_heuristics(P25Heuristics* heuristics)
{
    int i, j;
    for (i=0; i<4; i++) {
        for (j=0; j<4; j++) {
            initialize_symbol_heuristics(&(heuristics->symbols[i][j]));
        }
    }
    heuristics->bit_count = 0;
    heuristics->bit_error_count = 0;
}

static float evaluate_pdf(SymbolHeuristics* se, int value)
{
    float x = (se->count*((float)value) - se->sum);
    float y = -0.5F*x*x/(se->count*se->var_sum);
    float pdf = sqrtf(se->count / se->var_sum) * expf(y) / sqrtf(2.0F * ((float) M_PI));

    return pdf;
}

static void debug_log_pdf(P25Heuristics* heuristics, int previous_dibit, int analog_value)
{
    int i;
    float pdfs[4];

    for (i=0; i<4; i++) {
        pdfs[i] = evaluate_pdf(&(heuristics->symbols[previous_dibit][i]), analog_value);
    }

    printf("v: %i, (%e, %e, %e, %e)\n", analog_value, pdfs[0], pdfs[1], pdfs[2], pdfs[3]);
}

int estimate_symbol(int rf_mod, P25Heuristics* heuristics, int previous_dibit, int analog_value, int* dibit)
{
    int valid;
    int i;
    float pdfs[4];

#ifdef USE_PREVIOUS_DIBIT
    int use_prev_dibit = use_previous_dibit(rf_mod);

    if (use_prev_dibit == 0)
      {

        previous_dibit = 0;
      }
#else

#endif

    valid = 1;

    for (i=0; i<4; i++) {
        if (heuristics->symbols[previous_dibit][i].count >= MIN_ELEMENTS_FOR_HEURISTICS) {
            pdfs[i] = evaluate_pdf(&(heuristics->symbols[previous_dibit][i]), analog_value);
        } else {

            valid = 0;
            break;
        }
    }

    if (valid) {

        int max_index;
        float max;

        max_index = 0;
        max = pdfs[0];
        for (i=1; i<4; i++) {
            if (pdfs[i] > max) {
                max_index = i;
                max = pdfs[i];
            }
        }

        *dibit = max_index;
    }

#ifdef DISABLE_HEURISTICS
    valid = 0;
#endif

    return valid;
}

static void debug_print_symbol_heuristics(int previous_dibit, int dibit, SymbolHeuristics* sh)
{
    float mean, sd;
    int k;
    int n;

    n = sh->count;
    if (n == 0)
      {
        mean = 0;
        sd = 0;
      }
    else
      {
        mean = sh->sum/n;
        sd = sqrtf(sh->var_sum / ((float) n));
      }
    printf("%i%i: count: %2i mean: % 10.2f sd: % 10.2f", previous_dibit, dibit, sh->count, mean, sd);

    printf("\n");

}

void debug_print_heuristics(P25Heuristics* heuristics)
{
  int i,j;

  printf("\n");

  for(i=0; i<4; i++)
    {
      for(j=0; j<4; j++)
        {
          debug_print_symbol_heuristics(i, j, &(heuristics->symbols[i][j]));
        }
    }
}

void update_error_stats(P25Heuristics* heuristics, int bits, int errors)
{
    heuristics->bit_count += bits;
    heuristics->bit_error_count += errors;

    if ((heuristics->bit_count & 1) == 0 && (heuristics->bit_error_count & 1) == 0) {

        heuristics->bit_count >>= 1;
        heuristics->bit_error_count >>= 1;
    }
}

float get_P25_BER_estimate(P25Heuristics* heuristics)
{
    float ber;
    if (heuristics->bit_count == 0) {
        ber = 0.0F;
    } else {
        ber = ((float)heuristics->bit_error_count) * 100.0F / ((float)heuristics->bit_count);
    }
    return ber;
}
