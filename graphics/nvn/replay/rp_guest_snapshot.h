#ifndef S2A_RP_GUEST_SNAPSHOT_H
#define S2A_RP_GUEST_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

enum rp_land_reader { RP_LAND_READ_REPLAY, RP_LAND_READ_HOST, RP_LAND_READERS };
// Diagnostic owned copy. ENOSYS permits the existing read for untracked ranges.
// Other errors leave dst unusable. The caller owns dst until all I/O is done.
int rp_buf_guest_snapshot(uint64_t addr, void *dst, size_t len, enum rp_land_reader reader);

// The owned replay copy respects the same pool and alias checks. A nonzero
// producer identifies a completed mirror span that matched the whole copy.
// A reader holding a resource lock passes wait=0. ENOSYS selects legacy I/O.
int rp_buf_replay_snapshot(uint64_t addr, void *dst, size_t len, int wait, uint64_t *producer);
int rp_buf_replay_wait(uint64_t addr, size_t len);
int rp_buf_replay_versioned(void);

#endif
