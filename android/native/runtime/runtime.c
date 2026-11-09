#include "runtime.h"
#include "host_android.h"
#include "../dguard_android.h"
#include "../direct/graphics.h"
#include "../direct/json.h"
#include "../direct/selection.h"
#include "direct_session.h"
#include "../capprobe/capprobe.h"
#include "fault/fault.h"
#include "tracking/runtime.h"
#include "vm/memory.h"
#include "vm/writers.h"
#include "guest_main.h"
#include "guest_signal.h"
#include "s2a_native.h"
#include "s2a_gfx_mode.h"   // 冻结的图形模式（direct_session.c）：与选择器提交的 NVN 直连表对账
#include "direct/provider_vk.h"   // 弱符号：没有 provider 的构建里为 NULL
#include <pthread.h>
#include <stdio.h>
#include <string.h>

// 强引用：游戏目标必须链接真实共享 core 对象，诊断 s2a_native_stub.c 不能提供这些入口。
int s2a_window_ok(void);
void s2a_nro_table_load(const char *dir);
int s2a_selfcheck(void);
const char *s2a_selfcheck_hint(void);
void s2a_crash_install(void);
void s2a_term_install(void);
void s2a_det_init(void);
void load_modules(const char *, const char **, int);
void load_modules_from_source(const struct s2a_loader_source *, const char **, int);
void s2a_caps_apply(const char *);   // runtime/src/caps.c
unsigned s2a_unresolved_count(void);
uint64_t s2a_mem_init(const char *);
void *guest_sym(const char *);
#if defined(__ANDROID__)
// 同一 DSO 的私有 os.c 接缝，不扩展对外 native ABI。
__attribute__((visibility("hidden"))) int s2a_android_session_log_start(const char *, uint64_t);
#endif
// 可选：UI broker 只存在于 Android 外壳；宿主 runtime 单测没有它。调用是无条件的，
// 所以弱声明也必须在 Android 条件之外，否则 host 单测在 -Werror 下编不过。
__attribute__((weak)) int s2a_android_ui_bind_session(uint64_t);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct s2a_android_runtime_status g_status;
static const s2a_host_api_v1 *g_host;
static int g_used, g_services_ready, g_services_attempted;
static struct s2a_guest_main_args g_guest_args;

static int stop_requested(void) { return s2a_android_host_stop_requested(); }
static void phase(uint32_t state) {
    pthread_mutex_lock(&g_lock);
    g_status.state = state;
    if (state >= S2A_RUNTIME_LOADING && state <= S2A_RUNTIME_GUEST_RUNNING && state > g_status.last_phase)
        g_status.last_phase = state;
    pthread_mutex_unlock(&g_lock);
}
static int failed(int error, int services_error, const char *why) {
    pthread_mutex_lock(&g_lock);
    g_status.state = error == S2A_RUNTIME_STOP ? S2A_RUNTIME_STOP_REQUESTED : S2A_RUNTIME_FAILED;
    g_status.error = error;
    g_status.services_error = services_error;
    pthread_mutex_unlock(&g_lock);
    // 既有 HostApi 异步请求退出；此处不销毁仍可能使用中的 guest 内存、服务或 lease。
    g_host->stop->request_stop(g_host->host, S2A_HOST_STOP_ERROR, why);
    return error;
}
static int valid(const struct s2a_android_runtime_request *r, int allow_v2) {
    if (!r || r->services.size < sizeof r->services ||
        (r->services.version != S2A_ANDROID_SERVICES_ABI &&
         !(allow_v2 && r->services.version == S2A_ANDROID_SERVICES_ABI_V2)) ||
        !r->services.session_id || !r->services.native_owner || !r->services.lease_api ||
        !r->services.game_data_dir || r->services.game_data_dir[0] != '/' ||
        !r->services.state_dir || r->services.state_dir[0] != '/' ||
        r->services.console_mode > 1 || r->services.reserved || !r->program_id ||
        (!r->module_source && (!r->module_dir || r->module_dir[0] != '/')) || !r->module_names ||
        !r->module_count || r->module_count > 16) return 0;
    if ((r->module_source && !r->npdm_path) ||
        (r->npdm_path && (r->npdm_path[0] != '/' || strlen(r->npdm_path) >= 1100))) return 0;
    const s2a_host_api_v1 *h = r->services.host_api;
    // 只消费已经注册并校验过的同一 HostApi 表。
    if (!h || s2a_host_get() != h || !(h->capabilities & S2A_HOST_CAP_STOP) || !h->stop || !h->stop->request_stop) return 0;
    for (uint32_t i = 0; i < r->module_count; i++) {
        const char *n = r->module_names[i];
        if (!n || !*n || strlen(n) >= 16 || strchr(n, '/') || strchr(n, '\\') || !strcmp(n, ".") || !strcmp(n, "..")) return 0;
        // loader.c 随行表路径容量为 1024 字节；在选择产物或更改全局模块表之前拒绝截断。
        if (!r->module_source && strlen(r->module_dir) + strlen(n) + sizeof "/.patched.bin" > 1024) return 0;
        for (uint32_t j = 0; j < i; j++) if (!strcmp(n, r->module_names[j])) return 0;
    }
    return !strcmp(r->module_names[0], "main");
}
static void *enter_guest(void *args) {
    // 以实际 HostApi owner 的同锁准入排序，不能等 UI 消费 eventfd 后再转发停止。
    // 此点之后的停止属于已准入会话；不把一次 getter 检查冒充与进入 guest 原子排序。
    if (!s2a_android_host_try_enter_guest()) return NULL;
    phase(S2A_RUNTIME_GUEST_RUNNING);
    return s2a_guest_main(args);
}

// 未编入/未就绪的直连前端：direct 已 commit 时禁止继续跑 generic GPU。
int s2a_direct_frontend_ready(void) __attribute__((weak));

static int run_common(const struct s2a_android_runtime_request *r,
                      struct s2a_android_services_request_v2 *services_v2,
                      const struct s2a_graphics_descriptor_v1 *descriptor,
                      uint32_t mode_preference) {
    if (!valid(r, services_v2 != NULL)) return S2A_RUNTIME_ARGUMENT;
    pthread_mutex_lock(&g_lock);
    if (g_used) { pthread_mutex_unlock(&g_lock); return S2A_RUNTIME_ALREADY_USED; }
    g_used = 1;
    g_host = r->services.host_api;
    g_status.session_id = r->services.session_id;
    // UI broker 是进程单例：在 HLE 启动前把它绑定到本局真实 session，而不是固定 1。
    if (s2a_android_ui_bind_session) (void)s2a_android_ui_bind_session(r->services.session_id);
    g_status.state = g_status.last_phase = S2A_RUNTIME_LOADING;
    pthread_mutex_unlock(&g_lock);

#if defined(__ANDROID__)
    int log_error = s2a_android_session_log_start(r->services.state_dir, r->services.session_id);
    if (log_error) {
        char reason[128];
        snprintf(reason, sizeof reason, "Android session log unavailable (errno=%d)", log_error);
        return failed(S2A_RUNTIME_LOG, 0, reason);
    }
#endif
    if (!s2a_window_ok()) return failed(S2A_RUNTIME_WINDOW, 0, "native guest window unavailable");
    // selfcheck 核对仍未裁剪的真实64GiB窗口和已注册HostApi内存事实；必须在loader/NPDM之前。
    if (!s2a_selfcheck()) return failed(S2A_RUNTIME_SELFCHECK, 0, s2a_selfcheck_hint());
    if (s2a_android_runtime_signals_prepare()) return failed(S2A_RUNTIME_SIGNALS, 0, "NativeAOT signal preparation failed");
    s2a_crash_install();
    s2a_term_install();
    s2a_det_init();
    if (stop_requested()) return failed(S2A_RUNTIME_STOP, 0, "stop before module loading");
    // #1456 GS+遮挡查询运行时探测：与管线预编译同一处、在任何 Vulkan 设备创建前执行。
    // 每臂一个 posix_spawn 起的独立探测进程（该组合可能 DEVICE_LOST；不 fork——ART
    // 多线程进程 fork 出的子进程只剩调用线程，会继承其它线程持有的 malloc/loader
    // 锁而死锁）。整次探测有总时限（Java CapProbe.TOTAL_BUDGET_MS），结果写
    // files/capprobe/<指纹哈希>.json 并发布 S2A_CAPPROBE_RESULT；探测没写完不放行
    // 后面的图形选择/会话准备，UI 照常显示启动进度（本线程阻塞即启动仍在进行）。
    // 未配置或探测失败时返回非零/无结果文件，能力层读到的是"不支持"，方向安全。
    (void)s2a_capprobe_ensure();
    // 本局选择器是否真的提交了 NVN 直连表（frozen->mode == 1）。vk-direct 实验开关是另一套
    // 东西：它只接管 Vulkan 客体的 vk* 导入（direct_session.c），不提供 NVN 直连表。
    int frozen_direct = 0;
    // C1：唯一一次模式选择，必须早于任何模块重定位。v1 调用不进入这里（旧行为保持 generic）。
    if (services_v2) {
        // 选择事务唯一的工作目录来源：锁写失败时要能从日志看到到底是哪个目录。
        fprintf(stderr, "[s2a] graphics selection state dir: %s\n", r->services.state_dir);
        // provider 需要"revision 根 / 状态目录 / 窗口 lease / 刷新率"才能把引擎接上。
        // 刷新率外壳还没带进来（引擎按自身默认处理）；这就是现在 direct present 还不能用的原因之一。
        if (s2a_direct_provider_vk_configure) {
            struct s2a_direct_vk_context context;
            memset(&context, 0, sizeof context);
            // provider 按 <game_data_dir>/shaders/vulkan/direct 找离线库：这是已激活 revision 的根，
            // 不是 module_dir（module_dir 是 exefs 那一层，拼出来的路径不存在）。
            context.game_data_dir = r->services.game_data_dir;
            context.state_dir = r->services.state_dir;
            context.lease_api = r->services.lease_api;
            context.display_hz = 0;
            s2a_direct_provider_vk_configure(&context);
        }
        struct s2a_direct_selection selection;
        // selection 的原因里会带 identity 错误的双方 closure（见 profile_compose.c），160 字节不够。
        char why[560] = "";
        int32_t selected = s2a_direct_select(descriptor, mode_preference, r->services.session_id,
            r->services.state_dir, s2a_direct_provider_default(), &selection, why, sizeof why);
        if (selected < 0) return failed(S2A_RUNTIME_DIRECT, selected,
            why[0] ? why : "direct admission failed before relocation");
        if (s2a_direct_selection_publish(&selection)) return failed(S2A_RUNTIME_DIRECT, -9,
            "graphics selection already published");
        // 只能引用已发布的进程级 selection：栈上的局部在块结束后会被后续局部覆盖，
        // 而 services v2 / 编译器会一直持有 table 与 context。
        const struct s2a_direct_selection *frozen = s2a_direct_selection_current();
        if (!frozen) return failed(S2A_RUNTIME_DIRECT, -9, "graphics selection unavailable after publish");
        fprintf(stderr, "[s2a] graphics selection: %s (%s), devices=%u queues=%u workers=%u consumers=%u\n",
            frozen->mode == 1 ? "direct" : "generic", frozen->reason,
            frozen->counts.devices, frozen->counts.queues, frozen->counts.workers,
            frozen->counts.consumers);
        if (frozen->mode == 1) {
            if (!s2a_direct_frontend_ready || !s2a_direct_frontend_ready())
                return failed(S2A_RUNTIME_DIRECT, S2A_DIRECT_RES_UNSUPPORTED,
                    "direct committed without an NVN frontend");
            services_v2->graphics = &frozen->table;
            services_v2->graphics_mode = 1;
            frozen_direct = 1;
            // 直连安全网（共用 dguard.c，接口 dguard_android.h）：NVN 直连已提交、客体代码
            // 还没跑——武装点与 iOS host_main 的 begin/arm 时机一致。满限 + 无人值守收局，
            // 有人局提示后照常武装继续；debug.s2a.dguard=0 整局不武装（launch 里判）。
            int dg = s2a_android_dguard_launch(r->program_id, r->services.game_data_dir, frozen->table.version);
            if (dg) return failed(S2A_RUNTIME_DIRECT, dg,
                "direct safety guard stopped unattended launch (consecutive failures at limit)");
        } else {
            services_v2->graphics = NULL;
            services_v2->graphics_mode = 0;
        }
    }
    int graphics = s2a_android_vk_session_prepare(r->services.lease_api, r->services.game_data_dir, r->services.state_dir);
    if (graphics) {
        char detail[96];
        snprintf(detail, sizeof detail, "graphics cold-start preparation failed (errno=%d)", graphics);
        return failed(S2A_RUNTIME_SERVICES, graphics, detail);
    }
    const char *names[16];
    uint32_t count = 0;
    for (uint32_t i = 0; i < r->module_count; i++) {
        if (s2a_android_vk_skip_module(r->module_names[i])) {
            fprintf(stderr, "[vk-direct] 不装载客体Vulkan驱动模块：%s\n", r->module_names[i]);
        } else names[count++] = r->module_names[i];
    }
    // 包内能力清单（产物根 s2a_caps.tsv，tools/make_pack.py 放进游戏数据根，iOS / Mac 同一份）：装载前读，只在环境变量未设时写 S2A_WIDEN_CAPS / S2A_FPS60
    s2a_caps_apply(r->services.game_data_dir);
    if (r->module_source) {
        s2a_nro_table_load(r->services.game_data_dir);
        load_modules_from_source(r->module_source, names, (int)count);
    }
    else load_modules(r->module_dir, names, (int)count);
    if (s2a_unresolved_count()) return failed(S2A_RUNTIME_IMPORT, 0, "unresolved guest imports");
    if (stop_requested()) return failed(S2A_RUNTIME_STOP, 0, "stop after module loading");
    // vk-direct 实验开关（direct_session.c）冻结后，托管侧按 s2a_gfx_mode()==1 关掉通用 GPU、
    // 装一个成员一律抛错的 direct 渲染器；这只有 Vulkan 客体成立（客体自己的 vk* 导入由直连
    // Vulkan 前端应答）。同一个组合遇上 NVN 客体时，既没有 NVN 直连表（选择器是 generic），
    // 也没有通用 GPU，画面永远黑且没有红日志。重定位已经给出客体事实（是否问过
    // nvnBootstrapLoader），在这里按事实拒绝，并指出真正的 NVN 直连开关。
    if (services_v2 && !frozen_direct && s2a_gfx_mode() == 1 && s2a_android_vk_nvn_guest()) {
        const struct s2a_direct_selection *frozen = s2a_direct_selection_current();
        char detail[512];   // 文案 + 选择器原因：gcc -Werror=format-truncation 要求装得下最坏情况
        snprintf(detail, sizeof detail,
            "vk-direct experiment frozen but this guest imports nvnBootstrapLoader: NVN direct needs "
            "shaders/vulkan/direct/graphics.v1.json in the package and s2arm.graphics_mode=direct; "
            "the vk-direct switch only serves Vulkan guests (selection: %.160s)",
            frozen && frozen->reason[0] ? frozen->reason : "unrecorded");
        return failed(S2A_RUNTIME_DIRECT, S2A_DIRECT_RES_UNSUPPORTED, detail);
    }
    char npdm[1100];
    int n = r->npdm_path ? snprintf(npdm, sizeof npdm, "%s", r->npdm_path) :
        snprintf(npdm, sizeof npdm, "%s/main.npdm", r->module_dir);
    if (n < 0 || (size_t)n >= sizeof npdm) return failed(S2A_RUNTIME_ARGUMENT, 0, "module path too long");
    uint64_t stack = s2a_mem_init(npdm);
    if (!stack || (stack & 15) || s2a_program_id() != r->program_id)
        return failed(S2A_RUNTIME_MEMORY, 0, "native layout or program identity mismatch");
    phase(S2A_RUNTIME_MEMORY_READY);
    // prepared 只证资源、真实dispatcher安装和VM写入通道，最终同线程信号探针仍在SDK入口执行。
    if (s2a_android_guest_writers_init() || s2a_track_runtime_prepare() || s2a_android_tracking_vm_prepare())
        return failed(S2A_RUNTIME_TRACKING, 0, "native tracking preparation failed");
    // 在执行任何 SDK 指令前解析全部必需入口。DT_INIT 仍由共享 loader 按逆依赖顺序遍历。
    static const char *const sdk[] = {"_init_libc0", "nnosInitialize", "_init_libc1",
        "nninitInitializeSdkModule", "nninitStartup", "_init_libc2", "nnMain"};
    for (size_t i = 0; i < sizeof sdk / sizeof sdk[0]; i++)
        if (!guest_sym(sdk[i])) return failed(S2A_RUNTIME_SDK, 0, "required SDK entry missing");
    if (stop_requested()) return failed(S2A_RUNTIME_STOP, 0, "stop before services");
    struct s2a_android_services_status services = {sizeof services, S2A_ANDROID_SERVICES_ABI, r->services.session_id, 0, 0};
    // 调用 Start 前就置 attempted（main 的时机）：一旦调用过，收局就要核验 managed 清理结果（含部分成功的 Start）。
    pthread_mutex_lock(&g_lock);
    g_services_attempted = 1;
    pthread_mutex_unlock(&g_lock);
    int rc = services_v2 ? s2a_android_services_start_v2(services_v2, &services)
                         : s2a_android_services_start(&r->services, &services);
    fprintf(stderr, "[s2a] Android services start: v2=%d rc=%d state=%u error=%d\n",
            services_v2 ? 1 : 0, rc, services.state, services.error);
    if (rc || services.size < sizeof services || services.version != S2A_ANDROID_SERVICES_ABI ||
        services.session_id != r->services.session_id || services.state != S2A_SERVICES_READY || services.error)
        return failed(S2A_RUNTIME_SERVICES, rc ? rc : services.error, "services/GPU did not become ready");
    pthread_mutex_lock(&g_lock);
    g_services_ready = 1;
    pthread_mutex_unlock(&g_lock);
    phase(S2A_RUNTIME_SERVICES_READY);
    if (stop_requested()) {
        s2a_android_services_request_stop(r->services.session_id, &services);
        return failed(S2A_RUNTIME_STOP, 0, "stop before guest entry");
    }
    // 参数保活到进程退出：即使 join 失败，活跃 guest 也不能引用已返回的 worker 栈。保留 SDK 默认 argv。
    g_guest_args = (struct s2a_guest_main_args){stack, 0, NULL};
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return failed(S2A_RUNTIME_THREAD, 0, "guest thread attributes unavailable");
    rc = pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t guest;
    if (!rc) rc = pthread_create(&guest, &attr, enter_guest, &g_guest_args);
    pthread_attr_destroy(&attr);
    if (rc) return failed(S2A_RUNTIME_THREAD, 0, "guest thread creation failed");
    rc = pthread_join(guest, NULL);
    // 共享入口在 nnMain 返回后调用真实 native shutdown。单条 pthread 返回或 join 失败都不证明其它 guest 线程已退出。
    int outcome = rc ? S2A_RUNTIME_THREAD : stop_requested() ? S2A_RUNTIME_STOP : S2A_RUNTIME_RETURNED;
    // 在 shutdown 日志收尾**之前**清 attempt 锁：正常返回 / 用户停止算正常结束；
    // join 失败等异常保留锁，下次 AUTO 自动回 generic。
    s2a_direct_finish(outcome == S2A_RUNTIME_RETURNED || outcome == S2A_RUNTIME_STOP);
    return failed(outcome, 0, "guest thread returned; process teardown required");
}

int s2a_android_runtime_run(const struct s2a_android_runtime_request *r) {
    return run_common(r, NULL, NULL, S2A_DIRECT_MODE_AUTO);
}

int s2a_android_runtime_run_v2(const struct s2a_android_runtime_request_v2 *r) {
    if (!r || r->size < sizeof *r || r->version != S2A_ANDROID_RUNTIME_ABI_V2 ||
        r->reserved || r->reserved0 || r->mode_preference > S2A_DIRECT_MODE_DIRECT)
        return S2A_RUNTIME_ARGUMENT;
    // runtime 输入不得夹带已构造的 graphics table，也不得自行声明 direct mode：
    // 这两项只能由 native 在 Prepare/commit 后填写。
    if (r->services.base.size < S2A_ANDROID_SERVICES_REQUEST_V2_SIZE ||
        r->services.base.version != S2A_ANDROID_SERVICES_ABI_V2 ||
        r->services.graphics || r->services.graphics_mode || r->services.flags)
        return S2A_RUNTIME_ARGUMENT;
    if (r->graphics.bytes > S2A_JSON_MAX_BYTES || (r->graphics.bytes && !r->graphics.json))
        return S2A_RUNTIME_ARGUMENT;
    struct s2a_android_runtime_request v1;
    memset(&v1, 0, sizeof v1);
    v1.services = r->services.base;
    v1.module_dir = r->module_dir;
    v1.module_names = r->module_names;
    v1.module_count = r->module_count;
    v1.program_id = r->program_id;
    v1.module_source = r->module_source;
    v1.npdm_path = r->npdm_path;
    struct s2a_graphics_descriptor_v1 descriptor;
    const struct s2a_graphics_descriptor_v1 *parsed = NULL;
    char why[160] = "";
    if (r->graphics.bytes) {
        int parse = s2a_graphics_descriptor_parse(r->graphics.json, (size_t)r->graphics.bytes,
                                                  &descriptor, why, sizeof why);
        char expected[17];
        snprintf(expected, sizeof expected, "%016llX", (unsigned long long)r->program_id);
        if (!parse && strcmp(descriptor.title_id, expected)) {
            parse = S2A_GRAPHICS_IDENTITY;
            snprintf(why, sizeof why, "graphics titleId %s != program %s", descriptor.title_id, expected);
        }
        if (!parse) {
            // 描述文件是导入游戏时按当时引擎写的；换引擎后 translatorClosure 过期，现场编译会全部 IDENTITY
            // 拒绝（11-03 Pura X 标题黑屏；11-04 Mate90 新材质的绘制被跳过，贴图和拾取物闪烁）。
            // closure 只用于现场编译与私有缓存分目录，改用本引擎编译器自己的身份。旧 closure 的离线库
            // 是旧编译器的产物，本来就不能用，同时丢弃。
            extern int32_t s2a_direct_compiler_closure_v1(char *out, int32_t capacity) __attribute__((weak));
            char own[sizeof descriptor.translator_closure];
            if (s2a_direct_compiler_closure_v1 && s2a_direct_compiler_closure_v1(own, (int32_t)sizeof own) > 0 &&
                strcmp(own, descriptor.translator_closure)) {
                fprintf(stderr, "[s2a] 【红】graphics descriptor closure %s is not this engine's compiler (%s); "
                        "using the engine's identity and ignoring its offline library (descriptor predates the installed engine)\n",
                        descriptor.translator_closure, own);
                snprintf(descriptor.translator_closure, sizeof descriptor.translator_closure, "%s", own);
                descriptor.profile_count = 0;
            }
        }
        if (!parse) parsed = &descriptor;
        else if (r->mode_preference == S2A_DIRECT_MODE_DIRECT)
            return S2A_RUNTIME_DIRECT;   // 手动 direct 的坏描述/错标题不落到 generic
        else
            fprintf(stderr, "[s2a] graphics descriptor rejected (%d: %s); AUTO selects generic\n", parse, why);
    }
    struct s2a_android_services_request_v2 services;
    memset(&services, 0, sizeof services);
    services.base = v1.services;
    return run_common(&v1, &services, parsed, r->mode_preference);
}

int s2a_android_runtime_status(uint64_t session, struct s2a_android_runtime_status *out) {
    if (!out) return S2A_RUNTIME_ARGUMENT;
    pthread_mutex_lock(&g_lock);
    int ok = g_used && session == g_status.session_id;
    if (ok) *out = g_status;
    pthread_mutex_unlock(&g_lock);
    // 状态读取不嵌套运行时锁与 HostApi 锁。Failed 保留根因，其余状态反映已接受的停止。
    if (ok && out->state != S2A_RUNTIME_FAILED && stop_requested()) out->state = S2A_RUNTIME_STOP_REQUESTED;
    return ok ? 0 : S2A_RUNTIME_ARGUMENT;
}
int s2a_android_runtime_request_stop(uint64_t session) {
    pthread_mutex_lock(&g_lock);
    if (!g_used || session != g_status.session_id) { pthread_mutex_unlock(&g_lock); return S2A_RUNTIME_ARGUMENT; }
    int ready = g_services_ready;
    pthread_mutex_unlock(&g_lock);
    // 与 services fatal 使用同一原生停止 owner；此时不持 g_lock，且 HostApi 不回调 C#。
    g_host->stop->request_stop(g_host->host, S2A_HOST_STOP_USER, "runtime stop requested");
    if (ready) {
        struct s2a_android_services_status status = {sizeof status, S2A_ANDROID_SERVICES_ABI, session, 0, 0};
        return s2a_android_services_request_stop(session, &status);
    }
    return 0;
}

// 供同一进程的每条guest pthread使用唯一会话身份，不建立第二套服务状态。
uint64_t s2a_android_runtime_session(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t session = g_used ? g_status.session_id : 0;
    pthread_mutex_unlock(&g_lock);
    return session;
}
// 强引用真实NativeAOT reverse-PInvoke，不能用Host.Init发生在另一线程代替附着。
int s2a_android_runtime_enter_guest_thread(void) {
    uint64_t session = s2a_android_runtime_session();
    if (!session) return S2A_RUNTIME_ARGUMENT;
    if (stop_requested()) return failed(S2A_RUNTIME_STOP, 0, "stop before managed thread attach");
    if (!s2a_android_fault_thread_ready()) return failed(S2A_RUNTIME_SIGNALS, 0, "guest thread fault/xstack not ready");
    struct s2a_android_services_status status = {sizeof status, S2A_ANDROID_SERVICES_ABI, session, 0, 0};
    int rc = s2a_android_services_attach_thread(session, &status);
    if (rc || status.size < sizeof status || status.version != S2A_ANDROID_SERVICES_ABI ||
        status.session_id != session || status.state != S2A_SERVICES_READY || status.error)
        return failed(S2A_RUNTIME_SERVICES, rc ? rc : status.error, "managed guest thread attach failed");
    if (stop_requested()) return failed(S2A_RUNTIME_STOP, 0, "stop after managed thread attach");
    if (s2a_android_fault_probe()) return failed(S2A_RUNTIME_SIGNALS, 0, "guest thread fault self-probe failed");
    if (!s2a_track_runtime_ready()) return failed(S2A_RUNTIME_TRACKING, 0, "guest thread final tracking admission failed");
    if (stop_requested()) return failed(S2A_RUNTIME_STOP, 0, "stop after guest thread self-probe");
    return 0;
}

// 不把void CsApi返回或StopRequested当成Cleanup完成；真实服务owner必须发布Stopped。
int s2a_android_runtime_shutdown_result(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t session = g_status.session_id;
    int attempted = g_services_attempted;
    pthread_mutex_unlock(&g_lock);
    if (!attempted) {
        fprintf(stderr, "[s2a] Android services尚未调用Start，无managed cleanup待验\n");
        return 0;
    }
    struct s2a_android_services_status status = {sizeof status, S2A_ANDROID_SERVICES_ABI, session, 0, 0};
    int rc = s2a_android_services_status(session, &status);
    if (rc || status.size < sizeof status || status.version != S2A_ANDROID_SERVICES_ABI ||
        status.session_id != session || status.state != S2A_SERVICES_STOPPED || status.error) {
        fprintf(stderr, "[s2a] 【红】Android services清理未完成：session=%llu got=%llu state=%u error=%d query=%d\n",
            (unsigned long long)session, (unsigned long long)status.session_id, status.state, status.error, rc);
        return S2A_RUNTIME_SERVICES;
    }
    return 0;
}
