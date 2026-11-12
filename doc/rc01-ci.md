# RC01 BE10000 U-Boot CI

`.github/workflows/build-ipq-uboot.yml` 只做 **BE10000 单目标、只读验证**：不发布或删除 Release，
token 仅 `contents: read`，checkout 使用 `persist-credentials: false`。绿灯只代表本次静态构建候选
通过离线检查，**不代表**设备启动、前级 v6 接受、刷写、ENV 兼容或恢复能力。

## 触发与环境

- `push` 仅 `be10000-four-port`；任意 `pull_request`；`workflow_dispatch`（无输入）。无 schedule。
- 单 job，`ubuntu-24.04` x86_64，60 分钟超时；同一 ref 的新运行取消旧运行。
- Node `24.22.2`（setup-node）、uv `0.13.5`（setup-uv）、runner 自带 Python 3（`UV_PYTHON=python3`，
  禁止下载）；apt 只装构建/DTC 所需包，不装 npm minifier 或 zopfli。
- `RC01_CI_STRICT=1`：回归基线缺失或 sanitizer 编译失败即失败。
  `RC01_WEB_PROFILE=rename-gzip`：class rename + `gzip -9n`，不探测全局 minifier/zopfli。
- `TZ=UTC`、`LC_ALL=C.UTF-8`、`SOURCE_DATE_EPOCH=` 本次 commit 时间。
- 工具链：`1980490718/toolchain-arm_cortex-a7_gcc-5.2.0` 精确 commit
  `0daf0879219130c46812cd5029fbcc2940316b16`，按 SHA `fetch --depth=1` 到 `../staging_dir`
  （build.sh 的固定位置）并核对 HEAD。不使用归档，因此不套用归档 SHA-256。
- Action 固定到 commit SHA（主线程在线核对，注释为版本标签）。

## 步骤

1. 完整历史 checkout；`git cat-file -e` 检查 `test/rc01/test_*.py` 中全部 `BASELINE_REV`。
2. 严格宿主回归，逐个执行（任一非零即停）：`test_nand_write_safety.py`、`test_upgrade_results.py`、
   `test_env_save.py`、`test_ci_failures.py`、`test_ci_candidate.py`。
3. 树内 `./build.sh ipq9574_xiaomi_be10000_defconfig`（不用 `O=`，沿用 build.sh 的 strip + v6/SHA-384 打包）。
4. `tools/ci/check_be10000_candidate.py` 检查本次产物；全部通过才生成候选目录。
5. 上传 artifact `be10000-uboot-candidate-<sha>`（14 天，缺文件即失败）：唯一 MBN、`manifest.json`、
   `SHA256SUMS`、`check-result.json`。失败时另传 `be10000-uboot-diagnostics-<sha>-<attempt>`
   （日志、check-result、`.config`、`u-boot.cfg`，7 天），它不是候选。

## 候选检查（`tools/ci/check_be10000_candidate.py`）

只用标准库加 `fdtget`、`node`。输入均为同一次树内构建：`u-boot`、`.config`、`u-boot.cfg`、
`dtb_combined.bin.gz`、`failsafe/fsdata.c`、`failsafe/html/pig/`，以及 `bin/` 中的 MBN。

| 阶段 | 内容 |
|---|---|
| config | ARM32、只选 BE10000、`SYS_BOARD=ipq9574`；`u-boot.cfg` 宏求值得 `CONFIG_ENV_SIZE=65536`、ENV 在 NAND、无冗余 ENV 宏、`LWIP_HTTPD`；取 `CONFIG_SYS_TEXT_BASE` |
| mbn | MBN/ELF 均为 ELF32 LE ARM；所有段在 EOF 内且文件长度等于最后段末尾；唯一 LOAD 的头与载荷和 `u-boot` 相同；入口 = LOAD vaddr = TEXT_BASE；段 0 为 ELF 头 + PHDR、段 1 为 hash 段，其余与 ELF 对应；v6 头（版本 6、image/code size = phnum×48、无签名/证书/metadata）；每个 SHA-384 项重算比对 |
| budget | APPSBL 1572864 bytes（RC01 layout），按 2 KiB 页、128 KiB 擦块取整，块数不得超过 12；剩余块只是名义值，实际坏块未知，不能替代 UB01 运行期预检 |
| dtb | 在最终 LOAD 中找到恰好一个含 FDT 的 gzip，且只含一个 FDT，与 `dtb_combined.bin.gz` 相同；`model`、`machid=0x8050301`、C4/rdp433 `config_name`、`switch_mac_mode0=0x15`、port 0–3 QCA8084（地址 1–4）、port 4–5 unused |
| web | 解析 `fsdata.c`，资源清单与 `html/pig` 一致；状态行、Content-Type；文本资源须为完整 gzip（无残余数据）；每个响应的原始字节都出现在最终 LOAD 中；把解压后的 `common.js/about.html/term.html` 交给真实 `env_web_fixture.js`，要求 exit 0、零 FAIL、37/37 且与 `--list` 一致 |

```sh
uv run --offline --no-project python -B tools/ci/check_be10000_candidate.py \
  --tree . --mbn bin/openwrt-ipq9574_xiaomi_be10000-u-boot.mbn \
  --result /tmp/out/check-result.json --candidate-dir /tmp/out/candidate --meta source_sha=$(git rev-parse HEAD)
```

退出码：0 通过，1 拒绝（不写候选目录），2 用法错误。

## 负例（`test/rc01/test_ci_candidate.py`）

不需要交叉工具链：用真实 `makefsdatac`（rename-gzip）生成页面，dtc 生成带 BE10000 身份的 DTB，
放入合成 ELF32 ARM 的 LOAD，再用真实 `tools/elftombn_py3.py -v 6` 打包。参考例必须通过并生成候选；
以下注入必须在对应阶段被拒绝且无候选目录：空 MBN、截断、尾部多余数据、LOAD 字节翻转、hash 表翻转、
截断 ELF 经真实打包器（打包器退出 0）、错误 model/machid/port3、`.config` 选 BE7000、冗余 ENV、
128 KiB ENV、空 about.html（页面夹具 24 FAIL）、构建后 fsdata 变更（响应不在 LOAD）。
设置 `RC01_CI_TREE=<已构建树>` 时还检查真实 MBN 通过、截断/翻转副本被拒。

## 未覆盖

可复现双构建、缓存、npm minifier/zopfli profile、其他板型、签名、Release 流程、前级加载/实机启动，
本 CI 均未覆盖。
