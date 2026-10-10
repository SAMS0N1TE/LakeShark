/* Written from RFC 1950 and RFC 1951. No external implementation used. */
#include "ingest.h"
#include <string.h>
typedef struct { int (*read)(void *); void *input; size_t remaining; unsigned bits,n; int error; } bits;
typedef struct { unsigned short fast[512], code[288]; unsigned char len[288]; unsigned count; } tree;
static unsigned take(bits *b,unsigned n) {
    unsigned v;
    while(b->n<n && b->remaining) { b->bits|=(unsigned)b->read(b->input)<<b->n; b->n+=8; b->remaining--; }
    if(b->n<n) { b->error=1; return 0; }
    v=b->bits&((1u<<n)-1); b->bits>>=n; b->n-=n; return v;
}
static unsigned reverse(unsigned v,unsigned n) { unsigned r=0; while(n--) { r=(r<<1)|(v&1); v>>=1; } return r; }
static int make(tree *t,const unsigned char *len,unsigned n,int codes) {
    unsigned counts[16]={0},next[16]={0},c=0; int left=1;
    memset(t,0,sizeof(*t)); t->count=n;
    for(unsigned i=0;i<n;i++) { if(len[i]>15) return 0; counts[len[i]]++; t->len[i]=len[i]; }
    for(unsigned i=1;i<=15;i++) { left=left*2-(int)counts[i]; if(left<0) return 0; }
    /* Code-length alphabets must be complete. Literal/distance alphabets may
       have one length-one symbol; an unused distance alphabet may be empty. */
    if(left && (codes || (n-counts[0]!=0 && !(n-counts[0]==1 && counts[1]==1)))) return 0;
    /* Zero length symbols do not participate in canonical numbering. */
    c=0; counts[0]=0;
    for(unsigned i=1;i<=15;i++) { c=(c+counts[i-1])<<1; next[i]=c; }
    for(unsigned i=0;i<n;i++) if(len[i]) {
        unsigned l=len[i],r=reverse(next[l]++,l); t->code[i]=(unsigned short)r;
        if(l<=9) for(unsigned j=r;j<512;j+=1u<<l) t->fast[j]=(unsigned short)((i<<4)|l);
    }
    return 1;
}
static int symbol(bits *b,const tree *t) {
    unsigned v,l;
    while(b->n<9 && b->remaining) { b->bits|=(unsigned)b->read(b->input)<<b->n; b->n+=8; b->remaining--; }
    v=t->fast[b->bits&511]; l=v&15;
    if(l && l<=b->n) { take(b,l); return (int)(v>>4); }
    v=0;
    for(l=1;l<=15;l++) {
        v|=take(b,1)<<(l-1); if(b->error) return -1;
        for(unsigned i=0;i<t->count;i++) if(t->len[i]==l && t->code[i]==v) return (int)i;
    }
    b->error=1; return -1;
}
typedef struct { const unsigned char *p; } memory_input;
static int inflate_memory_read(void *v) { memory_input *m=v; return *m->p++; }
typedef struct { unsigned char *p; } memory_output;
static int memory_emit(void *v,unsigned char c) { memory_output *m=v; *m->p++=c; return 1; }
static int output_byte(unsigned char *history,size_t *used,unsigned *a,unsigned *c,
    int (*emit)(void *,unsigned char),void *output,unsigned char v) {
    if(!emit(output,v)) return 0;
    history[(*used)++&32767]=v; *a=(*a+v)%65521; *c=(*c+*a)%65521; return 1;
}
int cc_inflate_stream(int (*read_byte)(void *),void *input,size_t size,
    int (*emit)(void *,unsigned char),void *output,unsigned char *history,
    size_t cap,size_t *written) {
    static const unsigned lb[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const unsigned char le[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned db[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const unsigned char de[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    bits b; size_t used=0; unsigned final=0,a=1,c=0;
    if(!read_byte || !emit || !history || !written || size<6) return 0;
    unsigned cmf=(unsigned)read_byte(input),flg=(unsigned)read_byte(input);
    if((cmf&15)!=8 || (cmf>>4)>7 || ((cmf*256+flg)%31) || (flg&32)) return 0;
    b=(bits){read_byte,input,size-6,0,0,0};
    while(!final && !b.error) {
        unsigned type; tree lit,dist; unsigned char lengths[320]={0};
        final=take(&b,1); type=take(&b,2);
        if(type==0) {
            unsigned n,inv; take(&b,b.n%8); n=take(&b,16); inv=take(&b,16);
            if(b.error || (n^inv)!=65535 || n>cap-used) return 0;
            while(n--) { unsigned char v=(unsigned char)take(&b,8); if(b.error || !output_byte(history,&used,&a,&c,emit,output,v)) return 0; }
            continue;
        }
        if(type==1) {
            for(unsigned i=0;i<288;i++) lengths[i]=(unsigned char)(i<144?8:i<256?9:i<280?7:8);
            if(!make(&lit,lengths,288,0)) return 0;
            memset(lengths,5,32); if(!make(&dist,lengths,32,0)) return 0;
        } else if(type==2) {
            static const unsigned char order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            unsigned nl=take(&b,5)+257,nd=take(&b,5)+1,nc=take(&b,4)+4,pos=0; tree cl;
            unsigned char clens[19]={0};
            if(nl>286) return 0;
            for(unsigned i=0;i<nc;i++) clens[order[i]]=(unsigned char)take(&b,3);
            if(!make(&cl,clens,19,1)) return 0;
            while(pos<nl+nd) {
                int s=symbol(&b,&cl); unsigned repeat,value;
                if(s<0) return 0;
                if(s<16) { lengths[pos++]=(unsigned char)s; continue; }
                if(s==16) { if(!pos) return 0; repeat=take(&b,2)+3; value=lengths[pos-1]; }
                else { repeat=take(&b,s==17?3:7)+(s==17?3:11); value=0; }
                if(repeat>nl+nd-pos) return 0;
                while(repeat--) lengths[pos++]=(unsigned char)value;
            }
            if(!lengths[256] || !make(&lit,lengths,nl,0) || !make(&dist,lengths+nl,nd,0)) return 0;
        } else return 0;
        for(;;) {
            int s=symbol(&b,&lit); unsigned n,d; int ds;
            if(s<0) return 0;
            if(s<256) { if(used==cap) return 0; if(!output_byte(history,&used,&a,&c,emit,output,(unsigned char)s)) return 0; continue; }
            if(s==256) break;
            if(s>285) return 0;
            n=lb[s-257]+take(&b,le[s-257]); ds=symbol(&b,&dist);
            if(ds<0 || ds>=30) return 0;
            d=db[ds]+take(&b,de[ds]);
            if(b.error || d>used || d>(1u<<((cmf>>4)+8)) || n>cap-used) return 0;
            while(n--) { unsigned char v=history[(used-d)&32767]; if(!output_byte(history,&used,&a,&c,emit,output,v)) return 0; }
        }
    }
    if(b.error || b.remaining || b.n>=8) return 0;
    unsigned adler=0; for(unsigned i=0;i<4;i++) adler=(adler<<8)|(unsigned)read_byte(input);
    if(((c<<16)|a)!=adler) return 0;
    *written=used; return 1;
}
int cc_inflate(const unsigned char *src,size_t size,unsigned char *dst,size_t cap,size_t *written) {
    if(!src || (!dst && cap)) return 0;
    unsigned char history[32768]; memory_input in={src}; memory_output out={dst};
    return cc_inflate_stream(inflate_memory_read,&in,size,memory_emit,&out,history,cap,written);
}
