#include "lap_csv.h"
#include <string.h>
void lap_csv_reader_init(lap_csv_reader *r, const char *s, size_t n) { *r=(lap_csv_reader){s,n,0,false}; }
int lap_csv_next(lap_csv_reader *r, char *out, size_t cap, bool *end) {
    if(!r || !r->data || !out || !cap || !end) return -1;
    if(r->pos==r->len && !r->trailing) return 0;
    size_t p=r->pos, n=0; bool quoted=p<r->len && r->data[p]=='"', closed=!quoted;
    if(quoted) p++;
    while(p<r->len) {
        char c=r->data[p++];
        if(quoted && c=='"') {
            if(p<r->len && r->data[p]=='"') p++;
            else { closed=true; break; }
        } else if(!quoted && (c==','||c=='\r'||c=='\n')) { p--; break; }
        else if(!quoted && c=='"') return -1;
        if(!c || n+1>=cap) return -1;
        out[n++]=c;
    }
    if(!closed) return -1;
    bool row=true, trailing=false;
    if(p<r->len) {
        char c=r->data[p++];
        if(c==',') { row=false; trailing=true; }
        else if(c=='\r') { if(p<r->len && r->data[p]=='\n') p++; }
        else if(c!='\n') return -1;
    }
    out[n]=0; *end=row; r->pos=p; r->trailing=trailing; return 1;
}
bool lap_csv_write(char *out, size_t cap, size_t *used, const char *s, size_t n, bool end) {
    if(!out || !used || !s || *used>=cap) return false;
    size_t need=end?2:1; bool quote=false;
    for(size_t i=0;i<n;i++) { if(!s[i]) return false; if(strchr(",\r\n\"",s[i])) quote=true; if(s[i]=='"') need++; }
    if(n>cap-*used || need>cap-*used-n || (quote && cap-*used-n-need<2)) return false;
    need+=n+(quote?2:0); if(need>=cap-*used) return false;
    size_t p=*used; if(quote) out[p++]='"';
    for(size_t i=0;i<n;i++) { out[p++]=s[i]; if(s[i]=='"') out[p++]='"'; }
    if(quote) out[p++]='"';
    if(end) out[p++]='\r';
    out[p++]=end?'\n':',';
    out[p]=0; *used=p; return true;
}
