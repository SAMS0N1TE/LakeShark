/* One seek per blob, bulk sector reads into reusable internal DMA memory.
 * Caller serializes this descriptor and its bounce buffer. */
#ifndef LS_CARTO_SD_IO_H
#define LS_CARTO_SD_IO_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
/* Single-owner diagnostic; successful/failed requests count actual scratch use. */
static size_t carto_sd_bounce_peak;
static int carto_read_exact(int fd,void *buffer,size_t length) {
    uint8_t *p=buffer;
    while(length) {
        ssize_t n=read(fd,p,length);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return 0;
        p+=n; length-=(size_t)n;
    }
    return 1;
}
static int carto_sd_read_at(int fd,uint64_t size,uint64_t offset,void *buffer,
                            size_t length,void *bounce,size_t capacity) {
    if(offset>size || length>size-offset || offset>LONG_MAX) return 0;
    if(!length) return 1;
    if(!bounce) return lseek(fd,(off_t)offset,SEEK_SET)==(off_t)offset && carto_read_exact(fd,buffer,length);
    if(capacity<512 || capacity%512) return 0;
    uint64_t pos=offset & ~UINT64_C(511);
    size_t skip=(size_t)(offset-pos); uint8_t *dst=buffer;
    if(lseek(fd,(off_t)pos,SEEK_SET)!=(off_t)pos) return 0;
    while(length) {
        size_t n=length>capacity-skip?capacity:((length+skip+511)/512)*512;
        if(n>size-pos) n=(size_t)(size-pos); /* final file sector may be short */
        if(n>carto_sd_bounce_peak) carto_sd_bounce_peak=n;
        if(n<=skip || !carto_read_exact(fd,bounce,n)) return 0;
        size_t take=n-skip; if(take>length) take=length;
        memcpy(dst,(uint8_t *)bounce+skip,take);
        dst+=take; length-=take; pos+=n; skip=0;
    }
    return 1;
}
#endif
