
#ifndef P25P1_CHECK_LDU_H_c1734445a67e47caa25673d7a4ce7520
#define P25P1_CHECK_LDU_H_c1734445a67e47caa25673d7a4ce7520

#ifdef __cplusplus
extern "C" {
#endif

int check_and_fix_hamming_10_6_3(char* hex, char* parity);

void encode_hamming_10_6_3(char* hex, char* out_parity);

int check_and_fix_reedsolomon_24_12_13(char* data, char* parity);

void encode_reedsolomon_24_12_13(char* hex_data, char* fixed_parity);

int check_and_fix_reedsolomon_24_16_9(char* data, char* parity);

void encode_reedsolomon_24_16_9(char* hex_data, char* fixed_parity);

#ifdef __cplusplus
}
#endif

#endif
