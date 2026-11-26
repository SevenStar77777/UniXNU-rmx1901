#!/usr/bin/env bash
# Build U-Boot for realme X (RMX1901) and pack it into a boot image.
set -euo pipefail

UB=${1:-uboot}
OUT=${2:-out}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT_ABS=$(mkdir -p "$OUT" && cd "$OUT" && pwd)
CROSS=${CROSS_COMPILE:-aarch64-linux-gnu-}
DEVICE_TREE=${DEVICE_TREE:-qcom/sdm710-realme-rmx1901}
DTB_REL="dts/${DTB_SUBDIR:-upstream/src/arm64/qcom}/sdm710-realme-rmx1901.dtb"

build() { make -s O="$1" -j"$(nproc)" DEVICE_TREE="$DEVICE_TREE" CROSS_COMPILE="$CROSS"; }

require_config() {
	local dir=$1 sym; shift
	for sym in "$@"; do
		grep -qx "CONFIG_${sym}=y" "$dir/.config" || { echo "error: CONFIG_$sym missing from $dir/.config" >&2; exit 1; }
	done
	echo "    required config ok: $*"
}

fix_image_header() {
	python3 - "$1" <<'PY'
import struct, sys
path = sys.argv[1]
blob = bytearray(open(path, "rb").read())
if len(blob) < 64 or blob[0x38:0x3c] != b"ARMd":
	sys.exit(f"error: {path} has no arm64 Image header")
struct.pack_into("<Q", blob, 0x08, 0)
struct.pack_into("<Q", blob, 0x10, len(blob))
open(path, "wb").write(blob)
text_off, image_size, flags = struct.unpack_from("<QQQ", blob, 8)
if text_off != 0 or image_size != len(blob):
	sys.exit(f"error: {path}: text_offset {text_off:#x} image_size {image_size:#x}")
print(f"    image header: text_offset 0 image_size {image_size:#x} flags {flags:#x}")
PY
}

make_payload() {
	local dir=$1 out=$2
	local nodtb="$dir/u-boot-nodtb.bin" dtb="$dir/$DTB_REL" bin="$dir/u-boot+dtb.bin"
	[ -f "$nodtb" ] || { echo "error: $nodtb missing" >&2; exit 1; }
	[ -f "$dtb" ] || { echo "error: $dtb missing" >&2; exit 1; }
	cat "$nodtb" "$dtb" > "$bin"
	fix_image_header "$bin"
	gzip -9 -n -c "$bin" > "$out"
}

pack() { python3 "$ROOT/scripts/pack-boot.py" "$1" --dtb-table "$OUT_ABS/stock-fb.bin" -o "$2"; }

cd "$UB"
echo "== defconfig"
make -s O="$OUT_ABS" qcom_defconfig qcom-phone.config rmx1901.config
require_config "$OUT_ABS" EFI_PARTITION CMD_PART CMD_FAT FS_FAT CMD_FS_GENERIC \
	FASTBOOT_FLASH_BLOCK CMD_FASTBOOT USB_DWC3_GADGET FASTBOOT_OEM_RUN \
	FASTBOOT_CMD_OEM_CONSOLE CONSOLE_RECORD

echo "== build"
build "$OUT_ABS"

python3 "$ROOT/scripts/patch-stock-dtb.py" "$ROOT/firmware/stock-appended-dtbs.bin" \
        "$OUT_ABS/stock-fb.bin"

echo "== pack"
make_payload "$OUT_ABS" "$OUT_ABS/payload.gz"
pack "$OUT_ABS/payload.gz" "$OUT_ABS/boot.img"

cat "$OUT_ABS/payload.gz" "$OUT_ABS/$DTB_REL" \
    > "$OUT_ABS/payload-external.gz"
python3 "$ROOT/scripts/pack-boot.py" "$OUT_ABS/payload-external.gz" -o "$OUT_ABS/boot-boarddtb.img"

ls -l "$OUT_ABS/boot.img" "$OUT_ABS/boot-boarddtb.img" "$OUT_ABS/stock-fb.bin" "$OUT_ABS/u-boot+dtb.bin"

build_env_variant() {
	local name=$1 envf=$2 img=$3 dir
	dir="$(dirname "$OUT_ABS")/out-$name"
	echo "== build $name ($dir)"
	make -s O="$dir" qcom_defconfig qcom-phone.config rmx1901.config
	bash ./scripts/config --file "$dir/.config" --set-str ENV_DEFAULT_ENV_TEXT_FILE "$envf"
	make -s O="$dir" olddefconfig
	build "$dir"
	grep -q "$(basename "$envf")" "$dir/include/generated/autoconf.h" || { echo "error: $name env did not take effect" >&2; exit 1; }
	make_payload "$dir" "$dir/payload.gz"
	pack "$dir/payload.gz" "$OUT_ABS/$img"
	ls -l "$OUT_ABS/$img"
}

build_env_variant probe board/realme/rmx1901/rmx1901-probe.env boot-probe.img
build_env_variant diag  board/realme/rmx1901/rmx1901-diag.env  boot-diag.img
build_env_variant usb   board/realme/rmx1901/rmx1901-usb.env   boot-usb.img
build_env_variant efi   board/realme/rmx1901/rmx1901-efi.env   boot-efi.img
