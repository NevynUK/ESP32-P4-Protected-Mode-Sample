#!/usr/bin/env bash
# flash.sh -- build and flash the board.
#
#   ./flash.sh                  autodetect the esptool port
#   ./flash.sh /dev/cu.usbmodem101   use a specific one
#
# The board has two USB paths and they are NOT interchangeable:
#
#   /dev/cu.usbmodem*    the P4's USB-Serial/JTAG port -- esptool and OpenOCD
#   /dev/cu.usbserial-*  the external bridge on UART0 -- the console
#
# sdkconfig.defaults puts the console on UART0 deliberately, so that the board
# can be reset and reflashed through one port while a capture is held open on
# the other.  Flash here, watch with ./monitor.sh.

set -euo pipefail
cd "$(dirname "$0")"
# shellcheck source=scripts/idf-env.sh
. scripts/idf-env.sh

PORT="${1:-${ESPPORT:-}}"
if [ -z "$PORT" ]; then
    PORT="$(pick_port '/dev/cu.usbmodem*' || true)"
fi

if [ -z "$PORT" ]; then
    echo "error: no /dev/cu.usbmodem* found; pass the port explicitly" >&2
    ls /dev/cu.* 2>/dev/null >&2 || true
    exit 1
fi

echo "==> flashing via $PORT"
./build.sh
idf_py -p "$PORT" flash
