/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original implementation of 3GPP TS 36.211 6.11 PSS/SSS generation.
 * Deliberately conservative: 3 individually matched SSS observations with
 * 5 ms timing and alternating half-frame identities. Never a CSS verdict.
 */
#include "lte_sync.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#define N 512
#define STEP (N-127)
#define PEAKS 64
#ifndef LTE_SYNC_SEARCH_SAMPLES
#define LTE_SYNC_SEARCH_SAMPLES 10240
#endif
#define TAU 6.2831853071795864769f
typedef struct {float r,i;} cx;
typedef struct {float score;int pos,nid2,fo;} peak_t;
typedef struct {
    cx tw[N/2],a[N],b[N],ref[N],pss[3][62],td[3][128];
    cx p[128],s[128],h[62],sm[62],eq[62];
    float energy[STEP];
    int8_t sss[3][336][62];
    peak_t peaks[PEAKS],seeds[PEAKS];int count;
    int counts[504],pairs[504],last[504],half_last[504];
    int frame_start;
} workspace_t;
static cx mul(cx a,cx b){return (cx){a.r*b.r-a.i*b.i,a.r*b.i+a.i*b.r};}
static cx conjx(cx a){return (cx){a.r,-a.i};}
static float power(cx a){return a.r*a.r+a.i*a.i;}
static cx cis(float a){return (cx){cosf(a),sinf(a)};}
static void fft(cx *a,int n,bool inverse,const cx *tw)
{
    for(int i=1,j=0;i<n;i++) {
        int bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;
        if(i<j){cx t=a[i];a[i]=a[j];a[j]=t;}
    }
    for(int len=2;len<=n;len<<=1)for(int off=0;off<n;off+=len)for(int k=0;k<len/2;k++) {
        cx w=tw[k*(N/len)];if(inverse)w.i=-w.i;
        cx b=mul(a[off+k+len/2],w),v=a[off+k];
        a[off+k]=(cx){v.r+b.r,v.i+b.i};a[off+k+len/2]=(cx){v.r-b.r,v.i-b.i};
    }
    if(inverse)for(int i=0;i<n;i++){a[i].r/=n;a[i].i/=n;}
}
static void sequences(workspace_t *w)
{
    for(int i=0;i<N/2;i++)w->tw[i]=cis(-TAU*i/N);
    const int roots[3]={25,29,34};
    for(int j=0;j<3;j++) {
        memset(w->td[j],0,sizeof(w->td[j]));
        for(int k=0;k<62;k++) {
            int m=k<31?k*(k+1):(k+1)*(k+2);
            w->pss[j][k]=cis(-TAU*.5f*roots[j]*m/63);
            int bin=k<31?97+k:k-30;w->td[j][bin]=w->pss[j][k];
        }
        fft(w->td[j],128,true,w->tw);
    }
    int s[31]={0,0,0,0,1},c[31]={0,0,0,0,1},z[31]={0,0,0,0,1};
    for(int n=0;n<26;n++) {
        s[n+5]=(s[n]+s[n+2])&1;c[n+5]=(c[n]+c[n+3])&1;
        z[n+5]=(z[n]+z[n+1]+z[n+2]+z[n+4])&1;
    }
    for(int n=0;n<31;n++){s[n]=1-2*s[n];c[n]=1-2*c[n];z[n]=1-2*z[n];}
    for(int j=0;j<3;j++)for(int id=0;id<168;id++) {
        int qp=id/30,q=(id+qp*(qp+1)/2)/30,mp=id+q*(q+1)/2;
        int m0=mp%31,m1=(m0+mp/31+1)%31;
        for(int half=0;half<2;half++)for(int n=0;n<31;n++) {
            int a=half?m1:m0,b=half?m0:m1;
            w->sss[j][2*id+half][2*n]=s[(n+a)%31]*c[(n+j)%31];
            w->sss[j][2*id+half][2*n+1]=s[(n+b)%31]*c[(n+j+3)%31]*z[(n+a%8)%31];
        }
    }
}
static void peak(workspace_t *w,float score,int pos,int nid2,int fo)
{
    if(score<.12f || pos<140)return;
    int weakest=0;
    for(int i=0;i<w->count;i++) {
        int delta=w->peaks[i].pos-pos;
        if(w->peaks[i].nid2==nid2 && delta>-40 && delta<40) {
            if(w->peaks[i].score<score)w->peaks[i]=(peak_t){score,pos,nid2,fo};
            return;
        }
        if(w->peaks[i].score<w->peaks[weakest].score)weakest=i;
    }
    if(w->count<PEAKS)weakest=w->count++;
    else if(w->peaks[weakest].score>=score)return;
    w->peaks[weakest]=(peak_t){score,pos,nid2,fo};
}
static void extract(const uint8_t *iq,int pos,int fo,cx *out,const cx *tw)
{
    cx phase={1,0},step=cis(-TAU*fo/1920000);
    for(int n=0;n<128;n++) {
        cx x={(float)iq[2*(pos+n)]-127.5f,(float)iq[2*(pos+n)+1]-127.5f};
        out[n]=mul(x,phase);phase=mul(phase,step);
    }
    fft(out,128,false,tw);
}
size_t lte_sync_workspace_size(void){return sizeof(workspace_t);}
int lte_sync_frame_start(const void *memory)
{
    return memory?((const workspace_t *)memory)->frame_start:-1;
}
bool lte_sync_find(const uint8_t *iq,size_t samples,void *memory,
                   lte_sync_result_t *out,lte_sync_yield_fn yield,void *arg)
{
    if(memory)((workspace_t *)memory)->frame_start=-1;
    if(!out)return false;
    memset(out,0,sizeof(*out));out->pci=-1;
    if(!iq || !memory || samples<28800 || samples>153600)return false;
    workspace_t *w=memory;memset(w,0,sizeof(*w));w->frame_start=-1;sequences(w);
    /* Overlap-save matched filtering, 128-sample useful OFDM symbol.
     * Search +/-40 kHz oscillator error in 5 kHz steps around the carrier. */
    /* One full half-frame plus boundary margin locates timing candidates.
     * Check their repetitions against the remaining IQ, rather than doing
     * every expensive frequency search again for each identical occasion. */
    /* The PC build searches the full capture: USB recordings can contain
     * timing discontinuities before a later, usable continuous segment. */
    const size_t search_samples=samples<LTE_SYNC_SEARCH_SAMPLES?samples:LTE_SYNC_SEARCH_SAMPLES;
    for(int j=0;j<3;j++)for(int fo=-40000;fo<=40000;fo+=5000) {
        memset(w->ref,0,sizeof(w->ref));
        for(int n=0;n<128;n++)w->ref[127-n]=conjx(mul(w->td[j][n],cis(TAU*fo*n/1920000)));
        fft(w->ref,N,false,w->tw);
        for(size_t off=0;off+128<=search_samples;off+=STEP) {
            memset(w->a,0,sizeof(w->a));int available=(int)(search_samples-off);if(available>N)available=N;
            float energy=0;
            for(int n=0;n<available;n++) {
                w->a[n]=(cx){(float)iq[2*(off+n)]-127.5f,(float)iq[2*(off+n)+1]-127.5f};
                if(n<128)energy+=power(w->a[n]);
            }
            int count=available-127;
            for(int n=0;n<count;n++) {
                w->energy[n]=energy;
                if(n+128<available)energy+=power(w->a[n+128])-power(w->a[n]);
            }
            fft(w->a,N,false,w->tw);
            for(int n=0;n<N;n++)w->b[n]=mul(w->a[n],w->ref[n]);
            fft(w->b,N,true,w->tw);
            for(int n=0;n<count;n++)peak(w,power(w->b[n+127])/(w->energy[n]*(62.f/128)+1e-9f),(int)off+n,j,fo);
            if(yield && yield(arg))return false;
        }
    }
    int seed_count=w->count;
    memcpy(w->seeds,w->peaks,sizeof(w->seeds));w->count=0;
    for(int i=0;i<seed_count;i++) {
        peak_t p=w->seeds[i];
        for(int pos=p.pos%9600;pos+128<=(int)samples;pos+=9600) {
            if(pos<140)continue;
            cx sum={0,0},phase={1,0},step=cis(-TAU*p.fo/1920000);float energy=0;
            for(int n=0;n<128;n++) {
                cx x={(float)iq[2*(pos+n)]-127.5f,(float)iq[2*(pos+n)+1]-127.5f};
                energy+=power(x);
                cx v=mul(mul(x,phase),conjx(w->td[p.nid2][n]));
                sum.r+=v.r;sum.i+=v.i;phase=mul(phase,step);
            }
            peak(w,power(sum)/(energy*(62.f/128)+1e-9f),pos,p.nid2,p.fo);
        }
        if(yield && yield(arg))return false;
    }
    /* Sort peaks in time. Each SSS match must independently beat every
     * competing cell sequence; a maximum alone is not evidence. */
    for(int i=1;i<w->count;i++) {peak_t p=w->peaks[i];int k=i;while(k && w->peaks[k-1].pos>p.pos){w->peaks[k]=w->peaks[k-1];k--;}w->peaks[k]=p;}
    int *counts=w->counts,*pairs=w->pairs,*last=w->last,*half_last=w->half_last;
    for(int pi=0;pi<w->count;pi++) {
        peak_t p=w->peaks[pi];float best=0,best_ratio=0;int id_best=-1,pos_best=0,fo_best=0;
        for(int delta=-2;delta<=2;delta++)for(int fo=p.fo-2500;fo<=p.fo+2500;fo+=500) {
            int pos=p.pos+delta;if(pos<137 || pos+128>(int)samples)continue;
            extract(iq,pos,fo,w->p,w->tw);extract(iq,pos-137,fo,w->s,w->tw);
            for(int k=0;k<62;k++){int bin=k<31?97+k:k-30;w->h[k]=mul(w->p[bin],conjx(w->pss[p.nid2][k]));}
            float energy=0;
            for(int k=0;k<62;k++) {
                cx sum={0,0};for(int d=-2;d<=2;d++){int m=k+d;if(m<0)m=0;if(m>61)m=61;sum.r+=w->h[m].r;sum.i+=w->h[m].i;}
                sum.r*=.2f;sum.i*=.2f;int bin=k<31?97+k:k-30;
                w->eq[k]=mul(w->s[bin],conjx(sum));energy+=power(w->eq[k]);
            }
            float first=0,second=0;int winner=-1;
            for(int id=0;id<336;id++) {
                cx sum={0,0};for(int k=0;k<62;k++){sum.r+=w->eq[k].r*w->sss[p.nid2][id][k];sum.i+=w->eq[k].i*w->sss[p.nid2][id][k];}
                float v=power(sum);if(v>first){second=first;first=v;winner=id;}else if(v>second)second=v;
            }
            float score=first/(62*energy+1e-9f);
            if(score>best){best=score;best_ratio=first/(second+1e-9f);id_best=winner;pos_best=pos;fo_best=fo;}
        }
        if(best>.3f && best_ratio>2 && id_best>=0) {
            int pci=3*(id_best/2)+p.nid2,half=id_best&1;
            int dt=pos_best-last[pci],n=(dt+4800)/9600;
            if(counts[pci] && n>0 && abs(dt-n*9600)<=5+n*2 && (half^half_last[pci])==(n&1))pairs[pci]++;
            counts[pci]++;last[pci]=pos_best;half_last[pci]=half;
            if(pairs[pci]>=2 && pairs[pci]>=out->pairs) {
                *out=(lte_sync_result_t){pci,counts[pci],pairs[pci],fo_best,p.score,best};
                w->frame_start=(pos_best-832-half*9600)%19200;
                if(w->frame_start<0)w->frame_start+=19200;
            }
        }
        if(yield && yield(arg)){memset(out,0,sizeof(*out));out->pci=-1;w->frame_start=-1;return false;}
    }
    return out->pci>=0;
}
