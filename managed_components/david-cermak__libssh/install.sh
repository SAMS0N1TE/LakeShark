#!/bin/bash

# libssh installation script
# This script patches the libssh upstream mirror (adds optional MLKEM/SNTRUP
# support and mbedTLS v4 / PSA-Crypto backend support).
#
# Patches are applied in the order listed below and must be applied from the
# libssh component root directory (the parent of libssh-mirror/).

set -e  # Exit on any error

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Configuration
LIBSSH_VERSION="mirror"    # Developer's mode
PATCH_DIR="${SCRIPT_DIR}/patches"

# Patches are applied in this order; mbedtls_v4 must go first so the later
# port patches build on top of the mbedTLS v4 backend changes.
PATCH_ORDER="mbedtls_v4.patch esp_idf_port.patch"

# Apply ESP-IDF port patches
for patch_name in ${PATCH_ORDER}; do
    patch_file="${PATCH_DIR}/${patch_name}"
    [ -f "$patch_file" ] || {
        echo "Missing patch: ${patch_name}"
        exit 1
    }
    echo "Applying patch: ${patch_name}..."
    if ! patch -p0 < "$patch_file"; then
        echo "Failed to apply patch: ${patch_name}"
        exit 1
    fi
done

echo "libssh ${LIBSSH_VERSION} installed successfully."
