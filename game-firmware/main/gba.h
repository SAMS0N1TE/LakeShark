#pragma once
#include "tk21.h"
#include <stdbool.h>
#include <stdint.h>

bool gba_open(bool from_sd);
void gba_close(void);
void gba_discard_frame(void);
void gba_frame(uint16_t *frame, tk21_state input);
void gba_log(void);
