# 参与 S2Arm

欢迎提交可复现的问题、补充设备测试、修正文档，或改进代码。开始前先搜索已有 Issue 和 PR，避免重复工作。

## 选择改动范围

问题反馈使用[问题模板](https://github.com/jhaizhou-ops/S2Arm/issues/new/choose)，写清游戏名与版本、设备与系统、包号、复现步骤和实际现象。诊断导出方法见[使用教程](docs/内测教程.md#遇到问题怎么回报)。

准备修复代码或实现功能时，先查看[可领取任务](https://github.com/jhaizhou-ops/S2Arm/issues?q=is%3Aissue%20is%3Aopen%20label%3A%E5%8F%AF%E9%A2%86%E5%8F%96)，在对应 Issue 说明范围并按协作循环认领。不对应任务卡的新功能先开 Issue 写一句方案，维护方 1 小时内回「做 / 不做 / 换做法」后再动手；错字、失效链接等文档小修可以直接提 PR。可领取任务开出 24 小时没人认领，维护方会收回自己做。

从最新 `origin/main` 创建自己的分支，名称可用 `docs/<主题>`、`runtime/<主题>` 等。Android 也从最新 `origin/main` 开始；旧 `android/integration` 已于 10-27 冻结。不要直接推送 `main`，也不要修改他人的分支。

动手前阅读[仓库规范](docs/standards/repository.md)、[代码风格](docs/standards/style.md)和相关模块设计，按问题检索[坑记录](docs/坑记录.md)与研究目录。AI 代理还应先读 [AGENTS.md](AGENTS.md)。性能、能耗或稳定性改动须遵守测量与开发经验。

## 开发环境与构建

在仓库根目录检查开发工具：

```sh
bash tools/doctor.sh
```

检查只读取环境，不安装或下载依赖。开发与测试主要使用 Apple 芯片 Mac；完整要求及安装方法见[开发测试套件](docs/贡献者开发测试套件.md)。当前工具链的主要入口如下。

| 工具 | 版本与配置来源 |
| --- | --- |
| Xcode | 使用本机已安装的 Xcode，不限定固定版本；选择规则及 iOS SDK / Metal 能力检查见 [tools/toolchain.sh](tools/toolchain.sh)，部署目标见 [tools/toolchain.env](tools/toolchain.env) |
| .NET SDK | 微软官方 .NET 10 SDK，具体版本与滚动策略见 [global.json](compat/tree/global.json)；Homebrew 源码构建版的 NativeAOT 链接限制见环境检查输出 |
| Python | 3.12 或更新版本；改写器依赖见 [tools/rewriter-requirements.txt](tools/rewriter-requirements.txt) |
| bash | 5 或更新版本；macOS 自带的 bash 3.2 不满足开发检查要求 |

编译配置中的最低部署版本与已经验证的游戏运行范围分别维护；玩家使用要求见[中文 README](README.zh-CN.md#开始使用)。下面三条构建路径按改动对象选择。

### 构建运行组件

`make host` 生成 `build/s2arm-host`，默认用于原生层开发；需要完整系统服务与图形接入时加 `S2A_CS=1`。它构建运行组件，Mac 启动器应用另按下一节构建。

```sh
make host
make host S2A_CS=1
make test
```

`make test` 运行仓库登记的单元测试，部分检查需要额外工具或测试资源。怎样用自己的游戏启动、记录日志和判读结果，见[开发测试套件](docs/贡献者开发测试套件.md)。

### 构建 Mac 启动器

先跑一键准备：检查 Xcode / Metal、.NET SDK（按 [global.json](compat/tree/global.json) 的版本与滚动规则，由 dotnet 自己判定）、Homebrew 工具、Rust，建构建用 venv（缺省本树 `build/venv`）装齐钉版 Python 依赖（含字体生成器的 fonttools / skia-pathops），并准备 C 改写器固定依赖（`build/rewriter-deps`）。缺什么逐项列出并给装法；Metal 编译器版本与锁文件不同只出 note。

```sh
python3 launchers/macos/prepare_build.py
build/venv/bin/python3 launchers/macos/build_app.py
```

产物为 `build/macos/S2Arm.app`，`build_app.py` 缺运行环境时自己调 `bundle_runtime.py`。命令行出包与跑测试前把 `S2A_PY` 指向同一个解释器（环境变量，或写进 `tools/machine.local`，见 `tools/machine.local.example`）。依赖表、组件审计和调试方法见 [Mac 构建指南](launchers/macos/README.md#building-from-source)。

### 为自己的游戏出包

只需要出包时，可先运行专用环境检查，再将示例路径替换为自己的路径：

```sh
python3 pack/jobs/applenv.py
bash pack/oneclick/oneclick.sh "/absolute/path/to/game" --keys "/absolute/path/to/prod.keys"
```

这条命令生成 IPA 与资源包。具体参数、输出目录、签名和真机运行步骤见[开发测试套件](docs/贡献者开发测试套件.md)及[使用教程](docs/内测教程.md)。Android 构建入口见[平台文档](android/README.md)。

## 许可证与 SPDX

仓库根目录的 [LICENSE](LICENSE) 是 Apache License 2.0。版权人写 **S2Arm contributors**（仓库没有另写一个个人或公司作为整体版权人）。第三方组件各自的许可不因此改变，清单在 [NOTICE](NOTICE) 和 [第三方依赖](docs/第三方依赖.md)。

新写的源文件在文件头加两行，年份用首次提交的年份：

```
SPDX-License-Identifier: Apache-2.0
Copyright 2026 S2Arm contributors
```

不要为了补头去批量改已经在库里的文件。第三方文件、从上游原样拿来的文件，保留原来的版权声明和 `SPDX-License-Identifier`，不要改成 Apache-2.0。改到第三方文件时，在文件头说明改了什么（Apache-2.0 第 4(b) 条）。

## 改了什么就要做什么

| 改动目录 | 要跑的检查 |
| --- | --- |
| `runtime/` | `make host` + `make test` + `bash runtime/gate.sh` |
| `graphics/` | `bash graphics/gate.sh` |
| `translator/` | `make test`（改写器相关单测） |
| `services/` | `bash services/gate.sh`；改了跨平台接缝还需 `bash tests/platform_scan_test.sh` |
| `pack/` | `python3 pack/jobs/applenv.py` + `make test`（pack 相关单测） |
| `launchers/` | `python3 launchers/macos/prepare_build.py` + `build/venv/bin/python3 launchers/macos/build_app.py` |
| `android/` | `bash android/test.sh`（无设备 CPU 测，见 [安卓平台文档](android/README.md)）；Linux 上另跑 `bash android/linux_ctest.sh` |
| `docs/` 或 `*.md` | `python3 tests/docs_consistency_test.py`；本地链接可用 `make lint` 自查（自愿工具，见下文「验证改动」） |
| 任意改动 | `bash tools/contrib_check.sh --base origin/main`（一级自检，提 PR 必跑） |

Linux 暂不支持。跑不了的步骤写清原因和未测范围，不省略。

## 安卓两条翻译路（设备现编 / 出包预编库）

安卓直连的两条路共用 Ryujinx 着色器翻译核心。调试包使用设备现编。正式包可先从随包分发的 LibPack 取件，缺件时走设备现编。

| 环节 | 设备现编 | 出包预编库（LibPack） |
| --- | --- | --- |
| 回答层 | 从 Vulkan 能力和当前绑定收集事实，按设备回答格式、类型和能力查询。[device_caps.c](android/native/direct/device_caps.c)、[compile_request.c](android/native/direct/compile_request.c)、[DirectGpuAccessor.cs](graphics/tools/direct/compiler/DirectGpuAccessor.cs) | 从出包配置档回答查询，生成 SPIR-V 和反射数据并写入库。[dmsh_xlate/Program.cs](translator/dmsh_xlate/Program.cs)、[VkLib.cs](graphics/tools/direct/vklib/VkLib.cs) |
| 收编层 | 编译器输出 v1 程序结果，设备解析结果并建管线。[DirectShaderCompiler.cs](graphics/tools/direct/compiler/DirectShaderCompiler.cs)、[nvk_shader.c](android/native/gfx_plat/nvk_shader.c) | 设备按键取 SPIR-V 和反射数据，核对能力、重建绑定并合成 v1 结果，再走同一解析和建管线路径。[libpack_vk.c](android/native/direct/libpack_vk.c)、[nvn_vk_program_result.c](android/native/nvn/nvn_vk_program_result.c) |

两条路的绑定模型在现编结果、LibPack 反射转换以及共用解析和回放之间传递。改动时沿着 guest 槽来源、描述符种类、图像维度与数值类型核到实际绑定。纹素缓冲和普通纹理即使槽号相同，也要保留各自的资源类型。[DirectShaderCompiler.cs](graphics/tools/direct/compiler/DirectShaderCompiler.cs)、[libpack_vk.c](android/native/direct/libpack_vk.c)、[nvn_vk_program_result.c](android/native/nvn/nvn_vk_program_result.c)、[rp_draw.c](graphics/nvn/replay/rp_draw.c)

改回答层、收编层或绑定模型任一处，逐一核对两条路的对应实现。比对描述符集合与类型、图像声明格式与维度、共享内存与工作组大小、能力与扩展声明，以及着色阶段输入输出。补充回归时使用仓内夹具和合成的最小着色器，用设备能力档案重放查询。运行同口径检查并在 PR 写出结果。[门禁设计 #1836](https://github.com/jhaizhou-ops/S2Arm/issues/1836)

```sh
bash tests/libpack_device_parity_test.sh
```

该命令随 #1836 的小语料门禁接入。实际出库的全量检查由出包流程负责。收编改动须验证库件不符时会回到设备现编，现编也无法处理时会具名报红。

曾遇到的差异包括无像素互锁能力的设备收到多余声明、只读按位读的库版声明 `R32ui` 而现编用 `Unknown`、计算程序的共享内存超出设备上限，以及反射无法映射的绑定产生槽 `-1`。这些差异需要用带 LibPack 的正式包进可操控场景复核，确认库已打开且有命中，并检查 GPU 故障和拒收后跳过的计数。[案例和正式包冒烟判据 #1836](https://github.com/jhaizhou-ops/S2Arm/issues/1836)

## 验证改动

先运行与问题对应的最小充分检查，确认修复覆盖原场景。文档和配置检查内容、链接、格式及实际配置行为；运行代码需要验证受影响的调用路径。界面、设备、图形和性能结论应附对应运行证据，不能用编译成功代替。

完成本地 Git 提交、确认已跟踪文件没有未提交修改后，运行贡献者自检：

```sh
bash tools/contrib_check.sh --base origin/main
```

自检按改动范围选择本机可运行的检查，在流程末尾将摘要写入 `build/contrib_check/summary.md`。若前置检查提前退出，摘要可能未刷新，应以本次终端报错为准，不引用旧摘要。退出码及含义见下方从实际自检脚本生成的规则块。缺工具或其他阻塞要写清原因、影响和未测范围；修复后重跑相关检查。自检未通过时可以提草稿 PR，说明卡点。

需要维护者机器、游戏资产或真机的第二层检查由维护者处理。PR 审查后，维护者通过合入队列统一运行门禁；贡献者无需自行重复整套维护者门禁，职责与路由见协作循环。

格式工具由开发者自愿使用，不是新增合入要求：

```sh
make style-setup       # 手动安装固定版本工具，需要网络
make format-check     # 检查增量格式
make lint             # 静态问题、文档链接与日志事件设计
```

## 合入队列怎么批

- **分道成批。** 按改动范围分三道：只改文档的、只改工具 / 测试 / 出包脚本的（tool 档）、动到产品代码的（full 档）。同一道里排着的分支合成一批，只跑一份门禁；两档上限见下方生成规则块，可用相应环境变量覆盖。
- **推测流水。** full 道排着不止一批时，下一批不等上一批出结论，直接在上一批的确切合并结果上同时跑门禁。上一批原样合入后，下一批立刻推送；上一批红了，下一批作废、留队重跑，不会因此被退回。

你看到「一个一个在验证」，通常是下面几种情况：

1. **红批归责。** 先排除偶发和已知红，再核干净 main。能定位改动文件/路由时隔离相关支；确属 main 的红不归责任何一支。相关支的分批、并行深度和逐支核规则见下方生成块。推送时 main 前进则补核受影响步骤，不把旧输入结论用于新输入。
2. **单跑。** 急件（主线修复、换链合批）由维护者设为单跑，下一轮只取这一支。今天共 5 次。
3. **换链窗口。** 会改直连翻译链号的分支不随到随合，攒到窗口里一起合，链号一次只换一次。
4. **道里只有你一支。** 队列空的时候就是单支批。

<!-- S2ARM_POLICY:BEGIN -->
以下内容由 `python3 tools/docs_consistency.py --render` 从实际脚本生成；修改后用同一工具核对。
- 批上限：快档 `TRAIN_BATCH_MAX` 缺省 10 支；full 档 `TRAIN_FULL_MAX` 缺省 10 支。
- 红批归责：先按红行/红步定位并隔离相关支；无关支只继承输入未变的检查结论。相关支多于 `TRAIN_BISECT_MIN`（缺省 1）时，实际 `seqchk` 会对半分批，只核红步及两半共路由的步骤；并行深度低于 `TRAIN_PAR_DEPTH`（缺省 2）时两半同时跑，到上限后先后跑；不满足分批条件时逐支核。路由不明则跑整份门禁。
- 自检退出码：`0` 第一层本机已证通过（待宿主/第二层仍单列）；`1` 有红；`2` 不完整或参数/基线错误；`75` 环境未就绪；`130` 被中断。
<!-- S2ARM_POLICY:END -->

你能让批次更顺：

- 入队前在合并后的状态下跑自检（`bash tools/contrib_check.sh --base origin/main`）。红的分支进车会让整批陪跑一轮归责。
- 一个 PR 只做一件事，并在入队前合入最新 `origin/main`。冲突会被直接退回，不进门禁。
- 尽量少改公用登记文件（路由表、源文件清单这类每加一个文件就要改一处的清单）。两个分支同改一行就会冲突，这类文件正在逐个改成按目录自动发现。

## 提交 Pull Request

- 修好了 `docs/名单_能力强信号/<程序号>.tsv` 登记过的问题（某款的满屏 / 60 帧已知坏），请在同一个 PR 里删掉那一行并写明复测依据；
  实现改了这条不会自动撤，会标「过时·待重新实测」并保持关（`python3 tools/caps_policy.py retest` 列出复测清单）。

按 [PR 模板](.github/pull_request_template.md)写清关联 Issue、问题、最终改动、实际验证和未测项，附上本次一级自检摘要；若前置检查提前退出，附上本次终端报错。改共用目录时说明苹果路径受影响的范围及验证依据。标题可用 `fix(native): 修复地址窗口探测` 这样的格式。

**分支要短**：一个 PR 只做一件事，目标几小时内合入；做不完就拆成几个能各自验收的 PR。开了 24 小时还没合入，巡检会提醒「合最新 main 或拆小」；48 小时标 `待拆`。**main 红了先修 main**：你的合入把 main 弄红了，当轮修，修好前不在同一块继续堆改动。细则见 协作循环 §3.4。

保持一个 PR 的目标集中，避免混入无关格式化或目录搬迁。公共 ABI 改动要同步调用双方、文档和测试；改 Ryujinx 时按[补丁流程](compat/README.md)提交源码、补丁、`series` 与改动清单，不只提交 `tree/` 的修改。

## 保护数据与来源

- 游戏文件、固件、密钥、资源包和存档不进仓库，也不上传到 Issue、PR 或评论；诊断附件提交前检查脱敏结果。
- 从游戏派生的东西同样不进仓库：反编译 / 反汇编 / 重编译输出、改写后的游戏模块、从游戏着色器编出的 metallib / 着色器包、游戏截图和录像。测试只用原创的合成夹具。
- 项目**永远不提供**：打好的 IPA / APK 成品、AltStore / SideStore 软件源、预先改写好的游戏模块、获取密钥或固件的方法。Issue / PR 里请求这些的，一律不回应具体做法。
- 签名私钥、描述文件、令牌和个人配置保存在本地。`.gitignore` 只防止常见误加入，提交前仍须检查实际差异和附件。
- 清理只处理归属明确、可重建的产物，保留游戏库、存档与他人的运行目录。进程组信号使用 `tools/safe_killpg.sh`；测试与门禁不得隐式下载依赖。
- 新增或升级第三方代码时，保留来源、版本和许可，并更新[第三方依赖](docs/第三方依赖.md)。

AI 辅助贡献见 [LLM_POLICY.md](LLM_POLICY.md)，社区交流见 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。内部协调和维护者合入流程继续见[联合开发者指南](docs/贡献指南.md)与协作循环。

## 从旧仓库改投补丁

旧仓库上的补丁可以用 `python3 tools/port_old_patch.py <补丁文件>` 把路径换成当前目录结构，再 `git apply`。映射不到的文件会列出来；正文对不上的部分需要手工合并。
