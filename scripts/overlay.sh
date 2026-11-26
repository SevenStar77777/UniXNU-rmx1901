#!/usr/bin/env bash
# Copy this repository's board files into a U-Boot checkout, then apply patches.
set -euo pipefail

UB=${1:-uboot}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

[ -d "$UB/dts/upstream/src/arm64/qcom" ] || {
	echo "error: $UB is not a u-boot checkout with dts/upstream" >&2; exit 1; }

install -D -m 644 "$ROOT/dts/sdm710-realme-rmx1901.dts" \
	"$UB/dts/upstream/src/arm64/qcom/sdm710-realme-rmx1901.dts"
for f in "$ROOT"/board/realme/rmx1901/*; do
	install -D -m 644 "$f" "$UB/board/realme/rmx1901/$(basename "$f")"
done

echo "overlaid rmx1901 board files into $UB"

for p in "$ROOT"/patches/uboot/*.patch; do
	[ -e "$p" ] || continue
	echo "applying $(basename "$p")"
	patch -p1 -d "$UB" --batch --forward --no-backup-if-mismatch < "$p" 2>&1 \
		|| echo "  (patch may already be upstream, continuing)"
done

grep -q 'GCC_UFS_PHY_RX_SYMBOL_1_CLK' "$UB/drivers/clk/qcom/clock-sdm845.c" \
	&& echo "UFS PHY RX lane 1 clock confirmed" \
	|| echo "UFS PHY RX lane 1 clock NOT found (continuing)"

echo "patched $UB"
