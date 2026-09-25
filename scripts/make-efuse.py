#!/usr/bin/env python3
#
# Generate an eFuse blob for esp-emu's --efuse with a chosen silicon revision.
#
# Usage:
#   ./tools/make-efuse.py --chip esp32c3 --chip-rev 1.1 -o c3_rev1p1.efuse
#   esp-emu --chip esp32c3 --firmware merged_flash.bin --efuse c3_rev1p1.efuse
#
# --chip-rev takes either MAJOR.MINOR (1.1) or ESP-IDF's integer form (101, as
# in CONFIG_ESP32C3_REV_MIN_101).
#
# ESP-IDF derives the revision as efuse_hal_chip_revision() =
# major * 100 + minor, reading the two halves through the per-chip
# efuse_ll_get_chip_wafer_version_{major,minor}() accessors. FIELDS below
# mirrors those bit positions from components/efuse/<chip>/esp_efuse_table.csv;
# INJECT mirrors EfuseChipConfig::default_wafer_* in src/periph/<chip>/efuse.rs,
# so the script can reject a revision the emulator would silently override.

import argparse
import struct
import sys

# Field chunks are (block, bit-offset-within-block, width), least-significant
# chunk first: C3 splits WAFER_VERSION_MINOR into LO+HI, P4 splits MAJOR.
#   minor / major: the fields ESP-IDF's efuse_ll accessors read.
#   calib:         (block, bit, width, value) — the chip's BLK_VERSION default,
#                  matching EFUSE_CONFIG.calib_version_default. A blob replaces
#                  esp-emu's defaults wholesale, so without this the bootloader
#                  reports "efuse block revision: v0.0" and ADC calibration
#                  stops matching a real devkit.
#   inject:        (BLK1 word, mask, bits) — esp-emu ORs `bits` into that word
#                  when `word & mask == 0`.
CHIPS = {
    "esp32c3": {
        "minor": [(1, 114, 3), (1, 183, 1)],
        "major": [(1, 184, 2)],
        "calib": (2, 128, 2, 1),  # BLK_VERSION_MAJOR = 1
        "inject": (3, 0x7 << 18, 3 << 18),
    },
    "esp32c5": {
        "minor": [(1, 64, 4)],
        "major": [(1, 68, 2)],
        "calib": (1, 72, 3, 1),  # BLK_VERSION_MINOR = 1
        "inject": (2, 0x3F, 1 << 4),
    },
    "esp32c6": {
        "minor": [(1, 114, 4)],
        "major": [(1, 118, 2)],
        "calib": (1, 123, 3, 1),
        "inject": (3, 0xF << 18, 3 << 18),
    },
    "esp32h2": {
        "minor": [(1, 114, 3)],
        "major": [(1, 117, 2)],
        "calib": (2, 130, 3, 1),
        "inject": (3, 0x1F << 18, 2 << 18),
    },
    "esp32p4": {
        "minor": [(1, 64, 4)],
        "major": [(1, 68, 2), (1, 87, 1)],
        "calib": None,
        "inject": (2, 0x0080003F, (1 << 0) | (3 << 4)),
    },
    "esp32s31": {
        "minor": [(1, 114, 4)],
        "major": [(1, 118, 2)],
        "calib": None,
        "inject": (2, 0x0080003F, 0),
    },
}

# File word index of each block, per the QEMU-compatible layout esp-emu reads
# (Efuse::load_from_binary): BLK0 6 words, BLK1 6, BLK2 8, BLK3 8, keys, BLK10.
BLK_WORD_BASE = {0: 0, 1: 6, 2: 12, 3: 20}
NWORDS = 84  # 336 bytes


def put(words, chunks, value):
    """Scatter `value` across `chunks`, least-significant chunk first."""
    for blk, bit, width in chunks:
        if bit % 32 + width > 32:
            raise AssertionError(f"field at BLK{blk} bit {bit} straddles a word")
        words[BLK_WORD_BASE[blk] + bit // 32] |= (value & ((1 << width) - 1)) << (bit % 32)
        value >>= width
    if value:
        raise ValueError("value too wide for the field")


def get(words, chunks):
    """Reassemble a value scattered across `chunks`."""
    value, shift = 0, 0
    for blk, bit, width in chunks:
        field = (words[BLK_WORD_BASE[blk] + bit // 32] >> (bit % 32)) & ((1 << width) - 1)
        value |= field << shift
        shift += width
    return value


def parse_rev(text):
    if "." in text:
        major, minor = (int(x) for x in text.split(".", 1))
    else:
        major, minor = divmod(int(text), 100)
    return major, minor


def reachable(chip, major):
    """Minor values for `major` that survive esp-emu's default injection."""
    spec = CHIPS[chip]
    out = []
    for minor in range(1 << sum(w for _, _, w in spec["minor"])):
        probe = [0] * NWORDS
        put(probe, spec["minor"], minor)
        put(probe, spec["major"], major)
        if effective(chip, probe) == (major, minor):
            out.append(minor)
    return out


def effective(chip, words):
    """The (major, minor) esp-emu will report for this blob, after injection."""
    word, mask, bits = CHIPS[chip]["inject"]
    words = list(words)
    idx = BLK_WORD_BASE[1] + word
    if words[idx] & mask == 0:
        words[idx] |= bits
    return get(words, CHIPS[chip]["major"]), get(words, CHIPS[chip]["minor"])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--chip", required=True, choices=sorted(CHIPS))
    p.add_argument("--chip-rev", required=True, metavar="MAJOR.MINOR",
                   help="silicon revision, e.g. 1.1 or IDF's integer form 101")
    p.add_argument("--mac", default="24:0A:C4:00:00:01",
                   help="factory MAC (default: esp-emu's own default)")
    p.add_argument("--no-calib", action="store_true",
                   help="leave BLK_VERSION at v0.0 instead of the chip default")
    p.add_argument("-o", "--output", required=True)
    a = p.parse_args()

    spec = CHIPS[a.chip]
    major, minor = parse_rev(a.chip_rev)
    words = [0] * NWORDS

    # MAC_FACTORY is read back in reverse byte order, so BLK1 word 0 holds
    # mac[2..5] and word 1 mac[0..1]. A blob supplies the whole eFuse image, so
    # skipping this would boot the firmware with MAC 00:00:00:00:00:00.
    mac = bytes(int(b, 16) for b in a.mac.split(":"))
    if len(mac) != 6:
        p.error("--mac must be 6 colon-separated hex bytes")
    words[BLK_WORD_BASE[1] + 0] = int.from_bytes(mac[2:6], "big")
    words[BLK_WORD_BASE[1] + 1] = int.from_bytes(mac[0:2], "big")

    try:
        put(words, spec["minor"], minor)
        put(words, spec["major"], major)
    except ValueError:
        p.error(f"v{major}.{minor} does not fit {a.chip}'s wafer-version fields")

    if spec["calib"] and not a.no_calib:
        blk, bit, width, val = spec["calib"]
        put(words, [(blk, bit, width)], val)

    # esp-emu re-injects its default wafer bits whenever the masked word reads
    # zero, which can override the request (C3/C6 mask only the minor field, so
    # any minor that leaves it zero is unreachable). Refuse rather than emit a
    # blob that boots as some other revision.
    got = effective(a.chip, words)
    if got != (major, minor):
        alts = reachable(a.chip, major)
        print(f"error: esp-emu would report v{got[0]}.{got[1]}, not v{major}.{minor}.\n"
              f"  Its default wafer bits are re-injected when the masked word reads zero\n"
              f"  (EfuseChipConfig::default_wafer_* in src/periph/{a.chip}/efuse.rs).\n"
              f"  Reachable minors for major {major}: "
              f"{', '.join(str(m) for m in alts) if alts else '(none)'}",
              file=sys.stderr)
        return 1

    with open(a.output, "wb") as f:
        f.write(b"".join(struct.pack("<I", w) for w in words))
    print(f"{a.output}: {a.chip} rev v{major}.{minor}, MAC {a.mac}")
    return 0


if __name__ == "__main__":
    sys.exit(main())