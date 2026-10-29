#include "ls_map_share.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "ls_textpack.h"

static bool place(double lat,double lon)
{
    return isfinite(lat) && isfinite(lon) && fabs(lat)<=85 && fabs(lon)<=180;
}
static bool name_ok(const char *s)
{
    size_t n=0;
    if(!s || !*s) return false;
    for(;n<LS_MARK_NAME && s[n];n++)
        if(s[n]<32 || s[n]>126 || s[n]=='[' || s[n]==']') return false;
    return n<LS_MARK_NAME && s[0]!=' ' && s[n-1]!=' ';
}
int ls_share_parts(const ls_share_item_t *s)
{
    if(!s || !name_ok(s->name) || s->n<1 || s->n>LS_SKETCH_PTS ||
        (!s->line && s->n!=1) || (s->line && s->n<2)) return 0;
    for(int i=0;i<s->n;i++) if(!place(s->lat[i],s->lon[i])) return 0;
    return (s->n+LS_SHARE_PART_POINTS-1)/LS_SHARE_PART_POINTS;
}
bool ls_share_encode(const ls_share_item_t *s,unsigned part,char *out,size_t cap)
{
    int total=ls_share_parts(s);
    if(!out || !cap) return false;
    out[0]=0;
    if(!total || part<1 || part>(unsigned)total) return false;
    static EXT_RAM_BSS_ATTR char buf[LS_SHARE_TEXT+1];
    int n=LS_SNPRINTF(t_ls_map_share_s_s_ls1_08x, buf,sizeof(buf),"%s %s [LS1 %08x %u/%d] ",
        s->line ? "RTE" : "WPT",s->name,(unsigned)s->id,part,total);
    int start=(part-1)*LS_SHARE_PART_POINTS;
    for(int i=start;i<s->n && i<start+LS_SHARE_PART_POINTS;i++) {
        int k=snprintf(buf+n,sizeof(buf)-(size_t)n,"%s%.5f,%.5f",
            i==start ? "" : ";",s->lat[i],s->lon[i]);
        if(k<0 || k>=(int)sizeof(buf)-n) return false;
        n+=k;
    }
    if((size_t)n>=cap) return false;
    memcpy(out,buf,(size_t)n+1);return true;
}
/* Decimal grammar deliberately excludes exponents, NaN, whitespace and hex. */
static bool coord(const char **p,double *out,int limit)
{
    const char *s=*p;int sign=1,whole=0,frac=0,digits=0;
    if(*s=='-') { sign=-1;s++; }
    while(*s>='0' && *s<='9') {
        if(++digits>3) return false;
        whole=whole*10+*s++-'0';
    }
    if(!digits || *s++!='.') return false;
    for(int i=0;i<5;i++) {
        if(*s<'0' || *s>'9') return false;
        frac=frac*10+*s++-'0';
    }
    if(whole>limit || (whole==limit && frac)) return false;
    *out=sign*(whole+frac/1e5);*p=s;return true;
}
bool ls_share_decode(const char *text,ls_share_part_t *out)
{
    if(!text || !out) return false;
    size_t len=0;
    while(len<=LS_SHARE_TEXT && text[len]) len++;
    if(len>LS_SHARE_TEXT || len<4) return false;
    bool line=!strncmp(text,"RTE ",4);
    if(!line && strncmp(text,"WPT ",4)) return false;
    const char *tag=strstr(text+4," [LS1 ");
    if(!tag || tag-text-4<1 || tag-text-4>=LS_MARK_NAME) return false;
    memset(out,0,sizeof(*out));out->item.line=line;
    memcpy(out->item.name,text+4,(size_t)(tag-text-4));
    if(!name_ok(out->item.name)) return false;
    const char *p=tag+6;
    for(int i=0;i<8;i++) {
        char c=*p++;unsigned v;
        if(c>='0' && c<='9') v=c-'0';
        else if(c>='a' && c<='f') v=c-'a'+10;
        else return false;
        out->item.id=(out->item.id<<4)|v;
    }
    if(*p++!=' ') return false;
    unsigned *nums[]={&out->part,&out->total};
    for(int i=0;i<2;i++) {
        if(*p<'1' || *p>'9') return false;
        *nums[i]=*p++-'0';
        if(*p>='0' && *p<='9') *nums[i]=*nums[i]*10+*p++-'0';
        if(*p++!=(i ? ']' : '/')) return false;
    }
    if(*p++!=' ' || !out->part || out->part>out->total || out->total>LS_SHARE_PARTS ||
       (!line && (out->part!=1 || out->total!=1))) return false;
    do {
        int i=out->item.n;
        if(i>=LS_SHARE_PART_POINTS || !coord(&p,&out->item.lat[i],85) || *p++!=',' ||
            !coord(&p,&out->item.lon[i],180)) return false;
        out->item.n++;
        if(!*p) break;
        if(*p++!=';' || !*p) return false;
    } while(1);
    if(!line && out->item.n!=1) return false;
    if(line && out->total==1 && out->item.n<2) return false;
    return out->part==out->total || out->item.n==LS_SHARE_PART_POINTS;
}
void ls_share_expire(ls_share_rx_t *rx,uint32_t now)
{
    if(!rx) return;
    for(int i=0;i<LS_SHARE_SLOTS;i++)
        if(rx->slots[i].used && (uint32_t)(now-rx->slots[i].started)>=LS_SHARE_TIMEOUT)
            memset(&rx->slots[i],0,sizeof(rx->slots[i]));
    for(int i=0;i<LS_SHARE_RECENT;i++)
        if(rx->recent[i].used && (uint32_t)(now-rx->recent[i].stamp)>=LS_SHARE_TIMEOUT)
            rx->recent[i].used=false;
}
int ls_share_receive(ls_share_rx_t *rx,const char *source,const char *text,
                     uint32_t now,ls_share_item_t *out)
{
    /* UI-owned scratch, not a 1 KB task-stack temporary. */
    static EXT_RAM_BSS_ATTR ls_share_part_t p;
    if(!rx || !out || !source || !*source || strlen(source)>=sizeof(rx->slots[0].source) ||
       !ls_share_decode(text,&p)) return -1;
    ls_share_expire(rx,now);
    for(int i=0;i<LS_SHARE_RECENT;i++)
        if(rx->recent[i].used && rx->recent[i].id==p.item.id && !strcmp(rx->recent[i].source,source)) return 0;
    ls_share_slot_t *s=NULL,*empty=NULL;
    for(int i=0;i<LS_SHARE_SLOTS;i++) {
        ls_share_slot_t *t=&rx->slots[i];
        if(!t->used) { if(!empty) empty=t;continue; }
        if(t->item.id==p.item.id && !strcmp(t->source,source)) { s=t;break; }
    }
    if(!s) {
        if(!empty) return -1;
        s=empty;memset(s,0,sizeof(*s));s->used=true;s->started=now;
        s->total=p.total;s->item.line=p.item.line;s->item.id=p.item.id;
        strcpy(s->item.name,p.item.name);strcpy(s->source,source);
    }
    if(s->total!=p.total || s->item.line!=p.item.line || strcmp(s->item.name,p.item.name)) return -1;
    unsigned offset=(p.part-1)*LS_SHARE_PART_POINTS,bit=1u<<(p.part-1);
    if(s->mask&bit) {
        if(s->count[p.part-1]!=p.item.n) return -1;
        for(int i=0;i<p.item.n;i++)
            if(s->item.lat[offset+i]!=p.item.lat[i] || s->item.lon[offset+i]!=p.item.lon[i]) return -1;
        return 0;
    }
    for(int i=0;i<p.item.n;i++) { s->item.lat[offset+i]=p.item.lat[i];s->item.lon[offset+i]=p.item.lon[i]; }
    s->count[p.part-1]=p.item.n;s->mask|=bit;
    if(p.part==p.total) s->item.n=offset+p.item.n;
    if(s->mask!=(1u<<s->total)-1) return 0;
    *out=s->item;
    ls_share_seen_t *seen=&rx->recent[rx->next_recent++%LS_SHARE_RECENT];
    seen->used=true;seen->stamp=now;seen->id=s->item.id;strcpy(seen->source,source);
    memset(s,0,sizeof(*s));return 1;
}
int ls_share_import(const ls_share_item_t *s,const char *sender)
{
    if(!ls_share_parts(s)) return -1;
    int i;
    if(!s->line) i=ls_marks_add(s->lat[0],s->lon[0],LS_MARK_FLAG,s->name);
    else i=ls_sketch_import(s->lat,s->lon,s->n,s->name);
    if(i>=0) ls_marks_received(s->line,i,sender);
    return i;
}
