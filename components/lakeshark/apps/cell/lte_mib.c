/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original implementation of TS 36.211 6.6/6.10 and TS 36.212 5.1/5.3.1.
 * Bounded, single-frame PBCH decoding. No heap allocations or global state.
 */
#include "lte_mib.h"
#include <math.h>
#include <string.h>

#define TAU 6.2831853071795864769f
typedef struct { float r,i; } cx;
typedef struct {
    cx fft[128], tw[64], grid[5][72], ce[4][5][72];
    cx pilots[3][12], rx[240], h[4][240];
    float llr[480], coded[40][3], gamma[40][8], score[2][64];
    uint64_t trace[40];
    uint8_t bits[40], scramble[1920], code[128];
    int map[120];
    lte_mib_result_t candidates[12];
    int candidate_count;
} workspace_t;
static cx add(cx a,cx b){return (cx){a.r+b.r,a.i+b.i};}
static cx sub(cx a,cx b){return (cx){a.r-b.r,a.i-b.i};}
static cx scale(cx a,float b){return (cx){a.r*b,a.i*b};}
static cx mul(cx a,cx b){return (cx){a.r*b.r-a.i*b.i,a.r*b.i+a.i*b.r};}
static cx cj(cx a){return (cx){a.r,-a.i};}
static cx cis(float a){return (cx){cosf(a),sinf(a)};}
static void gold(uint32_t seed,uint8_t *out,int count)
{
    uint32_t x1=1,x2=seed;
    for(int n=0;n<1600+count;n++) {
        if(n>=1600)out[n-1600]=(uint8_t)((x1^x2)&1);
        unsigned a=(x1^(x1>>3))&1,b=(x2^(x2>>1)^(x2>>2)^(x2>>3))&1;
        x1=(x1>>1)|(a<<30);x2=(x2>>1)|(b<<30);
    }
}
static unsigned parity(unsigned x)
{
    x^=x>>4;x^=x>>2;x^=x>>1;return x&1;
}
static void initialize(workspace_t *w,int pci)
{
    for(int n=0;n<64;n++)w->tw[n]=cis(-TAU*n/128);
    /* CRS for slot 1, symbols 0/1/4. Central 6 RBs use m'=104..115. */
    for(int t=0;t<3;t++) {
        int symbol=t==2?4:t;
        gold((uint32_t)(1024*(14+symbol+1)*(2*pci+1)+2*pci+1),w->scramble,440);
        for(int m=0;m<12;m++) {
            int b=2*(104+m);
            w->pilots[t][m]=(cx){(1-2*w->scramble[b])*.7071067812f,(1-2*w->scramble[b+1])*.7071067812f};
        }
    }
    gold((uint32_t)pci,w->scramble,1920);
    const int permutation[32]={1,17,9,25,5,21,13,29,3,19,11,27,7,23,15,31,0,16,8,24,4,20,12,28,2,18,10,26,6,22,14,30};
    int n=0;
    for(int stream=0;stream<3;stream++)for(int col=0;col<32;col++)for(int row=0;row<2;row++) {
        int bit=32*row+permutation[col]-24;
        if(bit>=0)w->map[n++]=3*bit+stream;
    }
    /* Current input is the low bit here, so reverse the specification's
     * 7-bit generator notation (133,171,165 octal). */
    for(unsigned r=0;r<128;r++)
        w->code[r]=(uint8_t)(4*parity(r&0155)+2*parity(r&0117)+parity(r&0127));
}
static void transform(workspace_t *w,const uint8_t *iq,int start,int cfo,cx *out)
{
    cx phase=cis(-TAU*(float)((double)cfo*start/1920000.0)),step=cis(-TAU*cfo/1920000);
    for(int n=0;n<128;n++) {
        cx x={(float)iq[2*(start+n)]-127.5f,(float)iq[2*(start+n)+1]-127.5f};
        w->fft[n]=mul(x,phase);phase=mul(phase,step);
    }
    for(int i=1,j=0;i<128;i++) {
        int bit=64;for(;j&bit;bit>>=1)j^=bit;j^=bit;
        if(i<j){cx t=w->fft[i];w->fft[i]=w->fft[j];w->fft[j]=t;}
    }
    for(int len=2;len<=128;len<<=1)for(int off=0;off<128;off+=len)for(int k=0;k<len/2;k++) {
        cx a=w->fft[off+k],b=mul(w->fft[off+k+len/2],w->tw[k*128/len]);
        w->fft[off+k]=add(a,b);w->fft[off+k+len/2]=sub(a,b);
    }
    for(int sc=0;sc<72;sc++)out[sc]=w->fft[sc<36?92+sc:sc-35];
}
static void estimate(workspace_t *w,int pci,int port,int symbol,int pilot_symbol)
{
    int v;
    if(port<2)v=(port*3+(symbol==4?3:0))%6;
    else v=port==2?3:0; /* odd slot */
    int shift=(pci+v)%6;
    cx pilot[12];
    for(int m=0;m<12;m++)pilot[m]=mul(w->grid[symbol][6*m+shift],cj(w->pilots[pilot_symbol][m]));
    for(int sc=0;sc<72;sc++) {
        float x=(sc-shift)/6.f;int left=(int)floorf(x);float t=x-left;
        if(left<0)w->ce[port][symbol][sc]=pilot[0];
        else if(left>=11)w->ce[port][symbol][sc]=pilot[11];
        else w->ce[port][symbol][sc]=add(scale(pilot[left],1-t),scale(pilot[left+1],t));
    }
}
static void extract(workspace_t *w,const uint8_t *iq,int start,int cfo,int pci)
{
    /* Move two samples into the CP to tolerate small sync timing errors. */
    for(int l=0;l<5;l++)transform(w,iq,start+968+137*l,cfo,w->grid[l]);
    for(int p=0;p<2;p++) {estimate(w,pci,p,0,0);estimate(w,pci,p,4,2);}
    cx rotation={0,0};
    for(int p=0;p<2;p++)for(int sc=0;sc<72;sc++)
        rotation=add(rotation,mul(w->ce[p][4][sc],cj(w->ce[p][0][sc])));
    float phase=atan2f(rotation.i,rotation.r)/4;
    for(int l=1;l<5;l++)for(int sc=0;sc<72;sc++)w->grid[l][sc]=mul(w->grid[l][sc],cis(-phase*l));
    for(int p=0;p<2;p++) {
        estimate(w,pci,p,4,2);
        for(int l=1;l<4;l++)for(int sc=0;sc<72;sc++)
            w->ce[p][l][sc]=add(scale(w->ce[p][0][sc],1-l*.25f),scale(w->ce[p][4][sc],l*.25f));
    }
    for(int p=2;p<4;p++) {
        estimate(w,pci,p,1,1);
        for(int l=0;l<4;l++)if(l!=1)memcpy(w->ce[p][l],w->ce[p][1],sizeof(w->ce[p][1]));
    }
    int n=0;
    for(int l=0;l<4;l++)for(int sc=0;sc<72;sc++) {
        if(l<2 && sc%3==pci%3)continue;
        w->rx[n]=w->grid[l][sc];
        for(int p=0;p<4;p++)w->h[p][n]=w->ce[p][l][sc];
        n++;
    }
}
static void demodulate(workspace_t *w,int ports)
{
    for(int n=0;n<240;n+=2) {
        cx d[2];
        if(ports==1) {
            d[0]=mul(cj(w->h[0][n]),w->rx[n]);
            d[1]=mul(cj(w->h[0][n+1]),w->rx[n+1]);
        } else {
            int a=ports==2?0:(n%4==0?0:1),b=ports==2?1:(n%4==0?2:3);
            cx h0=scale(add(w->h[a][n],w->h[a][n+1]),.5f),h1=scale(add(w->h[b][n],w->h[b][n+1]),.5f);
            d[0]=add(mul(cj(h0),w->rx[n]),mul(h1,cj(w->rx[n+1])));
            d[1]=sub(mul(cj(h0),w->rx[n+1]),mul(h1,cj(w->rx[n])));
        }
        /* Unnormalized soft values retain channel reliability in fades. */
        for(int k=0;k<2;k++){w->llr[2*(n+k)]=d[k].r;w->llr[2*(n+k)+1]=d[k].i;}
    }
}
static float viterbi(workspace_t *w)
{
    for(int t=0;t<40;t++)for(int c=0;c<8;c++)
        w->gamma[t][c]=(c&4?-1:1)*w->coded[t][0]+(c&2?-1:1)*w->coded[t][1]+(c&1?-1:1)*w->coded[t][2];
    float best=-1e30f;
    /* Exact tail-biting maximum likelihood: same initial and final state.
     * No CRC-based selection of alternate, lower-scoring codewords. */
    for(int initial=0;initial<64;initial++) {
        for(int s=0;s<64;s++)w->score[0][s]=s==initial?0:-1e30f;
        for(int t=0;t<40;t++) {
            float *previous=w->score[t&1],*next=w->score[(t+1)&1];uint64_t trace=0;
            for(int s=0;s<64;s++) {
                int p=s>>1;
                float a=previous[p]+w->gamma[t][w->code[s]],b=previous[p+32]+w->gamma[t][w->code[s+64]];
                next[s]=a>b?a:b;if(b>a)trace|=(uint64_t)1<<s;
            }
            w->trace[t]=trace;
        }
        if(w->score[0][initial]>best) {
            best=w->score[0][initial];int state=initial;
            for(int t=39;t>=0;t--) {w->bits[t]=(uint8_t)(state&1);state=(state>>1)|((int)((w->trace[t]>>state)&1)<<5);}
        }
    }
    return best;
}
static bool decode(workspace_t *w,int quarter,int ports,lte_mib_result_t *out)
{
    memset(w->coded,0,sizeof(w->coded));
    for(int n=0;n<480;n++) {
        int m=w->map[n%120];
        w->coded[m/3][m%3]+=w->llr[n]*(w->scramble[quarter*480+n]?-1:1);
    }
    float energy=0;
    for(int t=0;t<40;t++)for(int s=0;s<3;s++)energy+=fabsf(w->coded[t][s]);
    /* All-zero soft input has a valid all-zero CRC. Require measured soft
     * evidence and codeword agreement, not just that algebraic coincidence. */
    if(!isfinite(energy) || energy<1 || viterbi(w)<.55f*energy)return false;
    uint16_t crc=0,received=0;uint32_t payload=0;
    for(int n=0;n<24;n++) {
        unsigned top=(crc>>15)^w->bits[n];crc=(uint16_t)(crc<<1);if(top)crc^=0x1021;
        payload=(payload<<1)|w->bits[n];
    }
    for(int n=24;n<40;n++)received=(uint16_t)((received<<1)|w->bits[n]);
    uint16_t mask=ports==1?0:ports==2?0xffff:0x5555;
    int packed=(int)(payload>>21);
    if((uint16_t)(crc^mask)!=received || packed>5)return false;
    const int rb[6]={6,15,25,50,75,100};
    *out=(lte_mib_result_t){.n_rb=rb[packed],.antenna_ports=ports,.phich_duration=(payload>>20)&1,
        .phich_resource=(payload>>18)&3,.sfn=(int)(((payload>>10)&255)*4+quarter),.payload=payload,.frames=1};
    return true;
}
size_t lte_mib_workspace_size(void){return sizeof(workspace_t);}
static bool find(const uint8_t *iq,size_t samples,int pci,int frame_start,
                 int cfo_hz,void *memory,lte_mib_result_t *out,lte_sync_yield_fn yield,void *arg,bool confirm)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));
    if(!iq || !memory || samples<28800 || samples>153600 || pci<0 || pci>503 ||
       frame_start<0 || frame_start>=19200 || cfo_hz < -50000 || cfo_hz>50000)return false;
    workspace_t *w=memory;memset(w,0,sizeof(*w));initialize(w,pci);
    for(int frame=0;frame<8;frame++) {
        int start=frame_start+19200*frame;
        if((size_t)(start+1644)>samples)break;
        extract(w,iq,start,cfo_hz,pci);
        for(int ports=1;ports<=4;ports*=2) {
            demodulate(w,ports);
            for(int q=0;q<4;q++) {
                if(yield && yield(arg)){memset(out,0,sizeof(*out));return false;}
                lte_mib_result_t r;
                if(!decode(w,q,ports,&r))continue;
                bool matched=false;
                for(int n=0;n<w->candidate_count;n++) {
                    lte_mib_result_t *prior=&w->candidates[n];
                    if(frame<=prior->first_frame || ports!=prior->antenna_ports ||
                       (r.payload&0xfc03ff)!=(prior->payload&0xfc03ff) ||
                       ((r.sfn-prior->sfn+1024)%1024)!=frame-prior->first_frame)continue;
                    prior->frames++;matched=true;
                    if(prior->frames>out->frames)*out=*prior;
                    if(confirm && out->frames>=2)return true;
                    break;
                }
                if(!matched && w->candidate_count<12) {
                    r.first_frame=frame;w->candidates[w->candidate_count++]=r;
                }
            }
        }
    }
    if(out->frames>=2)return true;
    memset(out,0,sizeof(*out));return false;
}
bool lte_mib_find(const uint8_t *iq,size_t samples,int pci,int frame_start,
                  int cfo_hz,void *memory,lte_mib_result_t *out,lte_sync_yield_fn yield,void *arg)
{
    return find(iq,samples,pci,frame_start,cfo_hz,memory,out,yield,arg,false);
}
bool lte_mib_confirm(const uint8_t *iq,size_t samples,int pci,int frame_start,
                     int cfo_hz,void *memory,lte_mib_result_t *out,lte_sync_yield_fn yield,void *arg)
{
    return find(iq,samples,pci,frame_start,cfo_hz,memory,out,yield,arg,true);
}
