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
static void lssim_maps_split_transactions(void) {
    cells_init();assert(cw.direct && cells_states_count==1);
    uint32_t id=1+77*256+93,bytes;uint8_t sha[32];struct stat st;
    putenv("LSSIM_MAPS_TOKEN=");
    assert(cells_id_allowed(id) && !cells_id_allowed(id+65536));
    cells_state_command(7,('C'<<8)|'O');
    unsigned steps=0;
    while(cw.queue_loaded && steps++<200) { if(dl.active)cells_download_step();else cells_queue_step(); }
    assert(!cw.queue_loaded && steps<200);
    assert(cells_entry(id,&bytes,sha) && cells_present(id,bytes));
    assert(stat(CELLS_ROOT "cells/8-77-93.detail.ctile",&st));
    assert(!cells_download(6,"cells/8/77/93.detail.ctile","cells/8-77-93.detail.ctile",100,sha));
    cells_states_dirty=true;cells_states_publish();cells_publish();
    assert(strstr(cells_work.states,"|0\n"));
    assert(cells_states[0].base_bytes && cells_states[0].detail_bytes);
    putenv("LSSIM_MAPS_TOKEN=fixture-entitlement");
    cells_state_command(7,('C'<<8)|'O');cells_queue_step();assert(dl.active && strstr(dl.relative,".detail.ctile"));
    cells_download_step();uint64_t offset=dl.offset;assert(offset>0);
    putenv("LSSIM_MAPS_TOKEN=");cells_step();assert(!dl.active && cw.queue_loaded);
    putenv("LSSIM_MAPS_TOKEN=fixture-entitlement");cells_retry_reset(NULL);
    cells_queue_step();assert(dl.active && dl.offset==offset);
    steps=0;while(cw.queue_loaded && steps++<200) { if(dl.active)cells_download_step();else cells_queue_step(); }
    assert(!cw.queue_loaded && steps<200);
    assert(cells_entry(id+65536,&bytes,sha) && cells_present(id+65536,bytes));
    cells_states_dirty=true;cells_states_publish();cells_publish();assert(strstr(cells_work.states,"|1\n"));
    cells_state_command(8,('C'<<8)|'O');assert(stat(CELLS_ROOT "cells/8-77-93.ctile",&st));
    assert(stat(CELLS_ROOT "cells/8-77-93.detail.ctile",&st));
    puts("PASS: split base all states, detail token gate, revoke/resume, detail publication and removal");
}
/* Nationwide backend fixture: no network, board or screen interaction. */
static void lssim_maps_transactions(void) {
    if(getenv("LSSIM_MAPS_SPLIT_TEST")) { lssim_maps_split_transactions();return; }
    cells_init();assert(cw.catalog>=0 && cw.direct && !cw.index_size);assert(cells_states_count==2);
    assert(cells_asset_valid(CELLS_ROOT "cells/catalog.ccm","cells/catalog.ccm"));
    close(cw.catalog);cw.catalog=-1;assert(!cells_file_current(0,CELLS_ROOT "cells/overview.ctile"));assert(cells_catalog());
    const char *bad=CELLS_ROOT "cells/bad.json";FILE *bad_file=fopen(bad,"wb");assert(bad_file);fputs("{\"NH\":{\"name\":\"Broken\",\"tier\":\"free\",\"bytes\":0,\"cells\":[[256,0]]}}",bad_file);fclose(bad_file);
    assert(!cells_states_parse(bad,true));assert(cells_states_count==2);unlink(bad);
    const char *full=getenv("LSSIM_MAPS_STATES_FIXTURE");assert(full && cells_states_parse(full,false));assert(cells_states_count==2);
    uint8_t malformed[224];assert(carto_sd_read_at(cw.catalog,sizeof(malformed),0,malformed,sizeof(malformed),NULL,0));malformed[220]=0;
    FILE *bad_catalog=fopen(CELLS_ROOT "cells/bad.ccm","wb");assert(bad_catalog);assert(fwrite(malformed,1,sizeof(malformed),bad_catalog)==sizeof(malformed));fclose(bad_catalog);
    assert(!cells_asset_valid(CELLS_ROOT "cells/bad.ccm","cells/catalog.ccm"));unlink(CELLS_ROOT "cells/bad.ccm");
    cells_jtoken small_tokens[8];const char *invalid[]={"{", "[1,]", "{\"a\":01}", "{\"a\":1e}", "[[[[[[[[[0]]]]]]]]]"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) { cells_jparser parser={.s=invalid[i],.n=strlen(invalid[i]),.t=small_tokens,.cap=8};assert(!cells_jparse(&parser)); }
    uint32_t part,count;assert(cells_parts(1+77*256+93,&part,&count));assert(part==2u*1024u*1024u && count==26);
    uint32_t size;uint8_t sha[32];assert(cells_entry(1+77*256+93,&size,sha));
    assert(!cells_entry(65536,&size,sha));
    putenv("LSSIM_MAPS_TOKEN=");assert(ls_cells_state_action(false,"CO"));cells_step();
    assert(cw.queue_loaded);assert(!cells_token_load());
    assert(cells_id_allowed(1+77*256+93));assert(!cells_id_allowed(65537+77*256+93));
    assert(cells_json_bounds("{}",2));assert(!cells_json_bounds("[[[[[[[[[0]]]]]]]]]",19));
    assert(!ls_cells_state_action(false,"../"));
    assert(ls_cells_state_action(false,"NH"));cells_step();assert(cw.queue_loaded);
    cells_queue_step();assert(dl.active && dl.kind==6 && dl.part_count==26 && dl.offset==0);
    for(unsigned i=0;i<3;i++)assert(!cells_download_step());
    uint64_t saved_offset=dl.offset;assert(saved_offset>0 && saved_offset<part);
    cells_download_fail("test interruption");cells_retry_reset(NULL);cells_queue_step();assert(dl.active && dl.offset==saved_offset);
    /* Resume inside a late part, not just p0. */
    unsigned advance=0;
    while(dl.offset<=24u*part && advance++<8000) {
        if(dl.active)cells_download_step();else cells_queue_step();
    }
    assert(dl.active && dl.offset>=24u*part && dl.offset<25u*part);
    saved_offset=dl.offset;cells_download_fail("late part interruption");cells_retry_reset(NULL);
    cells_queue_step();assert(dl.active && dl.offset==saved_offset && strstr(dl.remote,".p24"));
    unsigned steps=0;while(cw.queue_loaded && steps++<8000) {
        if(dl.active)cells_download_step();else cells_queue_step();
    }
    assert(!cw.queue_loaded);assert(steps<8000);
    assert(cells_entry(1+77*256+93,&size,sha));assert(cells_present(1+77*256+93,size));
    struct stat file_stat;assert(!stat(CELLS_ROOT "cells/8-77-93.ctile",&file_stat));assert(stat(CELLS_ROOT "cells/index.cci",&file_stat));
    cells_states_dirty=true;cells_states_publish();cells_publish();
    static char rows[8192];assert(ls_cells_maps_text(0,rows,sizeof(rows)));
    assert(strstr(rows,"NH|New Hampshire|free|1|") && strstr(rows,"CO|Colorado|free|1|"));
    assert(strstr(rows,"|1|1|"));float progress;assert(ls_cells_maps_progress(&progress) && progress==1);
    /* A second state owns the same cell. Removal must keep its bytes. */
    putenv("LSSIM_MAPS_TOKEN=fixture-entitlement");cells_state_command(7,('C'<<8)|'O');
    putenv("LSSIM_MAPS_TOKEN=");cells_queue_step();assert(!cw.queue_loaded);
    cells_state_command(7,('C'<<8)|'O');
    putenv("LSSIM_MAPS_TOKEN=fixture-entitlement");
    steps=0;while(cw.queue_loaded && steps++<10)cells_queue_step();assert(!cw.queue_loaded);
    cells_state_command(8,('N'<<8)|'H');assert(!stat(CELLS_ROOT "cells/8-77-93.ctile",&file_stat));
    cells_state_command(8,('C'<<8)|'O');assert(stat(CELLS_ROOT "cells/8-77-93.ctile",&file_stat));
    /* Split metadata and detail transport use the same persisted token seam. */
    assert(cells_states[0].base_bytes && cells_states[0].detail_bytes==0);
    assert(!strcmp(cells_work.free_max_zoom,"12"));
    const char *split=CELLS_ROOT "cells/split.json";
    FILE *sf=fopen(split,"wb");assert(sf);
    fputs("{\"free_max_zoom\":12,\"CO\":{\"name\":\"Colorado\",\"bytes\":300,\"base_bytes\":100,\"detail_bytes\":200,\"cells\":[[77,93]]}}",sf);fclose(sf);
    assert(cells_states_parse(split,false));unlink(split);
    /* Corrupted second host part fails the hash of the joined whole file. */
    char remote[1200];snprintf(remote,sizeof(remote),"%scells/8/77/93.ctile.p1",cw.server+7);
    FILE *f=fopen(remote,"r+b");assert(f);int c=fgetc(f);rewind(f);fputc(c^1,f);fclose(f);
    cells_state_command(7,('N'<<8)|'H');steps=0;
    while(steps++<8000) {
        if(dl.active)cells_download_step();else { if(!cells_retry_ready(dl.relative))break;cells_queue_step(); }
    }
    assert(strstr(cells_work.message,"SHA256"));assert(!cells_present(1+77*256+93,size));
    assert(stat(CELLS_ROOT "cells/8-77-93.ctile.part",&file_stat));
    puts("PASS: CCMAPS2 catalog, states parse, zoom entitlement, resumable ordered part joining, whole SHA rejection, shared removal, no-index map open");
}
