/* Real worker transaction tests, invoked only by the isolated host fixture. */
static void lssim_cells_transactions(void) {
    cells_init();assert(cw.catalog>=0);
    const char *relative="cells/catalog.ccm",*path=CELLS_ROOT "cells/catalog.ccm";
    struct stat st;assert(!stat(path,&st));size_t size=(size_t)st.st_size;
    uint8_t *data=malloc(size);assert(data);FILE *f=fopen(path,"rb");assert(f && fread(data,1,size,f)==size);fclose(f);
    uint8_t sha[32];char hex[65];assert(!mbedtls_sha256(data,size,sha,0));
    for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",sha[i]);
    assert(!cells_upload_begin("cells/../oops",size,hex));
    assert(cells_upload_begin(relative,size,hex));assert(upload.offset==0);
    assert(!cells_upload_data(0,data,128,0));assert(!upload.offset);
    assert(!cells_upload_data(1,data,128,cells_crc(data,128)));
    assert(cells_upload_data(0,data,128,cells_crc(data,128)));
    upload.active=false;assert(cells_upload_begin(relative,size,hex));assert(upload.offset==128);
    assert(cells_upload_data(0,data,128,cells_crc(data,128)));assert(upload.offset==128);
    assert(cells_upload_data(128,data+128,size-128,cells_crc(data+128,size-128)));
    assert(cells_upload_end());assert(cells_matches(path,size,sha));
    char wrong[65];memset(wrong,'0',64);wrong[64]=0;
    assert(cells_upload_begin(relative,size,wrong));assert(cells_upload_data(0,data,size,cells_crc(data,size)));
    assert(!cells_upload_end());assert(cells_matches(path,size,sha));
    char size_text[24];snprintf(size_text,sizeof(size_text),"%zu",size);
    char *begin[]={"carto","upload","begin",(char*)relative,size_text,hex};assert(!cells_upload_command(6,begin));
    for(size_t at=0;at<size;at+=32) {
        size_t n=size-at<32?size-at:32;char offset[24],crc[9],bytes[65];
        snprintf(offset,sizeof(offset),"%zu",at);snprintf(crc,sizeof(crc),"%08x",cells_crc(data+at,n));
        for(size_t i=0;i<n;i++)snprintf(bytes+2*i,3,"%02x",data[at+i]);
        char *chunk[]={"carto","upload","data",offset,crc,bytes};assert(!cells_upload_command(6,chunk));
    }
    char *end[]={"carto","upload","end"};assert(!cells_upload_command(3,end));free(data);
    if(cw.catalog>=0) { close(cw.catalog);cw.catalog=-1; }
    assert(!rename(path,CELLS_ROOT "cells/catalog.ccm.bak"));cells_recover(CELLS_ROOT "cells");assert(!stat(path,&st));assert(cells_catalog());
    memset(cw.cover,0,CELLS_BITS);unsigned id=77*256+93;cw.cover[id/8]|=(1u<<(id&7));
    assert(cells_record(1,"Shared A",cw.cover,false));assert(cells_record(2,"Shared B",cw.cover,false));
    char file[96],rel[64];cells_name(id+1,rel,sizeof(rel),false);snprintf(file,sizeof(file),CELLS_ROOT "%s",rel);
    f=fopen(file,"wb");assert(f);assert(fwrite("owned",1,5,f)==5);fclose(f);
    assert(cells_delete(1));assert(!stat(file,&st));assert(cells_delete(2));assert(stat(file,&st));
    printf("PASS: real worker upload CRC, offset, duplicate ACK, resume, SHA rejection, atomic recovery, shared ownership\n");
    printf("cells memory: worker=%zu mailbox=%zu publication=%zu download=%zu upload=%zu bytes PSRAM; guard=%zu bytes internal\n",
        sizeof(cw),sizeof(cells_mail)+sizeof(cells_request),sizeof(cells_pub)+sizeof(cells_work),sizeof(dl),sizeof(upload),sizeof(cells_guard));
}
