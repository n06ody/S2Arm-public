// 直连逐帧 TSV（宿主，确定性）：表头只收源存在的列（拿不到整列不出现、不许 0 冒充）、
//   每 present 行的计数源差分、gpuq_us 由本模块 fence 账供数、开关关/无目录不出文件。
// unity 引入实现文件：白盒测静态（g_on / g_f / 列掩码），probe 走 S2A_TSV_TEST 替身缝。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvn/nvn_vk_frame_tsv.c"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- 脚本化快照源：替身把 g_script 交给模块 probe ----
static struct s2a_tsv_snap g_script;
void s2a_tsv_test_probe(struct s2a_tsv_snap *s) { memcpy(s, &g_script, sizeof *s); }

static char g_dir[512];
static char g_path[640];
static void reset_all(void) {
    if (g_f) { fclose(g_f); g_f = NULL; }
    g_on = -1; g_cols = 0; memset(&g_prev, 0, sizeof g_prev);
    g_prev_s = 0; g_rows = 0;
    atomic_store(&g_gpuq_ns, 0);
    memset(&g_script, 0, sizeof g_script);
    unlink(g_path);
}
static int read_file(char *buf, size_t cap) {
    if (g_f) fflush(g_f);
    FILE *f = fopen(g_path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0; fclose(f); return (int)n;
}
static struct s2a_tsv_snap all_sources(void) {
    struct s2a_tsv_snap s = {0};
    s.mask = S2A_TSV_M_DRAWPASS | S2A_TSV_M_PROG | S2A_TSV_M_OFFLINE | S2A_TSV_M_RB;
    return s;
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/s2a-tsv-test-%ld", (long)getpid());
    mkdir(g_dir, 0777);
    snprintf(g_path, sizeof g_path, "%s/frame-tsv-%ld.tsv", g_dir, (long)getpid());
    setenv("S2A_FRAME_LOG_DIR", g_dir, 1);

    // 场景 1：开关关 → present 不产生文件（默认关，量的成本不默认付）。
    reset_all();
    unsetenv("debug.s2a.frame_tsv");
    nvn_vk_frame_tsv_present(1, 1.0);
    CHECK(access(g_path, F_OK) != 0);

    // 场景 2：开关开 → 表头恰含可接列；逐行差分与源快照一致；首行 wall_us=0。
    reset_all();
    setenv("debug.s2a.frame_tsv", "1", 1);
    g_script = all_sources();
    // 基线快照（打开时取一次，首行差分应为 0——累计起点不计入第 1 帧）。
    g_script.draws = 100; g_script.passes = 40; g_script.compiles = 3;
    g_script.pipes = 7; g_script.pipe_ns = 9e6; g_script.offline_miss = 2; g_script.rb_ns = 4e6;
    nvn_vk_frame_tsv_present(1, 10.0);
    g_script.draws = 105; g_script.passes = 42; g_script.compiles = 4;
    g_script.pipes = 9; g_script.pipe_ns = 14e6; g_script.offline_miss = 3; g_script.rb_ns = 9e6;
    nvn_vk_frame_tsv_present(2, 10.020);   // 20 ms 间隔
    g_script.draws = 107; g_script.passes = 42; g_script.offline_miss = 3;
    nvn_vk_frame_tsv_fence_wait(12000);    // 提交路径一次 12 us 等待 → 下行 gpuq_us=12
    nvn_vk_frame_tsv_present(3, 10.060);   // 40 ms 间隔
    char buf[4096];
    CHECK(read_file(buf, sizeof buf) > 0);
    char *l1 = strtok(buf, "\n");
    char *l2 = strtok(NULL, "\n");
    char *l3 = strtok(NULL, "\n");
    char *l4 = strtok(NULL, "\n");
    CHECK(l1 && !strcmp(l1, "frame\twall_us\tdraw_n\tpass_n\txlate_n\tpso_n\tpso_us\tshader_miss\trb_us\tgpuq_us"));
    CHECK(l2 && !strcmp(l2, "1\t0\t0\t0\t0\t0\t0\t0\t0\t0"));
    CHECK(l3 && !strcmp(l3, "2\t20000\t5\t2\t1\t2\t5000\t1\t5000\t0"));
    CHECK(l4 && !strcmp(l4, "3\t40000\t2\t0\t0\t0\t0\t0\t0\t12"));

    // 场景 3：源缺失 → 对应列整列不进表头（iOS 口径：拿不到不出现，不写 0 占位）。
    reset_all();
    g_script = all_sources();
    g_script.mask &= ~S2A_TSV_M_DRAWPASS;          // first_draw 源不在
    g_script.mask &= ~S2A_TSV_M_OFFLINE;           // 离线统计源不在
    nvn_vk_frame_tsv_present(1, 1.0);
    g_script.compiles = 1; g_script.pipes = 1; g_script.pipe_ns = 2e6; g_script.rb_ns = 1e6;
    nvn_vk_frame_tsv_present(2, 1.016);
    CHECK(read_file(buf, sizeof buf) > 0);
    l1 = strtok(buf, "\n"); l2 = strtok(NULL, "\n"); l3 = strtok(NULL, "\n");
    CHECK(l1 && !strcmp(l1, "frame\twall_us\txlate_n\tpso_n\tpso_us\trb_us\tgpuq_us"));
    CHECK(l2 && !strcmp(l2, "1\t0\t0\t0\t0\t0\t0"));
    CHECK(l3 && !strcmp(l3, "2\t16000\t1\t1\t2000\t1000\t0"));

    // 场景 4：表头绝不能出现拿不到的列（负控：disp/gpu/macro/upload/rw/alloc/gc 一律不接）。
    CHECK(!strstr(buf, "disp_us") && !strstr(buf, "gpu_us") && !strstr(buf, "macro_us") &&
          !strstr(buf, "upload_n") && !strstr(buf, "rw_kb") && !strstr(buf, "alloc_b") &&
          !strstr(buf, "gc_pause"));

    // 场景 5：目录缺失 → 开关打不开但不崩（记红行，不静默吞）。
    reset_all();
    unsetenv("S2A_FRAME_LOG_DIR");
    nvn_vk_frame_tsv_present(1, 1.0);
    CHECK(g_f == NULL);

    printf("nvn_vk_frame_tsv_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
