# RC01 UB03：ENV 保存读回验证与网页反馈

2026-11-13，基线 `603ce5954c17eaed43d918707d3dd7d6411e6942`（UB01 `3e77a17`、UB02
均已接受并提交）。只实现 UB03；未改 UB01/UB02 源码、测试或交接；外层总记录由主线程维护。
保持 64 KiB 非冗余 ENV、APPSBLENV 分区几何、FIT 与默认启动策略；
未实现冗余 ENV、原子保存、RAM 回滚或新 CRC 格式。Astra 已独立复验 ACCEPT，
本模块以独立本地提交交付，不推送或发布。

## 独立验收

2026-11-13 [Astra 复验](../../docs/reviews/uboot-ub03-astra-recheck-2026-11-13.md)
接受 B1/B2 两项小修，原 BLOCK 保留。受检生产文件摘要与复验版本一致；
长期 UB03、UB01/UB02 组合回归全部通过，独立 NAND 边界 48/48、终端与通用
setenv 当前/基线成对对照 26/26。未重复目标构建，沿用主线程最终 ARM32 和生成资源证据。
此结论限源码及离线开发级验收，设备与可刷写 MBN 仍未验收。

## 改动

修改：

- `common/env_nand.c`：NAND ENV 保存前预检、逐块有界读写、写后独立读回比较。
- `common/cmd_nvedit.c`：删除 ENV 变量失败时保留失败返回，并输出明确错误。
- `failsafe/failsafe_httpd.c`：`httpd_handle_env_set` 的结果与长度检查。
- `failsafe/html/pig/about.html`：四个 `/env_set` 入口检查回复。
- `failsafe/html/pig/term.html`：修改/删除/单键恢复/全部恢复改为 `&& saveenv`。

新增：`test/rc01/test_env_save.py`、`env_save_fixture.c`、`env_set_fixture.c`、
`env_web_fixture.js`、`env_hush_fixture.c`、`test_env_terminal.py`，以及本文件。

### NAND ENV（`common/env_nand.c`）

- 新 `env_nand_xfer(offset, span, buf, write)`：在物理 ENV 区 `[offset, offset+span)`
  内按擦块顺序跳过已知坏块，每个良块从块首传输 `min(剩余, erasesize)` 字节，
  共 `CONFIG_ENV_RANGE`（RC01 = 64 KiB）字节。每次查询/传输前检查仍在区内
  且整块不越过区尾；负坏块查询、传输错误、短传输、零传输、良块用尽均失败。
  读返回 `-EUCLEAN` 且长度完整视为读成功（内容由调用者比较）。入口检查擦块大小
  为 2 的幂、offset 擦块对齐、区域在设备内、格式范围不超过缓冲区
  `CONFIG_ENV_SIZE`。
- `writeenv` 与非 SPL `readenv` 都调用它，所以保存与加载是同一块分布。原
  `readenv` 给 `nand_read_skip_bad` 的 limit 是芯片容量，现改为在已选良块上
  直接有界 `nand_read`。对格式范围是擦块整数倍或不超过一块（64 KiB、256 KiB）
  的配置，分布与原实现相同；原实现固定 `len=min(blocksize, RANGE)`，非整块倍数
  格式会越过缓冲区，现每块重算。
- `nand_saveenv`：先检查 `0 < RANGE <= board_env_size` 且 `RANGE <= CONFIG_ENV_SIZE`；
  再分配导出缓冲与独立读回缓冲，任一失败直接返回；随后 `env_export`，失败返回。
  以上任何失败都不查询、不擦除。
- `erase_and_write_env`：先拒绝未按实际擦块对齐的物理 `board_env_size`；
  该物理上限不允许向上取整扩大。冗余分支的格式大小擦除请求若需取整，先确认
  取整增量能由既有物理容量容纳。再用 UB01 的 `nand_check_range_capacity` 扫描整个
  实际待擦范围，
  载荷为 `CONFIG_ENV_RANGE`；容量不足、任一块（含尾部）查询失败、几何/范围非法时
  打印 `... check failed (...), nothing erased` 并返回 1，零擦写。之后保持原擦除
  选项（偏移/长度不变）调用 `nand_erase_opts`；写入；再用独立缓冲按同一分布读回
  并与导出数据（含 CRC 头）`memcmp`。全部成功才打印 `OK`、返回 0，否则 `FAILED!`
  （比较不一致另打印 `read back mismatch`）。不调用 `env_import`，RAM 不变。
- 冗余配置代码路径同步传入读回缓冲并在所有出口释放；RC01 不使用冗余，仅宿主编译与
  两个场景验证。SPL `readenv`（`nand_spl_load_image`）未改，`env_nand_xfer` 仅在
  `CMD_SAVEENV` 或非 SPL 时编译。

可见行为变化：

- 加载/保存现在要求 ENV offset 擦块对齐（SMEM 分区以块为单位，正常满足）；
  不再使用区尾不足一整块的残余空间（原写路径可越过区尾）。
- 加载路径坏块查询负值不再当坏块跳过，而是加载失败（回落默认 ENV，与原读错误一致）。
- 保存在擦除后失败时 ENV 区可能已部分擦写；单副本无回滚、无断电原子性，不变。

### `/env_set`（`failsafe/failsafe_httpd.c`）

- 名称 ≥64 或值 ≥128 字节直接回复 `error`，不调用 setenv（原 `memcpy/strcpy`
  可溢出栈缓冲）。不扩展通用 HTTP 解析；请求体仍须在同一个接收段内。
- 仍只接受 `reset_key`、`config_name`。`setenv` 失败：不保存，回复 `error`。
- `setenv` 成功后，`reset_key` 立即 `env_reset_gpio_invalidate()`（缓存随 RAM，
  与保存结果无关）；`saveenv` 失败回复 `save_failed`，成功才回复 `ok`。
  不回滚 RAM，不声称旧 RAM/NAND 已恢复；串口另打印对应信息。

### 关于页（`about.html`）

新增局部 `envSet(body)`（直接用 XHR，以便处理 `onerror/ontimeout/onabort`；未改
`common.js`）。配置名恢复/修改、reset_key 恢复/修改四处都用它：回复非 `ok`
（`error`、`save_failed`、非 200、网络错误、超时）各弹出一次说明，均含
“持久化未确认”；`save_failed` 说明内存已改、旧内存值和闪存均未恢复。之后照常
重新加载，并说明重新加载只反映内存值。成功时无提示（不新增“保存成功”文案）。

### 终端页（`term.html`）

`SAVE_TAIL = ' && saveenv'`：`setenv n v && saveenv`、`setenv n && saveenv`、
`env default -f n && saveenv`、`env default -a && saveenv`。HTTP 排队、轮询、输出
序号和连接 ✓ 语义未改，一般命令未改。

失败表达：没有额外 echo。实测旧 hush `run_list_real` 在 `a && b` 的 a 失败后会
跳过直到下一个 `;` 结尾的所有管道，`a && saveenv || echo ...` 在 a 失败时不会输出
echo，因此不加该尾巴。失败由命令自身输出体现：saveenv 失败有 `FAILED!` /
`nothing erased` 等；删除不存在或不可删除变量的失败出口现在明确输出
`Error: failed to delete environment variable`，不再依靠缺少保存输出推断。

## 宿主验证（实际运行）

```sh
UV_CACHE_DIR=/tmp/uv-cache uv run --offline --no-project \
    python -B test/rc01/test_env_save.py
```

测试工作目录用 `tempfile.mkdtemp()`，不固定 `/tmp` 路径；每个场景单独进程运行，
基线上的 ASan 中止只影响该场景。需 `cc` 与 `node`（无 node 时测试判失败）。

- **NAND ENV**：把完整真实 `common/env_nand.c` `#include` 进夹具（其预处理结构原样
  编译），配合按符号抽取的真实 `nand_erase_opts`、`nand_check_range_capacity`、
  `check_skip_len`、`nand_read_skip_bad` 及 `nand.h` 内联。仅模拟 NAND 介质（只能
  1→0 编程、按块注入故障、访问计数）、`env_export`（确定内容 + 真实 CRC32）、
  `env_import`（记录真实加载路径 `nand_env_relocate_spec → readenv` 交出的数据）。
  - fmt64：64 KiB 格式、4 块（512 KiB）分区，RC01 几何：35/35。
  - fmt256：256 KiB 格式跨两块、8 块分区：35/35。
  - 冗余配置：2/2（写第二副本；第二副本不可用时只预检不擦，回落主副本）。
  - 只编译矩阵：SPL 无保存、SPL 有保存、非 SPL 无保存均编译通过
    （`-Werror=unused-function/implicit-function-declaration`）。
  - 覆盖：无坏块/起始坏块/中间坏块/容量恰好/设备末端保存成功且加载一致；连续两次保存；
    未对齐物理长度（192/320 KiB）、无坏块及首块坏均零查询、零擦写、邻区内容不变；
    尾部查询失败、容量不足、设备末端容量不足、未对齐/格式大于分区/区域越过设备末端
    均零擦写；首个/读回缓冲分配失败、导出失败零查询零擦；擦除失败不写；写错/短写/
    零写/写丢失（读回不一致）、读回 EBADMSG/EIO/短读/零读/数据错误、EUCLEAN+错误
    均失败且不继续写后续块；EUCLEAN 内容一致成功；写阶段查询失败；加载路径坏块、
    EUCLEAN、短读、零读、EBADMSG、查询失败、设备末端全坏。每个场景还检查分区外
    块零访问且内容不变、无越过设备访问、无坏块被编程、无内存泄漏。
- **`/env_set`**：抽取真实 `httpd_handle_env_set`，桩为两变量 RAM/闪存模型和
  `lib/ipq_api.c` 同语义的 reset_key 缓存：14/14。覆盖设置/恢复两种变量成功、
  `&` 截断、setenv 失败零保存、保存失败非 ok 且 RAM/缓存一致、未知名称、无请求体、
  超长名称/值（127 字节可用，128 拒绝）。
- **页面**：Node vm 运行真实 `common.js` 与 `about.html`、`term.html` 内联脚本，
  最小 DOM/XHR/fetch/对话框桩，点击真实按钮：37/37。关于页 4 个入口 × 6 种结果
  （ok/error/save_failed/HTTP 500/网络错误/超时）；终端页 4 个动作 × 3 种结果
  （成功/前半失败/保存失败）及查询不变。终端命令由 hush 列表跳过规则的 JS 模型
  执行（按 `run_list_real` 源码实现，非真实 hush）。
- **完整真实 hush**：新增夹具包含完整 `common/cli_hush.c`，抽取真实
  `_do_env_set/do_env_set/do_env_default`、`cmd_process/cmd_call`、`run_command`。
  Node 捕获四类按钮实际 POST 的命令，交给真实解析与执行控制流：成功/保存失败共
  8 例，加删除不存在/拒绝删除两例，10/10 通过；失败有明确错误且不保存。
  ENV 哈希表和保存后端为桩。相同命令对基线的真实删除函数会静默失败，负对照已检出。
- **最小变异**（当前树单点回退，所列场景必须失败）：15 组全部检出——去掉预检、
  去掉比较、去掉读回、接受短传输、EUCLEAN 当错误、负查询当坏块、固定块长、
  去掉读回缓冲分配检查、setenv 失败仍保存、保存失败回 ok、缓存失效只在保存成功后、
  去掉长度检查、关于页忽略回复、关于页不处理网络错误、终端改回 `; saveenv`。
- **基线 603ce59**：同一夹具运行原代码，fmt64 12/12、fmt256 14/14（另含跨块容量不足
  擦旧 ENV）、`/env_set` 7/7（含两处栈溢出）、页面 16/16 预期缺陷全部检出。基线
  另外失败的场景：区域越过设备末端时擦到设备末端并越界查询、零写后继续写后续块、
  加载路径查询失败被当坏块；以及因原代码无读回缓冲/不读回而失败的两个场景。
  64 KiB 单块格式下“容量不足”即全坏，原代码本也无块可擦，该缺陷由 256 KiB 验证。

组合回归（未改其源码/测试）：

- UB01 `test_nand_write_safety.py`：29/29、IPQ806X 30/30，基线 6/6，RESULT OK。
- UB02 `test_upgrade_results.py`：rc01 24/24、other 28/28，变异与基线全部检出，RESULT OK。
- `git diff --check` 通过。

## 未验证与待处理

- 主线程已完成 BE10000 ARM32 完整 ELF 编译链接、实际网页资源生成；从 fsdata 解压
  common.js/about.html/term.html，确认其 HTTP 响应字节确实嵌入 u-boot.bin，并对解压
  页面运行 37 个 Node 场景。修正版日志位于外层
  `build/uboot-validation-20261112/ub03-b1-b2-arm-full-build.log` 和
  `ub03-b1-b2-generated-web.log`。未验证可刷写 MBN、真实浏览器或设备，未刷写、未保存现场 ENV。
- 宿主夹具不覆盖真实 QPIC/ECC 行为、断电、驱动内部标坏及并发；完整真实 hush 测试
  的 ENV 哈希表和保存后端仍为桩。
- 既有限制保留：单副本 ENV 断电风险；保存在擦除后失败会留下部分写入的 ENV 区；
  `/env_set` 不做 URL 解码（页面用 `encodeURIComponent`，如 `config@x` 会以
  `config%40x` 存入，原有行为，未在本模块处理）；请求体跨 TCP 段仍按原解析失败。
- Astra 初验曾阻断物理范围被取整扩大、终端静默失败两项；已按最小建议修复，
  复验 ACCEPT、两项阻断解除，初验报告保留。单副本断电及失败后无回滚的限制继续保留。
