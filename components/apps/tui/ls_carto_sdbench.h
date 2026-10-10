/* Read-only benchmark. Timing includes seeks/copies; checksums and yields
 * are outside timing. Filesystem/card caches can be warm. */
static uint32_t sdbench_hash(uint32_t h,const uint8_t *p,size_t n) {
    while(n--) h=(h^*p++)*UINT32_C(16777619);
    return h;
}
static int sdbench(int mb) {
    char path[512]=MAP_DIR "/central_nh_compact.ctile"; struct stat st;
    if(stat(path,&st)) { path[0]=0; if(map_directory(path,sizeof(path))) return 1; }
    if(stat(path,&st) || st.st_size<128*1024 || (uint64_t)st.st_size>LONG_MAX) {
        puts("sdbench: need an existing .ctile >=128 KiB"); return 1;
    }
    size_t size=(size_t)st.st_size,total=(size_t)mb*1024*1024;
    if(total>size) total=size;
    uint8_t *psram=heap_caps_aligned_alloc(64,128*1024,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    char *stdio_buf=heap_caps_malloc(32*1024,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!psram || !stdio_buf) { heap_caps_free(psram); heap_caps_free(stdio_buf); return 1; }
    printf("sdbench: %s file=%zu sequential=%zu bytes; decimal MB/s; random=64 reads/case\n",path,size,total);
    puts("sdbench: seek/copy included; checksums/yields excluded; FAT/card caches may be warm");
    static const char *names[]={"fread default","fread unbuffered","fread setvbuf4K","fread setvbuf32K","read PSRAM","read DMA+copy"};
    const size_t chunks[]={4096,32768,131072};
    uint32_t reference[4]={0}; int result=0;
    double best=0; const char *best_name=NULL; size_t best_chunk=0,selected=0;
    xSemaphoreTake(view_lock,portMAX_DELAY);
    for(int mode=0;mode<6;mode++) for(int c=0;c<3;c++) {
        size_t chunk=chunks[c]; void *bounce=NULL; FILE *f=NULL; int fd=-1;
        if(mode==5) {
            bounce=heap_caps_aligned_alloc(64,chunk,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
            if(!bounce) { printf("  %s %zuK skipped: internal DMA allocation\n",names[mode],chunk/1024); continue; }
        }
        if(mode<4) {
            f=fopen(path,"rb");
            if(f && mode && setvbuf(f,mode==1?NULL:stdio_buf,mode==1?_IONBF:_IOFBF,mode==2?4096:32768)) { fclose(f); f=NULL; }
        } else fd=open(path,O_RDONLY);
        if((mode<4 && !f) || (mode>=4 && fd<0)) { result=1; heap_caps_free(bounce); continue; }
        for(int test=0;test<4;test++) {
            uint32_t hash=UINT32_C(2166136261),seed=12345;
            int64_t us=0; size_t done=0; unsigned count=0;
            size_t bytes=test?((size_t)16384<<(test-1)):total;
            while(test?count<64:done<total) {
                size_t n=test?bytes:total-done; if(!test && n>chunk) n=chunk;
                seed=seed*UINT32_C(1664525)+UINT32_C(1013904223);
                size_t offset=test?(size_t)seed%(size-n+1):done;
                int64_t start=esp_timer_get_time();
                int ok=mode<4?(((!test && count) || !fseek(f,(long)offset,SEEK_SET)) && fread(psram,1,n,f)==n):
                    carto_sd_read_at(fd,size,offset,psram,n,bounce,chunk);
                us+=esp_timer_get_time()-start;
                if(!ok) break;
                hash=sdbench_hash(hash,psram,n); done+=n; count++; vTaskDelay(1);
            }
            bool complete=test?count==64:done==total;
            if(!mode && !c && complete) reference[test]=hash;
            bool valid=complete && hash==reference[test]; if(!valid) result=1;
            double rate=us>0?count*1e6/us:0;
            if(test) printf("    random %2zuK %.1f reads/s %.2f MB/s %s\n",bytes/1024,rate,us>0?(double)done/us:0,valid?"verified":"FAILED");
            else printf("  %-19s chunk=%3zuK sequential %.2f MB/s %s\n",names[mode],chunk/1024,us>0?(double)done/us:0,valid?"verified":"FAILED");
            if(valid && test==2 && mode>=4 && rate>best) {
                best=rate; best_name=names[mode]; best_chunk=chunk; selected=mode==5?chunk:0;
            }
        }
        if(f) fclose(f);
        if(fd>=0) close(fd);
        heap_caps_free(bounce);
    }
    if(best_name && !result) {
        sd_bounce_bytes=selected;
        if(sd_map && (!selected || selected!=sd_map->bounce_capacity || !sd_map->bounce)) {
            void *replacement=selected?heap_caps_aligned_alloc(64,selected,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT):NULL;
            if(!selected || replacement) {
                heap_caps_free(sd_map->bounce); sd_map->bounce=replacement; sd_map->bounce_capacity=selected;
            } else puts("sdbench: active map keeps its buffer (allocation unavailable); selection applies on next open");
        }
        printf("sdbench: selected fd random32K: %s chunk=%zuK %.1f reads/s (this boot)\n",best_name,best_chunk/1024,best);
    }
    xSemaphoreGive(view_lock);
    heap_caps_free(stdio_buf); heap_caps_free(psram); return result;
}
