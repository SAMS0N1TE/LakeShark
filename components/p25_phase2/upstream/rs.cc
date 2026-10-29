
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <assert.h>
#include <op25_imbe_frame.h>

#ifdef DEBUG
/*
 * Convert bit vector to hex dump format and print
 */
static inline void
dump_cw(const_bit_vector cw, int len, FILE* fp)  // len in bytes
{
        int i, j;
        for (i = 0; i < len; i++){
                int p = 0;
                for (j = 0; j <  8; j++){
                        p = (p << 1) + cw[ i*8 + j ];
                }
                fprintf(fp, "%02x ", p);
                if (!((i+1) % 16))
                        fprintf(fp, "\n");
        }
        fprintf(fp, "\n");
}
#endif // DEBUG

/* LakeShark: the Golay (23,12) syndrome table was a 2048-entry constant (8 KB
   of flash). Entry s belongs to the one error pattern e of weight w <= 3 with
   gly23127GetSyn(e) == s (the code is perfect, so the 2048 such patterns hit
   each syndrome once): e's twelve data bits in place, and w in the low bits,
   which the decoders shift out. Built before app_main, into PSRAM, by
   ls_p2_gly23127_build below; bench test_golay_tables checks it against the
   old table. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
EXT_RAM_BSS_ATTR uint32_t ls_p2_gly23127DecTbl[2048];

uint32_t gly23127GetSyn (uint32_t pattern) {
	uint32_t aux = 0x400000;

	while(pattern & 0xFFFFF800) {
		while ((aux & pattern) == 0) {
			aux = aux >> 1;
		}
		pattern = pattern ^ (aux / 0x800 * 0xC75) ;//generator is C75
	}
	return pattern;
}

static void ls_p2_gly23127_put(uint32_t e, uint32_t w) {
	ls_p2_gly23127DecTbl[gly23127GetSyn(e)] = (e & ~0x7FFu) | w;
}

__attribute__((constructor)) static void ls_p2_gly23127_build(void) {
	ls_p2_gly23127_put(0, 0);
	for (int i = 0; i < 23; i++) {
		ls_p2_gly23127_put(1u << i, 1);
		for (int j = i + 1; j < 23; j++) {
			ls_p2_gly23127_put(1u << i | 1u << j, 2);
			for (int k = j + 1; k < 23; k++)
				ls_p2_gly23127_put(1u << i | 1u << j | 1u << k, 3);
		}
	}
}

uint32_t ls_p2_gly24128Dec (uint32_t n, size_t* errs) { //based on ls_p2_gly23127Dec
	uint32_t CW = n >> 1 ; //toss the parity bit
	uint32_t correction = ls_p2_gly23127DecTbl[gly23127GetSyn(CW)];
	CW = (CW ^ correction) >> 11;

	if (errs != NULL)
		*errs = __builtin_popcount(correction);

	return CW;
}
uint32_t ls_p2_gly23127Dec (uint32_t CW, size_t* errs) {
	uint32_t correction = ls_p2_gly23127DecTbl[gly23127GetSyn(CW)];
	CW = (CW ^ correction) >> 11;

	if (errs != NULL)
		*errs = __builtin_popcount(correction);

	return CW;
}

