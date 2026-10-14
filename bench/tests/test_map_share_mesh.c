#include "ls_test.h"
#include "../../components/apps/tui/screens/ext/scr_mesh.c"
static ls_mesh_msg_t messages[5];
static int count,received,polls;
static uint32_t seq,seen[5];
uint32_t ls_mesh_incoming_seq(void) { return seq; }
int ls_mesh_messages(ls_mesh_msg_t *out,int max) { int n=count<max ? count:max;memcpy(out,messages,n*sizeof(*out));return n; }
void ls_scr_map_share_poll(void) { polls++; }
bool ls_scr_map_receive(const ls_mesh_msg_t *m,ls_notice_t *n) {
    seen[received++]=m->id;snprintf(n->body,sizeof(n->body),"route complete");return received==3;
}
int ls_tui_screen_index_of(const ls_tui_screen_t *s) { (void)s;return 5; }
void audio_events_mesh_message(const char *text,bool direct) { (void)text;(void)direct; }
LS_CASE(mesh_notice_feeds_every_live_part_once_and_ignores_own_text) {
    ls_notice_t n={0};s_told_init=false;count=1;messages[0].id=10;seq=1;
    LS_CHECK(!ls_scr_mesh_notice(&n));LS_EQ_INT(received,0);
    for(int i=1;i<5;i++) { messages[i].id=10+i;strcpy(messages[i].text,"part"); }
    messages[2].mine=true;count=5;seq=4;
    LS_CHECK(ls_scr_mesh_notice(&n));LS_EQ_INT(received,3);
    LS_EQ_UINT(seen[0],11);LS_EQ_UINT(seen[1],13);LS_EQ_UINT(seen[2],14);
    LS_EQ_STR(n.body,"route complete");LS_CHECK(!ls_scr_mesh_notice(&n));LS_EQ_INT(received,3);LS_EQ_INT(polls,3);
}
