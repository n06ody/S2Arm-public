// 直连逐帧 TSV（诊断，默认关）：与 iOS host_adapt.m 的 frames.tsv 同名同义——
//   每次成功 present 记一行、列名照 iOS、拿不到的列整列不进表头（不写 0 冒充，
//   tools/frame_gate.py 缺列判「未测」）。
//   本模块出得起的列：frame wall_us draw_n pass_n xlate_n pso_n pso_us shader_miss
//   rb_us gpuq_us。disp_us（真上屏间隔要 display_timing 扩展，未合）/ gpu_us（归
//   另一任务）/ macro_us（无宏执行计时）/ upload_n（无主目标外切遍计数）/ rw_kb
//   （无附件带宽记账）/ alloc_b/gc*（iOS C# 钩子）拿不到——整列不出现。
// 开关：同 frame_log 双调试门（调试构建 + debug.s2a.debug_env=1）+ debug.s2a.frame_tsv=1；
//   宿主探针走同名环境变量。文件：S2A_FRAME_LOG_DIR/frame-tsv-<pid>.tsv（JNI 侧
//   configureFrameLogDir 设的私有目录；没设 = 开关打不开、记红行，不静默吞）。
// 记站点：mtl_present 成功点调 present()（与 frame_log 同一点）；提交路径 fence
//   等待点调 fence_wait()——先判 on() 再计时，关态只剩一次分支判断。
#pragma once
#include <stdint.h>

int nvn_vk_frame_tsv_on(void);
void nvn_vk_frame_tsv_present(uint64_t frame, double now_s);
void nvn_vk_frame_tsv_fence_wait(uint64_t wait_ns);
