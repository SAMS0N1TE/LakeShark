# e3d: a software 3D engine for the ESP32-P4, as an ESP-IDF component.
# (PAPP builds compile the same sources with examples/papp/papp_common.py.)
idf_component_register(
    SRCS "src/e3d.c" "src/e3d_render.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_timer heap
)

# The engine is speed-bound: -O2 instead of the project's -Os
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# The renderer uses the P4's PIE (128-bit integer SIMD) for tile fills, copies
# and batched lighting. IDF's GCC flags name the extension xespv; the engine's
# inline assembly is written for the xesppie name.
if(CONFIG_IDF_TARGET_ESP32P4)
    set_source_files_properties(src/e3d_render.c PROPERTIES
        COMPILE_OPTIONS "-march=rv32imafc_zicsr_zifencei_xesppie")
endif()
