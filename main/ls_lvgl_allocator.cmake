# Keep the complete LVGL allocator contract in one place, shared with its
# real-lv_mem host test. Plain realloc migrates small PSRAM blocks into the
# default INTERNAL heap; every resize must retain the original capabilities.
function(ls_configure_lvgl_allocator target)
    target_compile_options(${target} PRIVATE
        "-DLV_MEM_CUSTOM_ALLOC(x)=heap_caps_malloc(x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
        "-DLV_MEM_CUSTOM_REALLOC(p,x)=heap_caps_realloc(p, x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
        "-DLV_MEM_CUSTOM_FREE(p)=heap_caps_free(p)")
endfunction()
