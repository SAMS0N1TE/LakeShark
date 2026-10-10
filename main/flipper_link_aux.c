#include "flipper_link_aux.h"
#include <stdio.h>
#include <string.h>
static void token(char *out,size_t cap,const char *in,size_t bytes) {
    size_t i=0;
    for(;i+1<cap && i<bytes && in[i];i++) {
        unsigned char c=in[i];out[i]=(c<=32 || c==127 || c=='=')?'_':c;
    }
    out[i]=0;if(!i) {out[0]='-';out[1]=0;}
}
int ls_link_aux_encode(char *buf,size_t len,const ls_link_aux_t *s) {
    if(!buf || !len || !s) return 0;
    char ssid[33],ip[16],id[21],contact[2][24];
    token(ssid,sizeof(ssid),s->ssid,sizeof(s->ssid));
    token(ip,sizeof(ip),s->ip,sizeof(s->ip));
    token(id,sizeof(id),s->drone_id,sizeof(s->drone_id));
    for(unsigned i=0;i<2;i++) {
        if(!s->serial[i]) strcpy(contact[i],"-");
        else snprintf(contact[i],sizeof(contact[i]),"%c%lu:%d",
            s->category[i]<5?"CBDTA"[s->category[i]]:'?',(unsigned long)s->serial[i],s->rssi[i]);
    }
    int n=snprintf(buf,len,"& av=1 wc=%d wn=%d ws=%s wi=%s sr=%d sm=%d "
        "s0=%u s1=%u s2=%u s3=%u s4=%u c0=%s c1=%s dn=%u dr=%d di=%s\n",
        s->connected,s->saved,ssid,ip,s->running,s->muted,
        s->counts[0],s->counts[1],s->counts[2],s->counts[3],s->counts[4],
        contact[0],contact[1],s->drones,s->nearest_m,id);
    /* Never send a partial key or a line without its newline. */
    if(n<0 || (size_t)n>=len || n>=LS_AUX_WIRE_MAX) {buf[0]=0;return 0;}
    return n;
}
