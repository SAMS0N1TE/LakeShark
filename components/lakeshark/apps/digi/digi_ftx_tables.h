/* FT8/FT4 protocol constants shared by the encoder and the decoder. */
#ifndef DIGI_FTX_TABLES_H
#define DIGI_FTX_TABLES_H
#include <stdint.h>

#define DIGI_LDPC_N       174   /* codeword bits                    */
#define DIGI_LDPC_K       91    /* 77 message bits + 14 CRC bits     */
#define DIGI_LDPC_M       83    /* parity checks                     */
#define DIGI_LDPC_N_BYTES 22
#define DIGI_LDPC_K_BYTES 12

extern const uint8_t digi_ft8_costas[7];
extern const uint8_t digi_ft4_costas[4][4];
extern const uint8_t digi_ft8_gray[8];
extern const uint8_t digi_ft4_gray[4];
/* FT4 XORs the 77 message bits with this before the CRC. */
extern const uint8_t digi_ft4_xor[10];
/* Parity bit i is the parity of (row i AND the 91 message bits). */
extern const uint8_t digi_ldpc_gen[DIGI_LDPC_M][DIGI_LDPC_K_BYTES];
/* Check m covers codeword bits nm[m][0..nrw[m]-1], 1-origin. */
extern const uint8_t digi_ldpc_nm[DIGI_LDPC_M][7];
/* Bit n sits in checks mn[n][0..2], 1-origin. */
extern const uint8_t digi_ldpc_mn[DIGI_LDPC_N][3];
extern const uint8_t digi_ldpc_nrw[DIGI_LDPC_M];

#endif
