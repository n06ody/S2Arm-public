// Vulkan 驱动管线缓存（VkPipelineCache）的落盘/恢复文件层。
//
// 目的（方案 §8.5.11）：进程内 VkPipelineCache 只在本次会话有效，重开一局所有管线
// 都要重新让驱动编译。这里把驱动返回的 cache 数据原子写到应用私有目录，下次用
// initialData 恢复；是否命中仍由驱动按 pipeline create info（含 SPIR-V）判断。
//
// 隔离：文件名带 pipelineCacheUUID（设备+驱动），并带头里的格式版本；不同驱动/不同
// 缓存格式不会互读。损坏（magic/版本/长度/SHA-256 不符、短读、超上限）一律拒绝，
// 当作没有缓存，绝不把坏数据交给驱动。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVK_DRIVER_CACHE_MAGIC 0x53324150u   // "S2AP" little-endian
#define NVK_DRIVER_CACHE_VERSION 1u
#define NVK_DRIVER_CACHE_MAX (64u << 20)     // 64 MiB：超过就不落盘/不加载
#define NVK_DRIVER_CACHE_NAME 64

// <uuid 32 hex>-v<版本>.bin；uuid 为 VkPhysicalDeviceProperties.pipelineCacheUUID。
int nvk_driver_cache_name(const uint8_t uuid[16], char out[NVK_DRIVER_CACHE_NAME]);

struct nvk_driver_cache_stamp {
    size_t size;            // 0 = 没有已验证的磁盘版本
    uint8_t digest[32];
};

// read 同时返回已核验的磁盘身份。store 成功后更新身份，1 = 内容一样，0 = 已原子写入。
int nvk_driver_cache_read_stamp(const char *dir, const uint8_t uuid[16], void **data, size_t *size,
                                struct nvk_driver_cache_stamp *stamp, char *why, size_t why_size);
int nvk_driver_cache_store(const char *dir, const uint8_t uuid[16], const void *data, size_t size,
                           struct nvk_driver_cache_stamp *stamp, char *why, size_t why_size);

// 读：成功返回 0，*data = malloc 的负载（调用方 free），*size = 负载字节数。
// 文件不存在返回 1（不是错误）；损坏/超限返回负数并写 why。
int nvk_driver_cache_read(const char *dir, const uint8_t uuid[16], void **data, size_t *size,
                          char *why, size_t why_size);

// 写：先写同目录临时文件并 fsync，再 rename 原子替换；失败返回负数并写 why。
// size 为 0 或超过上限直接拒绝（空缓存没有恢复价值）。
int nvk_driver_cache_write(const char *dir, const uint8_t uuid[16], const void *data, size_t size,
                           char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
