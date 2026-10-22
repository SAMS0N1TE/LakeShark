/* Bounded JSON tokenization. All storage belongs to the PSRAM caller; no heap
 * allocation per token and no global cJSON allocator changes. */
typedef struct { unsigned start,end,next,count;char type; } cells_jtoken;
typedef struct { const char *s;size_t n,pos;cells_jtoken *t;unsigned used,cap; } cells_jparser;
static void cells_jspace(cells_jparser *p) { while(p->pos<p->n && (p->s[p->pos]==' ' || p->s[p->pos]=='\n' || p->s[p->pos]=='\r' || p->s[p->pos]=='\t'))p->pos++; }
static int cells_jvalue(cells_jparser *p,unsigned depth) {
    cells_jspace(p);if(depth>8 || p->pos==p->n || p->used==p->cap)return -1;
    unsigned at=p->used++;cells_jtoken *t=p->t+at;memset(t,0,sizeof(*t));char c=p->s[p->pos++];t->type=c;t->start=(unsigned)p->pos;
    if(c=='{' || c=='[') {
        cells_jspace(p);char close=c=='{'?'}':']';
        if(p->pos<p->n && p->s[p->pos]==close)p->pos++;
        else for(;;) {
            if(c=='{') {
                cells_jspace(p);if(p->pos==p->n || p->s[p->pos]!='"' || cells_jvalue(p,depth+1)<0)return -1;
                cells_jspace(p);if(p->pos==p->n || p->s[p->pos++]!=':')return -1;
            }
            if(cells_jvalue(p,depth+1)<0)return -1;
            t->count++;
            cells_jspace(p);if(p->pos==p->n)return -1;char sep=p->s[p->pos++];
            if(sep==close)break;
            if(sep!=',')return -1;
        }
    } else if(c=='"') {
        bool closed=false;
        while(p->pos<p->n) {
            unsigned char ch=(unsigned char)p->s[p->pos++];if(ch=='"') { closed=true;break; }
            if(ch<32)return -1;
            if(ch=='\\') {
                if(p->pos==p->n)return -1;
                char e=p->s[p->pos++];
                if(e=='u') { for(unsigned i=0;i<4;i++) { if(p->pos==p->n || !isxdigit((unsigned char)p->s[p->pos++]))return -1; } }
                else if(!strchr("\"\\/bfnrt",e))return -1;
            }
        }
        if(!closed)return -1;
        t->end=(unsigned)p->pos-1;
    } else if(c=='t' || c=='f' || c=='n') {
        const char *rest=c=='t'?"rue":c=='f'?"alse":"ull";size_t n=strlen(rest);
        if(n>p->n-p->pos || memcmp(p->s+p->pos,rest,n))return -1;
        p->pos+=n;
    } else {
        p->pos--;size_t start=p->pos;
        if(p->s[p->pos]=='-')p->pos++;
        if(p->pos==p->n)return -1;
        if(p->s[p->pos]=='0')p->pos++;
        else { if(p->s[p->pos]<'1' || p->s[p->pos]>'9')return -1;while(p->pos<p->n && isdigit((unsigned char)p->s[p->pos]))p->pos++; }
        if(p->pos<p->n && p->s[p->pos]=='.') { size_t first=++p->pos;while(p->pos<p->n && isdigit((unsigned char)p->s[p->pos]))p->pos++;if(first==p->pos)return -1; }
        if(p->pos<p->n && (p->s[p->pos]=='e' || p->s[p->pos]=='E')) { p->pos++;if(p->pos<p->n && (p->s[p->pos]=='+' || p->s[p->pos]=='-'))p->pos++;size_t first=p->pos;while(p->pos<p->n && isdigit((unsigned char)p->s[p->pos]))p->pos++;if(first==p->pos)return -1; }
        t->type='#';t->start=(unsigned)start;
    }
    if(t->type!='"')t->end=(unsigned)p->pos;
    t->next=p->used;return (int)at;
}
static bool cells_jparse(cells_jparser *p) { if(cells_jvalue(p,0)!=0)return false;cells_jspace(p);return p->pos==p->n; }
static bool cells_jstring(const cells_jparser *p,int at,char *out,size_t cap) {
    if(at<0 || p->t[at].type!='"' || !cap)return false;
    cells_jtoken t=p->t[at];size_t n=0;
    for(unsigned i=t.start;i<t.end;i++) {
        unsigned char c=(unsigned char)p->s[i];
        if(c=='\\') {
            c=(unsigned char)p->s[++i];
            if(c=='u') { unsigned v=0;for(unsigned j=0;j<4;j++) { unsigned char h=(unsigned char)p->s[++i];v=v*16+(h<='9'?h-'0':(h|32)-'a'+10); }if(v<32 || v>127)return false;c=(unsigned char)v; }
            else if(c=='b' || c=='f' || c=='n' || c=='r' || c=='t')return false;
        }
        if(c<32 || c==127 || n+1>=cap)return false;
        out[n++]=(char)c;
    }
    out[n]=0;return true;
}
static int cells_jget(const cells_jparser *p,int object,const char *key) {
    if(object<0 || p->t[object].type!='{')return -1;
    unsigned at=(unsigned)object+1;
    for(unsigned i=0;i<p->t[object].count;i++) {
        char name[64];int value=(int)at+1;
        if(cells_jstring(p,(int)at,name,sizeof(name)) && !strcmp(name,key))return value;
        at=p->t[value].next;
    }return -1;
}
static bool cells_jnumber(const cells_jparser *p,int at,double *out) {
    if(at<0 || p->t[at].type!='#')return false;
    char *end;double value=strtod(p->s+p->t[at].start,&end);
    if(!isfinite(value) || end!=p->s+p->t[at].end)return false;
    *out=value;return true;
}
