#include "lap_assets.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void lap_asset_arena_init(lap_asset_arena_t *a, void *p, size_t n) {
    if (a) { a->data=p; a->size=p?n:0; a->used=0; }
}
static void *take(lap_asset_arena_t *a, size_t n) {
    if (!a || !a->data || a->used>a->size) return NULL;
    size_t pad=(4-((uintptr_t)(a->data+a->used)&3))&3;
    if (pad>a->size-a->used || n>a->size-a->used-pad) return NULL;
    void *p=a->data+a->used+pad; a->used+=pad+n; return p;
}
static uint16_t le16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
static uint32_t le32(const uint8_t *p) { return le16(p)|(uint32_t)le16(p+2)<<16; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
int lap_mesh_load(l3_mesh *out,const void *src,size_t n,lap_asset_arena_t *a) {
    const uint8_t *p=src;
    if (!out||!a||!p||n<16||memcmp(p,"L3M1",4)||le16(p+4)!=1||le16(p+6)!=1) return LAP_ASSET_EFORMAT;
    uint32_t nv=le32(p+8), nt=le32(p+12);
    if (!nv||nv>65535||!nt||nt>65535||n!=16+(size_t)nv*12+(size_t)nt*7) return LAP_ASSET_EFORMAT;
    size_t mark=a->used;
    l3_vec *v=take(a,nv*sizeof(*v)); uint16_t *idx=take(a,nt*6); uint8_t *m=take(a,nt);
    if (!v||!idx||!m) { a->used=mark; return LAP_ASSET_EBUDGET; }
    p+=16;
    for (uint32_t i=0;i<nv;i++) {
        float xyz[3];
        for (int k=0;k<3;k++,p+=4) { uint32_t bits=le32(p); memcpy(xyz+k,&bits,4); if (!isfinite(xyz[k])) goto bad; }
        v[i]=l3v(xyz[0],xyz[1],xyz[2]);
    }
    for (uint32_t i=0;i<nt*3;i++,p+=2) { idx[i]=le16(p); if (idx[i]>=nv) goto bad; }
    for (uint32_t i=0;i<nt;i++) { m[i]=*p++; if (m[i]>=32) goto bad; }
    *out=(l3_mesh){v,idx,m,(int)nv,(int)nt}; return 0;
bad: a->used=mark; return LAP_ASSET_EFORMAT;
}

/* Small bounded RFC1951 decoder. Canonical Huffman codes, no global tables. */
typedef struct { const uint8_t *p,*end; unsigned bits,n; int bad; } bits_t;
static unsigned bits(bits_t *b,unsigned n) {
    while (b->n<n) { if (b->p==b->end) { b->bad=1; return 0; } b->bits|=(unsigned)*b->p++<<b->n; b->n+=8; }
    unsigned v=b->bits&((1u<<n)-1); b->bits>>=n; b->n-=n; return v;
}
typedef struct { uint16_t count[16],symbol[288]; } tree_t;
static int tree(tree_t *t,const uint8_t *lens,int n) {
    uint16_t off[16]; memset(t,0,sizeof(*t));
    for (int i=0;i<n;i++) { if (lens[i]>15) return 0; t->count[lens[i]]++; }
    int left=1;
    for (int i=1;i<16;i++) { left=left*2-t->count[i]; if (left<0) return 0; }
    off[1]=0; for (int i=1;i<15;i++) off[i+1]=off[i]+t->count[i];
    for (int i=0;i<n;i++) if (lens[i]) t->symbol[off[lens[i]]++]=(uint16_t)i;
    return 1;
}
static int symbol(bits_t *b,const tree_t *t) {
    unsigned code=0,first=0,index=0;
    for (int len=1;len<=15;len++) {
        code|=bits(b,1); unsigned count=t->count[len];
        if (code>=first && code-first<count) return t->symbol[index+code-first];
        index+=count; first=(first+count)<<1; code<<=1;
    }
    b->bad=1; return -1;
}
typedef struct { tree_t lit,dist,cl; uint8_t lens[320]; } inflate_work_t;
static int inflate(const uint8_t *p,size_t n,uint8_t *out,size_t cap,inflate_work_t *w) {
    static const uint16_t lb[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t lx[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t db[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t dx[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    if (n<6||(p[0]&15)!=8||(p[0]>>4)>7||(((unsigned)p[0]<<8)|p[1])%31||(p[1]&32)) return 0;
    bits_t b={p+2,p+n-4,0,0,0}; size_t used=0; unsigned final;
    do {
        final=bits(&b,1); unsigned type=bits(&b,2);
        if (type==0) {
            b.bits=0; b.n=0;
            unsigned len=bits(&b,16),inv=bits(&b,16);
            if (b.bad||(len^65535)!=inv||len>cap-used) return 0;
            while (len--) out[used++]=(uint8_t)bits(&b,8);
        } else if (type<3) {
            int nl=288,nd=32;
            if (type==1) {
                for(int i=0;i<288;i++) w->lens[i]=(uint8_t)(i<144?8:i<256?9:i<280?7:8);
                if (!tree(&w->lit,w->lens,288)) return 0;
                memset(w->lens,5,32); if (!tree(&w->dist,w->lens,32)) return 0;
            } else {
                static const uint8_t order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                nl=(int)bits(&b,5)+257; nd=(int)bits(&b,5)+1; int nc=(int)bits(&b,4)+4;
                if (nl>286) return 0;
                memset(w->lens,0,19);
                for(int i=0;i<nc;i++) w->lens[order[i]]=(uint8_t)bits(&b,3);
                if (!tree(&w->cl,w->lens,19)) return 0;
                int i=0;
                while(i<nl+nd&&!b.bad) {
                    int s=symbol(&b,&w->cl);
                    if(s<0) return 0;
                    if(s<16) w->lens[i++]=(uint8_t)s;
                    else {
                        if(s==16&&!i) return 0;
                        int count=(int)bits(&b,s==16?2:s==17?3:7)+(s==18?11:3);
                        uint8_t value=s==16?w->lens[i-1]:0;
                        if(count>nl+nd-i) return 0;
                        while(count--) w->lens[i++]=value;
                    }
                }
                if(b.bad||!w->lens[256]||!tree(&w->lit,w->lens,nl)||!tree(&w->dist,w->lens+nl,nd)) return 0;
            }
            for (;;) {
                int s=symbol(&b,&w->lit); if(b.bad||s<0) return 0;
                if(s==256) break;
                if(s<256) { if(used==cap) return 0; out[used++]=(uint8_t)s; }
                else {
                    if(s>285) return 0;
                    unsigned len=lb[s-257]+bits(&b,lx[s-257]); int d=symbol(&b,&w->dist);
                    if(d<0||d>29) return 0;
                    unsigned distance=db[d]+bits(&b,dx[d]);
                    if(b.bad||distance>used||len>cap-used) return 0;
                    while(len--) { out[used]=out[used-distance]; used++; }
                }
            }
        } else return 0;
        if(b.bad) return 0;
    } while(!final);
    if(used!=cap||b.p!=b.end) return 0;
    uint32_t s1=1,s2=0;
    for(size_t i=0;i<used;i++) { s1=(s1+out[i])%65521; s2=(s2+s1)%65521; }
    return ((s2<<16)|s1)==be32(p+n-4);
}
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t c=~0u;
    while(n--) { c^=*p++; for(int k=0;k<8;k++) c=(c>>1)^(0xedb88320u&(0u-(c&1))); }
    return ~c;
}
static int paeth(int a,int b,int c) {
    int p=a+b-c,pa=abs(p-a),pb=abs(p-b),pc=abs(p-c); return pa<=pb&&pa<=pc?a:pb<=pc?b:c;
}
int lap_image_png(lap_image_t *out,const void *src,size_t n,lap_asset_arena_t *a,const uint16_t inks[16]) {
    const uint8_t *p=src;
    if(!out||!a||!inks||!p||n<33||memcmp(p,"\x89PNG\r\n\x1a\n",8)) return LAP_ASSET_EFORMAT;
    uint32_t width=0,height=0; int depth=0,type=0,channels=0,np=0; size_t zipn=0;
    uint8_t palette[256][4]; memset(palette,255,sizeof(palette));
    int ihdr=0,iend=0,idat=0,ended=0,trns=0;
    uint16_t transparent[3]={0};
    for(size_t pos=8;pos<n;) {
        if(n-pos<12) return LAP_ASSET_EFORMAT;
        uint32_t len=be32(p+pos); const uint8_t *tag=p+pos+4,*v=tag+4;
        if(len>n-pos-12||crc(tag,(size_t)len+4)!=be32(v+len)) return LAP_ASSET_EFORMAT;
        for(int k=0;k<4;k++) if(!((tag[k]>='A'&&tag[k]<='Z')||(tag[k]>='a'&&tag[k]<='z'))) return LAP_ASSET_EFORMAT;
        if(tag[2]&32) return LAP_ASSET_EFORMAT;
        if(!ihdr&&memcmp(tag,"IHDR",4)) return LAP_ASSET_EFORMAT;
        if(!memcmp(tag,"IHDR",4)) {
            if(ihdr++||len!=13) return LAP_ASSET_EFORMAT;
            width=be32(v); height=be32(v+4); depth=v[8]; type=v[9];
            if(!width||!height||width>4096||height>4096||v[10]||v[11]||v[12]) return LAP_ASSET_EFORMAT;
            channels=type==0?1:type==2?3:type==3?1:type==4?2:type==6?4:0;
            if(!channels||(type==3?(depth!=1&&depth!=2&&depth!=4&&depth!=8):depth!=8)) return LAP_ASSET_EFORMAT;
        } else if(!memcmp(tag,"PLTE",4)) {
            if(idat||np||!len||len%3||len>768||type==0||type==4||(type==3&&len/3>(1u<<depth))) return LAP_ASSET_EFORMAT;
            np=(int)len/3;
            for(int i=0;i<np;i++) memcpy(palette[i],v+i*3,3);
        } else if(!memcmp(tag,"tRNS",4)) {
            if(idat||trns++) return LAP_ASSET_EFORMAT;
            if(type==3) { if(!np||len>(unsigned)np) return LAP_ASSET_EFORMAT; for(unsigned i=0;i<len;i++) palette[i][3]=v[i]; }
            else if((type==0&&len==2)||(type==2&&len==6)) for(unsigned i=0;i<len/2;i++) transparent[i]=(uint16_t)(v[i*2]<<8|v[i*2+1]);
            else return LAP_ASSET_EFORMAT;
        } else if(!memcmp(tag,"IDAT",4)) {
            if(ended||(type==3&&!np)) return LAP_ASSET_EFORMAT;
            zipn+=len; idat=1;
        } else if(!memcmp(tag,"IEND",4)) {
            if(len||pos+12!=n||!idat) return LAP_ASSET_EFORMAT;
            iend=1;
        } else { if(!(tag[0]&32)) return LAP_ASSET_EFORMAT; if(idat) ended=1; }
        pos+=(size_t)len+12;
    }
    if(!iend||!zipn) return LAP_ASSET_EFORMAT;
    size_t mark=a->used, pixels=(size_t)width*height, stride=((size_t)width*channels*depth+7)/8;
    uint8_t *dst=take(a,pixels); size_t keep=a->used;
    uint8_t *raw=take(a,(stride+1)*height),*zip=take(a,zipn); inflate_work_t *work=take(a,sizeof(*work));
    if(!dst||!raw||!zip||!work) { a->used=mark; return LAP_ASSET_EBUDGET; }
    size_t z=0;
    for(size_t pos=8;pos<n;) { uint32_t len=be32(p+pos); if(!memcmp(p+pos+4,"IDAT",4)) { memcpy(zip+z,p+pos+8,len); z+=len; } pos+=(size_t)len+12; }
    if(!inflate(zip,zipn,raw,(stride+1)*height,work)) goto badpng;
    int bpp=(channels*depth+7)/8;
    for(unsigned y=0;y<height;y++) {
        uint8_t *row=raw+(stride+1)*y+1,*prev=y?row-stride-1:NULL; unsigned filter=row[-1];
        if(filter>4) goto badpng;
        for(size_t x=0;x<stride;x++) {
            int left=x>=(unsigned)bpp?row[x-bpp]:0, up=prev?prev[x]:0,ul=prev&&x>=(unsigned)bpp?prev[x-bpp]:0;
            row[x]+=(uint8_t)(filter==1?left:filter==2?up:filter==3?(left+up)/2:filter==4?paeth(left,up,ul):0);
        }
        for(unsigned x=0;x<width;x++) {
            unsigned r,g,b,alpha=255;
            if(type==3) {
                unsigned index=(row[x*depth/8]>>(8-depth-(x*depth%8)))&((1u<<depth)-1);
                if(index>=(unsigned)np) goto badpng;
                r=palette[index][0]; g=palette[index][1]; b=palette[index][2]; alpha=palette[index][3];
            } else {
                const uint8_t *q=row+x*channels; r=q[0]; g=channels<3?r:q[1]; b=channels<3?r:q[2];
                if(type==4) alpha=q[1];
                if(type==6) alpha=q[3];
                if(trns&&r==transparent[0]&&(type==0||(g==transparent[1]&&b==transparent[2]))) alpha=0;
            }
            int best=255,dist=0x7fffffff;
            if(alpha>=128) for(int k=0;k<16;k++) {
                int dr=(int)r-((inks[k]>>11)*255/31),dg=(int)g-(((inks[k]>>5)&63)*255/63),db=(int)b-((inks[k]&31)*255/31);
                int d=dr*dr+dg*dg+db*db; if(d<dist) { dist=d; best=k; }
            }
            dst[(size_t)y*width+x]=(uint8_t)best;
        }
    }
    a->used=keep; *out=(lap_image_t){dst,(int)width,(int)height}; return 0;
badpng: a->used=mark; return LAP_ASSET_EFORMAT;
}
static int plain(const char *s) {
    if(!s||!*s||*s=='.'||strlen(s)>48||strstr(s,"..")) return 0;
    for(;*s;s++) if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.')) return 0;
    return 1;
}
int lap_asset_begin(lap_asset_file_t *f,const char *id,const char *name,void *buffer,size_t budget) {
    if(!f||!buffer||!budget||budget>4u*1024*1024||!plain(id)||!plain(name)) return LAP_SD_EINVAL;
    if(f->state==1||f->state==2) return LAP_ASSET_ESTATE;
    memset(f,0,sizeof(*f)); f->data=buffer; f->capacity=budget;
    snprintf(f->path,sizeof(f->path),"/sdcard/apps_data/%s/%s",id,name);
    f->job=lap_sd_size(f->path); f->state=f->job<0?3:1; f->error=f->job<0?f->job:0;
    return f->error;
}
int lap_asset_poll(lap_asset_file_t *f) {
    if(!f||!f->state) return LAP_ASSET_ESTATE;
    if(f->state==3) return f->error?f->error:1;
    int r=lap_sd_poll(f->job); if(r==LAP_SD_PENDING) return 0;
    if(r<0) goto fail;
    if(f->state==1) {
        f->size=(size_t)r;
        if(!r) { r=LAP_ASSET_EFORMAT; goto fail; }
        if(f->size>f->capacity) { r=LAP_ASSET_EBUDGET; goto fail; }
        f->state=2;
    } else {
        size_t want=f->size-f->offset; if(want>65536) want=65536;
        if((size_t)r!=want) { r=LAP_SD_EIO; goto fail; }
        f->offset+=(size_t)r;
        if(f->offset==f->size) { f->state=3; return 1; }
    }
    size_t want=f->size-f->offset; if(want>65536) want=65536;
    f->job=lap_sd_read(f->path,(uint32_t)f->offset,f->data+f->offset,(uint32_t)want);
    if(f->job<0) { r=f->job; goto fail; }
    return 0;
fail: f->state=3; f->error=r; return r;
}
