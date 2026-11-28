// W2.6 宿主回归：managed 编译 provider 的登记身份。
//
// 判据：同一 context 只能对应一个编译指针；Activate 冻结后不能换；会话销毁必须解绑，
// 否则表项（4 项）泄漏到下一次会话、后续会话无处登记。反例基线：把"换个指针"当成功，
// 本局实际用的编译器就会在无人察觉时被换掉，编译事实与请求身份随之不再对应。
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "direct/s2a_direct.h"

static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static int32_t compile_a(uint64_t session, uint64_t generation, s2a_direct_bytes request,
                         s2a_direct_emit_fn emit, void *sink) {
    (void)session; (void)generation; (void)request; (void)emit; (void)sink;
    return S2A_DIRECT_RES_OK;
}

static int32_t compile_b(uint64_t session, uint64_t generation, s2a_direct_bytes request,
                         s2a_direct_emit_fn emit, void *sink) {
    (void)session; (void)generation; (void)request; (void)emit; (void)sink;
    return S2A_DIRECT_RES_COMPILE;
}

static int32_t encode_legacy(s2a_direct_bytes result, s2a_direct_emit_fn emit, void *sink) {
    (void)result; (void)emit; (void)sink;
    return S2A_DIRECT_RES_OK;
}

int main(void) {
    void *ctx = (void *)(uintptr_t)0x100;
    void *other = (void *)(uintptr_t)0x200;

    CHECK(s2a_direct_bound_compile(ctx) == NULL, "unbound context has no compiler");
    CHECK(s2a_direct_bound_compile_any() == NULL, "no binding yet");

    CHECK(s2a_direct_bind_compiler_v1(ctx, compile_a) == S2A_DIRECT_RES_OK, "first bind");
    CHECK(s2a_direct_bind_compiler_v1(ctx, compile_a) == S2A_DIRECT_RES_OK, "same pointer is idempotent");
    CHECK(s2a_direct_bound_compile(ctx) == compile_a, "bound compile is readable");

    // 换指针：拒绝，且原来的登记不受影响。
    CHECK(s2a_direct_bind_compiler_v1(ctx, compile_b) == S2A_DIRECT_RES_IDENTITY,
          "rebinding a different pointer is rejected");
    CHECK(s2a_direct_bound_compile(ctx) == compile_a, "rejected rebind left the original compiler");

    // 未登记就冻结：参数错误，不能把表项凭空造出来。
    CHECK(s2a_direct_freeze_compiler_v1(other) == S2A_DIRECT_RES_ARGUMENT, "freeze without bind is rejected");

    CHECK(s2a_direct_freeze_compiler_v1(ctx) == S2A_DIRECT_RES_OK, "freeze after activation");
    CHECK(s2a_direct_bind_compiler_v1(ctx, compile_a) == S2A_DIRECT_RES_OK, "same pointer after freeze is still idempotent");
    CHECK(s2a_direct_bind_compiler_v1(ctx, compile_b) == S2A_DIRECT_RES_IDENTITY, "rebind after freeze is rejected");
    CHECK(s2a_direct_bound_compile(ctx) == compile_a, "freeze keeps the original compiler");

    // 另一个 context 不受影响：绑定/读取互不串联。
    CHECK(s2a_direct_bind_compiler_v1(other, compile_b) == S2A_DIRECT_RES_OK, "second context binds independently");
    CHECK(s2a_direct_bound_compile(other) == compile_b, "second context reads its own compiler");
    CHECK(s2a_direct_bound_compile_any() == compile_a, "bound_compile_any returns the first registration only");
    CHECK(s2a_direct_bind_binary_compiler_v1(other, compile_b, encode_legacy) == S2A_DIRECT_RES_OK,
          "binary provider registers after Prepare");
    CHECK(!s2a_direct_bound_binary_compiler(other).compile, "unfrozen binary provider stays hidden");
    CHECK(s2a_direct_freeze_compiler_v1(other) == S2A_DIRECT_RES_OK, "Activate freezes the second context");
    struct s2a_direct_live_compiler binary = s2a_direct_bound_binary_compiler(other);
    CHECK(binary.compile == compile_b && binary.encode_legacy == encode_legacy,
          "active context reads both binary callbacks despite a prior JSON registration");
    CHECK(!s2a_direct_bound_binary_compiler(ctx).compile, "JSON-only context keeps its own transport");

    // 解绑（会话销毁路径）：读不到、可以重新登记，表项不泄漏。
    CHECK(s2a_direct_unbind_compiler_v1(ctx) == S2A_DIRECT_RES_OK, "unbind");
    CHECK(s2a_direct_bound_compile(ctx) == NULL, "unbound context is empty again");
    CHECK(s2a_direct_unbind_compiler_v1(ctx) == S2A_DIRECT_RES_OK, "unbind is idempotent");
    CHECK(s2a_direct_bind_compiler_v1(ctx, compile_b) == S2A_DIRECT_RES_OK, "context can be rebound after unbind");

    // 表满：不覆盖别人的登记，明确报错；解绑后可以再用。此时已占 2 项（ctx、other），
    // 表共 4 项，所以再填 2 项就满。
    for (uintptr_t i = 0x300; i <= 0x301; i++)
        CHECK(s2a_direct_bind_compiler_v1((void *)i, compile_a) == S2A_DIRECT_RES_OK, "fill table");
    CHECK(s2a_direct_bind_compiler_v1((void *)(uintptr_t)0x400, compile_a) == S2A_DIRECT_RES_INTERNAL,
          "a full table refuses instead of overwriting another session");
    CHECK(s2a_direct_unbind_compiler_v1(other) == S2A_DIRECT_RES_OK, "free one slot");
    CHECK(!s2a_direct_bound_binary_compiler(other).compile, "unbinding clears the binary callbacks");
    CHECK(s2a_direct_bind_compiler_v1((void *)(uintptr_t)0x400, compile_a) == S2A_DIRECT_RES_OK,
          "the freed slot is reusable");

    if (fails) { printf("direct compiler bind test: %d FAILED\n", fails); return 1; }
    printf("direct compiler bind test: ok\n");
    return 0;
}
