#include "ls_music_marks.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    assert(argc==2);char path[512],root[512];
    snprintf(root,sizeof(root),"%s/markers",argv[1]);
    snprintf(path,sizeof(path),"%s/tone.wav",argv[1]);
    ls_music_marks *m=ls_music_marks_new(root);assert(m);
    assert(ls_music_marks_prepare(m,path));assert(ls_music_marks_service(m));
    ls_music_marks_clear(m);assert(ls_music_marks_service(m));
    assert(ls_music_marks_add(m,1400));assert(ls_music_marks_service(m));
    assert(ls_music_marks_add(m,500));assert(ls_music_marks_service(m));
    assert(ls_music_marks_add(m,500));assert(ls_music_marks_service(m));
    assert(ls_music_marks_count(m)==2);uint32_t at;
    assert(ls_music_marks_get(m,0,&at) && at==500);
    ls_music_marks_free(m);m=ls_music_marks_new(root);assert(m);
    assert(ls_music_marks_prepare(m,path) && ls_music_marks_service(m));
    assert(ls_music_marks_count(m)==2 && ls_music_marks_get(m,1,&at) && at==1400);
    snprintf(path,sizeof(path),"%s/tone.flac",argv[1]);
    assert(ls_music_marks_prepare(m,path) && ls_music_marks_service(m));
    assert(ls_music_marks_count(m)==0);
    for(unsigned i=0;i<32;i++){assert(ls_music_marks_add(m,i*100));assert(ls_music_marks_service(m));}
    assert(!ls_music_marks_add(m,9000));assert(!ls_music_marks_get(m,32,&at));
    ls_music_marks_clear(m);assert(ls_music_marks_service(m));
    ls_music_marks_free(m);puts("PASS per-file marks: sorted, duplicate-safe, persistent, isolated, bounded");return 0;
}
