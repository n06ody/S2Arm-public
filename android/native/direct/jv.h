// 跨度式 JSON 读取器（不分配内存、不具现字符串——只返回字段在原文里的跨度）。
//   原为 nvn_vk_program_result.c 内嵌实现，#1434 抽成共用件：_b.json 含长串字段
//   （pos_slice 表 700+ 字符 base64），s2a_json 逐字段解码短串上限吃不下。
// 只校验被扫到的语法边界；消费方按自己的 schema 判字段。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct s2a_jv { const char *p; size_t n; };

const char *s2a_jv_ws(const char *p, const char *end);
// 扫一个完整 JSON 值到 *out_end（不含尾随空白）；语法/边界错返回 -1。
int s2a_jv_scan(const char *p, const char *end, const char **out_end);
int s2a_jv_is_object(struct s2a_jv v);
int s2a_jv_is_array(struct s2a_jv v);
int s2a_jv_is_null(struct s2a_jv v);
int s2a_jv_is_string(struct s2a_jv v);
// 字符串值解码进 out（处理 \\ 转义）；不是字符串或放不下返回 -1。
int s2a_jv_str(struct s2a_jv v, char *out, size_t cap);
int s2a_jv_u32(struct s2a_jv v, uint32_t *out);
int s2a_jv_bool(struct s2a_jv v, int *out);
// 对象成员按键查找（重复键返回先到的）；没有或语法错返回 -1。
int s2a_jv_obj_find(struct s2a_jv obj, const char *key, struct s2a_jv *out);
// 数组元素个数（语法错 -1）与第 index 个元素的跨度。
int s2a_jv_arr_count(struct s2a_jv arr);
int s2a_jv_arr_item(struct s2a_jv arr, int index, struct s2a_jv *out);

#ifdef __cplusplus
}
#endif
