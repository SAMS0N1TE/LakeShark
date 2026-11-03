#include "lakeshark_app.h"
#include "lap_loop.h"
#include "lap_json.h"
#include "lap_csv.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    lap_loop_t loop; float alpha;
    assert(!lap_loop_init(&loop, 0, 4, 0));
    assert(lap_loop_init(&loop, 60, 8, 0));
    unsigned ticks=0;
    for (unsigned ms=1; ms<=1000; ms++) ticks+=lap_loop_advance(&loop, ms, &alpha);
    assert(ticks==60 && alpha==0);
    assert(lap_loop_advance(&loop, 5000, &alpha)==8);
    lap_loop_focus(&loop, false, 5000, 2, 2);
    assert(!loop.paused);
    lap_loop_focus(&loop, false, 5000, 2, 3);
    assert(lap_loop_advance(&loop, 9000, &alpha)==0 && alpha==0);
    lap_loop_focus(&loop, true, 9000, 2, 3);
    assert(lap_loop_advance(&loop, 9010, &alpha)==0 && alpha>0.59f && alpha<0.61f);
    lap_loop_init(&loop, 100, 8, 0xfffffff0u);
    assert(lap_loop_advance(&loop, 4, &alpha)==2);
#if defined(LAP_API_23)
    lap_input_event_t event={.kind=LAP_INPUT_FOCUS_LOST};
    lap_loop_event(&loop,&event,10);
    assert(loop.paused && lap_loop_advance(&loop,1000,&alpha)==0);
    event.kind=LAP_INPUT_FOCUS_GAINED;
    lap_loop_event(&loop,&event,1000);
    assert(!loop.paused && lap_loop_advance(&loop,1020,&alpha)==2);
#endif

    lap_json_token tokens[32];
    const char *good="{\"a\":[1,-2.5e+2,true,null,\"x\\u1234\"]}";
    int count=lap_json_read(good,strlen(good),tokens,32);
    assert(count==8 && tokens[0].kind==LAP_JSON_OBJECT && tokens[3].parent==2);
    const char *bad[]={"", "{", "[1,]", "{\"a\" 2}", "01", "1.", "1e+", "true false", "\"x\n\"", "\"\\uXYZ0\"", "[null,,0]", "{\"x\":true,}"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(lap_json_read(bad[i],strlen(bad[i]),tokens,32)==-1);
    assert(lap_json_read(good,strlen(good),tokens,1)==-2);
    char deep[81]; memset(deep,'[',40); memset(deep+40,']',40); deep[80]=0;
    assert(lap_json_read(deep,80,tokens,32)<0);
    char json[64]; lap_json_writer w;
    lap_json_writer_init(&w,json,sizeof(json));
    assert(lap_json_raw(&w,"[",1)); assert(lap_json_string(&w,"a\"\n\\",4)); assert(lap_json_raw(&w,"]",1));
    assert(lap_json_read(json,w.len,tokens,32)==2);
    lap_json_writer_init(&w,json,4); assert(!lap_json_string(&w,"long",4) && w.failed);

    const char *csv="\"a,b\",\"line\r\nnext\",\"say \"\"hi\"\"\"\r\n,";
    lap_csv_reader r; char field[64]; bool end;
    lap_csv_reader_init(&r,csv,strlen(csv));
    assert(lap_csv_next(&r,field,sizeof(field),&end)==1 && !end && !strcmp(field,"a,b"));
    assert(lap_csv_next(&r,field,sizeof(field),&end)==1 && !end && !strcmp(field,"line\r\nnext"));
    assert(lap_csv_next(&r,field,sizeof(field),&end)==1 && end && !strcmp(field,"say \"hi\""));
    assert(lap_csv_next(&r,field,sizeof(field),&end)==1 && !end && !*field);
    assert(lap_csv_next(&r,field,sizeof(field),&end)==1 && end && !*field);
    assert(lap_csv_next(&r,field,sizeof(field),&end)==0);
    lap_csv_reader_init(&r,"\"broken",7); assert(lap_csv_next(&r,field,64,&end)==-1 && r.pos==0);
    lap_csv_reader_init(&r,"large",5); assert(lap_csv_next(&r,field,3,&end)==-1 && r.pos==0);
    char out[64]=""; size_t used=0;
    assert(lap_csv_write(out,64,&used,"a,b",3,false));
    assert(lap_csv_write(out,64,&used,"a\"b",3,true));
    assert(!strcmp(out,"\"a,b\",\"a\"\"b\"\r\n"));
    size_t old=used; assert(!lap_csv_write(out,used+1,&used,"x",1,true) && used==old);
    puts("PASS: SDK loop, JSON and CSV boundary/round-trip tests");
}
