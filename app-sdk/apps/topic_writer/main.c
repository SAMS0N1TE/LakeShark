#include "lakeshark_app.h"
#include <stdio.h>
static char status[96]="Open TOPIC READER once first, then press Enter here.";
static bool key(lap_key_t k,char ch) {
    (void)ch; if(k!=LAP_KEY_ENTER) return false;
    int n=lap_ipc_publish("demo.greeting",1,"Hello from the writer",21);
    snprintf(status,sizeof(status),"Delivered to %d mailbox(es). Reopen TOPIC READER.",n); return true;
}
static void draw(void) {
    lap_clear(LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,1,"TOPIC WRITER",LAP_ATTR(LAP_CYAN|LAP_BRIGHT,LAP_BLACK));
    lap_put_str(1,3,status,LAP_ATTR(LAP_WHITE,LAP_BLACK));
    lap_put_str(1,5,"Enter sends schema 1 on demo.greeting",LAP_ATTR(LAP_YELLOW,LAP_BLACK));
    lap_put_str(1,7,"One resident service; messages work across app switches.",LAP_ATTR(LAP_WHITE,LAP_BLACK));
}
LAP_APP(.id="topic_writer",.name="TOPIC WRITER",.sub="Publish a copied message",.version="1.0",.author="LakeShark",
        .icon=LAP_ICON_CHIP,.color=LAP_YELLOW,.draw=draw,.key=key);
