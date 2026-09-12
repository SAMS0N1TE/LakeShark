# upstream/ is unmodified MeshCore (MIT, see upstream/LICENSE.meshcore) plus

set(MESH_SRCS
    ls_mesh.cpp
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
list(REMOVE_ITEM MESH_UPSTREAM_SRCS ls_mesh.cpp)
set_source_files_properties(${MESH_UPSTREAM_SRCS} PROPERTIES COMPILE_OPTIONS "-w")

set_source_files_properties(ls_mesh.cpp PROPERTIES COMPILE_OPTIONS "-Wno-reorder")
