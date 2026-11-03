#include "lakeshark_app.h"
#include <stdio.h>
static char status[192]="Open TOPIC WRITER, send, then reopen here.";
static bool subscribed;
static void enter(void) { subscribed=lap_ipc_subscribe("demo.greeting")==0; }
static void draw(void) {
    lap_ipc_message_t m;
    while(lap_ipc_next(&m)>0) {
        if(m.type==1) snprintf(status,sizeof(status),"%s: %.*s (dropped %lu)",m.sender,m.data_len,m.data,(unsigned long)m.dropped);
    }
    lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,1,"TOPIC READER",LAP_ATTR(LAP_CYAN|LAP_BRIGHT,LAP_BLACK));
    lap_put_str(1,3,status,LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,5,subscribed?"Subscribed (mailbox survives app close)":"Unsubscribed",LAP_ATTR(LAP_YELLOW,LAP_BLACK));
    lap_put_str(1,7,"S subscribe  U unsubscribe and clear queued messages",LAP_ATTR(LAP_WHITE,LAP_BLACK));
}
static bool key(lap_key_t k,char ch) {
    (void)k;
    if(ch=='u'||ch=='U') { lap_ipc_unsubscribe("demo.greeting"); subscribed=false; return true; }
    if(ch=='s'||ch=='S') { enter(); return true; } return false;
}
LAP_APP(.id="topic_reader",.name="TOPIC READER",.sub="Bounded typed messages",.version="1.0",.author="LakeShark",
        .icon=LAP_ICON_CHIP,.color=LAP_CYAN,.enter=enter,.draw=draw,.key=key);
