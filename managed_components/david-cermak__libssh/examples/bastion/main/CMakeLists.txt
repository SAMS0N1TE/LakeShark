idf_component_register(SRCS
        bastion.c
        tunnel.c
        server.c
        ssh_vfs.c
        softap.c
    INCLUDE_DIRS .
    EMBED_TXTFILES ssh_host_ed25519_key
)
