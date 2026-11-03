#include "lap_music.h"
#include <string.h>
#include <math.h>

static int number(const char *s,int max) {
    if(!*s) return -1;
    unsigned v=0;
    for(;*s;s++) { if(*s<'0'||*s>'9'||v>(unsigned)max/10) return -1; v=v*10+(unsigned)(*s-'0'); if(v>(unsigned)max) return -1; }
    return (int)v;
}
static int note(const char *s,lap_music_note_t *n) {
    if(!strcmp(s,"...")) { *n=(lap_music_note_t){0,0}; return 1; }
    if(!strcmp(s,"---")) { *n=(lap_music_note_t){255,0}; return 1; }
    const char *names="C D EF G A B"; const char *at=strchr(names,*s);
    if(!*s||!at||*s==' ') return 0;
    int semitone=(int)(at-names); s++;
    if(*s=='#') { semitone++; s++; }
    if(*s<'0'||*s>'8') return 0;
    int midi=(*s++-'0'+1)*12+semitone;
    if(*s++!='/') return 0;
    int inst=number(s,7); if(inst<0||midi>119) return 0;
    *n=(lap_music_note_t){(uint8_t)midi,(uint8_t)inst}; return 1;
}
int lap_music_parse(lap_music_song_t *out,const char *text,size_t bytes,int *error_line) {
    if(error_line) *error_line=0;
    if(!out) return -200;
    memset(out,0,sizeof(*out));
    if(!text||bytes>65536) return -200;
    out->loop_order=255;
    size_t pos=0; int line=0,header=0,tempo=0,loop=0,selected=-1; unsigned insts=0;
    while(pos<bytes) {
        line++; char buf[192]; size_t len=0;
        while(pos<bytes&&text[pos]!='\n') { if(len+1>=sizeof(buf)||!text[pos]) goto bad; buf[len++]=text[pos++]; }
        if(pos<bytes) pos++;
        buf[len]=0; char *hash=strchr(buf,'#');
        /* Sharp notes contain #; only # at the start of a token is comment. */
        while(hash&&hash!=buf&&hash[-1]!=' '&&hash[-1]!='\t') hash=strchr(hash+1,'#');
        if(hash) *hash=0;
        char *t[34]; int count=0; char *s=buf;
        while(*s) {
            while(*s==' '||*s=='\t'||*s=='\r') s++;
            if(!*s) break;
            if(count==34) goto bad;
            t[count++]=s; while(*s&&*s!=' '&&*s!='\t'&&*s!='\r') s++;
            if(*s) *s++=0;
        }
        if(!count) continue;
        if(!header) { if(count!=1||strcmp(t[0],"LTM1")) goto bad; header=1; continue; }
        if(!strcmp(t[0],"tempo")) {
            if(count!=3||tempo++) goto bad;
            int bpm=number(t[1],300),rpb=number(t[2],16); if(bpm<30||rpb<1) goto bad;
            out->bpm=(uint16_t)bpm; out->rows_per_beat=(uint8_t)rpb;
        } else if(!strcmp(t[0],"inst")) {
            if(count!=6) goto bad;
            int id=number(t[1],7),wave=number(t[2],4),volume=number(t[3],100),attack=number(t[4],1000),release=number(t[5],1000);
            if(id<0||wave<0||volume<0||attack<0||release<0||(insts&(1u<<id))) goto bad;
            insts|=1u<<id; out->instruments[id]=(lap_music_instrument_t){(uint8_t)wave,(uint8_t)volume,(uint16_t)attack,(uint16_t)release};
        } else if(!strcmp(t[0],"pattern")) {
            if(count!=3) goto bad;
            selected=number(t[1],7); int rows=number(t[2],64);
            if(selected<0||rows<1||out->rows[selected]) goto bad;
            out->rows[selected]=(uint8_t)rows;
        } else if(!strcmp(t[0],"row")) {
            if(count!=6||selected<0) goto bad;
            int row=number(t[1],out->rows[selected]-1); if(row<0) goto bad;
            for(int c=0;c<4;c++) {
                lap_music_note_t n; if(!note(t[c+2],&n)||(n.note&&n.note!=255&&!(insts&(1u<<n.instrument)))) goto bad;
                out->patterns[selected][row][c]=n;
            }
        } else if(!strcmp(t[0],"order")) {
            if(count<2||count>LAP_MUSIC_ORDERS+1||out->order_count) goto bad;
            out->order_count=(uint8_t)(count-1);
            for(int i=1;i<count;i++) { int id=number(t[i],7); if(id<0||!out->rows[id]) goto bad; out->order[i-1]=(uint8_t)id; }
        } else if(!strcmp(t[0],"loop")) {
            if(count!=2||loop++) goto bad;
            int v=number(t[1],255); if(v<0) goto bad; out->loop_order=(uint8_t)v;
        } else goto bad;
    }
    if(!header||!tempo||!loop||!out->order_count||(out->loop_order!=255&&out->loop_order>=out->order_count)) goto bad;
    if(error_line) *error_line=0;
    return 0;
bad: memset(out,0,sizeof(*out)); if(error_line) *error_line=line; return -200;
}
void lap_music_start(lap_music_player_t *p,const lap_music_song_t *song) {
    if(!p) return;
    memset(p,0,sizeof(*p)); p->song=song; p->playing=song&&song->bpm>=30&&song->rows_per_beat&&song->order_count;
}
static void nextrow(lap_music_player_t *p) {
    const lap_music_song_t *s=p->song;
    if(p->order>=s->order_count) { if(s->loop_order==255) { p->playing=false; return; } p->order=s->loop_order; }
    unsigned pattern=s->order[p->order];
    unsigned divisor=(unsigned)s->bpm*s->rows_per_beat;
    unsigned numerator=960000u+p->row_fraction;
    p->row_left=numerator/divisor; p->row_fraction=numerator%divisor;
    for(int c=0;c<4;c++) {
        lap_music_note_t n=s->patterns[pattern][p->row][c]; lap_music_voice_t *v=p->voices+c;
        if(n.note==255) { v->gate=v->age; continue; }
        if(!n.note) continue;
        lap_music_instrument_t i=s->instruments[n.instrument];
        memset(v,0,sizeof(*v)); v->wave=i.wave; v->volume=i.volume; v->noise=12345u+(unsigned)c;
        v->increment=(uint32_t)(440.0f*powf(2.0f,((int)n.note-69)/12.0f)*(4294967296.0f/16000.0f));
        v->attack=i.attack_ms*16u; v->release=i.release_ms*16u; v->gate=p->row_left*3/4;
    }
    if(++p->row>=s->rows[pattern]) { p->row=0; p->order++; }
}
void lap_music_render(lap_music_player_t *p,int16_t *pcm,unsigned samples) {
    if(!p||!pcm) return;
    for(unsigned j=0;j<samples;j++) {
        if(p->playing&&!p->row_left) nextrow(p);
        if(p->playing) p->row_left--;
        int mix=0;
        for(int c=0;c<4;c++) {
            lap_music_voice_t *v=p->voices+c;
            if(!v->volume||v->age>=v->gate+v->release) continue;
            float env=1;
            if(v->attack&&v->age<v->attack) env=(float)v->age/v->attack;
            if(v->age>=v->gate) env=v->release?env*(1-(float)(v->age-v->gate)/v->release):0;
            float phase=(float)(v->phase>>8)/16777216.0f,value;
            switch(v->wave) {
            case LAP_WAVE_SINE: {
                /* Parabolic sine approximation; avoids thousands of libm calls
                 * per frame on small boards. Error <0.002 at full amplitude. */
                float x=phase<0.5f?phase*2:phase*2-2;
                float v=4*x*(1-fabsf(x));
                value=0.225f*(v*fabsf(v)-v)+v; break;
            }
            case LAP_WAVE_SQUARE: value=phase<0.5f?1:-1; break;
            case LAP_WAVE_TRIANGLE: value=1-4*fabsf(phase-0.5f); break;
            case LAP_WAVE_SAW: value=2*phase-1; break;
            default: v->noise=v->noise*1664525u+1013904223u; value=(float)(v->noise>>8)/8388608.0f-1; break;
            }
            mix+=(int)(value*env*v->volume*81); v->age++; v->phase+=v->increment;
        }
        pcm[j]=(int16_t)(mix>32767?32767:mix< -32768?-32768:mix);
    }
}
int lap_music_pump(lap_music_player_t *p) {
    if(!p||!p->song) return -200;
    if(p->offset==p->count) { p->offset=0; p->count=1024; lap_music_render(p,p->pending,p->count); }
    int n=lap_mixer_write(LAP_MIX_MUSIC,p->pending+p->offset,(int)(p->count-p->offset));
    if(n>0) p->offset+=(unsigned)n;
    return n;
}
void lap_music_stop(lap_music_player_t *p) {
    if(p) { p->playing=false; p->song=NULL; p->count=p->offset=0; memset(p->voices,0,sizeof(p->voices)); }
    lap_mixer_clear(LAP_MIX_MUSIC);
}
