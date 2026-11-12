# RC01 UB01：有界 NAND 写入、擦除前容量预检与擦除错误传播

2026-11-12，基线 `c08230eb454bc5dd3a87cbbe1f78be42cffb23b4`。只实现 UB01；未改
ENV、网页、UBI 目标选择或 `flash` 的 errno 规范化（后者随 UB02 原子交付）。
完成 Astra 初验修正及独立复验，UB01 开发级验收通过。

## 改动

修改文件：

- `common/cmd_flashwrite.c`：`write_to_flash` 的 NAND/QSPI NAND 分支。
- `arch/arm/cpu/armv7/qca/common/smem.c`：`getpart_offset_size` 在 64 位计算后、
  转换到 32 位输出前检查起点/长度；NAND flash 入口复用此解析器，不再先做会回绕的乘法。
- `drivers/mtd/nand/nand_util.c`：擦除错误传播、有界扫描、公共预检 helper、
  写/读循环的逐块上界及短传输检查。
- `include/nand.h`：声明 `nand_check_range_capacity`。
- 新增 `test/rc01/test_nand_write_safety.py`、`test/rc01/nand_write_safety_fixture.c`。
- 新增本文件。

### flash NAND 分支（`write_to_flash`）

1. 仍用 `run_command("nand device N [&& ipq_nand <layout>]")` 做设备与 layout
   准备，保留板级选择 hook 与 IPQ806X 重建 NAND 状态；失败直接返回失败。
2. 之后取 `nand_info[nand_curr_device]`（与原数值 `nand erase/write` 使用同一
   当前设备，且是 layout 切换后的几何），按其页大小进行有溢出检查的载荷取整。
3. `nand_check_range_capacity(nand, offset, part_size, file_size)` 扫描完整待擦
   分区；失败零擦写。
4. `nand_erase_opts`：`offset/length=part_size`，`lim=part_size`，`spread=0`；
   失败即返回，不写。
5. `nand_write_skip_bad(..., lim=part_size, WITH_WR_VERIFY)`；返回非零或
   写入长度不等于请求即失败。
6. 未定义 `CONFIG_CMD_NAND` 时该分支返回失败（原来 `nand` 命令不存在同样失败）。
   MMC、SPI NOR 分支未改，仍是原命令串和块/字节单位；`fl_erase` 未改（数值
   `nand erase` 现可通过 `nand_erase_opts` 报告擦除错误）。

### `nand_check_range_capacity`（新公共 helper）

检查几何（erasesize/writesize 非零且 2 的幂、size 非零）、范围（offset≥0、
range>0、offset<size、`range <= size - offset` 用减法避免溢出、擦块对齐）、
载荷（非零、页对齐、≤range）。随后查询范围内**每一个**块（即使容量已够，
也查尾部，因为整个范围会被擦），不查询范围外。已知坏块允许；查询返回负值
立即返回该错误；良块容量不足返回 `-EFBIG`；参数非法返回 `-EINVAL`。

### `nand_erase_opts`

`mtd_erase` 或 JFFS2 cleanmarker `mtd_write_oob` 失败立即返回（负 errno，正值
转为 `-EIO`），不再 `continue` 后返回 0。已知坏块跳过、`spread`、`lim`、`scrub`
语义不变；驱动是否标坏不影响本次返回失败。

### `nand_write_skip_bad` / `nand_read_skip_bad` / `check_skip_len`

- `check_skip_len` 增加 `lim`：在查询每块之前先检查设备末端（`-EINVAL`）与
  `offset - start >= lim`（`-EFBIG`），不再先扫到芯片末尾再比较；最终
  `used > lim` 检查保留（未对齐 lim）。负查询返回错误而非当作坏块。
  消息文本与原来相同（按 read/write 区分）。
- 写循环：每次查询、跳块和写入前都检查设备末端与 `lim`；写入块长度不得超过
  剩余 `lim` 与设备剩余；负查询失败；`nand_write` 返回 0 但长度短于请求视为
  `-EIO`。失败后停止，不在下一块续写同一载荷；`*length` 仍为已写长度。
- 快路径（扫描无坏块且无 TRIMFFS）保留：其范围在扫描时已证明 ≤lim，连续写
  不自行跳块；短写检查已加。
- `WITH_WR_VERIFY`、`WITH_DROP_FFS`（TRIMFFS 的截断长度作为请求长度）保持。
- 读路径同样有界、负查询失败、短读失败；`-EUCLEAN` 仍视为成功。
- `nand_verify`：读回长度短于请求视为 `-EIO`（原来会以短长度比较后继续，
  零长度时可能死循环）。

### 对其他调用者的可见行为变化（需验收关注）

- `-EFBIG` 时 `actual` 现为扫描到限位为止的跨度，而不是完整所需长度。全树调用者
  （cmd_nand、dfu_nand、fb_nand、env_nand、bootm、splash）只在成功时使用 `actual`。
- 坏块查询负值、短读/短写现在对所有 skip-bad 调用者都是失败（之前分别被当成坏块
  跳过或成功）。
- 擦除/cleanmarker 错误现在对 `nand erase` 命令、`flasherase`、env_nand 保存、
  dfu、fastboot、crashdump 都返回失败（各调用者已检查返回值）。
- `nand read/write` 命令以 `maxsize=chip-off` 调用，行为与前一致（回归覆盖）。

## 宿主验证

夹具按符号与 C 语法边界（括号/花括号匹配，跳过注释和字符串）从实际源码抽取：
`nand.h` 的 `struct nand_erase_options`、typedef、`WITH_*`、三个 inline 及公开原型；
`nand_util.c` 的 `nand_erase_opts`、`check_skip_len`、`nand_check_range_capacity`、
`drop_ffs`、`nand_verify`、`nand_write_skip_bad`、`nand_read_skip_bad`；
`cmd_flashwrite.c` 的 `write_to_flash`、`fl_erase`、`do_flash`。带 `#line` 保留来源。
模拟 NAND：只能 1→0 编程、擦除置 0xff，按块注入坏块/查询错/擦除错（可标坏）/
写错/短写/读错/短读/读回篡改/可纠正 ECC/第 N 次查询变坏；统计每块查询/擦/写，
越界访问计数。`run_command` 桩只执行 `nand device`、`ipq_nand`（模拟 deinit 后
重新注册几何）并记录 MMC/sf 命令；其他命令（如数值 `nand erase`）视为失败。
几何为 RC01 APPSBL `[0x01180000,0x01300000)`（12×128 KiB），APPSBL_1 紧随且已擦除
（越界写最坏情况）。

命令（在 `uboot/` 下）：

```sh
UV_CACHE_DIR=/tmp/ub01-uv-cache uv run --offline --no-project \
    python -B test/rc01/test_nand_write_safety.py [--keep] [--verbose]
git diff --check
```

生成物在 `/tmp/ub01-nand-*`（默认删除）。优先以 ASan/UBSan 编译，失败才退化。

初版结果（2026-11-12）：`fixed tree: 28/28 scenarios passed`，
`baseline c08230eb454b: 6/6 defects detected`，`RESULT: OK`；`git diff --check` 无输出。

覆盖场景：

- flash 入口（`do_flash`→`write_to_flash`）：APPSBL 满尺寸 + 1 坏块零擦写、邻区零
  查询/擦/写、旧镜像保留；页向上取整后有余量成功；`0xfffff900` 取整回绕为 0 被拒；
  镜像大于分区；未知分区。
- `write_to_flash`：恰好容量（1 坏块，11 块载荷）成功且有界读回一致；坏块有余量；
  正常无坏块（全分区擦一次）；全坏零擦写；载荷前缀够但分区尾块查询失败零擦写；
  扫描后新增坏块（预检/擦除/写扫描均良，写循环查询时变坏）失败且不触碰 APPSBL_1；
  写扫描时新坏但有余量成功；擦除失败（标坏/不标坏）不写且停止后续擦除；
  快路径和跳块路径下写错、短写、读回错、短读、内容不一致均失败；可纠正 ECC 成功；
  `ipq_nand linux` 前几何无效、切换后几何生效；layout 切换失败零查询零擦写；
  QSPI NAND 类型；非对齐起点/大小/载荷、零载荷、载荷>分区、跨设备末端、uint32
  回绕范围均零擦写；MMC/NOR 命令串与基线 `write_to_flash` 一致（块/字节单位不变）。
- API：Astra 3PEB/2PEB 扫描后坏块反例；负查询；限位内容量不足不查询限位外；
  擦除失败与 cleanmarker 失败返回；短写；已知坏块跳过且 spread=0 计数；spread=1
  与其 lim 拒绝保留；TRIMFFS（全 FF 页不写、数据正确）；cmd_nand 风格芯片级 lim
  及设备末端 `-EINVAL`；read 回归（跳块读、分区 lim、快路径、EUCLEAN 成功、
  不可纠正/短读/循环内短读/查询失败均失败）；helper 边界（尾块坏、各非法输入）。
- 基线变异检查：同一批 6 个 API 场景对基线源码编译，全部失败（证实测试命中原缺陷）。
  另手工对修复代码做 5 种变异（去掉写循环限位、去掉 flash 预检、擦除错误改回
  continue、flash 传芯片级 lim、预检容量够即停止），各自被至少一个场景检出；
  该变异脚本未提交。

## 未验证条件

- 主线程已对初版及乘法修正版完成 BE10000 ARM32 完整 ELF 编译链接，固定 GCC 5.2.0
  工具链来源 `0daf0879219130c46812cd5029fbcc2940316b16`。生成目录
  `build-rc01-validation`；现有 O= 构建脚本假定 DTS 位于生成目录，故仅在该目录增加
  指向本树源 DTS 的符号链接，未修改构建系统。最新日志在外层
  `build/uboot-validation-20261112/ub01-b1-arm-full-build-retry.log`。
  未构建所有其他板型配置，未验证可刷写 MBN 或实机启动。
- 实际 QPIC/ipq_nand 驱动、ECC、坏块表、标坏行为、时序和断电未测；`nand_write`
  快路径在扫描后变坏块上的驱动行为依赖“连续写不自行跳块”的既有分析。
- 页取整现使用 device/layout 准备后的几何并检查加法溢出；入口回归涵盖旧几何不可用、
  切换后以正确页大小处理未对齐的原始长度。物理页大小真实切换行为仍未实机验证。
- `flash` 返回值仍为 0/1（本分支），errno 规范化、网页判断、UBI/rootfs 目标选择、
  ENV 读回均属 UB02/UB03，未做。
- 预检后新故障仍可能导致分区已部分擦写；此时失败返回，无回滚。
- 未连接设备、未刷写、未保存 ENV、未推送。

## Astra 初验 B1 修正（主线程）

初验阻断为 `do_flash` 的 32 位块数乘法：起点 `0x8001` 或长度 `0x800c` 乘
128 KiB 后可能回绕到合法低地址。现在 NAND 入口统一使用真实
`getpart_offset_size`，其起点和长度乘法先提升至 `uint64_t`、检查非零块大小及
32 位可表示范围，再输出；到设备末尾的特殊长度也在减法前检查起点。
混合存储已有的同一字节解析器同时获得保护；MMC 保持其块单位。
仅 `-ENOENT` 可进入原 UBI 卷后备查找，异常范围不会被转成卷写入。

长期夹具抽取真实字节解析器及 `do_flash`。新增两种乘法回绕和零块大小的零擦写断言、
到设备末尾范围检查，以及 IPQ806X 入口的非对齐原始长度/layout 准备顺序。
最新结果：默认 **29/29**，IPQ806X **30/30**；基线 **6/6** 缺陷检出，ASan/UBSan
运行通过，`git diff --check` 通过。错误时 `actual` 是已扫描跨度、可能不完整，注释已统一。
独立复验 ACCEPT：原 B1 两组反例均失败且零擦写，不进入 UBI 后备；
独立探针 12/12、14/14 组通过，两类临时变异被检出。
完整报告在外层 `docs/reviews/uboot-ub01-astra-recheck-2026-11-12.md`；
这是宿主及源码级验收，不代表真实后端或设备通过。
