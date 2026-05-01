#include "mode-s.h"
#include <stdio.h>

void on_msg(mode_s_t *self, struct mode_s_msg *mm)
{
    int msgLength = mm->msgbits / 8;
    for (int i = 0; i < msgLength; i++)
        fprintf(stdout, "%02X", mm->msg[i]);
}

void demodulate(uint8_t *source, int length)
{
    mode_s_t state;
    uint16_t *mag = malloc((length / 2) * sizeof(uint16_t));
    mode_s_init(&state);
    mode_s_compute_magnitude_vector(source, mag, length);
    mode_s_detect(&state, mag, length / 2, on_msg);
    free(mag);
}