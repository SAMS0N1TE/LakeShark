#include "host_cellset.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#ifndef CC_BUILD_PYTHON
#define CC_BUILD_PYTHON "python3"
#define CC_BUILD_CELLS_SCRIPT "tools/build_cells.py"
#endif
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif
int cc_host_is_directory(const char *path) { struct stat st; return !stat(path,&st) && (st.st_mode&S_IFDIR)!=0; }
static int read_file(void *ctx,uint64_t off,void *dst,size_t n) {
    FILE *f=ctx;
#ifdef _WIN32
    if(off>INT64_MAX || _fseeki64(f,(int64_t)off,SEEK_SET)) return 0;
#else
    if(off>LONG_MAX || fseek(f,(long)off,SEEK_SET)) return 0;
#endif
    return fread(dst,1,n,f)==n;
}
static int open_file(void *ctx,uint32_t id,void **handle,uint64_t *size) {
    cc_host_cellset *h=ctx; char path[1200];
    if(!id) snprintf(path,sizeof(path),"%s/overview.ctile",h->root);
    else snprintf(path,sizeof(path),"%s/8-%u-%u.ctile",h->root,(id-1)/256,(id-1)%256);
    FILE *f=fopen(path,"rb"); if(!f) return errno==ENOENT?0:-1;
    if(fseek(f,0,SEEK_END)) { fclose(f); return -1; } long n=ftell(f);
    if(n<64) { fclose(f); return -1; } *size=(uint64_t)n; *handle=f; return 1;
}
static void close_file(void *handle) { fclose(handle); }
int cc_host_cellset_open(cc_host_cellset *h,const char *root) {
    memset(h,0,sizeof(*h)); if(strlen(root)>=sizeof(h->root)) return 0; strcpy(h->root,root);
    char active[1200]; snprintf(active,sizeof(active),"%s/CURRENT",root); FILE *current=fopen(active,"rb");
    if(current) {
        char generation[128]; int ok=fgets(generation,sizeof(generation),current)!=0; fclose(current);
        if(!ok) return 0; generation[strcspn(generation,"\r\n")]=0;
        if(strncmp(generation,"gen-",4) || strspn(generation,"gen-0123456789abcdef")!=strlen(generation) ||
           snprintf(h->root,sizeof(h->root),"%s/%s",root,generation)>=(int)sizeof(h->root)) return 0;
    }
    char path[1200]; snprintf(path,sizeof(path),"%s/index.cci",h->root); h->index=fopen(path,"rb");
    h->directory=malloc(4096); h->staging=malloc(1); /* v7 streams blocks to arena */
    if(!h->index || !h->directory || !h->staging || fseek(h->index,0,SEEK_END)) goto fail;
    long n=ftell(h->index); if(n<64) goto fail;
    if(!cc_cellset_open(&h->set,read_file,h->index,(uint64_t)n,(cc_cellset_io){h,open_file,read_file,close_file},
                        h->directory,4096,h->staging,1)) goto fail;
    return 1;
fail: cc_host_cellset_close(h); return 0;
}
void cc_host_cellset_close(cc_host_cellset *h) {
    cc_cellset_close(&h->set); if(h->index) fclose(h->index); free(h->directory); free(h->staging); memset(h,0,sizeof(*h));
}
int cc_build_cells_cli(int argc,char **argv) {
    const char *python=getenv("CC_PYTHON"); if(!python || !*python) python=CC_BUILD_PYTHON;
    const char **args=calloc((size_t)argc+5,sizeof(*args)); if(!args) return 1;
    args[0]=python; args[1]=CC_BUILD_CELLS_SCRIPT; args[2]="--engine"; args[3]=argv[0];
    for(int i=2;i<argc;i++) args[i+2]=argv[i];
#ifdef _WIN32
    intptr_t code=_spawnvp(_P_WAIT,python,args); free(args); return code<0?1:(int)code;
#else
    pid_t pid=fork(); if(pid==0) { execvp(python,(char *const*)args); _exit(127); }
    free(args); int status; if(pid<0 || waitpid(pid,&status,0)<0) return 1;
    return WIFEXITED(status)?WEXITSTATUS(status):1;
#endif
}
