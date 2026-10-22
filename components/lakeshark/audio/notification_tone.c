#include "notification_tone.h"
#include "audio_out.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct { unsigned hz1, end1, ms1, gap, hz2, end2, ms2, attack, release; } preset_t;
/* Single fallback table: SD rows use exactly this order. No make-up gain. */
static const preset_t defaults[3] = {
    {1800,1800,55,35,1800,1800,55,1,3},
    {1000,1000,45,25,1500,1500,45,1,3},
    {1000,2500,85, 0,1000,1000, 0,1,4}
};
static EXT_RAM_BSS_ATTR preset_t presets[3];
static EXT_RAM_BSS_ATTR preset_t staging[3];
static EXT_RAM_BSS_ATTR char config[512];
static bool ready, loaded;
static void init(void) { if (!ready) { memcpy(presets,defaults,sizeof(presets)); ready=true; } }
static void ws(const char **p) { while (**p && isspace((unsigned char)**p)) ++*p; }
static bool literal(const char **p,const char *s)
{
    ws(p); size_t n=strlen(s);
    if (strncmp(*p,s,n)) return false;
    *p+=n; return true;
}
static bool integer(const char **p,unsigned *v)
{
    ws(p);
    if (!isdigit((unsigned char)**p)) return false;
    if (**p=='0' && isdigit((unsigned char)(*p)[1])) return false;
    char *end; unsigned long n=strtoul(*p,&end,10);
    if (n>3000) return false;
    *p=end; *v=(unsigned)n; return true;
}
bool notification_tone_configure(const char *json)
{
    if (!json) return false;
    const char *p=json;
    if (!literal(&p,"{") || !literal(&p,"\"version\"") || !literal(&p,":")) return false;
    unsigned version;
    if (!integer(&p,&version) || version!=1 || !literal(&p,",") ||
        !literal(&p,"\"presets\"") || !literal(&p,":") || !literal(&p,"[")) return false;
    for (int i=0;i<3;++i) {
        unsigned v[9];
        if ((i && !literal(&p,",")) || !literal(&p,"[")) return false;
        for (int j=0;j<9;++j)
            if ((j && !literal(&p,",")) || !integer(&p,&v[j])) return false;
        if (!literal(&p,"]")) return false;
        if (v[0]<300 || v[1]<300 || v[4]<300 || v[5]<300 ||
            v[2]<10 || v[2]>150 || v[3]>100 || v[6]>150 ||
            (v[6] && v[6]<10) || v[7]<1 || v[7]>5 || v[8]<1 || v[8]>10 ||
            v[7]+v[8]>v[2] || (v[6] && v[7]+v[8]>v[6])) return false;
        staging[i]=(preset_t){v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8]};
    }
    if (!literal(&p,"]") || !literal(&p,"}")) return false;
    ws(&p); if (*p) return false;
    memcpy(presets,staging,sizeof(presets)); ready=true; return true;
}
void notification_tone_load(void)
{
    init(); if (loaded) return; loaded=true;
    FILE *f=fopen("/sdcard/audio/notification.json","rb");
    if (!f) return;
    size_t n=fread(config,1,sizeof(config)-1,f);
    bool ok=!ferror(f) && fgetc(f)==EOF;
    fclose(f); config[n]=0;
    if (ok && !memchr(config,0,n)) (void)notification_tone_configure(config);
}
size_t notification_tone_render(int preset,size_t offset,int16_t *pcm,size_t capacity)
{
    if (preset<0 || preset>=3 || !pcm) return 0;
    init(); const preset_t *p=&presets[preset];
    const unsigned per_ms=AUDIO_RATE_HZ/1000;
    unsigned n1=p->ms1*per_ms, gap=p->gap*per_ms, n2=p->ms2*per_ms;
    size_t total=n1+(n2 ? gap+n2 : 0);
    if (offset>=total) return 0;
    size_t count=total-offset; if (count>capacity) count=capacity;
    for (size_t j=0;j<count;++j) {
        unsigned n=(unsigned)(offset+j), length=n1, f=p->hz1, end=p->end1;
        if (n>=n1) {
            if (n<n1+gap) { pcm[j]=0; continue; }
            n-=n1+gap; length=n2; f=p->hz2; end=p->end2;
        }
        unsigned attack=p->attack*per_ms, release=p->release*per_ms;
        float env=1.0f;
        if (n<attack) env=(float)n/attack;
        if (n>=length-release) env=(float)(length-1-n)/release;
        /* Integrated linear frequency ramp. Pure sine stays in voice band;
         * short edge ramps soften its broadband keying transient. */
        float t=(float)n/AUDIO_RATE_HZ;
        float cycles=f*t+0.5f*((float)end-f)*t*n/(float)length;
        pcm[j]=(int16_t)(4800.0f*env*sinf(2.0f*(float)M_PI*cycles));
    }
    return count;
}
