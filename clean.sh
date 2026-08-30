#!/usr/bin/env bash
# clean.sh -- drop the build directory.  Use --full to drop sdkconfig too, so
# the next build regenerates it from sdkconfig.defaults.

set -euo pipefail
cd "$(dirname "$0")"

rm -rf build
echo "==> removed build/"

if [ "${1:-}" = "--full" ]; then
    rm -f sdkconfig sdkconfig.old
    echo "==> removed sdkconfig; the next build will regenerate it"
fi
