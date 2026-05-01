idf_component_register(SRCS "mbelib.c" "imbe7200x4400.c" "ecc.c" "ambe3600x2450.c" "ambe3600x2400.c"
                    INCLUDE_DIRS "."
                    )
set_source_files_properties(mbelib.c imbe7200x4400.c ecc.c ambe3600x2450.c ambe3600x2400.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-variable -Wno-maybe-uninitialized -Wno-missing-braces"
)
