# RC01 UB02：升级结果、命令状态与单 rootfs 目标

2026-11-12。基线为已获 Astra 接受的 UB01：`3e77a17bf0878383e68f8a3f509461d428602771`。
本次只实现 UB02，作为一个完整交付单元：`flash`/`ubi create` 命令状态规范化、网页外层失败判断、
raw UBI 目标选择及 SYSUPGRADE 结果检查一起提交。未进入 UB03（ENV 保存/网页反馈），
未改 hush、`cmd_call`、UB01 的 NAND 实现与已接受测试，也未改外层 docs/sources。
已获 Astra 独立验收 ACCEPT；主线程完成 BE10000 ARM32 完整 ELF 编译链接。未操作设备。

## 修改文件

- `common/cmd_flashwrite.c`：`do_flash` 出口规范化。
- `common/cmd_ubi.c`：`do_ubi create` 结果规范化；新增 `ubi_get_volume_id`。
- `include/ubi_uboot.h`：声明 `ubi_get_volume_id`。
- `failsafe/failsafe_httpd.c`：网页外层升级判断。
- `net/httpd.c`：新增 `raw_ubi_nand_upgrade`，纯 NAND/QSPI NAND 的 raw UBI 改走该函数。
- `lib/sysupgrade_parser.c`：RC01 tar 备份拒绝、attach/remove/create/写入结果检查、RC01 卷 ID 校验。
- 新增 `test/rc01/test_upgrade_results.py`、`test/rc01/upgrade_results_fixture.c`。
- 新增本文件。

## 实现

### 1. 命令边界

- `do_flash`：在 `exit:` 中保留原有逻辑，即任意非零都复位 `flash_type_new`；随后，
  除 `CMD_RET_SUCCESS`、`CMD_RET_USAGE` 外的所有值都返回 `CMD_RET_FAILURE`。
  `getpart_offset_size` 的 `-ENOENT/-EINVAL` 以及 UBI 后备找不到卷时剩下的 `-ENOENT`，
  都不再落入 hush 中小于 -1 的“exit”分支而被当作成功。参数错误仍然返回 `CMD_RET_USAGE`。
  限制：非 LWIP 的 eMMC 构建中，`get_partition_info_efi_by_name` 的 -1 与 USAGE 数值相同，
  所以仍会显示用法。这是原有行为。hush 对它按失败处理，不触发 exit。
  `flasherase` 共用此出口。
- `do_ubi create`：`ubi_create_vol` 的任何非零返回都打印 `UBI create <vol> failed: <err>`，
  然后返回 `CMD_RET_FAILURE`；成功返回 0。`verify_mkvol_req` 原有的诊断保留。
  `ubi_create_volume` 返回的 `-ENOSPC/-EROFS/-ENOMEM/-EIO` 不再被 hush 转换成 0。
  显式用法错误（`ubi` 无参数时为 USAGE，“Incorrect type”/“Please see usage” 为 1）不变。
  remove、write 等其他子命令未改，原本就返回正数。
- 新增 `ubi_get_volume_id(name)`：只读查询当前已 attach 设备的卷表，未选设备或不存在时
  返回 `-ENODEV`。它不会 attach，也不打印信息。

### 2. 网页升级

- `failsafe_httpd_poll`：`do_http_upgrade(...) != 0` 即视为失败（原来是 `< 0`）。
  handler 返回 +1（执行命令失败）时，显示失败页面，不调用 `HttpdDone`/`do_reset`。
  MIBIB、initramfs 原有的 -1 处理不变。
- `do_firmware_upgrade` 的 NAND case 中，`fw_type == FW_TYPE_UBI` 且
  `flash_type != SMEM_BOOT_NORPLUSNAND`（即纯 NAND / QSPI NAND）时，改为返回
  `raw_ubi_nand_upgrade(size)`。
- `raw_ubi_nand_upgrade`：
  1. 目标列表：必选主 `rootfs`；勾选备份时加入物理分区 `rootfs_1`（必须存在）。
     **RC01**（`CONFIG_IPQ9574_XIAOMI_BE10000`）只用这两个目标，不尝试 `rootfs1/rootfs2`。
     **其他纯 NAND 板**：SMEM 中 `rootfs1/rootfs2` 不是 `-ENOENT` 时也加入，
     作为旧的多副本目标，按实际存在的目标选择。
  2. 执行与 `flash` 相同的 `nand device N`（IPQ40XX 使用 `is_spi_nand_available()`），
     保留板级选择 hook，然后取 `nand_info[nand_curr_device]` 的实际几何，
     按页大小做有溢出检查的取整。
  3. 在第一次擦除前，对**所有**目标用 `getpart_offset_size` 精确查询 SMEM 物理分区，
     并调用 UB01 的 `nand_check_range_capacity`。任一目标缺失、分区无效、容量不足、
     坏块查询失败或镜像大于分区，都打印 `nothing written` 并返回 -1。
     整个预检不调用 `ubi_vol_present`，不 attach UBI，不做 flash 探测。
  4. 依次执行 `flash <target> <addr> <size>`。`flash` 内部仍按 UB01 再做一次有界检查。
     任一目标失败都返回 -1；若不是第一个目标，会额外打印“已有 i/n 个目标写入，
     可能部分完成，无回滚”。
  5. `CONFIG_IPQ806X` 下的 `flash` 会按目标切换 layout，因此该配置的批量预检
     只查存在性；容量由各目标的 `flash`/UB01 在擦除前检查。如果后一个目标容量不足，
     前一个目标已经写入，此时按部分完成处理。

### 3. SYSUPGRADE（`sysupgrade_write_ubi_volumes`）

- RC01：勾选备份时，在 `sysupgrade_ubi_init`（其中包含 attach）以及任何
  `ubi part/remove/create/write` 之前返回 -1，并打印 `not supported ... nothing written`。
- `ubi part fs` 的结果必须检查，失败即停止。
- `rootfs_data` 及各待重建卷先用 `ubi_get_volume_id` 在已 attach 的设备上查询，
  不会额外 attach。卷存在时 `ubi remove` 必须成功，否则停止；卷不存在时直接 create。
- `kernel`、`rootfs`：`ubi create && ubi write` 的任一失败都会停止。由于 create 已规范化，
  失败后不会执行后续 write。
- 其他板型勾选备份时，`kernel_1/rootfs_1` 成为必需操作，失败返回 -1，不再“skip 后成功”。
- 最终 `ubi create rootfs_data` 的结果必须检查。
- RC01：最后校验 `kernel/rootfs/rootfs_data` 的卷 ID 是否为 0/1/2，不符合则返回 -1。
  这只是结果真实性检查，镜像此时已经写入，不会回滚。
- tar/HSQS 解析和 `sysupgrade_parts_valid` 的 OR 规则不变。不做原子回滚；
  失败后不保证旧镜像完好。

## 测试

```sh
cd uboot
UV_CACHE_DIR=/tmp/uv-cache uv run --offline --no-project python -B test/rc01/test_upgrade_results.py
UV_CACHE_DIR=/tmp/uv-cache uv run --offline --no-project python -B test/rc01/test_nand_write_safety.py
git diff --check
```

### UB02 测试的组成

`test_upgrade_results.py` 复用 UB01 测试的按符号提取工具，从当前树中提取真实函数：

- hush 控制链：`run_command → parse_stream_outer → run_list → run_list_real`；
- `cmd_process → cmd_call`；
- `do_flash/write_to_flash/fl_erase/ubi_vol_present/write_ubi_vol`、`getpart_offset_size`；
- `do_ubi`、`ubi_create_vol/verify_mkvol_req/ubi_remove_vol/ubi_volume_write/ubi_check/ubi_get_volume_id`；
- `sysupgrade_ubi_init/ubi_write_volume/rc01_check_volume_ids/sysupgrade_write_ubi_volumes`；
- `net/httpd.c` 中的全部升级 handler、`do_http_upgrade`、`raw_ubi_nand_upgrade`；
- `failsafe_httpd_poll`；
- UB01 的 `nand_check_range_capacity`、`nand_block_isbad`。

fixture 只模拟以下部分：

- NAND 坏块查询、擦除和写入的计数与注入；
- UBI 卷表，`ubi_create_volume` 仿照 vmt.c 的最小空闲 ID、重名、PEB 不足和只读返回，
  并可按卷名注入 errno；
- `ubi part`/`ubi_set_rootfs_part`；
- `nand device`、mmc/sf/bootm 命令；
- 命令串分词器：只拆分 `&&`、`;` 和 `$var`。分词结果交给真实 `parse_stream_outer`，
  `run_pipe_real` 按真实 U-Boot 分支调用真实 `cmd_process`。

因此，`do_flash`/`do_ubi` 的原始返回值（含负 errno）会经过真实 hush 的 exit 处理，
而不是由桩注入已经规范化的 +1。本次实际启用并核实二进制链接 `libasan`/`libubsan`；
共用 build helper 在 sanitizer 编译失败时允许回退，不能将此描述为强制启用。

### 实际结果（本次运行）

- 当前树 RC01 配置：**24/24**；其他纯 NAND + QCA_MMC 配置：**28/28**。
- 单项回退变异（在当前树上只撤销一个修复），预期失败均被检出：
  - `raw_chain`：保留 errno 规范化，但恢复无条件 `rootfs && rootfs1 && rootfs2` 链。
    RC01 和其他板的单 rootfs 正常升级都被报告为失败。这证明 errno 规范化不能和旧链
    单独交付。
  - `flash_errno`、`create_errno`、`web_dispatch`、`tar_backup`：各自对应的场景均失败。
- 基线 `3e77a17`（UB01）：RC01 **12/12**、other **8/8** 个列出的原缺陷均被检出。
  检出内容：
  - `-ENOENT/-EINVAL` 及四类 create errno 经 hush 变成成功；
  - 网页 +1 时仍然 reset；
  - RC01 raw 写入 `rootfs1`，并在 `rootfs2` 缺失时 attach UBI 后报告成功；
  - 请求缺失备份时写主区后仍报告成功；
  - RC01 tar 备份执行了 UBI 操作，建出 `kernel_1`；
  - attach/remove/data create 失败被忽略；
  - 卷 ID 错位不报错；
  - NOR 缺分区和 NOR 写失败时网页 reset。

  另有若干场景在基线上也失败（“also fails”），属于新行为，不计入检出统计。
- UB01 回归：`test_nand_write_safety.py` 默认 **29/29**、IPQ806X **30/30**，
  基线 **6/6** 缺陷检出。
- `git diff --check` 通过。

### 主要场景

- **命令边界**：
  - 缺分区和无效分区经 hush 返回 1，后续命令不执行，`flash_type_new` 复位；
  - 用法错误仍为 USAGE；
  - 擦除失败时不写；
  - `ubi create` 注入四类 errno，以及模拟出的真实 `-ENOSPC`（PEB 不足）和 `-EROFS`（只读），
    结果都是失败，后续 `ubi write` 不执行；
  - 正常 create 时 ID 按最小空闲分配。
- **网页**：
  - handler 返回 +1 时进入失败状态，不 reset；
  - 成功时 reset 一次；关闭自动重启时不 reset；
  - MIBIB 写失败、initramfs bootm 失败时不 reset。
- **RC01 raw UBI**：
  - 单 rootfs 只执行一次擦写，即使 SMEM 中存在 `rootfs1` 也不写，UBI 操作数为 0；
    `nand device` 在第一次坏块查询之前执行；
  - 请求备份但 `rootfs_1` 缺失时零擦写，也没有任何 `flash` 命令；
  - `rootfs_1` 存在时两个目标都写；
  - 下列情况在主区擦除前即被拒绝：主区良块不足，`rootfs_1` 过小，
    `rootfs_1` 末块坏块查询返回 `-EIO`，镜像大于分区，设备选择失败；
    良块数恰好够用时成功；
  - 第二目标写失败时报告失败；第一目标擦除失败时不写第二目标。
- **SYSUPGRADE**：
  - RC01 备份请求零 UBI 操作，三卷保持 0/1/2；
  - kernel、rootfs、rootfs_data 三处各注入四类 create errno，结果全部失败，
    且未创建的卷没有被写入；
  - attach 失败（保留旧的选择状态）时零 remove/create；
  - kernel、rootfs_data remove 失败时停止；
  - 卷不存在时直接 create，不执行 remove，最终 ID 为 0/1/2；
  - 写失败时不创建 data；
  - data create 失败时网页报告失败；
  - 正常情况 ID 为 0/1/2 且 reset；
  - RC01 卷 ID 错位（kernel 原在 ID 3，重建后取得 2）时报告失败；
  - 其他板型勾选备份时写入 4 卷，备份 create/write 失败时整体失败。
- **其他存储**：
  - 其他纯 NAND 仅有 rootfs 时成功；`rootfs1/rootfs2` 存在时按顺序写 3 个目标；
    第 3 个失败时报告失败；缺少 `rootfs_1` 备份时零擦写；
  - NOR+NAND 前缀后备仍执行原命令链（`flash rootfs … $filesize`、`flash rootfs1 …`）；
  - 纯 MMC：513 字节换算为 2 块，`mmc erase/write` 命令不变，写失败返回 1；
    LWIP 下缺分区仍是“skipped”并返回 0；
  - 纯 NOR：分区单位为字节，`sf probe/erase/write` 命令不变，失败返回 1；
    缺分区返回 1；
  - 网页 NOR FIT 写失败时不 reset，成功时 reset。

## 范围与未验证边界

- **RC01**：在纯 NAND 路由上，raw UBI 只写主 `rootfs` 一次。请求备份时，因为实际布局中
  没有物理 `rootfs_1`，会在主写之前拒绝。tar 备份同样拒绝。
- **其他纯 NAND 板**：raw 目标改为按 SMEM 中实际存在的物理分区选择。与旧链的差异有：
  - `rootfs1` 缺失而 `rootfs2` 存在时，现在也会写 `rootfs2`（旧 hush 会在 `rootfs1`
    处 exit）；
  - 名称不是物理分区、只是 UBI 卷时，不再通过 `ubi_vol_present` 把 raw UBI 镜像写进卷
    （旧链会这样做，属于无意义的嵌套写入）；
  - 主 `rootfs` 必须是物理分区；
  - 请求备份时，缺少 `rootfs_1` 会被拒绝，存在时写入（旧链只有在 `rootfs1/rootfs2` 都存在
    时才能写到 `rootfs_1`）。
- **NOR+NAND（`SMEM_BOOT_NORPLUSNAND`）、SPI+NAND（`SMEM_BOOT_SPI_FLASH` 且
  rootfs 属性为 NAND）**：raw 命令链和 SYSUPGRADE 入口不变。这些路由由属性或 `rootfs`
  前缀后备决定，不返回负 errno，所以规范化不改变它们的结果；原有语义（例如前缀后备
  会重复写同一区域）也没有修复。
- **eMMC/NOR+eMMC、纯 NOR**：路由、块/字节单位、GPT、BOOTCONFIG 不变；eMMC 缺分区时
  “skipped”返回 0，eMMC 备份“Warning … skipping”后继续，这两个原有缺口不在本模块范围内。
  网页外层改为非零即失败后，这些分支已有的 +1 失败现在不会再触发 reset。
- **U-Boot/CDT 备份链（`flash X && flash X_1`）**：没有修改。规范化后，若请求备份但 `_1`
  分区缺失，主区写入后整体报告失败（以前因 hush exit 而误报成功）。RC01 有
  `0:APPSBL_1`/`0:CDT_1`，不受影响。其他板上这种情况不会在主写之前拒绝。
- 预检通过后，若硬件出现新的故障，目标仍可能部分擦写，并以失败结束。多目标没有回滚；
  SYSUPGRADE 失败后不保证旧卷完好。
- `flupdate set mmc/nor` 改变 `flash_type_new` 后，raw 预检仍按 SMEM 的 NAND 判断，
  而 `flash` 会走被覆盖的路由。这需要手工控制台操作才会出现，未处理。
- 宿主测试中，NAND/MTD、UBI 卷管理（除 ID 分配外）、mmc/sf、bootm、SMEM 均为模拟。
  hush 的词法解析由 fixture 分词器代替，但 list runner/exit 处理使用真实代码。
  未验证真实 UBI 写入、真实 hush 词法、QPIC/ECC/BBT、断电以及网页浏览器表现。
  HTML 生成链没有改动，也没有运行。
- 主线程已完成 BE10000 ARM32 完整 ELF 编译链接，验证新增头文件/声明及链接关系。
  固定工具链来源 `0daf0879219130c46812cd5029fbcc2940316b16`，日志在外层
  `build/uboot-validation-20261112/ub02-arm-full-build.log`。其他板型完整构建、
  可刷写 MBN 及设备启动未验。
- 默认 bootcmd、64 KiB 非冗余 ENV、布局和分区几何均未改变。

## 独立验收

Astra ACCEPT（2026-11-13 完成，报告按任务开始日期命名）：UB02 24/24、28/28，
UB01 29/29、30/30 及基线/变异检查通过；独立补充探针 23/23，覆盖正常 kernel-only、
rootfs-only 及双成员 tar 的成功与固定 ID。完整报告位于外层
`docs/reviews/uboot-ub02-astra-acceptance-2026-11-12.md`。

保留异常卷表限制：末尾 ID 检查是写后检测，不是迁移器。额外卷占用 ID 0、主卷 ID
错位，或缺 kernel 又只上传 rootfs 时，可能已更新载荷后才因 ID 不符失败；重复上传
不能保证纠正此布局。正常契约三卷的单成员更新和可补齐的缺卷输入另已验证成功。
本批不承诺异常卷表自动修复、多目标回滚或旧 MMC 内部已吞错误的全面检测。
