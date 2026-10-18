/* The LoRa chip answer for UI tests that link no radio: an SX1262 board.
   A plain definition: MinGW's PE linker does not resolve a weak one here. */
#include "ls_lora.h"
ls_lora_chip_t ls_lora_chip(void) { return LS_LORA_CHIP_SX126X; }
