#include "ls_test.h"
#include "dsd.h"
#include "p25p1_ldu.h"
#include "p25p1_check_ldu.h"
#include <string.h>
static int failed, voices;
static unsigned alg;
void process_IMBE(dsd_opts *o, dsd_state *s, int *count)
{ (void)o; (void)count; voices++; s->pcm_out_write += 160; }
int getDibit(dsd_opts *o, dsd_state *s) { (void)o; (void)s; return 0; }
void read_and_correct_hex_word(dsd_opts *o, dsd_state *s, char *hex,
    int *count, AnalogSignal *a, int *idx)
{ (void)o; (void)s; (void)count; (void)a; (void)idx; memset(hex, 0, 6); }
int read_dibit(dsd_opts *o, dsd_state *s, char *out, int *count, int *a, int *b)
{ (void)o; (void)s; (void)count; (void)a; (void)b; out[0]=out[1]=0; return 0; }
int check_and_fix_reedsolomon_24_16_9(char *data, char *parity)
{
    (void)parity;
    for (int i=0; i<6; ++i) data[3*6+i]=(alg>>(7-i))&1;
    data[2*6]=(alg>>1)&1; data[2*6+1]=alg&1;
    return failed;
}
void encode_reedsolomon_24_16_9(char *d, char *p) { (void)d; memset(p,0,48); }
void correct_hamming_dibits(char *d, int n, AnalogSignal *a) { (void)d;(void)n;(void)a; }
void contribute_to_heuristics(int r, P25Heuristics *h, AnalogSignal *a, int n)
{ (void)r;(void)h;(void)a;(void)n; }
void update_error_stats(P25Heuristics *h, int b, int e) { (void)h;(void)b;(void)e; }
LS_CASE(failed_ess_must_not_publish_corrupt_clear_or_keep_old_clear)
{
    dsd_state s; dsd_opts o; memset(&o,0,sizeof(o));
    for (unsigned old=0; old<2; ++old) {
        memset(&s,0,sizeof(s)); s.p25_ess_valid=old; s.p25_algid=0x80;
        failed=1; alg=0x80; voices=0; processLDU2(&o,&s);
        LS_EQ_INT(voices,9); LS_EQ_INT(s.debug_header_critical_errors,1);
        LS_EQ_INT(s.p25_ess_valid,0);
        LS_EQ_INT(s.pcm_out_write,0);
        LS_CHECK(p25_ldu_should_mute_encrypted(&s,&o));
    }
}
LS_CASE(successful_ess_still_distinguishes_clear_and_encrypted)
{
    dsd_state s; dsd_opts o; memset(&o,0,sizeof(o)); failed=0;
    for (unsigned i=0; i<2; ++i) {
        memset(&s,0,sizeof(s)); alg=i ? 0x84 : 0x80;
        processLDU2(&o,&s); LS_EQ_INT(s.p25_ess_valid,1);
        LS_EQ_INT(s.p25_algid,alg); LS_EQ_INT(p25_ldu_should_mute_encrypted(&s,&o),i);
    }
}
