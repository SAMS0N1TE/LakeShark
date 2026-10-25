#!/bin/bash

INSTALL_DIR1=${1}
INSTALL_DIR2=${2}

abidiff \
    --headers-dir1 "${INSTALL_DIR1}/include/libssh/" \
    --headers-dir2 "${INSTALL_DIR2}/include/libssh/" \
    "${INSTALL_DIR1}/lib64/libssh.so" \
    "${INSTALL_DIR2}/lib64/libssh.so" \
    --fail-no-debug-info
abiret=$?

ABIDIFF_ERROR=$(((abiret & 0x01) != 0))
ABIDIFF_USAGE_ERROR=$(((abiret & 0x02) != 0))
ABIDIFF_ABI_CHANGE=$(((abiret & 0x04) != 0))
ABIDIFF_ABI_INCOMPATIBLE_CHANGE=$(((abiret & 0x08) != 0))
ABIDIFF_UNKNOWN_BIT_SET=$(((abiret & 0xf0) != 0))

if [ $ABIDIFF_ERROR -ne 0 ]; then
    echo "abidiff reported ABIDIFF_ERROR."
    exit 1
fi
if [ $ABIDIFF_USAGE_ERROR -ne 0 ]; then
    echo "abidiff reported ABIDIFF_USAGE_ERROR."
    exit 1
fi
if [ $ABIDIFF_UNKNOWN_BIT_SET -ne 0 ]; then
    echo "abidiff reported ABIDIFF_UNKNOWN_BIT_SET."
    exit 1
fi

if [ $ABIDIFF_ABI_INCOMPATIBLE_CHANGE -ne 0 ]; then
    echo "abidiff result ABIDIFF_ABI_INCOMPATIBLE_CHANGE, this breaks the API!"
    exit 1
fi

if [ $ABIDIFF_ABI_CHANGE -ne 0 ]; then
  echo "Ignoring abidiff result ABI_CHANGE"
fi

exit 0
