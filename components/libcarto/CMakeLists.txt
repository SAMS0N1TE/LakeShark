# libcarto, vendored. See DEVIATIONS.md for what differs from upstream and
# PROVENANCE.txt for which commit this is.
#
# cells.c is deliberately absent: it draws a map into a character grid, and
# this firmware draws the map into its own rect of direct pixels instead. See
# DEVIATIONS.md section 2 for the measurement behind that.
idf_component_register(
    SRCS
        "upstream/src/carto.c"
        "upstream/src/framebuffer.c"
        "upstream/src/geom.c"
        "upstream/src/mvt.c"
        "upstream/src/raster.c"
        "upstream/src/style.c"
    INCLUDE_DIRS
        "upstream/include"
    PRIV_INCLUDE_DIRS
        "upstream/src"
)

# Upstream is warning-clean under -Wall -Wextra on a desktop; the IDF adds a
# few of its own. Kept as warnings rather than silenced, so a real one is
# still visible in the gate's output.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter)
# Size build: MAP's FIELD views decode and draw every tile here, so it keeps
# -O2. Built for size it drew a tile in 148 ms against 22 ms (z10, LR board).
if(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
endif()
