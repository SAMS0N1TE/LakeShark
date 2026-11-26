#pragma once

#include <stdint.h>

typedef enum { ART_RELAY, ART_SALVAGE, ART_SIGNAL, ART_CARTRIDGE, ART_BSP, ART_TK21 } menu_art_kind;

void menu_art_draw(uint16_t *frame, int left, menu_art_kind kind, const uint16_t *palette,
                   unsigned theme, int64_t now, uint8_t held);
