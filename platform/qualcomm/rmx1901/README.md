# realme X (RMX1901) — HakiOS U-Boot profile

Status: **bring-up, not verified on hardware**. SoC: Qualcomm SDM710 (sm7100 / `qcom,sdm670-mtp`),
UFS, PM660 + PM660A, panel `dsi_oppo18041samsung_ams653tk01_1080_2340_cmd`.

## Why this path

The stock realme/OPPO ABL does not implement `fastboot boot` (verified: `Sending ... OKAY`,
`Booting ... FAILED (remote: 'unknown command')`, no `oem boot*` variants, no fastbootd).
HakiOS therefore cannot use the stock UEFI + `fastboot boot` handoff on this phone. This profile
builds U-Boot as the boot shim instead: the resulting Android `boot.img` is flashed to `boot`,
the stock ABL starts U-Boot from RAM, and U-Boot's `hakios` command starts `loader.efi` with the
XNUP payload at `0xF0000000`. A reboot returns to Android after restoring the stock boot image.

## Addresses (from the live FDT, not copied from another phone)

`hackios-device/fdt/phone.dts` on the build machine:

| Region | Address | Size |
| --- | --- | --- |
| hyp | 0x85700000 | 0x600000 |
| xbl | 0x85E00000 | 0x100000 |
| cont_splash (framebuffer) | 0x9C000000 | 0x2300000 |
| qseecom | 0x9E400000 | 0x1400000 |
| ramoops | 0xB7E00000 | 0x400000 |
| kboot log | 0xB8200000 | 0x100000 |
| HakiOS payload (U-Boot `PAYLOAD_BASE`) | 0xF0000000 | 63 MiB |

Device-tree supplied to `pack.py`: `local-firmware/rmx1901/uefi/device-tree.dtb`, copied from the
phone's `/sys/firmware/fdt` (overlays applied, including DTBO index 46 for the panel).

## Open items before hardware validation

1. `CONFIG_DEFAULT_DEVICE_TREE` still points at U-Boot's `qcom/sdm845-db845c` for the build;
   the phone's own DTB is appended to `boot.img` by `pack.py`. Replace it with a real
   `qcom/rmx1901.dts` once the board's clock/reset bindings are written.
2. SDM710 has no dedicated clock driver in U-Boot; `CLK_QCOM_SDM845` / `CLK_QCOM_SM7150` are
   compiled in as the closest known families. Bring-up must confirm which one the hardware accepts.
3. `0xF0000000` must be RAM and unused on this phone (6/8 GB variants). Verify with `bdinfo` /
   `meminfo` in the U-Boot shell before trusting the payload reservation.
4. USB gadget/DWC3 device mode and serial output must be confirmed on hardware.
