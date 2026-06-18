
#ifndef P25P1_CHECK_NID_H_3af071e917ea43fdb51326e2cbfbde0a
#define P25P1_CHECK_NID_H_3af071e917ea43fdb51326e2cbfbde0a

#ifdef __cplusplus
extern "C" {
#endif

int check_NID(char* bch_code, int* new_nac, char* new_duid, unsigned char parity);

int check_NID_ec(char* bch_code, int* new_nac, char* new_duid, unsigned char parity,
                 int* errors_out);

#ifdef __cplusplus
}
#endif

#endif
