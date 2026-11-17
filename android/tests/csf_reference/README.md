# 计算 shared → storage 的同输入参考执行

这是默认关闭的诊断与宿主 PoC，不是设备安全性或游戏可见性的验收。原版与 spill 输出相同，只能排除**该输入下**的改写差异；两者都输出零不证明视锥、包围盒、实例/变换索引等输入正确。真实输入、着色器请求、截图和二进制输出仅留在私有运行目录，不进仓库或公开报告。

## 收窄诊断

开关均以 `debug.s2a.` 为 Android property 前缀，每局重启进程：

| 名称 | 默认 | 作用 |
|---|---:|---|
| `nvk_csd` | 0 | 记目标派发/绑定/消费元数据及独立 GPU 时间戳，不新增 fence 等待 |
| `nvk_csf` | 0 | 同时启用元数据；只在目标派发及与其可写范围相交的间接参数消费前后等 fence |
| `nvk_csf_a`, `nvk_csf_b` | 0 | 可配置的 shader key 前 32 位选择器（十进制）；日志核对完整 64 位键 |
| `nvk_csc` | 0 | 对目标 A 抓输入与输出；必须同时开启 `nvk_csf` |
| `nvk_csc_x` | 0 | 0 不筛选；正数只抓该 group-x 的派发 |
| `nvk_csc_g` | 0 | 0 不筛选；正数只抓 x×y×z 等于该值的派发；与 x 筛选同时设置时取交集 |
| `nvk_csc_at` | 1 | 从第几笔符合组数的目标 A 开始 |
| `nvk_csc_n` | 1 | 最多抓几笔，上限 16 |
| `nvk_spill_pad` | 1 | 只接受 1/2；设 2 将 spill arena 预留量翻倍，descriptor range 和每组步长不变 |
| `nvk_csk` | 0 | 程序 resolve 成功后，仅跳过下面完整 64 位键的派发；不清输出 |
| `nvk_csk_hi`, `nvk_csk_lo` | 0 | 完整键高/低 32 位；按有符号 32 位十进制传入，已知且非零的完整键才匹配 |
| `nvk_spill` | 1 | 关闭时**跳过整个 spill 派发**，不是执行原版 shared 核 |

目标关系来自成功编码派发的实际 descriptor 表，记录 set/binding、来源槽、访问权限、guest 范围、后端 buffer/offset/size。只检查真实间接参数记录及 draw-count，不把 multi-draw 的 stride padding 当消费；无关直接/间接绘制不切段。`draw_encoded` 将调用记录与实际编码笔数分开。`encoded` 记录实际派发组数，间接组数只有成功前置 fence 才能读取。

`csf spill_range` 记录本笔实际绑定的 offset/bytes、后端 buffer 容量、minStorageBufferOffsetAlignment/maxStorageBufferRange、总组数、每组字节数、所需总量和最后一组的半开切片区间。`aligned/range_fits/slices_fit/word32_fits/backing_fits` 分别核对对齐、描述符范围、所有组切片、32 位字下标和 backing 边界；已知不符报红，未知维度/容量/设备能力报 WARN 并标 known=0。`reserved_bytes/pad/reserved_fits` 另核本笔 arena 预留量。pad=2 不扩大 descriptor range；后续子分配偏移及 backing 复用可能变化，必须记录实际值，不能保证两局物理布局相同。范围日志本身不改变派发；该范围满足不能证明 shader 动态访问正确。

总组数不能代替三维形状：14 组可能是 7×2×1。只知道总组数时用 `nvk_csc_x=0,nvk_csc_g=14`，再由日志确认 dims；旧 8×1×1、较大的 11/14 组与 1×1×1 都要分列，不能只取先出现的小派发。

`last_recorded_writer` 是最后登记的潜在 GPU 写或回放 CPU 写；`writer_covers` 区分完整/部分覆盖。反射可写不证明动态 store 发生，外部游戏 CPU 写未跟踪。目标历史最多 256 项、写者历史 512 项、每笔最多输出 128 条相交参数；淘汰与截断明确报 `coverage=incomplete`。池代际变化清空旧归属并报缺口，防地址复用产生假关系。失败/超时/额度不足时只报 `source=unavailable`，不读未完成数据，也不把日志里的占位零当实值。

## 单次派发时间戳与跳核边界

`csf dispatch_time` 使用紧邻实际 `vkCmdDispatch/Indirect` 的 TOP/BOTTOM query 对，按 seq/完整键记录；一个段两笔目标各有一份，未选中派发不记。只在成功 fence 后读取 query，不加 query WAIT 或额外诊断切段。每段最多 64 对；超额、能力不支持、query 未就绪、提交或完成失败均明确 `source=unavailable`，保留分母。`source=completed_query` 带原始 tick、valid_bits、period_ns、gpu_ns；区间为 `scope=dispatch_bracket`，不代表着色器独占时间，不能由 CPU 日志间隔代替。`csf compute_limits` 从实际 Vulkan properties/queue 记录三维 max_size/max_count、max_invocations、shared、SSBO 范围和时间戳能力。

`nvk_csk` 在程序编译/改写与 resolve 成功之后判断完整键，匹配时只是不提交该派发，记录 `program_resolved=1 output_action=preserved dispatch=0` 及 `compute_diag_key_skipped` 未接计数；相同前缀但不同低位不匹配。它保留缓冲原内容，不等于运行原版或生成正确输出，也不能单独区分核执行故障与输出消费者故障。只有 fence 成功和消费先后关系才能继续收窄。开关关闭为正常派发路径。

## 零值未接汇总

`nvk_csd` 或 `nvk_csf` 打开时，每 1024 次成功呈现输出一行 `csf unimpl summary`，零值也打。至少包含 `kinds`、`compute_compile_failed`（shader 类）、`compute_program_unresolved`（draw 类）。每行带 `snapshot_stable`；有并发更新时为 0，此时不能用该行证明零值。`table_overflow=1` 表示种类表已饱和，kinds 不是全量种类数；两项指定计数仍单独累计。

收局阶段分开标记：`session_stop` 是实际 provider Stop/Destroy 触发的快照，不代表在途工作已退休；`session_retire` 是共用 replay 清理；`normal_exit` 来自实际正常退出的 `s2a_direct_summary(1)`。复用已有 summary owner，在还没有纹理池/程序时也注册；不依赖无人调用的 `nvk_session_close()` 或 atexit。退出快照只读原子值、不等待工作线程持有的计数表锁。未看到行、非正常强杀、局尾缺日志都不能当零次；开关按进程固定、每局重启。

## 抓取与准备

QA 在独立捕获局，把 `S2A_DIRECT_DIAG_DIR` 环境变量指向该局独占、已存在、应用可写的私有目录；这是 BootContract/env 输入，不是 Android property。文件排他创建，禁止复用已有运行目录。

- `csf-<seq>.tsv`：版本、完整键、groups、program identity、每个实际绑定与 `status`。
- `csf-<seq>-<set>-<binding>.before.bin` / `.after.bin`：GPU 顺序中派发前后完整 buffer 范围，含 UBO、SSBO、相同 buffer 的精确 alias/offset。scratch 不作为输入。
- 图片的 `.before.bin`：实际 Vulkan 视图的全部 mip，保留格式、aspect、尺寸、swizzle。当前只支持 2D 单层、单采样、原生可复制格式。
- `csf-<identity>.request.json`：相同 identity 的精确 compiler 请求，含 program、facts、profile。

只有 `status=complete`、该 seq `dispatch=1`、前后 fence 成功且请求齐全，才可进入参考执行。单 buffer 上限 64 MiB、输入合计 128 MiB；资源未支持、文件未写全、失败 fence 都不能当完整快照。GPU 图片拷贝位于安卓具体 Vulkan 后端，因为需要实际 VkImage/View 格式、mip 与 swizzle；绑定关联及 buffer 捕获在共用 replay 层。没有改通用 `graphics/vulkan` 后端。

用诊断头对应 compiler CLI 构建出的 DLL：

```sh
python3 android/tests/csf_reference/prepare.py \
  --request PRIVATE/csf-IDENTITY.request.json \
  --compiler PRIVATE/S2A.DirectCompiler.Cli.dll --out PRIVATE/pair
```

`prepare.py` 保留 program/facts，原版只把 profile 的共享内存上限改为 65536 并重算 profileHash；输出 `original.spv`、`spill.spv` 和 `pair.json`。除新增 spill descriptor 外接口变化即拒绝。不要使用不同翻译闭包的 CLI。

## 执行与判读

`runner.cpp` 使用 NDK 29 真 Vulkan 头编译，开启 Khronos validation，实际查询 shared/local 上限，容量不足直接拒绝。它保留 buffer alias、offset、完整输入、图片各 mip/swizzle；不录 sampler 状态，因此 `compare.py` 拒绝采样、存储、稀疏图像操作，仅接受 texelFetch 路径。其他特性或 VUID 报红，不能关校验层绕过。

```sh
python3 android/tests/csf_reference/compare.py \
  --runner PRIVATE/runner --snapshot PRIVATE/csf-SEQ.tsv \
  --original PRIVATE/pair/original.spv --spill PRIVATE/pair/spill.spv \
  --spill-set SET --spill-binding BINDING --spill-bytes BYTES_PER_GROUP \
  --shared 45056 --out PRIVATE/new-result
```

SET/BINDING/BYTES_PER_GROUP 取自 `pair.json`。路径不能包含空白，输出目录必须尚不存在。原版重复两次、spill 两次 scratch 分别填 0/0xa5；比较所有可写绑定的每字节/每 32 位字，报告差异数、首差异偏移与 SHA256。原版重复或 spill 初值扰动不稳定，结论为 `unstable_or_uninitialized`；原版与 spill 不同为 `different`；`equal` 仍需另看 `reference_spill_vs_device`，宿主与设备差异不会被掩盖成设备验收。

`compact.comp` 是不含游戏资产的独立已知答案：local 1024、shared 45056、8×1×1，读取输入列表和整数纹理，写压缩列表/计数/尾哨兵；`fixture.py` 独立生成预期 32832 字节。`run.sh` 从 NDK29 取头、调用生产 `DirectSharedSpill.Rewrite`，通过现有离线 Vulkan 容器运行。stock lavapipe 共享上限可能只有 32768，此时拒绝属于环境不足，不能记绿。

本次本地实验以 Mesa 25.2.8 源码中 llvmpipe 的 `max_local_size` 单一上限由 32768 改为 65536；其 CPU workgroup 内存按请求动态分配。参考驱动改动不进产品。已做两级对照：32 KiB 同构核在 stock/放宽版上都等于独立答案；45,056 B 核在放宽版上原版两次/spill 两次全部等于独立答案，校验层开启。这证明 PoC 执行通路，真实 instance_cull 同输入对照仍待真机快照。

所有构建、编译与运行按任务要求经统一 heavy 队列，不直接执行上述命令。
