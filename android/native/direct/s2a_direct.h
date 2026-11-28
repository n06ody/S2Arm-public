// Android / OHOS native Vulkan direct-mode owner ABI v1（实施契约 §2.1）。
// 这是 native owner 与 NativeAOT 之间的私有 C ABI：table 由 native 工厂创建，托管侧只持有
// context 指针并调用表中入口；编译结果通过同步 emit 复制到 native 自有内存。
// 本文件被 C/NDK 与 C#（core-runtime/DirectAbi.cs）同时消费，字段一旦发布只能升版本。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2A_DIRECT_ABI 1u
#define S2A_DIRECT_TABLE_SIZE 64u
#define S2A_DIRECT_STATUS_SIZE 24u
#define S2A_DIRECT_SERVICES_REQUEST_V2_SIZE 80u

// 玩家/外壳请求的模式偏好：0 auto，1 generic，2 direct。与契约 §2.1 一致。
enum s2a_direct_mode_preference {
    S2A_DIRECT_MODE_AUTO = 0,
    S2A_DIRECT_MODE_GENERIC = 1,
    S2A_DIRECT_MODE_DIRECT = 2,
};

// 契约固定状态：Prepared=0 Active=1 Stopping=2 Stopped=3 Failed=4。
enum s2a_direct_state {
    S2A_DIRECT_PREPARED = 0,
    S2A_DIRECT_ACTIVE = 1,
    S2A_DIRECT_STOPPING = 2,
    S2A_DIRECT_STOPPED = 3,
    S2A_DIRECT_FAILED = 4,
};

// 等待义务位；Stopped 时全部清零。
enum s2a_direct_blocker {
    S2A_DIRECT_BLOCK_GUEST = 1u,
    S2A_DIRECT_BLOCK_COMPILE = 2u,
    S2A_DIRECT_BLOCK_SUBMIT = 4u,
    S2A_DIRECT_BLOCK_PRESENT = 8u,
    S2A_DIRECT_BLOCK_READBACK = 16u,
    S2A_DIRECT_BLOCK_WRITER = 32u,
    S2A_DIRECT_BLOCK_ALL = 63u,
};

// 契约固定返回码；负码永远不能当成功。
enum s2a_direct_result {
    S2A_DIRECT_RES_OK = 0,
    S2A_DIRECT_RES_PENDING = 1,
    S2A_DIRECT_RES_ARGUMENT = -1,
    S2A_DIRECT_RES_VERSION = -2,
    S2A_DIRECT_RES_UNSUPPORTED = -3,
    S2A_DIRECT_RES_IDENTITY = -4,
    S2A_DIRECT_RES_STOPPED = -5,
    S2A_DIRECT_RES_COMPILE = -6,
    S2A_DIRECT_RES_DRIVER = -7,
    S2A_DIRECT_RES_RETIREMENT = -8,
    S2A_DIRECT_RES_INTERNAL = -9,
};

typedef struct {
    const uint8_t *data;
    uint64_t bytes;
} s2a_direct_bytes;

typedef struct {
    uint32_t size, version, state, blockers;
    int32_t error;
    uint32_t reserved;
} s2a_direct_status_v1;

typedef struct s2a_direct_api_v1 {
    uint32_t size, version;      // LP64: 64 字节，version=1
    uint64_t session;            // 非零，与 runtime/lease 同一来源
    void *context;               // 偏移 16；native owner 拥有
    int32_t (*activate)(void *context);
    int32_t (*request_stop)(void *context);
    int32_t (*query)(void *context, s2a_direct_status_v1 *out);
    int32_t (*poll)(void *context, uint32_t budget_ms);
    int32_t (*destroy)(void *context);
} s2a_direct_api_v1;

typedef int32_t (*s2a_direct_emit_fn)(void *sink, s2a_direct_bytes result);
typedef int32_t (*s2a_direct_compile_fn)(uint64_t session, uint64_t generation,
    s2a_direct_bytes request, s2a_direct_emit_fn emit, void *sink);

typedef int32_t (*s2a_direct_encode_result_fn)(s2a_direct_bytes result,
    s2a_direct_emit_fn emit, void *sink);
struct s2a_direct_live_compiler {
    s2a_direct_compile_fn compile;
    s2a_direct_encode_result_fn encode_legacy;
};
// 可选二进制 provider。与已登记的 JSON 入口同会话，在 Activate 前补登记。
int32_t s2a_direct_bind_binary_compiler_v1(void *context, s2a_direct_compile_fn compile,
    s2a_direct_encode_result_fn encode_legacy);
// 仅返回指定会话已冻结的二进制入口。没有登记时两个指针均为空。
struct s2a_direct_live_compiler s2a_direct_bound_binary_compiler(void *context);

// 私有 C 导出：NativeAOT 同 DSO 直调；context 必须属于已冻结 session。
// 同一 context 重复登记同一指针是幂等的；登记成别的指针返回 S2A_DIRECT_RES_IDENTITY，
// 释放登记用 unbind（会话销毁时由 provider 调）。
int32_t s2a_direct_bind_compiler_v1(void *context, s2a_direct_compile_fn compile);
// Activate 成功后冻结该 context 的登记：之后只允许重复登记同一指针。未登记返回 RES_ARGUMENT。
int32_t s2a_direct_freeze_compiler_v1(void *context);
// 释放该 context 的登记（幂等）；会话销毁/收尾时调用，避免表项泄漏到下一次会话。
int32_t s2a_direct_unbind_compiler_v1(void *context);
// native 侧的读取入口：登记之后，需要"现场编一条 guest 着色器"的路径用这个取（未绑定返回 NULL）。
s2a_direct_compile_fn s2a_direct_bound_compile(void *context);
// 任意已登记的编译入口（无 owner ctx 的路径判断统一链是否就绪）。
s2a_direct_compile_fn s2a_direct_bound_compile_any(void);
// NativeAOT 侧 compile_fn 实现的稳定符号；native 在 Activate 前按此取地址并绑定，
// 不要求托管侧在运行时反射查找函数指针。返回值/所有权同 s2a_direct_compile_fn。
int32_t s2a_direct_compile_v1(uint64_t session, uint64_t generation,
    s2a_direct_bytes request, s2a_direct_emit_fn emit, void *sink);
int32_t s2a_direct_profile_v1(void *context, s2a_direct_emit_fn emit, void *sink);
// enabled profile 合成（NativeAOT 导出，唯一生产者）：入参是 native 实测的 s2a-device-caps-v1 JSON
// 与包描述符的 translatorClosure，出参是完整 s2a-direct-profile-v1。supportLayout hash / CompilerAbi /
// profileHash 只有编译器程序集能算，native 不重算、不补默认值；结果原样放进编译请求。
int32_t s2a_direct_profile_json_v1(s2a_direct_bytes caps, const char *translator_closure,
    s2a_direct_emit_fn emit, void *sink);
int32_t s2a_direct_video_v1(void *context, s2a_direct_bytes metadata,
    s2a_direct_bytes pixels);

// 只读校验：header 必须预置 size/version；成功时返回 0，失败保留 header 并写 error。
// 这两个 helper 是 native owner 与托管消费者共用的唯一入口校验，禁止各端另写一份宽松版本。
int32_t s2a_direct_status_init(s2a_direct_status_v1 *status);
int32_t s2a_direct_status_check(const s2a_direct_status_v1 *status);
int32_t s2a_direct_table_check(const s2a_direct_api_v1 *table);

#if UINTPTR_MAX == UINT64_MAX
#if defined(__cplusplus)
#define S2A_DIRECT_ASSERT static_assert
#else
#define S2A_DIRECT_ASSERT _Static_assert
#endif
S2A_DIRECT_ASSERT(sizeof(s2a_direct_status_v1) == S2A_DIRECT_STATUS_SIZE, "direct status ABI");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_status_v1, state) == 8, "direct status state offset");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_status_v1, blockers) == 12, "direct status blockers offset");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_status_v1, error) == 16, "direct status error offset");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_status_v1, reserved) == 20, "direct status reserved offset");
S2A_DIRECT_ASSERT(sizeof(s2a_direct_api_v1) == S2A_DIRECT_TABLE_SIZE, "direct table ABI");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_api_v1, session) == 8, "direct table session offset");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_api_v1, context) == 16, "direct table context offset");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_api_v1, activate) == 24, "direct table activate offset");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_api_v1, request_stop) == 32, "direct table stop offset");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_api_v1, query) == 40, "direct table query offset");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_api_v1, poll) == 48, "direct table poll offset");
S2A_DIRECT_ASSERT(offsetof(s2a_direct_api_v1, destroy) == 56, "direct table destroy offset");
#undef S2A_DIRECT_ASSERT
#endif

#ifdef __cplusplus
}
#endif
