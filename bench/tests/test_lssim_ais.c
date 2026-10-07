/* Production AIS list, detail, options and MAP through the native simulator. */
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
static void run(const char *app,const char *options,bool landscape)
{
#ifdef _WIN32
    _putenv_s("LSSIM_AIS", "1");
#else
    setenv("LSSIM_AIS", "1", 1);
#endif
    char command[1200];
    snprintf(command,sizeof(command),"\"%s\" %s -o \"%s\" -d -f 5 %s %s 2>&1",
        LSSIM_EXE,app,AIS_RENDER,landscape ? "-l" : "",options);
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
LS_CASE(vessel_list_and_detail_fit_both_postures)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("fm", "-k 2", wide);
        LS_CHECK(strstr(output, "HEARD VESSELS")); LS_CHECK(strstr(output, "PLEASURE 37"));
        LS_CHECK(strstr(output, "16.5kn / 255deg")); LS_CHECK(strstr(output, "1km 014 0m"));
        run("fm", "-k 2a", wide);
        LS_CHECK(strstr(output, "366123453")); LS_CHECK(strstr(output, "HDG 255"));
        LS_CHECK(strstr(output, "DEST BOSTON")); LS_CHECK(strstr(output, "88B / MSG 18"));
    }
}
LS_CASE(touch_opens_the_selected_vessel_in_both_postures)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("fm", wide ? "-k 2 -x 58:27" : "-k 2 -x 26:62", wide);
        LS_CHECK(strstr(output, "DEST BOSTON")); LS_CHECK(strstr(output, "366123453"));
    }
}
LS_CASE(layered_options_step_in_both_directions_with_keys_and_touch)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("fm", "-k 2o -x @enter,@right", wide); LS_CHECK(strstr(output, "35 min"));
        run("fm", "-k 2o -x @enter,@right,@left", wide); LS_CHECK(strstr(output, "30 min"));
    }
    run("fm", "-k 2o -x @enter,45:11", false); LS_CHECK(strstr(output, "35 min"));
    run("fm", "-k 2o -x @enter,45:11,30:11", false); LS_CHECK(strstr(output, "30 min"));
    run("fm", "-k 2o -x @down,@enter,@enter,@esc,@esc", false);
    LS_CHECK(strstr(output, "366123453")); LS_CHECK(!strstr(output, "SEA TEST"));
}
LS_CASE(map_shows_received_vessel_labels_in_both_postures)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("map", "", wide); LS_CHECK(strstr(output, "SEA TEST"));
    }
}

LS_CASE(map_layer_toggle_hides_vessels_without_hiding_other_layers)
{
    run("map", "-k l -x @down,@down,@down,@down,@down,@down,@down,@enter,@esc", false);
    LS_CHECK(!strstr(output, "SEA TEST")); LS_CHECK(strstr(output, "UAL442"));
}
