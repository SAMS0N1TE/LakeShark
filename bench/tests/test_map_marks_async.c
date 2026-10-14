#include "ls_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
static bool worker,fail_replace;
static FILE *guard_open(const char *p,const char *m) { LS_CHECK(worker);return fopen(p,m); }
static int guard_rename(const char *a,const char *b) { LS_CHECK(worker);if(fail_replace && strstr(a,".new")) { errno=EIO;return -1; }return rename(a,b); }
static int guard_remove(const char *p) { LS_CHECK(worker);return remove(p); }
#define fopen guard_open
#define rename guard_rename
#define remove guard_remove
#include "../../components/apps/tui/ls_map_marks.c"
#undef fopen
#undef rename
#undef remove
LS_CASE(deferred_marks_load_and_snapshot_save_never_do_ui_io) {
    char path[96];snprintf(path,sizeof(path),"%s/marks-async.txt",".");
    FILE *f=fopen(path,"w");LS_CHECK(f!=NULL);fputs("M 43 -71 0 SAVED\n",f);fclose(f);
    /* Enable before any UI getter. Loading remains pending until worker pumps. */
    ls_marks_defer_io();
    snprintf(read_job.path,sizeof(read_job.path),"%s",path);
    snprintf(s_path,sizeof(s_path),"%s",path);
    LS_EQ_INT(ls_marks_count(),0);LS_EQ_INT(ls_marks_add(43,-71,0,"EARLY"),-1);
    worker=true;ls_marks_io_step();worker=false;
    LS_EQ_INT(ls_marks_count(),1);LS_EQ_STR(ls_marks_at(0)->name,"SAVED");
    LS_CHECK(ls_marks_rename(0,"QUEUED"));LS_CHECK(ls_marks_save());
    LS_EQ_STR("MAP SAVING",ls_marks_save_status());
    LS_CHECK(ls_marks_rename(0,"LIVE"));
    worker=true;ls_marks_io_step();worker=false;
    LS_EQ_STR(ls_marks_at(0)->name,"LIVE");
    f=fopen(path,"r");char text[256]={0};fread(text,1,sizeof(text)-1,f);fclose(f);
    LS_CHECK(strstr(text,"QUEUED")!=NULL);LS_CHECK(strstr(text,"LIVE")==NULL);
    LS_CHECK(ls_marks_save());LS_CHECK(ls_marks_rename(0,"LATEST"));LS_CHECK(ls_marks_save());
    worker=true;ls_marks_io_step();worker=false;
    f=fopen(path,"r");memset(text,0,sizeof(text));fread(text,1,sizeof(text)-1,f);fclose(f);
    LS_CHECK(strstr(text,"LATEST")!=NULL);
    ls_marks_reload();LS_EQ_INT(ls_marks_count(),0);
    worker=true;ls_marks_io_step();worker=false;
    LS_EQ_STR(ls_marks_at(0)->name,"LATEST");
    ls_marks_rename(0,"UNSAVED");LS_CHECK(ls_marks_save());fail_replace=true;
    worker=true;ls_marks_io_step();worker=false;
    LS_CHECK(strstr(ls_marks_save_status(),"NOT SAVED"));LS_CHECK(ls_marks_error()!=NULL);
    ls_marks_reload();LS_EQ_STR("UNSAVED",ls_marks_at(0)->name);
    fail_replace=false;LS_CHECK(ls_marks_save());
    worker=true;ls_marks_io_step();worker=false;
    LS_EQ_STR("MAP SAVED",ls_marks_save_status());
    ls_marks_reload();worker=true;ls_marks_io_step();worker=false;
    LS_EQ_STR("UNSAVED",ls_marks_at(0)->name);
    LS_CHECK(ls_marks_move(0,44.12345,-72.54321));LS_CHECK(ls_marks_save());
    worker=true;ls_marks_io_step();worker=false;ls_marks_save_status();
    ls_marks_reload();worker=true;ls_marks_io_step();worker=false;
    LS_NEAR(ls_marks_at(0)->lat,44.12345,1e-6);
    LS_CHECK(ls_marks_delete(0));LS_CHECK(ls_marks_save());
    worker=true;ls_marks_io_step();worker=false;ls_marks_save_status();
    ls_marks_reload();worker=true;ls_marks_io_step();worker=false;
    LS_EQ_INT(ls_marks_count(),0);
    remove(path);char backup[104];snprintf(backup,sizeof(backup),"%s.bak",path);remove(backup);
}
