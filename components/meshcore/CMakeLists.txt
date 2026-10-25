# upstream/ is unmodified MeshCore (MIT, see upstream/LICENSE.meshcore) plus

set(MESH_SRCS
    ls_mesh.cpp
    ls_mesh_inspect.c
    upstream/src/Dispatcher.cpp
    upstream/src/Identity.cpp
    upstream/src/Mesh.cpp
    upstream/src/Packet.cpp
    upstream/src/Utils.cpp
    upstream/src/StaticPoolPacketManager.cpp
    upstream/src/helpers/AdvertDataHelpers.cpp
    upstream/ed25519/add_scalar.c
    upstream/ed25519/fe.c
    upstream/ed25519/ge.c
    upstream/ed25519/key_exchange.c
    upstream/ed25519/keypair.c
    upstream/ed25519/sc.c
    upstream/ed25519/seed.c
    upstream/ed25519/sha512.c
    upstream/ed25519/sign.c
    upstream/ed25519/verify.c
)

idf_component_register(
    SRCS ${MESH_SRCS}
    INCLUDE_DIRS compat upstream/src upstream/ed25519 .
    REQUIRES lakeshark mbedtls nvs_flash esp_timer esp_system
)

set(MESH_UPSTREAM_SRCS ${MESH_SRCS})
list(REMOVE_ITEM MESH_UPSTREAM_SRCS ls_mesh.cpp ls_mesh_inspect.c)
set_source_files_properties(${MESH_UPSTREAM_SRCS} PROPERTIES COMPILE_OPTIONS "-w")

# libssh (the SSH terminal) exports sha512_init, _update, _final and sha512
# as well. These are the ed25519 code's own, used nowhere else: renamed here
# so both link, with upstream/ left as it is.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    sha512_init=mesh_sha512_init sha512_update=mesh_sha512_update
    sha512_final=mesh_sha512_final sha512=mesh_sha512)

set_source_files_properties(ls_mesh.cpp PROPERTIES COMPILE_OPTIONS "-Wno-reorder")
