# Embedded ABI contract consumed by integration CMake files.
set(CC_EMBED_API_VERSION 2)
set(CC_CTILE_FORMAT_MAX 7)
# Require the matching public header contract before compiling any source.
if(NOT EXISTS "${CC_ROOT}/include/cartocore/core.h" OR NOT EXISTS "${CC_ROOT}/include/cartocore/ctile.h")
    message(FATAL_ERROR "CartoCore header mismatch: public core.h/ctile.h missing")
endif()
file(READ "${CC_ROOT}/include/cartocore/core.h" CC_CORE_HEADER)
file(READ "${CC_ROOT}/include/cartocore/ctile.h" CC_CTILE_HEADER)
if(NOT CC_CORE_HEADER MATCHES "#define CC_EMBED_API_VERSION 2([\r\n]|$)" OR
   NOT CC_CTILE_HEADER MATCHES "#define CC_CTILE_VERSION_MAX 7([\r\n]|$)" OR
   NOT CC_CTILE_HEADER MATCHES "#define CC_CTILE_VERSION_MIN 2([\r\n]|$)" OR
   NOT CC_CTILE_HEADER MATCHES "cc_ctile_header_check")
    message(FATAL_ERROR "CartoCore header/version mismatch: expected embed API 2, CTILE 2..7 and cc_ctile_header_check")
endif()
# Canonical freestanding/embedded engine sources. Caller sets CC_ROOT.
set(CC_CORE_SRCS
    "${CC_ROOT}/src/core/core.c"
    "${CC_ROOT}/src/core/ctile.c"
    "${CC_ROOT}/src/core/cellset.c"
    "${CC_ROOT}/src/core/zstd.c"
    "${CC_ROOT}/src/core/overzoom.c"
    "${CC_ROOT}/src/core/ctile_source.c"
    "${CC_ROOT}/src/core/decoded_cache.c"
    "${CC_ROOT}/src/core/render.c"
    "${CC_ROOT}/src/core/cache.c"
    "${CC_ROOT}/src/core/style.c"
    "${CC_ROOT}/src/core/simd.c"
    "${CC_ROOT}/src/core/arch.c"
    "${CC_ROOT}/src/core/profile.c"
    "${CC_ROOT}/src/core/wire.c"
    "${CC_ROOT}/src/core/terrain.c"
    "${CC_ROOT}/src/places/places.c"
)
set(CC_OUT_SRCS
    "${CC_ROOT}/src/out/out.c"
)
set(CC_MEDIA_SRCS
    "${CC_ROOT}/src/media/render.c"
    "${CC_ROOT}/src/media/png.c"
    "${CC_ROOT}/src/media/jpeg.c"
)
set(CC_INFLATE_SRCS
    "${CC_ROOT}/src/ingest/inflate.c"
)
set(CC_IDF_SRCS ${CC_CORE_SRCS} ${CC_OUT_SRCS} ${CC_MEDIA_SRCS} ${CC_INFLATE_SRCS})
set(CC_PUBLIC_INCLUDE_DIRS "${CC_ROOT}/include")
set(CC_P4_ASM "${CC_ROOT}/src/arch/riscv_esp32p4/kernels.S")
