/* The real route screens and their touch/options paths, through lssim. */
#include "ls_test.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif
static char output[32768];
static void run(const char *app,const char *options,bool landscape)
{
    char command[1200];
    snprintf(command,sizeof(command),"\"%s\" %s -g \"%s\" -o \"%s\" -d -f 20 %s %s 2>&1",
        LSSIM_EXE,app,ROUTE_FIXTURE,ROUTE_RENDER,landscape ? "-l" : "",options);
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
LS_CASE(route_guidance_is_drawn_in_map_at_both_postures)
{
    for(int wide=0;wide<2;wide++) {
        run("map","",wide);LS_CHECK(strstr(output,"223m left")!=NULL);LS_CHECK(strstr(output,"XTE 0m")!=NULL);
    }
}
LS_CASE(compass_go_to_page_follows_route_without_a_manual_target)
{
    for(int wide=0;wide<2;wide++) {
        run("compass","-k 3",wide);
        LS_CHECK(strstr(output,"223m left")!=NULL);
        LS_CHECK(strstr(output,"No target.")==NULL);
        /* The fixture pose heads 112.5 deg from north once declination is in.
           The view-vector solve (c03025e9) reads it 0.07 deg higher than the
           old one, over the rounding edge, so 112 became 113. The turn is
           the route's bearing (000) less that heading either way. */
        const char *turn=strstr(output,"TURN LEFT ");
        int deg=0;
        LS_CHECK(turn!=NULL && sscanf(turn,"TURN LEFT %d deg",&deg)==1);
        LS_CHECK_MSG(deg==112 || deg==113,"TURN LEFT %d deg",deg);
    }
}
LS_CASE(route_options_touch_and_keyboard_step_the_off_route_distance)
{
    run("map","-k o -x @down,@down,@down,@enter,@enter,45:11",false);
    LS_CHECK(strstr(output,"ROUTE > GUIDANCE")!=NULL);
    LS_CHECK(strstr(output,"45")!=NULL);LS_CHECK(strstr(output,"RTL-SDR")==NULL);
    run("map","-k o -x @down,@down,@down,@enter,@enter,@right",true);
    LS_CHECK(strstr(output,"45")!=NULL);
}
LS_CASE(clear_route_removes_guidance_through_options)
{
    run("map","-k o -x @down,@down,@down,@enter,@down,@down,@enter,@esc,@esc",false);
    LS_CHECK(strstr(output,"223m left")==NULL);
}
LS_CASE(map_labels_sit_on_no_symbol_and_no_other_label)
{
    /* The route fixture puts a mark, a node, aircraft, the receiver and a
       place name all within a few cells; lssim audits every label the map
       placed against every symbol cell and against each other.
       Landscape with its touch controls up (3d0adcf8: three bars of raised
       buttons, a pin ring round each mark) leaves the map nine rows. BASE
       CAMP has no free cell beside its pin there, so it hangs under the pin
       instead (label_spots keeps those spots for last). Tapping HIDE KEYS
       (cell 108:2) gives the map its rows back. Every name must be there. */
    static const struct { const char *name,*options; int wide; } VIEWS[]={
        { "portrait","",0 },{ "landscape with keys","",1 },{ "landscape, keys hidden","-x 108:2",1 } };
    for(int v=0;v<3;v++) {
        run("map",VIEWS[v].options,VIEWS[v].wide);
        const char *audit=strstr(output,"label audit: ");
        LS_CHECK_MSG(audit!=NULL,"no label audit in the output");
        if (!audit) continue;
        int labels=0,overlaps=-1;
        LS_CHECK(sscanf(audit,"label audit: %d labels, %d overlaps",&labels,&overlaps)==2);
        LS_CHECK_MSG(labels>=10,"only %d labels placed (%s)",labels,VIEWS[v].name);
        LS_CHECK_MSG(overlaps==0,"%d label overlaps (%s)",overlaps,VIEWS[v].name);
        /* None of the names that have a symbol is dropped. */
        LS_CHECK_MSG(strstr(output,"BASE CAMP")!=NULL,"BASE CAMP dropped (%s)",VIEWS[v].name);
        LS_CHECK_MSG(strstr(output,"NORTHFIELD")!=NULL,"NORTHFIELD dropped (%s)",VIEWS[v].name);
    }
}
