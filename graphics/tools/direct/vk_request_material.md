# 安卓 Vulkan 请求原料 v1

**文件**　`shaders/direct/request-material.bin` 是 Android revision 的可选资源，进入现行资源索引。只从玩家本次抽取的客体代码取材。现有 `lib.idx` 和 `warm-vk` 格式保持原样。

**封装**　64 字节头按顺序为 `magic[8]="S2ARQMAT"`、`version:u32=1`、`headerBytes:u32=64`、`indexBytes:u64`、`blobBytes:u64`、`bodySha256[32]`。整数字段小端。其后为无 BOM 的 UTF-8 JSON 对象和二进制 blob 区，SHA-256 覆盖整个 JSON 与 blob。总文件长度必须精确吻合。所有 JSON 范围 `[offset,length]` 都相对 blob 起点，以无符号十进制整数写出。要求 offset ≤ blobBytes，length ≤ blobBytes − offset。后台流式校验与读片段，允许 64 位文件长度。

**索引**　`chain` 取同库 12 位链号。以下引用为对应数组的零基下标。十六进制统一小写。
- `sources[] = {stage,key,sourceSha,code,control}`。阶段为 `vs/fs/gs/cs`，`key` 为 16 hex，`sourceSha` 为 64 hex。摘要采用设备 `source_store` 的完整规范摘要，即控制段按 NVN 键规则归一后与原始 code 串接。code 保留现场身份所用的完整原始区间，包括 0x30 或 0x100 头及尾段。control 按现场 nctl 切片，只有摘要副本按 NVN1 v2 规则置零。key 等于 sourceSha 前 8 字节，仅用于定位和排优先级。
- `rejects[] = {stage,key,source}`。每个交给现场编译的拒收项恰有一行。记录缺失或截断时 `source:null`，计入待现场事实。拒收身份按阶段和键记录，若另有 GS 单图元拒收则身份也包括图元。不能经现编的其他阶段单列理由。一个已通过的阶段只有在补全含拒收阶段的组合时才进入 sources。
- `requests[] = {sources,topology?,facts?}`。sources 为实际配对的 `[vs,fs]`、`[vs,fs,gs]` 或 `[cs]`，均须牵涉拒收项。已知 GS 图元沿用编译请求的枚举名。GS 图元与现场输入的拓扑须相同。CS 的 localSize、local 与 shared 由现有编译请求解析从同份 control 读出。只有 VS/FS 配对证据时可记录配对，完整 GS 请求另需真实证据。缺配对留在 rejects，无依据的组合不生成。
- `facts = {input,c1}` 可选。input 是现场产生的规范 `S2AI1` 字节范围，以设备已有的 `s2a_input_identity_decode` 支持的版本为准，包含来源、能力档、闭包、纹理及绘制/计算状态。c1 按阶段顺序给范围或 null，设备按 source_store 的 build_c1 口径与同份 code/control 交叉核对。仅能携带完整请求实际消费的段，warm-vk 的 0x100 前缀不能代替完整 C1。多个有证据的状态可为同一组合各留一行。缺字段保持缺失，编译器不能从事实推出有效请求时保留现场回退。

**消费**　缺文件、未知版本或完整性失败时视为无原料，启动在现有路径继续。后台按原预热清单及热度处理，复用 #1774 首次键，校验当前能力档、闭包、阶段全摘要/长度和规范输入，重算完整请求身份。真实首遇按现有完整身份匹配。事实或必要 C1 不符、缺来源、编译失败时，直接按 #1850 回退。宿主测试覆盖拒收一一对应、跨阶段组合、版本头和未知版本。

**当前来源边界**　现有 `shader_pairs.tsv` 提供 VS/FS 配对，现有 `warm-vk` 有身份摘要及 C1，完整纹理／状态需另有现场规范输入才写 facts。本片出包可先保存可证明的原料和配对。
