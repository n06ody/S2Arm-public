// Open a frozen revision leaf. GameInstall publishes regular files or exactly one
// relative link to the game's objects/<first two hex digits>/<SHA-256>.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int seed_close_parent(int parent, int child) {
    int error = errno;
    close(parent);
    errno = error;
    return child;
}

static inline int seed_open_object(int game, int parent, const char *leaf, size_t levels) {
    char target[PATH_MAX];
    ssize_t n = readlinkat(parent, leaf, target, sizeof target);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof target) { errno = ENAMETOOLONG; return -1; }
    target[n] = 0;
    char *tail = target;
    // From the leaf's parent, climb to the revision, then revisions/, then the game.
    for (size_t i = 0; i < levels + 2; ++i) {
        if (strncmp(tail, "../", 3)) { errno = EINVAL; return -1; }
        tail += 3;
    }
    if (strncmp(tail, "objects/", 8)) { errno = EINVAL; return -1; }
    char *bucket = tail + 8;
    if (strlen(bucket) != 67 || bucket[2] != '/' || strncmp(bucket, bucket + 3, 2) ||
        strspn(bucket + 3, "0123456789abcdef") != 64) { errno = EINVAL; return -1; }
    bucket[2] = 0;
    int dirs = O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;
    int objects = openat(game, "objects", dirs);
    if (objects < 0) return -1;
    int shard = seed_close_parent(objects, openat(objects, bucket, dirs));
    if (shard < 0) return -1;
    return seed_close_parent(shard, openat(shard, bucket + 3, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
}

static inline int s2a_installed_seed_open(const char *path) {
    const char *root = getenv("S2A_SHADER_REVISION_ROOT");
    if (!root || root[0] != '/' || !path) { errno = EINVAL; return -1; }
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/') --len;
    if (len >= PATH_MAX || strncmp(path, root, len) || path[len] != '/') { errno = EINVAL; return -1; }
    const char *rel = path + len;
    while (*rel == '/') ++rel;
    if (strlen(rel) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    char base[PATH_MAX], parts[PATH_MAX];
    memcpy(base, root, len); base[len] = 0;
    strcpy(parts, rel);
    int dirs = O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;
    char *revision = strrchr(base, '/') + 1;
    static const char revisions[] = "/revisions/";
    size_t prefix = (size_t)(revision - base);
    size_t component = sizeof revisions - 1;
    int game = -1, parent;
    if (prefix > component && !memcmp(base + prefix - component, revisions, component)) {
        base[prefix - component] = 0;
        game = open(base, dirs);
        if (game < 0) return -1;
        int revs = openat(game, "revisions", dirs);
        parent = revs < 0 ? -1 : seed_close_parent(revs, openat(revs, revision, dirs));
    } else {
        // In-place imports and old plain revisions have no associated object store.
        parent = open(base, dirs);
    }
    int fd = -1;
    size_t levels = 0;
    char *part = parts;
    while (parent >= 0) {
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { errno = EINVAL; break; }
        if (!slash) {
            fd = openat(parent, part, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
            if (fd < 0 && errno == ELOOP && game >= 0) fd = seed_open_object(game, parent, part, levels);
            break;
        }
        parent = seed_close_parent(parent, openat(parent, part, dirs));
        part = slash + 1;
        ++levels;
    }
    if (parent >= 0) seed_close_parent(parent, fd);
    if (game >= 0) seed_close_parent(game, fd);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) < 0) return seed_close_parent(fd, -1);
        if (!S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    }
    return fd;
}
