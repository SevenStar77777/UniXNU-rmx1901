# ShengUfs × SDM710 硬件核对表

目的：把官方存储组件 `ShengUfs`（按 SM8550 编写）需要的硬件常量，逐项对照本机 SDM710 的实际值，
确定接入前必须改动的点。来源固定如下：

- ShengUfs：`UniXNU/ShengUfs@fd420df`（`NativeUfs.cpp`、`SmmuTakeover.h`、`GccUfs.h`、`RootLayout.h`）
- 同代参考：Linux `arch/arm64/boot/dts/qcom/sdm845.dtsi`（SDM845 与 SDM710 同属 UFS 2.1 代，地址布局高度一致）
- 本机证据：ColorOS C.14 的设备树中 `1d84000.ufshc`、逻辑块 4096；fastboot `getvar product` = `sdm710`

## 1. 对照表

| 输入 | ShengUfs（SM8550） | SDM845 参考 | SDM710 本机 | 结论 |
|---|---|---|---|---|
| UFS 控制器 MMIO | `0x1d84000`，len `0x3000` | `ufshc@1d84000`，std `0x2500` + ice `0x1d90000/0x8000` | `1d84000.ufshc`（已确认） | 基址一致，**长度需按 0x2500 收窄** |
| PHY | `0x1d80000`，len `0x2000` | `phy@1d87000`，len `0x1000` | 待实测 | **必须改**，SDM710 极可能为 `0x1d87000` |
| 控制器中断 | 由 ShengUfs 内部路由 | `GIC_SPI 265` | 待实测 | 需从设备树确认 |
| GCC | `0x100000` | `gcc@100000` | 待实测 | 基址一致，**分支/门控位不同** |
| apps-SMMU | `0x15000000`，SMMUv2 | `apps_smmu@15000000`，len `0x80000` | 待实测 | 基址一致 |
| UFS stream ID | `0x60` | `iommus = <&apps_smmu 0x100 0xf>` → **`0x100`** | 待实测 | **必须改**，`0x60` 在本机是别的外设 |
| stream 掩码 | 单值匹配 | `0xf` | 待实测 | 需带掩码写入 S2CR |
| UFSHCI 版本 | 3.x（SM8550） | 2.1 | **待读 `VER`@`0x08`** | 决定 UTRD/UTRL 布局与 HCE 语义 |
| 逻辑块大小 | 4 KiB | 4 KiB | `4096`（已确认） | 一致 |
| 零拷贝 DMA 上限 | 由 bypass 位宽决定（窄时 32 位） | 同 | 待实测 SMMU 输出位宽 | 窄时只能访问低 4 GiB |
| 请求队列 | 32 槽，UTRD 32 字节 | 同 | 同 | 一致 |

## 2. 必须先测的三个值

SDM710 的这三项无法从公开资料定死，必须从设备上读，否则接上 `ShengUfs` 后无法判断卡在哪：

| 值 | 读法 | 判据 |
|---|---|---|
| `CAP`（`0x00`）、`VER`（`0x08`） | U-Boot 命令行 `md.l 0x1d84000 0x4` | `VER` 为 `0x00000210` 说明是 2.1；`0x00000300` 说明是 3.0 |
| UFS stream ID | 读 `apps_smmu` 的 S2CR/SMR 表，或从原厂 DTS 的 `iommus` 取 | 与 `sdm845` 的 `0x100` 对比 |
| PHY 基址 | 读 `0x1d87000` 前 4 字节，非 `0xffffffff`/`0` 即为存在 | 与 `0x1d80000` 对比 |

## 3. 接入前必须改的代码点

| 文件 | 位置 | 改动 |
|---|---|---|
| `NativeUfs.cpp` | `smmuBypassStream (0x60, ...)` | 改为本机 stream ID |
| `NativeUfs.cpp` | host / phy / gcc 常量 | 按上表替换 |
| `SmmuTakeover.h` | context bank 与输出位宽 | 按本机 SMMU 实测值 |
| `GccUfs.h` | 分支使能与硬件门控位 | 按 SDM710 GCC 位定义 |
| `board.toml` | `[components] include`、`[kernel] archives` | 加 `ShengUfs` / `component:ShengUfs` |
| `configs/rmx1901_defconfig` | — | 开 `CONFIG_XNU_NATIVE_UFS=y` |
| `board.toml` | `[payload.adt]` | 加 `root_match_volume_uuid` 与 `native-ufs-root=1` |

## 4. 风险

1. **UFSHCI 2.1 与 3.x 的门铃/完成语义不同**。ShengUfs 的完成路径按 3.x 写（UTRD 中断位 + doorbell 清位），在 2.1 上仍需同样机制，但寄存器偏移要逐项核对 `CAP.NUTRS`、`UTRLBA`。
2. **SMMU 接管是全局副作用**。`SmmuTakeover` 会重写 apps-SMMU 的 context bank；SDM710 上显示（DPU）与 WiFi 也需要 bank，接错会让其它设备一起失效。
3. **失败无串口**。本机 `printk.disable_uart=1`，只能靠屏幕上的最后一行判断阶段，建议先只做“寄存器读取 + 容量查询”，确认能读到 `CAP/VER` 与 LUN 0 容量后再开根卷。

## 5. 顺序

1. 用 U-Boot 读 `CAP/VER`、PHY、SMMU 三项 → 填满本表“本机”列
2. 改 `NativeUfs.cpp` / `SmmuTakeover.h` / `GccUfs.h` 常量
3. `board.toml` + defconfig 接入，只验证“控制器可读 + 容量正确”
4. 再开根卷（`verified-root`）与持久 Data
## 6. 实测结果（2026-11-26，U-Boot `md.l`）

| 地址 | 值 | 判读 |
|---|---|---|
| `0x1d84000 +0x00` | `0x1587031F` | CAP：NUTRS=31（32 槽），UTRLCLS=0（UTRD 32 字节） |
| `0x1d84000 +0x04` | `0x00000210` | **UFSHCI 2.1**（非 3.x） |
| `0x1d84000 +0x14` | `0x00000017` | IS 寄存器可读 |
| `0x1d87000` 起 8 字 | 全 0 | PHY 候选 A 被门控 |
| `0x1d80000` 起 8 字 | 全 0 | PHY 候选 B 被门控 |
| `0x15000000` | `0x00204025` | apps-SMMU 全局可读 |
| `0x100000` | `0x001E0005` | GCC 可读，UFS 分支未全关 |

结论：

1. 版本为 2.1，`ShengUfs` 的 32 槽 / 32 字节 UTRD 假设成立；需核对门铃与完成寄存器在 2.1 下的偏移。
2. PHY 基址不能靠读数判断，须取自设备 DTB。
3. U-Boot 自带 `qcom_ufshcd` 在 `ufshcd@1d84000` probe 失败（-19），原因是 DTS 缺 PHY/clock 子节点；补齐后 U-Boot 可直接访问 UFS，用作 XNU 驱动前的验证通道。

## 7. 待办

- [ ] 从设备 DTB 取 `ufs_mem_phy` 基址与 `iommus` stream ID
- [ ] 给 U-Boot 板级 DTS 补 PHY/clock 节点，验证 UFS 块读取
- [ ] 依 UFSHCI 2.1 核对 `NativeUfs.cpp` 门铃/完成偏移
## 8. 设备 DTB 实测（2026-11-26，`fdtpeek.py` 解析 ColorOS C.14 的 fdt.dtb）

| 节点 | 属性 | 值 | 说明 |
|---|---|---|---|
| `/soc/ufshc@1d84000` | `reg` | `0x1d84000` len `0x3000` | 与 ShengUfs 常量一致 |
| | `interrupts` | `<0 0x109 0>` | GIC_SPI 265 |
| | `phys` | phandle → `ufsphy_mem@1d87000` | PHY 基址确定 |
| | `iommus` | **不存在** | UFS 不走 apps-SMMU |
| | `resets` | GCC phandle, id `0x0e` | |
| | `clocks` | GCC `0xb1 0xb7 0x89 0xb3 0xb5 0x92 0x90` + XO | |
| | `lanes-per-direction` | `1` | SM8550 参考为 2 |
| `/soc/ufsphy_mem@1d87000` | `reg` | `0x1d87000` len `0xe00` | |
| | `compatible` | `qcom,ufs-phy-qmp-v3` | QMP v3 |
| | `lanes-per-direction` | `1` | |

结论（相对第 1 节的修正）：

1. **PHY**：`0x1d87000` / `0xe00` / 1 lane，替换 ShengUfs 的 `0x1d80000` / `0x2000` / 2 lane。
2. **SMMU**：本机 UFS 没有 `iommus`，**跳过 `smmuBypassStream(0x60, ...)` 与 context bank 接管**；DMA 直接使用物理地址，注意零拷贝路径的 32 位上限判断要保留。
3. **时钟/复位**：按上表 GCC ID 与 reset `0x0e` 替换 `GccUfs.h` 的位定义。
4. **中断**：GIC_SPI 265，与 SDM845 相同。

适配面因此从"重写驱动"缩小为"三个常量 + 一条分支"，风险显著可控。

## 9. U-Boot 时钟回归（2026-11-27）

设备上的 U-Boot 串口输出：

```
qcom-ufshcd ufshcd@1d84000: no reset control, ret:-2 (continuing)
qcom-ufshcd ufshcd@1d84000: Warning: cannot get reset GPIO
qcom-ufshcd ufshcd@1d84000: failed to initialize clocks, err:-2
qcom-ufshcd ufshcd@1d84000: Host controller init failed, err:-2
qcom-ufshcd ufshcd@1d84000: ufshcd_probe() failed, ret:-2
```

`-2` 是 `-ENOENT`。根因不是硬件，而是板级 DTS：commit `d163341` 为了“first probe”删掉了
`ufs_mem_hc` 和 `ufs_mem_phy` 的 `clocks` / `clock-names`。但 `ufs_qcom_init()` 的
`clk_get_bulk()` 需要继承来的 9 个时钟，PHY 驱动 `qmp_ufs_clk_init()` 需要 `ref` /
`ref_aux`，两者都会因属性缺失直接失败。

处理：恢复这两个节点的 `clocks` / `clock-names`；`resets` /
`interconnects` / `power-domains` 继续删除（optional-reset 补丁会容忍前者缺失，后两者被驱动忽略）。
`0001-clk-sdm670-add-ufs-phy-rx-symbol-1.patch` 补上 `GCC_UFS_PHY_RX_SYMBOL_1_CLK`，
否则恢复时钟后会回到 `unknown clock ID 137`。

下一项测量：重建 `boot-ufs.img`，确认 probe 通过、再用 `md.l` 读 CAP/VER 并做 LUN0 块读取。
`cannot get reset GPIO` 当前只是 warning；后续若链路训练失败，再回头核对 GPIO 150 的 TLMM
绑定，以及 DTS 继承的 `lanes-per-direction = <2>` 与设备 DTB 实测的 1 lane 是否一致。