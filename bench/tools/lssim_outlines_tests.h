/* Exercise the real metadata parser and serialized download transaction. */
static void lssim_outlines_transactions(void) {
    cells_init();assert(cells_states_count==1 && cells_outlines.bytes==106);
    assert(cells_outlines_needed());assert(!strcmp(cells_work.outlines,"missing"));
    cw.catalog_refresh=false;cells_step();assert(dl.active && dl.kind==8);
    assert(!strcmp(cells_work.outlines,"downloading"));
    unsigned steps=0;while(dl.active && steps++<10)cells_step();
    assert(!dl.active && !cells_outlines_pending && !strcmp(cells_work.outlines,"ready"));
    assert(!cells_outlines_needed());
    /* Size change, hash change, and same-size local corruption all need refresh. */
    cells_outlines.bytes++;assert(cells_outlines_needed());cells_outlines.bytes--;
    cells_outlines.sha[0]^=1;assert(cells_outlines_needed());cells_outlines.sha[0]^=1;
    assert(carto_digest_remove(CELLS_ROOT "outlines.lso"));
    FILE *f=fopen(CELLS_ROOT "outlines.lso","r+b");assert(f);fputc('X',f);fclose(f);
    assert(cells_outlines_needed());
    cells_outlines_pending=true;cells_step();assert(dl.active);
    /* A bad server hash must preserve the old file and publish the error. */
    cells_outlines.sha[0]^=1;dl.sha[0]^=1;
    steps=0;while(dl.active && steps++<10)cells_step();
    assert(strstr(cells_work.outlines,"SHA256"));
    f=fopen(CELLS_ROOT "outlines.lso","rb");assert(f && fgetc(f)=='X');fclose(f);
    char text[128];cells_publish();assert(ls_cells_maps_text(3,text,sizeof(text)) && strstr(text,"SHA256"));
    const char *fixtures=getenv("LSSIM_OUTLINES_FIXTURES");assert(fixtures);
    char path[1024];
    const char *bad[]={"bad-file","bad-bytes","bad-sha","duplicate","not-object"};
    for(unsigned i=0;i<5;i++) {
        snprintf(path,sizeof(path),"%s/%s.json",fixtures,bad[i]);
        assert(!cells_states_parse(path,true));assert(cells_states_count==1 && cells_outlines.bytes==106);
    }
    snprintf(path,sizeof(path),"%s/old.json",fixtures);assert(cells_states_parse(path,true));
    assert(cells_states_count==1 && !cells_outlines.bytes && !cells_outlines_needed());
    cells_retry_reset(NULL);cells_step();assert(!strcmp(cells_work.outlines,"missing"));
    snprintf(path,sizeof(path),"%s/full.json",fixtures);assert(cells_states_parse(path,true));
    assert(cells_states_count==52 && cells_outlines.bytes==106);
    printf("PASS: outlines parse, compatibility, missing/size/SHA decisions, download, corruption and atomic rejection\n");
}
