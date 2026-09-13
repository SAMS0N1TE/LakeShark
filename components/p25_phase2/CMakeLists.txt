file(GLOB P2_C "upstream/*.c")
file(GLOB P2_CPP "upstream/*.cc")
idf_component_register(SRCS p25_phase2.cpp ${P2_C} ${P2_CPP}
    INCLUDE_DIRS "." PRIV_INCLUDE_DIRS upstream)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-missing-field-initializers)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 $<$<COMPILE_LANGUAGE:C>:-fsingle-precision-constant>)
