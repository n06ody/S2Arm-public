// C1/C2：managed 编译 provider 的 native 侧登记处。
// 契约（s2a_direct.h）：native owner 在 Activate 之前调 s2a_direct_bind_compiler_v1(context, compile)，
// 之后所有"要编一条 guest 着色器"的 native 路径都用 s2a_direct_bound_compile(context) 取它。
// 只登记指针，不做翻译、不缓存结果——翻译仍然只有 DirectShaderCompiler 一份。
//
// 身份稳定：同一 context 登记同一个指针是幂等的；登记成别的指针一律拒绝（释放登记用 unbind）。
// Activate 会冻结该 context 的登记。否则"本局用的是哪个编译器"只能靠调用方自觉。
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include "direct/s2a_direct.h"

#define MAX_BOUND 4

struct bound_entry {
    void *context;
    s2a_direct_compile_fn compile;
    struct s2a_direct_live_compiler binary;
    int frozen;    // Activate 之后置位；当前策略冻结前后都不接受换指针
};

static struct bound_entry g_bound[MAX_BOUND];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct bound_entry *find_locked(void *context) {
    for (int i = 0; i < MAX_BOUND; i++)
        if (g_bound[i].context == context) return &g_bound[i];
    return NULL;
}

int32_t s2a_direct_bind_compiler_v1(void *context, s2a_direct_compile_fn compile) {
    if (!context || !compile) return S2A_DIRECT_RES_ARGUMENT;
    pthread_mutex_lock(&g_lock);
    struct bound_entry *entry = find_locked(context);
    if (entry) {
        if (entry->compile == compile) {          // 幂等：同一个指针重复登记放行
            pthread_mutex_unlock(&g_lock);
            return S2A_DIRECT_RES_OK;
        }
        pthread_mutex_unlock(&g_lock);
        return S2A_DIRECT_RES_IDENTITY;
    }
    for (int i = 0; i < MAX_BOUND; i++) {
        if (!g_bound[i].context) {
            g_bound[i].context = context;
            g_bound[i].compile = compile;
            g_bound[i].frozen = 0;
            pthread_mutex_unlock(&g_lock);
            return S2A_DIRECT_RES_OK;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return S2A_DIRECT_RES_INTERNAL;   // 表满：宁可报错，也不覆盖别的会话
}

int32_t s2a_direct_bind_binary_compiler_v1(void *context, s2a_direct_compile_fn compile,
    s2a_direct_encode_result_fn encode_legacy) {
    if (!context || !compile || !encode_legacy) return S2A_DIRECT_RES_ARGUMENT;
    pthread_mutex_lock(&g_lock);
    struct bound_entry *entry = find_locked(context);
    int32_t rc = S2A_DIRECT_RES_ARGUMENT;
    if (entry) {
        if (entry->binary.compile == compile && entry->binary.encode_legacy == encode_legacy)
            rc = S2A_DIRECT_RES_OK;
        else if (entry->frozen || entry->binary.compile)
            rc = S2A_DIRECT_RES_IDENTITY;
        else {
            entry->binary = (struct s2a_direct_live_compiler){compile, encode_legacy};
            rc = S2A_DIRECT_RES_OK;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

struct s2a_direct_live_compiler s2a_direct_bound_binary_compiler(void *context) {
    struct s2a_direct_live_compiler found = {0};
    if (!context) return found;
    pthread_mutex_lock(&g_lock);
    struct bound_entry *entry = find_locked(context);
    if (entry && entry->frozen) found = entry->binary;
    pthread_mutex_unlock(&g_lock);
    return found;
}

int32_t s2a_direct_freeze_compiler_v1(void *context) {
    if (!context) return S2A_DIRECT_RES_ARGUMENT;
    pthread_mutex_lock(&g_lock);
    struct bound_entry *entry = find_locked(context);
    if (!entry) {
        pthread_mutex_unlock(&g_lock);
        return S2A_DIRECT_RES_ARGUMENT;   // 没登记就不冻结：先 bind 再 Activate 是硬契约
    }
    entry->frozen = 1;
    pthread_mutex_unlock(&g_lock);
    return S2A_DIRECT_RES_OK;
}

int32_t s2a_direct_unbind_compiler_v1(void *context) {
    if (!context) return S2A_DIRECT_RES_ARGUMENT;
    pthread_mutex_lock(&g_lock);
    struct bound_entry *entry = find_locked(context);
    if (entry) {
        memset(entry, 0, sizeof *entry);
    }
    pthread_mutex_unlock(&g_lock);
    return S2A_DIRECT_RES_OK;
}

// 任意已登记的编译入口：运行时补编不持有 owner 的 ctx 时用它判断“统一生产链是否已就绪”。
// 它只证明"存在某个登记"，不能替代本局 s2a_direct_bound_compile(context) 的校验。
s2a_direct_compile_fn s2a_direct_bound_compile_any(void) {
    s2a_direct_compile_fn found = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_BOUND; i++)
        if (g_bound[i].context && g_bound[i].compile) { found = g_bound[i].compile; break; }
    pthread_mutex_unlock(&g_lock);
    return found;
}

s2a_direct_compile_fn s2a_direct_bound_compile(void *context) {
    if (!context) return NULL;
    s2a_direct_compile_fn found = NULL;
    pthread_mutex_lock(&g_lock);
    struct bound_entry *entry = find_locked(context);
    if (entry) found = entry->compile;
    pthread_mutex_unlock(&g_lock);
    return found;
}
