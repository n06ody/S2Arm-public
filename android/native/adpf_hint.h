// Android Performance Hint (ADPF): the replay thread and optional Vulkan
// recorder share one session. Frame work is correlated with present by sequence.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void s2a_adpf_set_foreground(int foreground);
void s2a_adpf_set_surface(int present);
// Vulkan backend's optional recorder, or zero when it is inactive.
int32_t s2a_adpf_recorder_tid(void);
// Begin the CPU work period after presentation. A duplicate begin for the same
// sequence keeps the first timestamp.
void s2a_adpf_work_begin(uint64_t seq);
// Exclude a blocking GPU dependency wait on the frame's work thread. Both
// timestamps are CLOCK_MONOTONIC nanoseconds.
void s2a_adpf_exclude_gpu_wait(uint64_t begin_ns, uint64_t end_ns);
// End the CPU work period before present/acquire pacing, using the current
// exchange interval as the target. Only the matching sequence may be reported.
void s2a_adpf_frame_end(int64_t target_ns, uint64_t seq);
void s2a_adpf_discard(void);
// Close an invisible renderer without changing the host lifecycle state.
void s2a_adpf_invisible(void);
void s2a_adpf_stop(void);
#ifdef S2A_ADPF_TEST
void s2a_adpf_test_reset(void);
#endif
#ifdef __cplusplus
}
#endif
