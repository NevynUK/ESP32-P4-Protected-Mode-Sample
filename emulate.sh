#!/usr/bin/env bash
# emulate.sh -- run the firmware under the ESP32 emulator instead of hardware.
#
#   ./emulate.sh                  run until you stop it
#   ./emulate.sh --timeout 10s    stop by itself; handy for a scripted capture
#   ./emulate.sh <anything else>  passed straight through to esp-emu
#
# TWO THINGS DIFFER FROM ./monitor.sh, and both catch people out:
#
#   - the console arrives on THIS terminal, not over a serial port.  There is
#     no /dev/cu.* involved and nothing to open separately;
#   - stop it with Ctrl-C.  The Ctrl-] that idf.py monitor uses does nothing
#     here, because this is not idf.py monitor.
#
# Needs build/merged-binary.bin, which ./build.sh produces on every run.

set -euo pipefail
cd "$(dirname "$0")"

FIRMWARE=build/merged-binary.bin
EFUSE="${ESP_EMU_EFUSE:-efuse.bin}"
PSRAM="${ESP_EMU_PSRAM:-32M}"
ROM="${ESP_EMU_ROM:-$HOME/.espressif/tools/esp-rom-elfs/20241011/esp32p4_rev0_rom.elf}"

if ! command -v esp-emu >/dev/null 2>&1; then
    echo "error: esp-emu is not on PATH" >&2
    exit 1
fi

if [ ! -f "$FIRMWARE" ]; then
    echo "error: no $FIRMWARE -- run ./build.sh first" >&2
    exit 1
fi

# The eFuse image is gitignored, so a fresh clone will not have one.  Make it
# rather than failing: the emulated part has to report a silicon revision this
# project's bootloader will accept, and the default it would otherwise use is
# not one.  sdkconfig.defaults pins the range (CONFIG_ESP32P4_REV_MIN_1 and a
# max of v1.99), and 1.3 sits inside it.
#
# esp-emu also writes this file back on exit ("eFuse state saved to ..."), so
# it is state rather than a fixed input -- which is another reason it is not in
# the repository.  Delete it to get a clean part back.
if [ ! -f "$EFUSE" ]; then
    echo "==> $EFUSE missing; generating one for an ESP32-P4 rev v1.3"
    python3 scripts/make-efuse.py --chip esp32p4 --chip-rev 1.3 -o "$EFUSE"
fi

args=(--chip esp32p4 --firmware "$FIRMWARE" --psram-size "$PSRAM" --efuse "$EFUSE")

# The ROM ELF only gives the emulator symbol names for ROM addresses, so it is
# a nicety rather than a requirement -- esp-emu carries its own if this one is
# not where it usually lives.
if [ -f "$ROM" ]; then
    args+=(--rom "$ROM")
else
    echo "note: no ROM ELF at $ROM; using the emulator's built-in"
fi

echo "==> esp-emu: ESP32-P4, ${PSRAM} PSRAM  (Ctrl-C to stop, NOT Ctrl-])"
exec esp-emu "${args[@]}" "$@"
