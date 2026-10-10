/* BSD-licensed zstd 1.5.7 decoder, static context only. Disable default heap
   functions even in unused upstream APIs: the embedded archive needs no heap. */
#include "cartocore/zstd.h"
#ifndef NDEBUG
#define NDEBUG
#endif
#define ZSTD_DEPS_MALLOC
#define ZSTD_malloc(s) ((void)(s),(void*)0)
#define ZSTD_calloc(n,s) ((void)(n),(void)(s),(void*)0)
#define ZSTD_free(p) ((void)(p))
#include "../../third_party/zstd/zstddeclib.c"
size_t cc_zstd_context_bytes(void) { return ZSTD_estimateDCtxSize(); }
int cc_zstd_header(cc_str tile,size_t *decoded) {
    if(tile.size<8 || memcmp(tile.data,"CZ7\0",4)) return 0;
    size_t n=(size_t)tile.data[4]|(size_t)tile.data[5]<<8|(size_t)tile.data[6]<<16|(size_t)tile.data[7]<<24;
    ZSTD_frameHeader h;
    if(n<32 || n>CC_ZSTD_MAX_OUTPUT || ZSTD_getFrameHeader(&h,tile.data+8,tile.size-8)!=0 ||
       h.frameType!=ZSTD_frame || h.dictID || !h.checksumFlag || h.windowSize>65536 || h.frameContentSize!=n) return 0;
    *decoded=n; return 1;
}
int cc_zstd_decode(cc_str tile,cc_arena *a,cc_str *out) {
    size_t n,mark=a->used; *out=(cc_str){0,0};
    if(!cc_zstd_header(tile,&n)) return 0;
    uint8_t *dst=cc_arena_alloc(a,n,8); size_t retained=a->used;
    void *workspace=cc_arena_alloc(a,cc_zstd_context_bytes(),8);
    if(!dst || !workspace) goto fail;
    ZSTD_DCtx *ctx=ZSTD_initStaticDCtx(workspace,cc_zstd_context_bytes());
    if(!ctx || ZSTD_isError(ZSTD_decompressBegin(ctx))) goto fail;
    size_t pos=8,used=0,take;
    while((take=ZSTD_nextSrcSizeToDecompress(ctx))!=0) {
        if(take>tile.size-pos) goto fail;
        size_t got=ZSTD_decompressContinue(ctx,dst+used,n-used,tile.data+pos,take);
        if(ZSTD_isError(got) || got>n-used) goto fail;
        pos+=take; used+=got;
    }
    if(pos!=tile.size || used!=n) goto fail;
    a->used=retained; *out=(cc_str){dst,n}; return 1;
fail: a->used=mark; return 0;
}
int cc_zstd_decode_read(int (*read)(void *,uint64_t,void *,size_t),void *opaque,uint64_t offset,
                         size_t bytes,cc_arena *a,cc_str *out) {
    uint8_t head[26];size_t n,mark=a->used,hn=bytes<sizeof(head)?bytes:sizeof(head);
    *out=(cc_str){0,0};
    if(!read || bytes<8 || offset>UINT64_MAX-bytes || !read(opaque,offset,head,hn) || !cc_zstd_header((cc_str){head,hn},&n)) return 0;
    uint8_t *dst=cc_arena_alloc(a,n,8);size_t retained=a->used;
    void *workspace=cc_arena_alloc(a,cc_zstd_context_bytes(),8);
    uint8_t *input=cc_arena_alloc(a,65536,8);
    if(!dst || !workspace || !input) goto fail;
    ZSTD_DCtx *ctx=ZSTD_initStaticDCtx(workspace,cc_zstd_context_bytes());
    if(!ctx || ZSTD_isError(ZSTD_decompressBegin(ctx))) goto fail;
    size_t pos=8,used=0,take;
    while((take=ZSTD_nextSrcSizeToDecompress(ctx))!=0) {
        if(take>65536 || take>bytes-pos || !read(opaque,offset+pos,input,take)) goto fail;
        size_t got=ZSTD_decompressContinue(ctx,dst+used,n-used,input,take);
        if(ZSTD_isError(got) || got>n-used) goto fail;
        pos+=take;used+=got;
    }
    if(pos!=bytes || used!=n) goto fail;
    a->used=retained;*out=(cc_str){dst,n};return 1;
fail:a->used=mark;return 0;
}
