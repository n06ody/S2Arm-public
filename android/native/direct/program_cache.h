// F5.1：编译结果的跨局私有缓存。
//
// 键是**规范化内容身份**（s2a_input_identity_encode 的 SHA-256），不是 requestId 也不是
// session/generation：后两者属传输身份，换一局就变（方案 §5.5.2 第 8 条）。拿它们当盘上键会让
// 缓存永远不命中——实测两局各编 54 次、19 个唯一 requestId 交集 19/19 就是这个后果。
//
// 目录由调用方给（应用私有）。本模块只在给定目录下建文件，绝不写安装 revision。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 本局缓存的私有目录：<state_dir>/direct-cache/nvn-vk。
// 与诊断目录分开（diagnostics 可清可弃，缓存要跨局复用）；按后端分目录，
// 内容/ABI/profile/闭包的隔离由身份本身承担（都在 input_identity 里）。
// 派生失败返回 -1；调用方就不导出 S2A_DIRECT_CACHE_DIR，缓存整体关闭而不是回落到 revision。
int s2a_program_cache_dir(const char *state_dir, char *out, size_t out_size,
                          char *why, size_t why_size);

// 命中返回 0：*bytes 由 malloc 给出（调用方 free），*len 是结果字节数。
// 未命中、损坏、身份或内容摘要不符一律返回 -1 并写 why（why 可为 NULL）。
// 损坏文件不在读取路径里删除：删除属于维护操作，读取只如实报告。
// result_sha 可为 NULL；非 NULL 时写入已校验的结果摘要（读时已经算过，复用方不必重算）。
int s2a_program_cache_load(const char *dir, const uint8_t identity[32],
                           uint8_t **bytes, size_t *len,
                           uint8_t result_sha[32], char *why, size_t why_size);

// 只读文件头（88 字节）：命中返回 0，给出头里记的结果摘要和负载长度；不读负载、也不重算摘要。
// 用途：调用方只想知道「这个身份的结果是不是某份已常驻的结果」，是的话不必把整份读进来再建一遍。
// 校验了：magic / 版本 / 头长 / 内部身份与查询身份一致 / 负载长度在界内 / 文件长度 = 头长 + 负载长度（截断的不认）。
// 没校验（不读负载就校验不了）：负载摘要——头里的结果摘要只是文件自己的声明（正式读取 s2a_program_cache_load 才重算比对）。
// 0 = 头对；1 = 没有这个身份的文件（ENOENT）；-1 = 参数不对 / 打不开 / 头不对。非 0 都写 why（可为 NULL）；
// 不计入 hits / misses / failures（那三个只数正式读取）。
int s2a_program_cache_probe(const char *dir, const uint8_t identity[32],
                            uint8_t result_sha[32], uint64_t *len, char *why, size_t why_size);

// 写入：同目录临时文件 → 文件 fsync → 原子 rename → 父目录 fsync；失败返回 -1。
// 文件同步失败不改名；目录同步失败时目标已可见，不回滚，不记为成功写入。
// 同步执行，供测试和收尾路径使用；热路径用 store_async。
int s2a_program_cache_store(const char *dir, const uint8_t identity[32],
                            const uint8_t *bytes, size_t len, char *why, size_t why_size);

// 方案 §5.5.2 第 5 条：当前绘制完成不等待磁盘写入。内部复制一份字节，起一次性分离线程写。
// 复制失败返回 -1；写盘失败只记 s2a_program_cache_failures()，不影响已经可用的内存结果。
int s2a_program_cache_store_async(const char *dir, const uint8_t identity[32],
                                  const uint8_t *bytes, size_t len);

// 等所有在途异步写结束；超时返回 -1（仍在途）。Stop 用它把写盘收干净。
int s2a_program_cache_drain(uint32_t timeout_ms);

uint64_t s2a_program_cache_hits(void);
uint64_t s2a_program_cache_misses(void);
uint64_t s2a_program_cache_stores(void);
uint64_t s2a_program_cache_failures(void);

#ifdef S2A_DIRECT_TEST_FAULTS
// 仅宿主回归：1/2 注入文件 EIO/EINVAL，3/4 注入目录 EIO/EINVAL；每次只消费一次。
void s2a_program_cache_test_fault(int fault);
unsigned s2a_program_cache_test_fault_hits(void);
#endif

#ifdef __cplusplus
}
#endif
