/* Metadata-bound digest cache. Legacy size-only records are never trusted. */
#ifndef LS_CARTO_DIGEST_H
#define LS_CARTO_DIGEST_H
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
static inline bool carto_digest_remove(const char *path) {
    if(strlen(path)>263) return false;
    char side[272]; snprintf(side,sizeof(side),"%s.sha",path);
    return unlink(side)==0 || errno==ENOENT;
}
/* The whole record through sscanf: fscanf would link newlib's FILE scanner,
   8 KB of app flash, for this one line. A record longer than 255 bytes is
   not one carto_digest_write made, and reads as invalid. */
static inline int carto_digest_scan(FILE *f,long long *size,long long *mtime,char hex[65]) {
    char buf[256],extra; size_t n=fread(buf,1,sizeof(buf)-1,f);
    if(n==sizeof(buf)-1 && fgetc(f)!=EOF) return 0;
    buf[n]=0;
    return sscanf(buf,"%lld %lld %64s %c",size,mtime,hex,&extra);
}
static inline bool carto_digest_cached(const char *path,const struct stat *st,const char *expected) {
    if(strlen(path)>263) return false;
    char side[272],hex[65]; long long size,mtime;
    if(strlen(expected)!=64) return false;
    snprintf(side,sizeof(side),"%s.sha",path);
    FILE *f=fopen(side,"r"); if(!f) return false;
    int n=carto_digest_scan(f,&size,&mtime,hex);
    bool ok=n==3 && !ferror(f) && size==(long long)st->st_size &&
        mtime==(long long)st->st_mtime && strlen(hex)==64 && !strcasecmp(hex,expected);
    fclose(f); return ok;
}
static inline bool carto_digest_write(const char *path,const struct stat *st,const char *hex) {
    if(strlen(path)>263) return false;
    char side[272],temp[280];
    snprintf(side,sizeof(side),"%s.sha",path); snprintf(temp,sizeof(temp),"%s.part",side);
    if(strlen(hex)!=64) return false;
    for(int i=0;i<64;i++) if(!isxdigit((unsigned char)hex[i])) return false;
    FILE *f=fopen(temp,"w"); if(!f) return false;
    bool ok=fprintf(f,"%lld %lld %s\n",(long long)st->st_size,(long long)st->st_mtime,hex)>0;
    if(fflush(f)) ok=false;
    if(fclose(f)) ok=false;
    /* Callers invalidate the old record before mutation/verification. */
    if(ok) ok=rename(temp,side)==0;
    if(!ok) unlink(temp);
    return ok;
}
#endif
