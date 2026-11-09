// Android Performance Hint (ADPF): one measured worker thread, no CPU affinity
// policy. Frame correlation is by sequence: the direct shim tags every job with
// the frame it executes in, so a span can only be reported against the present
// that actually depended on it — never a present interval mistaken for work.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void s2a_adpf_set_foreground(int foreground);
void s2a_adpf_set_surface(int present);
// A queued translation job is about to execute on this thread, tagged with the
// frame sequence it belongs to. The first job of a frame opens its work span;
// later jobs of the same frame are ignored (the span is a wall-clock critical
// path, not a sum of job durations).
void s2a_adpf_work_begin(uint64_t seq);
// The frame's GPU dependency is complete (translation queue drained), before
// present/acquire pacing. Reports the recorded span only when its sequence is
// still this frame; a stale sequence means its present never happened and the
// record is dropped instead of charged to the wrong frame.
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
