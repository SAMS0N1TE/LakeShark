
set(srcs
    "audio_player.cpp"
)

set(includes
    "include"
)

set(requires "")

# LakeShark distributes the WAV player without the Helix decoder.

if(CONFIG_AUDIO_PLAYER_ENABLE_WAV)
    list(APPEND srcs "audio_wav.cpp")
endif()

idf_component_register(SRCS "${srcs}"
                       REQUIRES "${requires}"
                       INCLUDE_DIRS "${includes}"
                       REQUIRES driver
)
