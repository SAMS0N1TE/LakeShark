file(GLOB IMBE_SRCS "*.cc")

idf_component_register(
    SRCS ${IMBE_SRCS} "imbe_shim.cpp"
    INCLUDE_DIRS "."
)

# Third-party fixed-point vocoder (OP25 / Pavel Yazev). Silence its warnings so
# the project's -Werror doesn't trip on legacy ETSI code we don't want to edit.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
