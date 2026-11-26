# RMX1901 移植记录（BRINGUP）

本文件记录 Realme X（RMX1901，SM710/SDM710）当前的移植状态、实测参数与下一步。
对照官方文档：<https://unixnu.com/zh/porting/steps>

最后更新：2026-11-26

---

## 1. 结论摘要

| 阶段 | 状态 | 证据 |
|---|---|---|
| Android bootloader 接受自定义镜像 | 通过 | ABL 不再回落 fastboot |
| 固件层（本仓库用 U-Boot 替代官方 UEFI） | 通过 | `U-Boot 2025.07`、`Model: Realme X (rmx1901)`、`DRAM: 2 GiB` |
| UniXNU 内核交接 | 通过 | 屏幕出现 XNU 诊断页，`[rmx1901-a1] ... live XNU -- native 4K` |
| `kernel_task` 启动 | 通过 | `kernel_task_boot_init`、`IODeviceTree: 8192 bytes` |
| 根卷挂载（layer 6 用户态） | **未通过** | 反复 `vfs_mountroot enter`，无 rootdev |
| 驱动（存储/显示/输入/USB） | **未实现** | 诊断页 `USB/EMAC/LCD/SMC step=0` |

**当前停在：内核已运行，缺存储驱动与根卷。**

---

## 2. 硬件输入

| 项 | 实测值 |
|---|---|
| SoC | SDM710（fastboot `getvar product` = `sdm710`） |
| UFS 控制器 | `1d84000.ufshc`，逻辑块 4096 |
| DRAM | 起始 `0x80000000`，物理 6 GB（本配置仅把 `0x20000000` 交给 EFI） |
| 面板 | Samsung AMS653TK01，1080×2340，CMD 模式 |
| 触控 | Synaptics S3706 @ i2c `0x20` |
| 指纹 | Goodix 光学屏下 |
| framebuffer | `0x9C000000`（cont_splash_region） |
| 串口 | ttyMSM0 @ `0xA90000`（`printk.disable_uart=1`，屏幕上无串口输出） |

### 分区（4K 块）

| 分区 | 起始 LBA | 大小 | 块设备 |
|---|---|---|---|
| `boot` | `0xC506` | 64 MiB | `sde10` |
| `vm-system`（ESP） | `0x2D08` | 128 MiB | `sda8` |
| `logfs` | `0x9F391` | 8 MiB | `sde31` |
| `userdata` | `0x163908` | `0xC8A6F3` | `sda13` |

---

## 3. 与官方架构的差异

官方六层中，本移植目前的实现是：

| 层 | 官方实现 | 本仓库现状 |
|---|---|---|
| 1 启动镜像 | `./unixnu bootimg` 生成（BootShim + FV + DT + payload） | 使用 `pack-boot.py` 风格自建镜像 |
| 2 固件 | `uefi-<device>`（Mu-Silicium）`SILICIUM_UEFI.fd` | **用 U-Boot 2025.07 替代**（无 rmx1901 的 Mu-Silicium 固件） |
| 3 启动管理器 | `UniXNU.efi` | 已随 payload 提供 |
| 4 XNU 加载器 | `DarwinBoot.efi` | 已随 payload 提供 |
| 5 内核 | `kernel.macho` + ADT | 已产出并运行 |
| 6 用户态 | 目标 IPSW 的 `OS`/`SystemOS` | **缺失** |

差异 2 是本项目最大的技术债：U-Boot 只能提供基本块设备与 EFI 服务，无法提供官方 UEFI 固件层的完整图形/输入/块设备协议。

---

## 4. 关键参数（踩过的坑）

1. **arm64 Image 头 `text_offset` 必须为 0**：非 0 时 ABL 直接回落 fastboot。
2. **`os_version` 必须是 `0x1400014A`**（ColorOS C.14 / Android 10 的值），与 stock boot 对齐。
3. **vendor DTB 表必须换成 rmx1901 自己的 15 个 DTB**（含 SDM710 / `0x172`）。沿用 rmx1971 的 6 个 DTB 会被 ABL 拒绝。
   - 处理脚本：`scripts/repack-uboot.py`（保留 gzip 载荷，替换 DTB 表与 `os_version`）。
4. **DTS 必须带 `/dts-v1/;`**，且 `dtc` 只认 `/include/ "x.dtsi"`，不处理 C 预处理器的 `#include`。
5. **内存窗口从 512 MiB 起步**（`owned_memory_size = 0x20000000`），给满 1.97 GiB 会 `EFI_OUT_OF_RESOURCES`。
6. **连续启动失败会触发 ABL 保护**（`misc` 分区 BCB），现象是无论刷什么都进 fastboot；`fastboot erase misc` 可清除。
7. **U-Boot fastboot gadget 写大镜像要分块**：`fastboot -S 4194304 flash <part> <img>`；不分块会 `AdbWriteEndpointSync failed (121)`，gadget 卡死后需断电重启。

---

## 5. 构建与刷写

```sh
# 内核（GitHub Actions，macOS runner）
workflow: Build kernel
  device_repository = SevenStar77777/UniXNU-rmx1901
  defconfig         = rmx1901_defconfig
  steps             = kernel
  xcode             = 26.6
# 产出 kernel.macho（52 MB）与 kernel.json

# 载荷卷（Ubuntu runner）
workflow: UniXNU payload volume
  unixnu_ref    = main
  kernel_run_id = <上一步的 run id>
# 产出 rmx1971-payload.img（128 MiB，FAT16）

# 启动镜像变体
workflow: Build U-Boot
# 产出 out/boot-efi.img（环境：bootcmd=bootefi bootmgr; fastboot usb 0）

# 本地重打包（换成 rmx1901 的 DTB 表）
python scripts/repack-uboot.py out/boot-efi.img
# → boot-efi-rmx1901.img

# 刷写（U-Boot fastboot gadget）
fastboot -S 4194304 flash vm-system rmx1971-payload.img
fastboot -S 4194304 flash boot boot-efi-rmx1901.img
fastboot reboot
```

---

## 6. 当前观测（2026-11-26）

屏幕输出要点：

```
[rmx1901-a1] UniXNU boot diagnostics -- live XNU -- native 4K -- no automatic reboot
time=27s idle=1 IRQ=942 ... processor=0 freq=1000000
CPU 0 MPIDR=0x81000000
kernel_task_boot_init: calling setcon?
kernel_task_boot_init: vfs_mountroot enter
IODeviceTree: 8192 bytes
lockdown_mode: ... in nvram is off
USB step=0 / EMAC step=0 / LCD step=0 / SMC step=0
UFS step=10 gpf=0/0 vfs=0x1
```

解读：内核、设备树、中断、定时器可用；**存储与显示驱动未实现，`vfs_mountroot` 无根设备**。

---

## 7. 下一步（按官方驱动顺序）

官方顺序：平台与内存 → 存储 → 显示与亮度 → 输入 → GPU → Wi-Fi/蓝牙 → USB。

| 优先级 | 工作 | 完成标准 |
|---|---|---|
| P0 | **存储**：XNU 侧 UFS 驱动（`1d84000.ufshc`） | 内核日志能枚举 `userdata`/`vm-system` 分区 |
| P1 | **根卷**：payload 增加 `rootdev` 与最小 `OS` 卷 | `vfs_mountroot` 不再循环，能进入 launchd |
| P2 | **显示**：DSI + DPU 初始化，复用 `cont_splash_region` | 屏幕出现内核帧缓冲内容而非仅 U-Boot 残留 |
| P3 | **输入**：Synaptics S3706 I2C 触控 | 触摸事件可读 |
| P4 | **USB**：dwc3 gadget | 可从 PC 侧 adb/ums 访问 |

---

## 8. 复现与回滚

- 回官方系统：`fastboot flash boot boot_stock.img`、`fastboot erase misc`、`fastboot reboot`
- 回到 U-Boot：刷 `boot-efi-rmx1901.img`
- 已验证可用的 U-Boot 变体：`boot.img` / `boot-probe.img` / `boot-diag.img` / `boot-usb.img` / `boot-efi.img`

---

## 9. 参考

- <https://unixnu.com/zh/porting/>
- <https://unixnu.com/zh/porting/steps>
- <https://unixnu.com/zh/concepts/boot-chain>
- <https://unixnu.com/zh/porting/drivers>
