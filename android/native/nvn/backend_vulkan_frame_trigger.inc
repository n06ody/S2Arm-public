// Included by the optional Vulkan draw export. All names are relative to the
// current application's private files directory; the QA station owns requests.
#include <fcntl.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

struct frame_identity {
    long pid;
    unsigned long long birth; // /proc field 22, in ticks
    char session[33], nonce[33], stem[128];
};

static int frame_hex32(const char *s) {
    return s && strlen(s) == 32 && strspn(s, "0123456789abcdef") == 32;
}

static int frame_session(char out[33]) {
#if defined(__ANDROID__)
    char property[PROP_VALUE_MAX];
    if (__system_property_get("debug.s2a.session_uuid", property) != 32) return 0;
    const char *value = property;
#else
    const char *value = getenv("S2A_TEST_DRAW_FRAME_SESSION");
#endif
    if (!frame_hex32(value)) return 0;
    memcpy(out, value, 33);
    return 1;
}

static int frame_identity_read(struct frame_identity *id) {
    if (!frame_session(id->session)) return 0;
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    int ok = !ferror(f) && feof(f);
    fclose(f);
    if (!n || !ok) return 0;
    buf[n] = 0;
    char *p = strrchr(buf, ')'); // comm may contain spaces and ')'
    if (!p || p[1] != ' ') return 0;
    p += 2;
    for (unsigned field = 3; field < 22; field++) {
        p = strchr(p, ' ');
        if (!p) return 0;
        while (*p == ' ') p++;
    }
    char *end;
    errno = 0;
    id->birth = strtoull(p, &end, 10);
    if (errno || !id->birth || *end != ' ') return 0;
    id->pid = (long)getpid();
    snprintf(id->stem, sizeof id->stem, "sf-%ld-%llu-%s", id->pid, id->birth, id->session);
    return 1;
}

static int frame_path(char path[4096], const char *leaf) {
    const char *root = getenv("S2A_FRAME_LOG_DIR");
    if (!root || root[0] != '/') return 0;
    int n = snprintf(path, 4096, "%s/%s", root, leaf);
    return n > 0 && n < 4096;
}

// One immutable request per lifetime. A persistent exclusive claim prevents
// re-arming if the backend is closed and reopened in the same game process.
static int frame_claim(struct frame_identity *id) {
    char leaf[192], req[4096], claim[4096], current[33];
    snprintf(leaf, sizeof leaf, "%s.request", id->stem);
    if (!frame_path(req, leaf)) return -1;
    int fd = open(req, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    char text[256] = {0}, prefix[160];
    int valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (off_t)sizeof text;
    ssize_t n = valid ? read(fd, text, sizeof text - 1) : -1;
    close(fd);
    int k = snprintf(prefix, sizeof prefix, "v=2 pid=%ld birth=%llu session=%s nonce=", id->pid, id->birth, id->session);
    if ((long)getpid() != id->pid || n != k + 33 || n != st.st_size || memcmp(text, prefix, (size_t)k) || text[n-1] != '\n' ||
        !frame_session(current) || strcmp(current, id->session)) return -1;
    text[n-1] = 0;
    if (!frame_hex32(text+k)) return -1;
    snprintf(leaf, sizeof leaf, "%s.claim", id->stem);
    if (!frame_path(claim, leaf)) return -1;
    fd = open(claim, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) return errno == EEXIST ? 0 : -1;
    close(fd);
    memcpy(id->nonce, text+k, 33);
    return 1;
}

static int frame_seal(FILE *f, const struct frame_identity *id, size_t limit) {
    char current[33];
    if ((long)getpid() != id->pid || !frame_session(current) || strcmp(current, id->session) || fflush(f) || fseek(f, 0, SEEK_SET)) return 0;
    s2a_sha256 hash;
    s2a_sha256_init(&hash);
    uint8_t chunk[8192], digest[32];
    char hex[65];
    size_t count, bytes = 0;
    while ((count = fread(chunk, 1, sizeof chunk, f))) {
        bytes += count;
        if (bytes > limit - 128) return 0;
        s2a_sha256_update(&hash, chunk, count);
    }
    if (ferror(f) || fseek(f, 0, SEEK_END)) return 0;
    s2a_sha256_final(&hash, digest);
    s2a_sha256_hex(digest, hex);
    return fprintf(f, "sealed bytes=%zu sha256=%s\n", bytes, hex) > 0 && !fflush(f) && !fsync(fileno(f));
}
