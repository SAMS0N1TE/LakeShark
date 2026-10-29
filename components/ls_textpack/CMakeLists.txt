# Text packs (see include/ls_textpack.h). Packed only where CartoCore's zstd
# decoder is already linked: the T-Display and a Nano with the TUI panel.
# Elsewhere LS_TEXT(id, "...") is the literal and nothing is generated.
idf_component_register(SRCS ls_textpack.c
                       INCLUDE_DIRS include
                       PRIV_REQUIRES cartocore heap)

if(NOT CMAKE_BUILD_EARLY_EXPANSION AND (CONFIG_LS_BOARD_T_DISPLAY_P4 OR CONFIG_LS_PANEL_RUNTIME))
    idf_build_get_property(LS_TEXT_PY PYTHON)
    idf_build_get_property(LS_PROJECT_DIR PROJECT_DIR)
    set(LS_TEXT_GEN ${CMAKE_CURRENT_LIST_DIR}/gen/textpack_gen.py)
    set(LS_TEXT_OUT ${CMAKE_CURRENT_BINARY_DIR}/gen)
    # Where LS_TEXT may be written. An id used outside these is a compile
    # error (LS_TXT_<id> undeclared), never a silent miss.
    set(LS_TEXT_DIRS ${LS_PROJECT_DIR}/main ${LS_PROJECT_DIR}/components/apps
                     ${LS_PROJECT_DIR}/components/lakeshark)
    set(LS_TEXT_GLOBS)
    foreach(d ${LS_TEXT_DIRS})
        list(APPEND LS_TEXT_GLOBS ${d}/*.c ${d}/*.cpp ${d}/*.cc ${d}/*.h ${d}/*.hpp ${d}/*.inc)
    endforeach()
    file(GLOB_RECURSE LS_TEXT_SCANNED CONFIGURE_DEPENDS ${LS_TEXT_GLOBS})
    set(LS_TEXT_CMD ${LS_TEXT_PY} ${LS_TEXT_GEN} text --root=${LS_PROJECT_DIR} ${LS_TEXT_OUT} ${LS_TEXT_DIRS})
    # Once at configure time as well, so the id header exists before any
    # component that includes it starts compiling.
    execute_process(COMMAND ${LS_TEXT_CMD} RESULT_VARIABLE LS_TEXT_RC)
    if(NOT LS_TEXT_RC EQUAL 0)
        message(FATAL_ERROR "textpack_gen.py failed (${LS_TEXT_RC})")
    endif()
    add_custom_command(OUTPUT ${LS_TEXT_OUT}/ls_textpack_ids.h ${LS_TEXT_OUT}/ls_textpack_data.c
        COMMAND ${LS_TEXT_CMD}
        DEPENDS ${LS_TEXT_GEN} ${CMAKE_CURRENT_LIST_DIR}/gen/lszstd.py ${LS_TEXT_SCANNED}
        COMMENT "Packing LS_TEXT strings"
        VERBATIM)
    add_custom_target(ls_textpack_gen DEPENDS ${LS_TEXT_OUT}/ls_textpack_ids.h ${LS_TEXT_OUT}/ls_textpack_data.c)
    add_dependencies(${COMPONENT_LIB} ls_textpack_gen)
    target_sources(${COMPONENT_LIB} PRIVATE ${LS_TEXT_OUT}/ls_textpack_data.c)
    target_include_directories(${COMPONENT_LIB} PUBLIC ${LS_TEXT_OUT})
    target_compile_definitions(${COMPONENT_LIB} PUBLIC LS_TEXT_PACKED=1)
endif()
