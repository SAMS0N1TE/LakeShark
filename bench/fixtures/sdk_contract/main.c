#include "lakeshark_app.h"
#include <string.h>
#include "lap_loop.h"
#include "lap_json.h"
#include "lap_csv.h"
static lap_net_status_t net;
static lap_service_t service;
static unsigned persisted, ticks;
static bool ready;
static int job=-1, phase, modal;
static unsigned input_frames;
static char sd_data[8];
static void tick(void) {
    ticks++;
    if (!lap_screen_open()) lap_log("fixture hidden tick %u", ticks);
}
static void stopped(void) { lap_log("fixture stopped"); }
static void enter(void) {
    if (ready) { lap_log("fixture reopened"); return; }
    ready=true;
    lap_loop_t loop; float alpha;
    lap_json_token tokens[4];
    char csv[16]=""; size_t used=0;
    lap_loop_init(&loop,60,4,0);
    lap_log("fixture SDK helpers %d", lap_loop_advance(&loop,20,&alpha)==1 &&
        lap_json_read("[1]",3,tokens,4)==2 && lap_csv_write(csv,sizeof(csv),&used,"a,b",3,true));
    int fd=lap_file_open("counter", "r");
    if (fd>=0) { lap_file_read(fd,&persisted,sizeof(persisted)); lap_file_close(fd); }
    lap_log("fixture persisted %u",persisted);
    lap_net_status(&net);
    lap_log("fixture network allowed %d",net.allowed);
    lap_net_status_t *heap=calloc(1,sizeof(*heap));
    if (heap) { lap_net_status(heap); lap_log("fixture tracked heap %d",heap->allowed); free(heap); }
    service=(lap_service_t){tick,stopped,50};
    lap_service_start(&service);
}
static void leave(void) {
    persisted++;
    int fd=lap_file_open("counter", "w");
    lap_file_write(fd,&persisted,sizeof(persisted)); lap_file_close(fd);
}
static void draw(void) {
    if (input_frames) {
        lap_input_state_t state;
        lap_input_event_t event;
        lap_input_state(&state);
        lap_log("fixture held %d %d buttons %u xy %d %d", lap_key_down(LAP_KEY_CHAR('w')),
                lap_key_down(LAP_KEY_LEFT), state.buttons, state.x, state.y);
        while (lap_input_next(&event)>0)
            if (event.kind==LAP_INPUT_WHEEL || event.kind==LAP_INPUT_RESIZE)
                lap_log("fixture input %u wheel %d size %d %d",event.kind,event.wheel,event.width,event.height);
        input_frames--;
    }
    if (modal>0) {
        char text[64]; int value=-1;
        int result=lap_dialog_poll(modal,text,sizeof(text),&value);
        if (result>0) { lap_log("fixture dialog %d value %d text %s",result,value,value<0 && result==1?text:""); modal=0; }
    }
    if (!phase) { job=lap_sd_write("/sdcard/contract.bin",0,"hello",5); phase=1; }
    else if (phase==1 && lap_sd_poll(job)!=LAP_SD_PENDING) {
        job=lap_sd_read("/sdcard/contract.bin",0,sd_data,5); phase=2;
    } else if (phase==2) {
        int n=lap_sd_poll(job);
        if (n!=LAP_SD_PENDING) { lap_log("fixture sd roundtrip %d",n==5 && !memcmp(sd_data,"hello",5)); phase=3; }
    }
}
static bool key(lap_key_t k, char ch) {
    if (k!=LAP_KEY_CHAR) return false;
    if (ch=='p') { lap_net_status_t stack; lap_log("fixture stack sync %d",lap_net_status(&stack)==0); return true; }
    if (ch=='r') { char stack[8]; lap_sd_read("/sdcard/contract.bin",0,stack,sizeof(stack)); return true; }
    if (ch=='z') { lap_net_status((lap_net_status_t *)1); return true; }
    if (ch=='i') { input_frames=32; return true; }
    if (ch=='h') { lap_caps_t caps; lap_caps(&caps,sizeof(caps)); lap_log("fixture caps %u radio %u",(caps.hardware&255u),caps.radio_present); return true; }
    if (ch=='t') { modal=lap_text_input("Name","Ada",40,0); lap_log("fixture modal %d",modal>0); return true; }
    if (ch=='c') { modal=lap_confirm("Continue?"); return true; }
    if (ch=='o') { const char *items[]={"A","B"}; modal=lap_choose(items,2); return true; }
    if (ch=='s') { char stack[]="checkpoint"; lap_log("fixture save %d",lap_save_write("progress",stack,sizeof(stack),7)>=0); return true; }
    if (ch=='v') { char stack[]="42"; lap_log("fixture setting %d",lap_settings_set("volume",stack,sizeof(stack))>=0); return true; }
    if (ch=='g') { char stack[32]; uint32_t schema=0;
        int n=lap_save_read("progress",stack,sizeof(stack),&schema);
        lap_log("fixture load %d schema %u",n==11 && !strcmp(stack,"checkpoint"),schema);
        n=lap_settings_get("volume",stack,sizeof(stack)); lap_log("fixture preference %d",n==3 && !strcmp(stack,"42")); return true; }
    if (ch=='m') { lap_mesh_advert(); return true; }
    return false;
}
LAP_APP(.id="sdk_contract", .name="CONTRACT", .version="1.0", .author="SDK test",
        .flags=LAP_CAP_NETWORK|LAP_CAP_RADIO_RX|LAP_CAP_SERVICE,
        .enter=enter,.leave=leave,.draw=draw,.key=key);
