#!/usr/bin/env bash
# Build U-Boot for realme X (RMX1901, SDM710) and pack into a boot image.
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
		grep -qx "CONFIG_${sym}=y" "$dir/.config" || { echo "error: CONFIG_$sym missing" >&2; exit 1; }
	done
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
	local dir=$1 out=$2 nodtb="$dir/u-boot-nodtb.bin" dtb="$dir/$DTB_REL" bin="$dir/u-boot+dtb.bin"
	[ -f "$nodtb" ] || { echo "error: $nodtb missing" >&2; exit 1; }
	[ -f "$dtb" ] || { echo "error: $dtb missing" >&2; exit 1; }
	cat "$nodtb" "$dtb" > "$bin"
	python3 - "$bin" "$nodtb" "$ROOT" <<'PY'
import struct, sys
binpath, nodtb, root = sys.argv[1], sys.argv[2], sys.argv[3]
off = len(open(nodtb, "rb").read())
blob = open(binpath, "rb").read()
if blob[off:off + 4] != b"\xd0\x0d\xfe\xed": sys.exit(f"error: no FDT at _end ({off:#x}) in {binpath}")
tot = struct.unpack_from(">I", blob, off + 4)[0]
if tot < 0x100 or off + tot > len(blob): sys.exit(f"error: DTB at {off:#x} claims {tot} bytes")
print(f"    dtb at _end: {off:#x}, {tot} bytes, {len(blob)} total")
sys.path.insert(0, root + "/scripts")
import importlib.util
spec = importlib.util.spec_from_file_location("p", root + "/scripts/patch-stock-dtb.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
dtb = blob[off:off + tot]
rt = m.find_root(m.parse(dtb)[0])
mem = rt.child("memory")
if mem is None: sys.exit("error: no literal /memory node")
reg = mem.props.get("reg", b"")
if len(reg) != 16: sys.exit(f"error: /memory reg is {len(reg)} bytes")
base, size = struct.unpack(">QQ", reg)
if not size: sys.exit("error: /memory has zero size")
fb = [c for c in rt.children if c.name.startswith("framebuffer@")]
model = rt.props.get("model", b"").split(b"\0")[0].decode()
print(f"    DT ok: /memory {base:#x}+{size:#x}, {len(fb)} fb node(s), model {model!r}")
PY
	local dtc=""
	for c in "$dir/scripts/dtc/dtc" "$(command -v dtc 2>/dev/null || true)"; do [ -n "$c" ] && [ -x "$c" ] && dtc=$c && break; done
	if [ -n "$dtc" ]; then
		python3 -c 'import sys;d=open(sys.argv[1],"rb").read();o=len(open(sys.argv[2],"rb").read());open(sys.argv[3],"wb").write(d[o:])' "$bin" "$nodtb" "$dir/u-boot.dtb.extracted"
		"$dtc" -I dtb -O dts -o "$dir/u-boot.dtb.extracted.dts" "$dir/u-boot.dtb.extracted"
		grep -qE "^[[:space:]]*memory \{" "$dir/u-boot.dtb.extracted.dts" || { echo "error: dtc output has no /memory node" >&2; exit 1; }
		echo "    dtc parsed internal DT"
	fi
	fix_image_header "$bin"
	gzip -9 -n -c "$bin" > "$out"
}

pack() { python3 "$ROOT/scripts/pack-boot.py" "$1" --dtb-table "$OUT_ABS/stock-fb.bin" -o "$2"; }

cd "$UB"
echo "== defconfig"
make -s O="$OUT_ABS" qcom_defconfig qcom-phone.config
require_config "$OUT_ABS" EFI_PARTITION CMD_PART CMD_FAT FS_FAT CMD_FS_GENERIC FASTBOOT_FLASH_BLOCK CMD_FASTBOOT USB_DWC3_GADGET FASTBOOT_OEM_RUN FASTBOOT_CMD_OEM_CONSOLE CONSOLE_RECORD

echo "== build"
build "$OUT_ABS"

python3 "$ROOT/scripts/patch-stock-dtb.py" "$ROOT/firmware/stock-appended-dtbs.bin" "$OUT_ABS/stock-fb.bin"

echo "== pack"
make_payload "$OUT_ABS" "$OUT_ABS/payload.gz"
pack "$OUT_ABS/payload.gz" "$OUT_ABS/boot.img"

ls -l "$OUT_ABS/boot.img" "$OUT_ABS/stock-fb.bin" "$OUT_ABS/u-boot+dtb.bin"

build_env_variant() {
	local name=$1 envf=$2 img=$3 dir
	dir="$(dirname "$OUT_ABS")/out-$name"
	echo "== build $name ($dir)"
	make -s O="$dir" qcom_defconfig qcom-phone.config
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
