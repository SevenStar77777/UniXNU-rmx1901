# UniXNU-rmx1901

U-Boot/UEFI bring-up for **realme X (RMX1901, SDM710)** toward a UniXNU (iOS-on-Android) port.

Forked from [AppleCampus/UniXNU-rmx1971](https://github.com/AppleCampus/UniXNU-rmx1971) and adapted for RMX1901.

## Changes from rmx1971

| Item | rmx1971 | **rmx1901** |
|---|---|---|
| SoC | SDM712 (Kryo 360 @ 2.3 GHz) | **SDM710 (Kryo 360 @ 2.2 GHz)** |
| UFS first_lba | 0x198900 | **0x163908** |
| UFS blocks | 0xD11EF3 | **0xC8A6F3** |
| Panel | Realme 5 Pro | **Samsung AMS653TK01 1080x2340** |
| RAM | 4 GiB | **6 GiB** |

## Milestones

- **M1**: U-Boot + UEFI come up, get EFI prompt (GitHub Actions builds boot.img)
- **M2**: UniXNU.efi loaded
- **M3**: XNU shows boot-progress panel (requires macOS + Xcode for kernel build)

See [porting.toml](porting.toml) for the full porting record.
