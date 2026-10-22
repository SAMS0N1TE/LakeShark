# The PMTiles reader, vendored from ZeroMesh where it runs on a Flipper Zero.
#
# A Flipper has about 256 KB of RAM for everything, so this reader was written
# against a budget far tighter than this board's: the root directory holds one
# entry per leaf and only a single leaf is ever resident, which is what keeps
# a statewide archive open without cost scaling with tile count.
#
# upstream/ is byte-identical to ZeroMesh so it can be re-synced with a copy.
# compat/ is what makes it build here: six storage calls and a logger. See
# components/meshcore/compat/ for the same arrangement.
idf_component_register(
    SRCS
        "upstream/zeromesh_pmtiles.c"
    INCLUDE_DIRS
        "upstream"
    PRIV_INCLUDE_DIRS
        "compat"
)

# Size build: every MAP tile is read and inflated here, so it keeps -O2.
if(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
endif()
