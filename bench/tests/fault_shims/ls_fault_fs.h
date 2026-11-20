/* Force-included into a unit under test: its /sdcard/ paths land in a temp
   root and every storage call can be made to fail on demand. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#endif

typedef struct {
    long write_budget;      /* bytes fwrite may still take, then a short write; -1 unlimited */
    int  fflush_fail, fclose_fail;
    int  rename_fail_nth;   /* fail the Nth rename from now (1 = next); 0 off */
    int  rename_calls;
    const char *open_fail_substr;   /* fopen of a path containing this fails */
    int  mkdir_fail;
} lf_fs_t;
extern lf_fs_t lf_fs;
extern char lf_root[260];
void lf_fs_reset(void);
const char *lf_real(const char *p, char *buf, size_t n);   /* /sdcard/x -> root/x */

FILE *lf_fopen(const char *p, const char *m);
size_t lf_fwrite(const void *b, size_t s, size_t n, FILE *f);
int lf_fflush(FILE *f);
int lf_fclose(FILE *f);
int lf_rename(const char *a, const char *b);
int lf_remove(const char *p);
int lf_stat(const char *p, struct stat *st);
int lf_mkdir(const char *p);
DIR *lf_opendir(const char *p);
int lf_unlink(const char *p);

#undef fopen
#undef fwrite
#undef fflush
#undef fclose
#undef rename
#undef remove
#undef stat
#undef mkdir
#undef opendir
#undef unlink
#define fopen lf_fopen
#define fwrite lf_fwrite
#define fflush lf_fflush
#define fclose lf_fclose
#define rename lf_rename
#define remove lf_remove
#define stat(p, s) lf_stat(p, s)
#define mkdir(p, m) lf_mkdir(p)
#define opendir lf_opendir
#define unlink lf_unlink
