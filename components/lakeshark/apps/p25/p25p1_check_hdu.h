
#ifndef P25P1_CHECK_HDU_H_f5f079faf2d64cf5831e1da1ab83b9ba
#define P25P1_CHECK_HDU_H_f5f079faf2d64cf5831e1da1ab83b9ba

#ifdef __cplusplus
extern "C" {
#endif

int check_and_fix_golay_24_6(char* hex, char* parity, int* fixed_errors);

int check_and_fix_golay_24_12(char* dodeca, char* parity, int* fixed_errors);

void encode_golay_24_6(char* hex, char* out_parity);

void encode_golay_24_12(char* dodeca, char* out_parity);

int check_and_fix_redsolomon_36_20_17(char* data, char* parity);

void encode_reedsolomon_36_20_17(char* hex_data, char* fixed_parity);

#ifdef __cplusplus
}
#endif

#endif
