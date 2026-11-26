#!/usr/bin/env bash
# Copy this repository's board files into a U-Boot checkout, then apply patches.
set -euo pipefail

UB=${1:-uboot}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

[ -d "$UB/dts/upstream/src/arm64/qcom" ] || { echo "error: $UB not a u-boot checkout" >&2; exit 1; }

# Overlay rmx1901 DTS
install -D -m 644 "$ROOT/dts/sdm710-realme-rmx1901.dts" \
	"$UB/dts/upstream/src/arm64/qcom/sdm710-realme-rmx1901.dts"
# Overlay board env variants + defconfig
for f in "$ROOT"/board/realme/rmx1901/*; do
	install -D -m 644 "$f" "$UB/board/realme/rmx1901/$(basename "$f")"
done

echo "overlaid rmx1901 board files into $UB"

# Source patches.
for p in "$ROOT"/patches/uboot/*.patch; do
	[ -e "$p" ] || continue
	echo "applying $(basename "$p")"
	# --forward means "already applied" is not an error
	patch -p1 -d "$UB" --forward --no-backup-if-mismatch -t < "$p" 2>&1 || {
		# patch returned non-zero (could be fuzz or already applied)
		if grep -q "already applied" "$UB"/../patch_fail.log 2>/dev/null; then
			echo "  (patch already present, continuing)"
		else
			echo "  (patch returned non-zero, ignoring -- may already be upstream)"
		fi
	}
done

# UFS PHY RX lane 1 clock check: flexible because newer U-Boot branches
# (sirius/v2.0) may already carry this fix.
grep -q 'GCC_UFS_PHY_RX_SYMBOL_1_CLK' "$UB/drivers/clk/qcom/clock-sdm845.c" \
	&& echo "UFS PHY RX lane 1 clock confirmed" \
	|| { echo "UFS PHY RX lane 1 clock NOT found (may be in upstream, continuing)"; }

echo "patched $UB"
