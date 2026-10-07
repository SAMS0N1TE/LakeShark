/* Production RS41 list, detail, options, recovery and MAP through the native simulator. */
#include "ls_test.h"
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif
static char output[32768];
static bool seed_empty;
static void run(const char *app,const char *options,bool landscape)
{
#ifdef _WIN32
    _putenv_s("LSSIM_RS41", "1");
    _putenv_s("LSSIM_LORA", "lr2021");
    _putenv_s("LSSIM_RS41_EMPTY", seed_empty ? "1" : "");
#else
    setenv("LSSIM_RS41", "1", 1);
    setenv("LSSIM_LORA", "lr2021", 1);
    if (seed_empty) setenv("LSSIM_RS41_EMPTY", "1", 1); else unsetenv("LSSIM_RS41_EMPTY");
#endif
    char command[1200];
    snprintf(command,sizeof(command),"\"%s\" %s -o \"%s\" -d -f 5 %s %s 2>&1",
        LSSIM_EXE,app,RS41_RENDER,landscape ? "-l" : "",options);
#ifdef _WIN32
    /* cmd.exe resolves the program path with native separators. */
    for(char *c=command+1;*c && *c!='"';c++) if(*c=='/') *c='\\';
#endif
#ifdef _WIN32
    char wrapped[1204];
    snprintf(wrapped,sizeof(wrapped),"\"%s\"",command);
    FILE *pipe=popen(wrapped,"r");
#else
    FILE *pipe=popen(command,"r");
#endif
    LS_CHECK(pipe!=NULL);
    if (!pipe) return;
    size_t used=0;char line[512];
    while(fgets(line,sizeof(line),pipe)) {
        size_t n=strlen(line);
        if(used+n<sizeof(output)) { memcpy(output+used,line,n);used+=n; }
    }
    output[used]=0;
    int rc=pclose(pipe);
    LS_CHECK_MSG(rc==0,"simulator failed (%d): %s",rc,output);
}
LS_CASE(sonde_list_detail_and_recovery_in_both_postures)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("experiments","-k h",wide);
        LS_CHECK(strstr(output,"SONDES / RX ONLY")); LS_CHECK(strstr(output,"N1234560"));
        LS_CHECK(strstr(output,"N1234561")); LS_CHECK(strstr(output,"N1234562"));
        LS_CHECK(strstr(output,"CLIMB -8.1")); LS_CHECK(strstr(output,"km / 343"));
        run("experiments","-k h -x @enter",wide);
        LS_CHECK(strstr(output,"ALTITUDE m")); LS_CHECK(strstr(output,"RECOVER")); LS_CHECK(strstr(output,"BATT 3.1"));
        run("experiments","-k h -x @enter,+g",wide);
        LS_CHECK(strstr(output,"COMPASS target: N1234560"));
        LS_CHECK(strstr(output,"SONDE TARGET N1234560 43.451800 -71.650300 / ROUTE off"));
    }
}
LS_CASE(touch_opens_a_sonde_detail_in_both_postures)
{
    run("experiments","-k h -x 5:15",false); LS_CHECK(strstr(output,"ALTITUDE m"));
    run("experiments","-k h -x 5:7",true); LS_CHECK(strstr(output,"ALTITUDE m"));
}
LS_CASE(layered_frequency_uses_ten_khz_steps)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("experiments","-k o -x @enter,@right",wide); LS_CHECK(strstr(output,"403.010")); LS_CHECK(strstr(output,"CUSTOM"));
        run("experiments","-k o -x @enter,@right,@left",wide); LS_CHECK(strstr(output,"403.000"));
        run("experiments","-k o -x @down,@enter,@right",wide); LS_CHECK(strstr(output,"KEEP HOURS")); LS_CHECK(strstr(output,"7 h"));
    }
}
LS_CASE(sonde_map_labels_and_layer_toggle)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("map","",wide); LS_CHECK(strstr(output,"N123456")); LS_CHECK(strstr(output,"RS41"));
    }
    run("map","-k l -x @down,@down,@down,@down,@down,@down,@down,@down,@enter,@esc",false);
    LS_CHECK(!strstr(output,"N123456")); LS_CHECK(strstr(output,"UAL442"));
}

LS_CASE(range_requires_a_fresh_receiver_fix)
{
    run("experiments","-e -k h -x @enter",false);
    LS_CHECK(strstr(output,"range needs fresh receiver GPS"));
    LS_CHECK(strstr(output,"ALTITUDE m"));
}

LS_CASE(recovery_replaces_active_route_guidance)
{
    run("experiments","-g \"" RS41_ROUTE "\" -k h -x @enter,+g",false);
    LS_CHECK(strstr(output,"SONDE TARGET N1234560 43.451800 -71.650300 / ROUTE off"));
}

LS_CASE(empty_sonde_list_says_listening_while_running_and_start_only_when_stopped)
{
    seed_empty = true;
    for (int wide = 0; wide < 2; ++wide) {
        /* lssim starts the receiver, so it is running here. */
        run("experiments","-k h",wide);
        LS_CHECK(strstr(output,"Listening / no flights heard yet"));
        LS_CHECK(!strstr(output,"START RS41"));
        /* S stops it on the page, then H opens the list. */
        run("experiments","-k sh",wide);
        LS_CHECK(strstr(output,"No flights heard / START RS41"));
        LS_CHECK(!strstr(output,"Listening / no flights"));
    }
    seed_empty = false;
}
LS_CASE(sonde_detail_shows_its_own_keys)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("experiments","-k h",wide);
        LS_CHECK(strstr(output,"ENTER detail / G recover in COMPASS"));
        run("experiments","-k h -x @enter",wide);
        LS_CHECK(strstr(output,"G recover in COMPASS / ESC list"));
        LS_CHECK(!strstr(output,"ENTER detail / G recover"));
    }
}
