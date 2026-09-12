function(ls_configure_lvgl_allocator target)
    target_compile_options(${target} PRIVATE
        "-DLV_MEM_CUSTOM_ALLOC(x)=heap_caps_malloc(x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
        "-DLV_MEM_CUSTOM_REALLOC(p,x)=heap_caps_realloc(p, x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
        "-DLV_MEM_CUSTOM_FREE(p)=heap_caps_free(p)")
endfunction()
