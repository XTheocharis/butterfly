#!/usr/bin/env bash
# Post-link check: verify the Adafruit-style single-tap-bypass magic
# 0x87EEB07C does NOT appear at flash offset 0x26200 in the CLUE image.
#
# The nRF52840 bootloader reads the 4 bytes at the start of the app
# image (app origin 0x26000 + offset 0x200 = flash 0x26200) and, if
# they equal 0x87EEB07C in little-endian byte order, treats the app
# as requesting single-tap-bypass. This breaks user-facing recovery
# (the device never enters DFU on a single reset tap), so the link
# must fail if those bytes accidentally land at that address.
#
# The linked .hex is read indirectly via its sibling .bin (a flat
# byte image of the app); offset 0x200 inside .bin == flash 0x26200.
#
# Exit codes:
#   0 — magic NOT present (good)
#   1 — magic present at 0x26200 (BAD)
#   2 — usage / missing input
set -euo pipefail

if [[ "$#" -ne 1 ]]; then
	echo "usage: $0 <image.hex>" >&2
	exit 2
fi

hex_file="$1"
bin_file="${hex_file%.hex}.bin"

if [[ ! -s "$bin_file" ]]; then
	echo "FAIL: $bin_file missing or empty (post-link needs both .hex and .bin)" >&2
	exit 2
fi

# If the image is smaller than 0x204 bytes, offset 0x26200 is unmapped
# (treated as erased 0xFF). Not the magic.
needed=$((0x200 + 4))
actual="$(wc -c <"$bin_file")"
if (( actual < needed )); then
	echo "OK: image only ${actual} bytes, offset 0x26200 unmapped"
	exit 0
fi

# Extract the 4 bytes at offset 0x200 and render as lowercase hex.
word_hex="$(dd if="$bin_file" bs=1 skip=$((0x200)) count=4 status=none \
	| od -An -tx1 -v | tr -d ' \n')"

# 0x87EEB07C in little-endian byte order = 7c b0 ee 87.
if [[ "$word_hex" == "7cb0ee87" ]]; then
	echo "FAIL: bootloader single-tap magic 0x87EEB07C at flash 0x26200 in ${hex_file}" >&2
	exit 1
fi

echo "OK: flash 0x26200 = 0x${word_hex} (LE bytes) - not single-tap magic"
exit 0
