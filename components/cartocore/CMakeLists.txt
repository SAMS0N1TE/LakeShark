set(CARTOCORE_DIR "$ENV{CARTOCORE_DIR}" CACHE PATH "CartoCore engine checkout (defaults to vendored snapshot)")
if(NOT CARTOCORE_DIR)
    set(CARTOCORE_DIR "${CMAKE_CURRENT_LIST_DIR}/upstream")
endif()
if(NOT CARTOCORE_DIR OR NOT EXISTS "${CARTOCORE_DIR}/src/core/core.c")
    message(FATAL_ERROR "CartoCore missing: set -DCARTOCORE_DIR=<engine checkout> or CARTOCORE_DIR in the environment")
endif()
if(NOT EXISTS "${CARTOCORE_DIR}/components/cartocore/sources.cmake")
    message(FATAL_ERROR "CartoCore source/header mismatch: shared sources.cmake missing; update CARTOCORE_DIR")
endif()
set(CC_ROOT "${CARTOCORE_DIR}")
include("${CC_ROOT}/components/cartocore/sources.cmake")
if(NOT CC_EMBED_API_VERSION EQUAL 2 OR NOT CC_CTILE_FORMAT_MAX EQUAL 7)
    message(FATAL_ERROR "LakeShark CartoCore version mismatch: requires embedded API 2 and CTILE through v7")
endif()
if(CONFIG_LS_BOARD_T_DISPLAY_P4 AND (NOT CONFIG_FATFS_ALLOC_PREFER_EXTRAM OR CONFIG_FATFS_FS_LOCK LESS 24))
    message(FATAL_ERROR "Cell maps require CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y and CONFIG_FATFS_FS_LOCK=24 (update existing sdkconfig; see bench/TILES.md)")
endif()
idf_component_register(SRCS ${CC_IDF_SRCS} INCLUDE_DIRS ${CC_PUBLIC_INCLUDE_DIRS})
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wall -Wextra)
target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)
# The picture decoders (media/png.c, jpeg.c) emit through media/stream.c,
# which the shared source list leaves out: the desktop decodes wallpapers.
if(EXISTS "${CC_ROOT}/src/media/stream.c")
    target_sources(${COMPONENT_LIB} PRIVATE "${CC_ROOT}/src/media/stream.c")
endif()

# The PIE kernels are off on every board; the portable C kernels do the same work.
# Two field hardware-watchdog resets on the SX T-Display (2.23.1, 2.23.2, MAP open)
# had core0 frozen with interrupts masked inside FreeRTOS's lazy PIE context save
# (rtos_save_pie_coproc, the q-register store into the previous PIE owner's save
# area, which sits at the bottom of that task's PSRAM stack). CartoCore is the only
# PIE code in the image, so without it that trap path never runs. They also fault
# on P4 rev 3 (the Nano v3.1 panicked at cc_pie_task_enter). -DLS_CARTO_PIE=ON
# brings them back for a bench comparison (`carto pie`, `carto bench`).
option(LS_CARTO_PIE "CartoCore PIE (SIMD) bit-row kernels on ESP32-P4 rev < 3" OFF)
if(LS_CARTO_PIE AND IDF_TARGET STREQUAL "esp32p4" AND NOT (CONFIG_ESP32P4_REV_MIN_FULL GREATER_EQUAL 300))
    target_sources(${COMPONENT_LIB} PRIVATE "${CC_P4_ASM}")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CC_HAVE_ESP32P4_PIE=1)
    set_source_files_properties("${CC_P4_ASM}"
        PROPERTIES COMPILE_OPTIONS "-march=rv32imafc_zicsr_zifencei_xesppie")
endif()

# Size build: hot code (map render / vocoders) keeps -O2.
if(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
endif()

# zstd's long-offset ("prefetch") sequence decoder runs only for windows past
# 16 MB or with a cold dictionary. Every frame here goes through cc_zstd_header,
# which refuses windows over 64 KB, and nothing loads a dictionary, so that
# decoder is never reached: build zstd with the short decoder alone (about 9 KB).
set_property(SOURCE "${CC_ROOT}/src/core/zstd.c" APPEND PROPERTY
    COMPILE_DEFINITIONS ZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT=1)

# Tile decode only. zstd inlines its Huffman/FSE templates into several copies
# (-O2, forced inline); one copy at -Os is about 20 KB smaller and the tiles it
# decodes are cached, so the slower decode is paid once per tile.
set_property(SOURCE "${CC_ROOT}/src/core/zstd.c" APPEND PROPERTY
    COMPILE_DEFINITIONS ZSTD_NO_INLINE=1)
set_property(SOURCE "${CC_ROOT}/src/core/zstd.c" APPEND PROPERTY
    COMPILE_OPTIONS -Os)
