#include "lap_json.h"
#include <string.h>
typedef struct { const char *s; size_t n,p,used,cap; lap_json_token *t; int error; } parser;
static void ws(parser *p) { while (p->p<p->n && strchr(" \t\r\n",p->s[p->p]) && p->s[p->p]) p->p++; }
static bool digit(char c) { return c>='0' && c<='9'; }
static int token(parser *p, lap_json_kind k, size_t start, int parent) {
    if (p->used>=p->cap) { p->error=-2; return -1; }
    int i=(int)p->used++; p->t[i]=(lap_json_token){k,start,0,parent}; return i;
}
static bool string(parser *p) {
    if (p->p>=p->n || p->s[p->p++]!='"') return false;
    while (p->p<p->n) {
        unsigned char c=p->s[p->p++];
        if (c=='"') return true;
        if (c<32) return false;
        if (c=='\\') {
            if (p->p==p->n) return false;
            c=p->s[p->p++];
            if (c=='u') { for (int i=0;i<4;i++) {
                if (p->p==p->n || !p->s[p->p] || !strchr("0123456789abcdefABCDEF",p->s[p->p++])) return false;
            }} else if (!c || !strchr("\"\\/bfnrt",c)) return false;
        }
    }
    return false;
}
static bool value(parser *p, int parent, unsigned depth) {
    ws(p); if (depth>32 || p->p==p->n) return false;
    size_t start=p->p; char c=p->s[p->p];
    lap_json_kind k=c=='{'?LAP_JSON_OBJECT:c=='['?LAP_JSON_ARRAY:c=='"'?LAP_JSON_STRING:
        c=='t'||c=='f'?LAP_JSON_BOOL:c=='n'?LAP_JSON_NULL:LAP_JSON_NUMBER;
    int i=token(p,k,start,parent); if (i<0) return false;
    if (c=='{' || c=='[') {
        char end=c=='{'?'}':']'; p->p++; ws(p);
        if (p->p<p->n && p->s[p->p]==end) p->p++;
        else for (;;) {
            if (c=='{') {
                ws(p); size_t key=p->p;
                if (!string(p)) return false;
                int j=token(p,LAP_JSON_STRING,key+1,i); if(j<0) return false;
                p->t[j].len=p->p-key-2; ws(p);
                if (p->p==p->n || p->s[p->p++]!=':') return false;
            }
            if (!value(p,i,depth+1)) return false;
            ws(p); if(p->p==p->n) return false;
            char sep=p->s[p->p++]; if(sep==end) break; if(sep!=',') return false;
        }
    } else if (c=='"') {
        if (!string(p)) return false;
        p->t[i].start++; p->t[i].len=p->p-start-2; return true;
    } else if (k==LAP_JSON_BOOL || k==LAP_JSON_NULL) {
        const char *lit=c=='t'?"true":c=='f'?"false":"null"; size_t n=strlen(lit);
        if (p->n-p->p<n || memcmp(p->s+p->p,lit,n)) return false;
        p->p+=n;
    } else {
        if(c=='-') p->p++;
        if(p->p==p->n) return false;
        if(p->s[p->p]=='0') p->p++;
        else { if(!digit(p->s[p->p]) || p->s[p->p]=='0') return false; while(p->p<p->n && digit(p->s[p->p])) p->p++; }
        if(p->p<p->n && p->s[p->p]=='.') { p->p++; size_t a=p->p; while(p->p<p->n && digit(p->s[p->p])) p->p++; if(a==p->p) return false; }
        if(p->p<p->n && (p->s[p->p]=='e'||p->s[p->p]=='E')) {
            p->p++; if(p->p<p->n && (p->s[p->p]=='+'||p->s[p->p]=='-')) p->p++;
            size_t a=p->p; while(p->p<p->n && digit(p->s[p->p])) p->p++; if(a==p->p) return false;
        }
    }
    p->t[i].len=p->p-start; return true;
}
int lap_json_read(const char *s, size_t n, lap_json_token *t, size_t cap) {
    if(!s || (!t && cap) || cap>0x7fffffff) return -1;
    parser p={.s=s,.n=n,.t=t,.cap=cap};
    if(!value(&p,-1,0)) return p.error?p.error:-1;
    ws(&p); return p.p==n?(int)p.used:-1;
}
void lap_json_writer_init(lap_json_writer *w, char *data, size_t cap) {
    *w=(lap_json_writer){.data=data,.capacity=cap,.failed=!data||!cap}; if(!w->failed) data[0]=0;
}
bool lap_json_raw(lap_json_writer *w, const char *s, size_t n) {
    if(w->failed || !s || n>=w->capacity-w->len) { w->failed=true; return false; }
    memcpy(w->data+w->len,s,n); w->len+=n; w->data[w->len]=0; return true;
}
bool lap_json_string(lap_json_writer *w, const char *s, size_t n) {
    const char hex[]="0123456789abcdef";
    if(!s) { w->failed=true; return false; }
    lap_json_raw(w,"\"",1);
    for(size_t i=0;i<n && !w->failed;i++) {
        unsigned char c=s[i]; char esc[6]={'\\','u','0','0',hex[c>>4],hex[c&15]};
        if(c<32) lap_json_raw(w,esc,6);
        else if(c=='"'||c=='\\') { char pair[2]={'\\',(char)c}; lap_json_raw(w,pair,2); }
        else lap_json_raw(w,s+i,1);
    }
    return lap_json_raw(w,"\"",1);
}
