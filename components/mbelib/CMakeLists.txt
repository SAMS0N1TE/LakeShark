idf_component_register(SRCS "mbelib.c" "imbe7200x4400.c" "ecc.c"
                    INCLUDE_DIRS "."
                    )
set_source_files_properties(mbelib.c imbe7200x4400.c ecc.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-variable -Wno-maybe-uninitialized -Wno-missing-braces"
)

# Size build: hot code (map render / vocoders) keeps -O2.
if(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
endif()
