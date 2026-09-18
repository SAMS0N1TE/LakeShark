file(GLOB IMBE_SRCS "*.cc")

idf_component_register(
    SRCS ${IMBE_SRCS} "imbe_shim.cpp"
    INCLUDE_DIRS "."
)

# Third-party fixed-point vocoder (OP25 / Pavel Yazev). Silence its warnings so
# the project's -Werror doesn't trip on legacy ETSI code we don't want to edit.
#
# -fwrapv because the wrap is the method, not an accident: L_add in basicop2.cc
# performs the addition that overflows and then inspects the sign bits to clamp
# to MIN_32/MAX_32, and v_synt.cc multiplies "by mod 1" in as many words. Signed
# overflow is undefined in C, so a compiler is entitled to assume it cannot
# happen and fold the very checks this code is built out of. It has not done so
# here, but that is luck rather than agreement, and the flag costs nothing.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -fwrapv)
