# 单帧 ROI 的跨帧 3D 写入历史

在调试包启动前同时启用既有单帧 ROI 探针与 `debug.s2a.nvk_roi_history=1`，并选定目标帧和片元。每次会话生成一份应用私有文件，宿主日志的 `nvk history:` 行给出完整路径。保留同一进程的文件、目标帧 ROI 和提交完成行。

1. 从 `history_consume` 取该帧实际 3D 视图的 image、segment、mip 与 seq。用 `texture` 记录关联 guest serial、后端存储代际、池与区间。跨会话需要重新定位。
2. `upload_source` 报告上传原因与原始源快照的首个非零字节位置和值。valid_lo、valid_hi 是相对该纹理源的有效窗口；padding 也计入原始快照。完整源超出 16 MiB 或快照取不到时，source_all_zero=-1。实际上传暂存数据另有 `volume_history`。显式初始化填零的原因见 `initialize`。
3. 以 request 串起 CS 请求、候选客体 serial、回放结果、后端录制/拒绝和实际写描述符。零组和失败不算写。模块、组数、绑定与区域描述一次录制；是否执行到着色器内某一坐标还要用纹素验证。
4. 用 segment 与 `history_submit` 对齐完成状态和 signal，再比较同段录制次序。seq 是诊断行的发出顺序。未完成、缺段或发现 gap 的记录保留未证。对同池重新分配、重叠区间和其他 serial 的写入一并核查。

运行 `android/tests/vk/run.sh --roi-probe` 的宿主验证覆盖两帧四次覆盖上传、关闭开关、源中唯一非零纹素、同池同区间的两个 serial、三维计算写两个 Z 切片，以及零组跳过。
