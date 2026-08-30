#!/usr/bin/env bash
# monitor.sh -- open the console.
#
#   ./monitor.sh                    autodetect the UART0 bridge
#   ./monitor.sh /dev/cu.usbserial-1130
#
# The console is UART0 (GPIO37/38), which on this board reaches the external
# USB bridge -- /dev/cu.usbserial-*, NOT the /dev/cu.usbmodem* that flash.sh
# uses.  Ctrl-] to quit.

set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=scripts/idf-env.sh
. scripts/idf-env.sh

PORT="${1:-${ESPMONITORPORT:-}}"
if [ -z "$PORT" ]; then
    PORT="$(pick_port '/dev/cu.usbserial-*' || true)"
fi

if [ -z "$PORT" ]; then
    echo "error: no /dev/cu.usbserial-* found; pass the port explicitly" >&2
    ls /dev/cu.* 2>/dev/null >&2 || true
    exit 1
fi

echo "==> console on $PORT (Ctrl-] to quit)"
idf_py -p "$PORT" monitor
