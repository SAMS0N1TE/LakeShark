idf_component_register(
    SRCS
        klatt-tts/src/config.cc
        klatt-tts/src/klatt.cc
        klatt-tts/src/phonemes.cc
        klatt-tts/src/synth_internal.cc
        klatt-tts/src/synth_stream.cc
        g2p/src/g2p.cc
        g2p/src/g2p_dict.cc
        g2p/src/g2p_numbers.cc
        g2p/src/g2p_phones.cc
        g2p/src/g2p_rules.cc
        g2p/src/ipa_tokens.cc
    INCLUDE_DIRS
        klatt-tts/include
        g2p/include
    PRIV_INCLUDE_DIRS
        klatt-tts/include/tts
        g2p/include/g2p
        g2p/src
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LAKESHARK_COMPACT_DICT)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
