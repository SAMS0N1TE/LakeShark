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
/* A damaged ESS in a call a valid ESS already proved clear keeps the call
   clear and keeps the nine frames just decoded. Measured on 154.7850,
   clearing here cost a third of one call's voice: each failure dropped its
   own 180 ms and muted the LDU1 after it. The corrupt word is still never
   published - the ESS stays what the good one said. */
LS_CASE(failed_ess_in_a_proven_clear_call_keeps_the_call_and_its_audio)
{
    dsd_state s; dsd_opts o; memset(&o,0,sizeof(o));
    memset(&s,0,sizeof(s)); s.p25_ess_valid=1; s.p25_algid=0x80;
    failed=1; alg=0x84; voices=0; processLDU2(&o,&s);
    LS_EQ_INT(voices,9); LS_EQ_INT(s.debug_header_critical_errors,1);
    LS_EQ_INT(s.p25_ess_valid,1);
    LS_EQ_INT(s.p25_algid,0x80);           /* not the corrupt word's 0x84 */
    LS_EQ_INT(s.pcm_out_write,9*160);
    LS_EQ_INT(s.p25_ess_rs_failed,1); LS_EQ_INT(s.p25_ess_rs_kept,1);
    LS_CHECK(!p25_ldu_should_mute_encrypted(&s,&o));
}

/* Unknown, or known encrypted: a damaged ESS proves nothing, so it still
   clears, drops the frame's audio and mutes. Nothing encrypted is let through
   on the strength of a word that failed its check. */
LS_CASE(failed_ess_without_a_proven_clear_call_still_mutes_and_drops)
{
    dsd_state s; dsd_opts o; memset(&o,0,sizeof(o));
    const struct { int valid; unsigned algid; } before[] = {
        {0, 0x00}, {0, 0x80}, {1, 0x84}, {1, 0x81},
    };
    for (unsigned i=0; i<sizeof(before)/sizeof(before[0]); ++i) {
        memset(&s,0,sizeof(s));
        s.p25_ess_valid=before[i].valid; s.p25_algid=(uint8_t)before[i].algid;
        failed=1; alg=0x80; voices=0; processLDU2(&o,&s);
        LS_EQ_INT(voices,9);
        LS_EQ_INT(s.p25_ess_valid,0);
        LS_EQ_INT(s.pcm_out_write,0);
        LS_EQ_INT(s.p25_ess_rs_kept,0);
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
