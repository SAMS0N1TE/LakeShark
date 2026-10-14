#include "ls_test.h"
#include "ls_map_share.h"
#include <stdio.h>
#include <string.h>
static ls_share_rx_t rx;
static ls_share_item_t item,out;
static ls_share_part_t part;
static char text[LS_SHARE_TEXT+1],parts[LS_SHARE_PARTS][LS_SHARE_TEXT+1];
static void seed(int n) {
    memset(&rx,0,sizeof(rx));memset(&item,0,sizeof(item));
    item.line=n>1;item.n=n;item.id=0x1234abcd;strcpy(item.name,"Trail Camp");
    for(int i=0;i<n;i++) { item.lat[i]=43.12345+i*.00001;item.lon[i]=-71.54321-i*.00001; }
}
LS_CASE(marker_roundtrip_and_precision) {
    seed(1);LS_EQ_INT(ls_share_parts(&item),1);
    LS_CHECK(ls_share_encode(&item,1,text,sizeof(text)));
    LS_EQ_STR(text,"WPT Trail Camp [LS1 1234abcd 1/1] 43.12345,-71.54321");
    LS_CHECK(ls_share_decode(text,&part));LS_CHECK(!part.item.line);
    LS_EQ_UINT(part.item.id,item.id);LS_EQ_STR(part.item.name,item.name);
    LS_NEAR(part.item.lat[0],item.lat[0],.000001);
    LS_NEAR(part.item.lon[0],item.lon[0],.000001);
    LS_EQ_INT(ls_share_receive(&rx,"D:abc",text,1,&out),1);
    LS_EQ_INT(ls_share_receive(&rx,"D:abc",text,2,&out),0);
}
LS_CASE(three_parts_out_of_order_and_timeout) {
    seed(12);LS_EQ_INT(ls_share_parts(&item),3);
    for(int i=0;i<3;i++) LS_CHECK(ls_share_encode(&item,i+1,parts[i],sizeof(parts[i])));
    LS_EQ_INT(ls_share_receive(&rx,"C0:KB1QWE",parts[2],10,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"C0:KB1QWE",parts[0],11,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"C0:KB1QWE",parts[0],12,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"C0:KB1QWE",parts[1],13,&out),1);
    LS_EQ_INT(out.n,12);
    for(int i=0;i<12;i++) { LS_NEAR(out.lat[i],item.lat[i],.000001);LS_NEAR(out.lon[i],item.lon[i],.000001); }
    memset(&rx,0,sizeof(rx));
    LS_EQ_INT(ls_share_receive(&rx,"A",parts[0],0,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"A",parts[1],LS_SHARE_TIMEOUT,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"A",parts[2],LS_SHARE_TIMEOUT+1,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"A",parts[0],LS_SHARE_TIMEOUT+2,&out),1);
}
LS_CASE(maximum_route_and_buffer_limits) {
    seed(LS_SKETCH_PTS);memset(item.name,'X',LS_MARK_NAME-1);item.name[LS_MARK_NAME-1]=0;
    for(int i=0;i<item.n;i++) { item.lat[i]=-85;item.lon[i]=-180; }
    for(int i=1;i<=LS_SHARE_PARTS;i++) {
        LS_CHECK(ls_share_encode(&item,i,text,sizeof(text)));LS_CHECK(strlen(text)<=LS_SHARE_TEXT);
        LS_CHECK(ls_share_decode(text,&part));
        LS_EQ_INT(ls_share_receive(&rx,"A",text,1,&out),i==LS_SHARE_PARTS ? 1 : 0);
    }
    LS_EQ_INT(out.n,LS_SKETCH_PTS);LS_NEAR(out.lon[63],-180,0);
    LS_CHECK(!ls_share_encode(&item,1,text,8));LS_EQ_STR(text,"");
    LS_CHECK(!ls_share_encode(&item,0,text,sizeof(text)));
    LS_CHECK(!ls_share_encode(&item,LS_SHARE_PARTS+1,text,sizeof(text)));
    item.lat[0]=NAN;LS_EQ_INT(ls_share_parts(&item),0);
}
LS_CASE(malformed_packets_never_import) {
    const char *bad[]={"", "WPT X [LS2 1234abcd 1/1] 43.00000,-71.00000",
        "WPT X [LS1 1234abcd 0/1] 43.00000,-71.00000",
        "WPT X [LS1 1234abcd 1/17] 43.00000,-71.00000",
        "WPT X [LS1 1234abcd 1/1] nan,-71.00000",
        "WPT X [LS1 1234abcd 1/1] 43e00000,-71.00000",
        "WPT X [LS1 1234abcd 1/1] 85.00001,-71.00000",
        "WPT X [LS1 1234abcd 1/1] 43.00000,180.00001",
        "WPT X [LS1 1234abcd 1/1] 43.000000,-71.00000",
        "WPT X [LS1 1234abcd 1/1] 43.00000,-71.00000;",
        "WPT X [LS1 1234abcd 1/1] 43.00000,-71.00000 junk",
        "WPT X [LS1 1234abcd 1/1] 43.00000,-71.00000;44.00000,-72.00000",
        "RTE X [LS1 1234abcd 1/2] 43.00000,-71.00000",
        "RTE X [LS1 1234abcd 1/1] 43.00000,-71.00000",
        "WPT X [LS1 1234abcd 1/1] 43.00000,-71.00000\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) LS_CHECK(!ls_share_decode(bad[i],&part));
    seed(1);LS_CHECK(ls_share_encode(&item,1,text,sizeof(text)));
    /* Every truncated prefix is invalid, including inside the tag. */
    size_t n=strlen(text);
    for(size_t i=0;i<n;i++) { char c=text[i];text[i]=0;LS_CHECK(!ls_share_decode(text,&part));text[i]=c; }
    char large[LS_SHARE_TEXT+2];memset(large,'X',sizeof(large));large[sizeof(large)-1]=0;
    LS_CHECK(!ls_share_decode(large,&part));
}
LS_CASE(source_isolation_conflicts_capacity_and_clock_wrap) {
    seed(5);ls_share_encode(&item,1,parts[0],sizeof(parts[0]));ls_share_encode(&item,2,parts[1],sizeof(parts[1]));
    LS_EQ_INT(ls_share_receive(&rx,"D:A",parts[0],0,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"C0:A",parts[1],0,&out),0);
    item.lat[0]=44;ls_share_encode(&item,1,text,sizeof(text));
    LS_EQ_INT(ls_share_receive(&rx,"D:A",text,2,&out),-1);
    LS_EQ_INT(ls_share_receive(&rx,"B",text,2,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"C",text,2,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"D",text,2,&out),-1);
    LS_EQ_INT(ls_share_receive(&rx,"D:A",parts[1],3,&out),1);
    LS_EQ_INT(ls_share_receive(&rx,"D",text,3,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"D:A",parts[0],4,&out),0);
    memset(&rx,0,sizeof(rx));
    LS_EQ_INT(ls_share_receive(&rx,"A",text,UINT32_MAX-10,&out),0);
    ls_share_expire(&rx,170);LS_CHECK(!rx.slots[0].used);
}
static void fresh(void) {
    remove("share_marks.txt");remove("share_marks.txt.bak");remove("share_marks.txt.new");
    ls_marks_use_file("share_marks.txt");
}
LS_CASE(received_lifecycle_does_not_autosave) {
    fresh();seed(1);int m=ls_share_import(&item,"KB1QWE");LS_EQ_INT(m,0);
    LS_CHECK(ls_marks_at(m)->origin.received);LS_EQ_STR(ls_marks_at(m)->origin.sender,"KB1QWE");
    LS_CHECK(ls_marks_save());ls_marks_reload();LS_EQ_INT(ls_marks_count(),0);
    m=ls_share_import(&item,"KB1QWE");LS_CHECK(ls_marks_accept(false,m));LS_CHECK(ls_marks_save());
    ls_marks_reload();LS_EQ_INT(ls_marks_count(),1);LS_CHECK(!ls_marks_at(0)->origin.received);
    LS_EQ_STR(ls_marks_at(0)->origin.sender,"KB1QWE");
    LS_CHECK(ls_marks_delete(0));LS_CHECK(ls_marks_save());ls_marks_reload();LS_EQ_INT(ls_marks_count(),0);
    ls_sketch_begin();ls_sketch_add_point(1,2);
    seed(9);int line=ls_share_import(&item,"KB1QWE");LS_EQ_INT(line,0);
    LS_CHECK(ls_sketch_drawing());LS_EQ_INT(ls_sketch_open()->n,1);ls_sketch_cancel();
    LS_CHECK(ls_sketch_at(line)->origin.received);LS_EQ_INT(ls_sketch_at(line)->n,9);
    LS_CHECK(ls_marks_accept(true,line));LS_CHECK(ls_marks_save());ls_marks_reload();
    LS_EQ_STR(ls_sketch_at(0)->origin.sender,"KB1QWE");LS_EQ_INT(ls_sketch_at(0)->n,9);
    LS_CHECK(ls_sketch_delete(0));LS_CHECK(ls_marks_save());ls_marks_reload();LS_EQ_INT(ls_sketch_count(),0);
    fresh();
}

LS_CASE(completed_markers_release_assembly_slots_and_keep_duplicate_cache) {
    seed(1);
    for(int i=0;i<LS_SHARE_RECENT;i++) {
        item.id=i;ls_share_encode(&item,1,text,sizeof(text));
        LS_EQ_INT(ls_share_receive(&rx,"A",text,1,&out),1);
    }
    item.id=0;ls_share_encode(&item,1,text,sizeof(text));
    LS_EQ_INT(ls_share_receive(&rx,"A",text,2,&out),0);
    LS_EQ_INT(ls_share_receive(&rx,"A",text,LS_SHARE_TIMEOUT+1,&out),1);
}
