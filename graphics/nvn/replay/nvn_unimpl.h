// 「未接 / 不支持」的唯一出口：按（类别，名字）计数，首次出现打一行日志，收局出汇总表。
// 回放核心里没接的命令、后端返回的不支持，都走这里；普查工具读 nvn_unimpl_dump 的输出。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// cls：类别（"op" = 录制命令，"be" = 前端入口，"gpu" = 后端能力，"fmt" = 格式 …）；name：命令名 / 入口名 / 能力名；
// site：调用点（命令记录地址或返回地址），只进首次出现的那行日志。线程：任意。返回这一项累计次数。
uint64_t nvn_unimpl(const char *cls, const char *name, const void *site);
// 这一项累计次数（没出现过 0）。
uint64_t nvn_unimpl_count(const char *cls, const char *name);
// 汇总表：每行 "类别\t名字\t次数\n"，按首次出现的先后。返回写入的字节数（不含结尾 0）；装不下的行不写半行。
size_t nvn_unimpl_dump(char *buf, size_t cap);
// 出现过的项数。
int nvn_unimpl_kinds(void);
// 默认关闭的诊断快照：nvk_csd / nvk_csf 开启时零值也输出；退出回调不取账本锁。
void nvn_unimpl_diag_summary(const char *phase, uint64_t frame);
// 清零（会话收尾 / 单测）。
void nvn_unimpl_reset(void);

#ifdef __cplusplus
}
#endif
