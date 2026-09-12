/* Production HDU parsing and IMBE gate; controlled FEC result, no RF/audio claim. */
#include "ls_test.h"
#include "dsd.h"
#include "p25p1_ldu.h"
#include <string.h>
static int failed, decoded, skipped;
static unsigned alg;
int getDibit(dsd_opts *o, dsd_state *s) { (void)o; (void)s; return 0; }
int get_dibit_and_analog_signal(dsd_opts *o, dsd_state *s, int *a)
{ (void)o; (void)s; *a=0; return 0; }
void skipDibit(dsd_opts *o, dsd_state *s, int n) { (void)o;(void)s;skipped+=n; }
int check_and_fix_golay_24_6(char *d,char *p,int *e)
{ (void)d;(void)p;*e=0;return 0; }
void encode_golay_24_6(char *d,char *p) { (void)d;memset(p,0,12); }
static void bits(char *d, int offset, unsigned value, int count)
{
    for (int i=0;i<count;++i) {
        int bit=offset+i;
        d[(19-bit/6)*6+bit%6]=(value>>(count-1-i))&1;
    }
}
int check_and_fix_redsolomon_36_20_17(char *d,char *p)
{
    (void)p; memset(d,0,120);
    for(int i=0;i<9;++i) bits(d,i*8,0xa0+i,8);
    bits(d,80,alg,8); bits(d,88,0x1234,16); bits(d,104,42,16);
    return failed;
}
void encode_reedsolomon_36_20_17(char *d,char *p) { (void)d;memset(p,0,96); }
int check_and_fix_hamming_10_6_3(char *d,char *p) { (void)d;(void)p;return 0; }
void encode_hamming_10_6_3(char *d,char *p) { (void)d;memset(p,0,4); }
void debug_print_heuristics(P25Heuristics *h) { (void)h; }
void contribute_to_heuristics(int r,P25Heuristics *h,AnalogSignal *a,int n)
{ (void)r;(void)h;(void)a;(void)n; }
void update_error_stats(P25Heuristics *h,int b,int e) { (void)h;(void)b;(void)e; }
void processMbeFrame(dsd_opts *o,dsd_state *s,char a[8][23],char b[4][24],char c[7][24])
{ (void)o;(void)s;(void)a;(void)b;(void)c;++decoded; }
LS_CASE(valid_hdu_controls_first_ldu1_imbe_gate_without_ldu2)
{
    for(int encrypted=0;encrypted<2;++encrypted) {
        dsd_opts o={0}; dsd_state s={0};
        alg=encrypted?0x84:0x80;failed=0;decoded=skipped=0;
        processHDU(&o,&s);
        LS_EQ_INT(s.p25_ess_valid,1);LS_EQ_INT(s.p25_algid,alg);
        LS_EQ_INT(s.p25_kid,0x1234);LS_EQ_INT(s.lasttg,42);
        for(int i=0;i<9;++i) LS_EQ_INT(s.p25_mi[i],0xa0+i);
        int status=21;
        for(int i=0;i<9;++i) process_IMBE(&o,&s,&status);
        LS_EQ_INT(decoded,encrypted?0:9);
        LS_EQ_INT(s.p25_enc_muted_frames,encrypted?9:0);
        LS_EQ_INT(skipped,5);
    }
}
LS_CASE(corrupt_hdu_cannot_publish_clear_or_retain_prior_permission)
{
    dsd_opts o={0}; dsd_state s={0};
    s.p25_ess_valid=1;s.p25_algid=0x80;s.lasttg=7;s.pcm_out_write=160;
    alg=0x80;failed=1;decoded=skipped=0;processHDU(&o,&s);
    LS_EQ_INT(s.p25_ess_valid,0);LS_EQ_INT(s.pcm_out_write,0);
    LS_EQ_INT(s.lasttg,7);LS_EQ_INT(skipped,5);
    int status=21;process_IMBE(&o,&s,&status);
    LS_EQ_INT(decoded,0);LS_EQ_INT(s.p25_enc_muted_frames,1);
}
