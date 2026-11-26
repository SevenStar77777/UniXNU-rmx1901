#!/usr/bin/env bash
# Copy this repository's board files into a U-Boot checkout, then apply patches.
set -euo pipefail

UB=${1:-uboot}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

[ -d "$UB/dts/upstream/src/arm64/qcom" ] || { echo "error: $UB not a u-boot checkout" >&2; exit 1; }

# Overlay rmx1901 DTS
install -D -m 644 "$ROOT/dts/sdm710-realme-rmx1901.dts" \
	"$UB/dts/upstream/src/arm64/qcom/sdm710-realme-rmx1901.dts"
# Overlay board env variants
for f in "$ROOT"/board/realme/rmx1901/*; do
	install -D -m 644 "$f" "$UB/board/realme/rmx1901/$(basename "$f")"
done

echo "overlaid rmx1901 board files into $UB"

# Source patches
for p in "$ROOT"/patches/uboot/*.patch; do
	[ -e "$p" ] || continue
	echo "applying $(basename "$p")"
	patch -p1 -d "$UB" --batch --forward --no-backup-if-mismatch < "$p"
done

# UFS PHY RX lane 1 clock patch sanity check
grep -q 'GCC_UFS_PHY_RX_SYMBOL_1_CLK,\t\t0x770a8' \
	"$UB/drivers/clk/qcom/clock-sdm845.c" || { echo "error: UFS PHY RX lane 1 patch did not apply" >&2; exit 1; }

echo "patched $UB"
