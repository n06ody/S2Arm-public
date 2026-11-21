// SPDX-License-Identifier: MIT
// Simulate a denied mount list while retaining the host's per-path space query.
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

static __typeof__(getfsstat) deny_mounts;
static __typeof__(statfs) inject_space;

static int deny_mounts(struct statfs *buffer, int size, int flags)
{
    (void)buffer;
    (void)size;
    (void)flags;
    const char message[] = "fixture: mount enumeration denied\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    errno = EACCES;
    return -1;
}

static int inject_space(const char *path, struct statfs *stats)
{
    const char *target = getenv("S2A_TEST_SPACE_PATH");
    const char *mode = getenv("S2A_TEST_SPACE_MODE");
    if (!target || !mode || strcmp(path, target) != 0)
        return statfs(path, stats);

    if (strcmp(mode, "denied") == 0) {
        const char message[] = "fixture: statfs denied\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        errno = EACCES;
        return -1;
    }

    int result = statfs(path, stats);
    if (result == 0 && strcmp(mode, "full") == 0) {
        const char message[] = "fixture: statfs no available blocks\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        stats->f_bavail = 0;
    }
    return result;
}

__attribute__((used, section("__DATA,__interpose")))
static const struct {
    const void *replacement;
    const void *original;
} space_faults[] = {
    { (const void *)deny_mounts, (const void *)getfsstat },
    { (const void *)inject_space, (const void *)statfs },
};
