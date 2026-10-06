
#ifndef P25P1_CHECK_NID_H_3af071e917ea43fdb51326e2cbfbde0a
#define P25P1_CHECK_NID_H_3af071e917ea43fdb51326e2cbfbde0a

#ifdef __cplusplus
extern "C" {
#endif

int check_NID(char* bch_code, int* new_nac, char* new_duid, unsigned char parity);

int check_NID_ec(char* bch_code, int* new_nac, char* new_duid, unsigned char parity,
                 int* errors_out);

/* How many of the 63 received BCH bits differ from the NID codeword for this
 * NAC and DUID (0 to 15). */
int nid_distance(const char* bch_code, int nac, int duid);

#ifdef __cplusplus
}
#endif

#endif
