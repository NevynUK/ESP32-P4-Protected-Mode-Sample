#!/usr/bin/env bash
# build.sh -- configure for the ESP32-P4 and build.
#
#   ./build.sh              build
#   ./build.sh --reconfigure  re-run cmake first (after editing sdkconfig.defaults)
#   ./build.sh fullclean      throw the build away first
#
# IDF_VERSION=5.5.3 ./build.sh picks a different toolchain.
#
# Always finishes with `idf.py merge-bin`, which flattens the bootloader,
# partition table and app into build/merged-binary.bin.  That single image is
# what ./emulate.sh feeds the emulator; flashing a real board does not need it,
# but it costs a moment and means the emulator is never a rebuild behind.

set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=scripts/idf-env.sh
. scripts/idf-env.sh

echo "==> ESP-IDF ${ESP_IDF_VERSION:-?} at $IDF_PATH"

# set-target rewrites sdkconfig from sdkconfig.defaults; only do it if the
# existing sdkconfig is missing or is for another chip.
if ! grep -q '^CONFIG_IDF_TARGET="esp32p4"$' sdkconfig 2>/dev/null; then
    echo "==> setting target esp32p4"
    idf_py set-target esp32p4
fi

idf_py "$@" build

# One flat image for the emulator.  See emulate.sh.
echo "==> merging to build/merged-binary.bin"
idf_py merge-bin
