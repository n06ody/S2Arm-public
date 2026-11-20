#include "aaudio_sink.h"
#include "capacity_affinity.h"
#include "pcm_ring.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct s2a_aaudio {
    struct s2a_pcm_ring ring;
    AAudioStream *stream;
    int format, sample_rate, channels;
    int running;                  // the C# session asked to play
    int output_started;           // a start request has been accepted for this stream
    struct s2a_aaudio *next;
    _Atomic int disconnected;     // set by the error callback, handled by s2a_aaudio_service
    uint32_t reopens;
    int wake_fd;                  // eventfd the managed thread sleeps on
    _Atomic uint64_t notify_at;   // frames_out that wakes it; UINT64_MAX = disarmed
};

/* The activity only changes this policy and wakes the owner thread. All AAudio
 * stream operations remain serialized by the managed session lock. */
static _Atomic int output_allowed = 1;
static _Atomic int app_foreground = 1;
static pthread_mutex_t sinks_lock = PTHREAD_MUTEX_INITIALIZER;
static struct s2a_aaudio *sinks;

static void wake(struct s2a_aaudio *a) {
    uint64_t one = 1;
    while (write(a->wake_fd, &one, sizeof one) < 0 && errno == EINTR) {}   // non-blocking, never waits
}

static void wake_sinks(void) {
    pthread_mutex_lock(&sinks_lock);
    for (struct s2a_aaudio *a = sinks; a; a = a->next) wake(a);
    pthread_mutex_unlock(&sinks_lock);
}
void s2a_aaudio_set_output_allowed(int allowed) {
    atomic_store(&output_allowed, !!allowed);
    wake_sinks();
}
void s2a_aaudio_set_foreground(int foreground) {
    atomic_store(&app_foreground, !!foreground);
    wake_sinks();
}

static aaudio_data_callback_result_t on_data(AAudioStream *stream, void *user, void *audio, int32_t frames) {
    (void)stream;
    struct s2a_aaudio *a = user;
    if (!atomic_load(&app_foreground)) {
        memset(audio, 0, (size_t)frames * a->ring.frame_bytes);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    s2a_pcm_ring_read(&a->ring, audio, (uint32_t)frames);
    /* A foreground game must keep consuming buffers while it lacks focus. */
    if (!atomic_load(&output_allowed) || !atomic_load(&app_foreground))
        memset(audio, 0, (size_t)frames * a->ring.frame_bytes);
    uint64_t at = atomic_load_explicit(&a->notify_at, memory_order_relaxed);
    if (atomic_load_explicit(&a->ring.frames_out, memory_order_relaxed) >= at
        && atomic_compare_exchange_strong(&a->notify_at, &at, UINT64_MAX)) wake(a);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// AAudio forbids closing or reopening a stream from its callbacks.
static void on_error(AAudioStream *stream, void *user, aaudio_result_t error) {
    (void)stream;
    if (error != AAUDIO_ERROR_DISCONNECTED) return;
    atomic_store(&((struct s2a_aaudio *)user)->disconnected, 1);
    wake(user);
}

static int open_stream_impl(void *arg) {
    struct s2a_aaudio *a = arg;
    AAudioStreamBuilder *b;
    aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    const int requested_format = a->format == S2A_PCM_I16 ? AAUDIO_FORMAT_PCM_I16 : AAUDIO_FORMAT_PCM_FLOAT;
    AAudioStreamBuilder_setFormat(b, requested_format);
    AAudioStreamBuilder_setSampleRate(b, a->sample_rate);
    AAudioStreamBuilder_setChannelCount(b, a->channels);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_EXCLUSIVE);   // AAudio falls back to shared
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, on_data, a);
    AAudioStreamBuilder_setErrorCallback(b, on_error, a);
    r = AAudioStreamBuilder_openStream(b, &a->stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { a->stream = NULL; return r; }
    __android_log_print(ANDROID_LOG_INFO, "S2Arm",
        "AAudio open requested_format=%d actual_format=%d requested_rate=%d actual_rate=%d requested_channels=%d actual_channels=%d",
        requested_format,
        AAudioStream_getFormat(a->stream), a->sample_rate, AAudioStream_getSampleRate(a->stream),
        a->channels, AAudioStream_getChannelCount(a->stream));
    // Rate / format / channels are what we asked for or the open fails; AAudio resamples.
    if (AAudioStream_getFormat(a->stream) != requested_format ||
        AAudioStream_getSampleRate(a->stream) != a->sample_rate || AAudioStream_getChannelCount(a->stream) != a->channels) {
        AAudioStream_close(a->stream);
        a->stream = NULL;
        return AAUDIO_ERROR_INVALID_FORMAT;
    }
    // Two bursts of device buffering: low latency, the ring absorbs managed-side stalls.
    AAudioStream_setBufferSizeInFrames(a->stream, 2 * AAudioStream_getFramesPerBurst(a->stream));
    return AAUDIO_OK;
}

static int open_stream(struct s2a_aaudio *a) {
    int result;
    int handled = s2a_capacity_call_wide(open_stream_impl, a, &result);
    if (handled > 0) return result;
    if (handled < 0) {
        __android_log_print(ANDROID_LOG_WARN, "S2Arm", "【黄】AAudio affinity helper failed (%d); opening on the caller", handled);
    }
    return open_stream_impl(a);
}

int s2a_aaudio_open(int format, int sample_rate, int channels, int ring_ms, s2a_aaudio **out) {
    *out = NULL;
    if (sample_rate <= 0 || ring_ms <= 0) return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    struct s2a_aaudio *a = calloc(1, sizeof *a);
    if (!a) return AAUDIO_ERROR_NO_MEMORY;
    a->format = format; a->sample_rate = sample_rate; a->channels = channels;
    atomic_store(&a->notify_at, UINT64_MAX);
    a->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (a->wake_fd < 0) { free(a); return AAUDIO_ERROR_NO_MEMORY; }
    if (s2a_pcm_ring_init(&a->ring, format, (uint32_t)channels, (uint32_t)((int64_t)sample_rate * ring_ms / 1000))) {
        close(a->wake_fd);
        free(a);
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }
    int r = open_stream(a);
    if (r != AAUDIO_OK) { s2a_pcm_ring_free(&a->ring); close(a->wake_fd); free(a); return r; }
    pthread_mutex_lock(&sinks_lock);
    a->next = sinks; sinks = a;
    pthread_mutex_unlock(&sinks_lock);
    *out = a;
    return AAUDIO_OK;
}

int s2a_aaudio_start(s2a_aaudio *a) {
    a->running = 1;
    if (!a->stream) return AAUDIO_ERROR_DISCONNECTED;
    if (!atomic_load(&app_foreground)) return AAUDIO_OK; /* deferred until foreground */
    int r = AAudioStream_requestStart(a->stream);
    if (r == AAUDIO_OK) a->output_started = 1;
    return r;
}

int s2a_aaudio_pause(s2a_aaudio *a) {
    a->running = 0;
    if (!a->stream || !a->output_started) return AAUDIO_OK;
    int r = AAudioStream_requestPause(a->stream);
    if (r == AAUDIO_OK) a->output_started = 0;
    return r;
}

void s2a_aaudio_service(s2a_aaudio *a) {
    if (atomic_exchange(&a->disconnected, 0) || !a->stream) {
        if (a->stream) { AAudioStream_close(a->stream); a->stream = NULL; }
        a->output_started = 0;
        if (!atomic_load(&app_foreground) || open_stream(a) != AAUDIO_OK) {
            atomic_store(&a->disconnected, 1); return;
        }
        a->reopens++;
        __android_log_print(ANDROID_LOG_INFO, "S2Arm", "AAudio stream reopened after a disconnect (%u)", a->reopens);
    }
    const int want = a->running && atomic_load(&app_foreground);
    if (want != a->output_started) {
        int r = want ? AAudioStream_requestStart(a->stream) : AAudioStream_requestPause(a->stream);
        if (r == AAUDIO_OK) a->output_started = want;
        else __android_log_print(ANDROID_LOG_ERROR, "S2Arm", "AAudio focus transition failed: %d", r);
    }
}

void s2a_aaudio_notify_at(s2a_aaudio *a, uint64_t frames) { atomic_store(&a->notify_at, frames); }
void s2a_aaudio_wake(s2a_aaudio *a) { wake(a); }

int s2a_aaudio_wait(s2a_aaudio *a, int timeout_ms) {
    struct pollfd p = {.fd = a->wake_fd, .events = POLLIN};
    if (poll(&p, 1, timeout_ms) <= 0) return 0;
    uint64_t n;
    while (read(a->wake_fd, &n, sizeof n) < 0 && errno == EINTR) {}
    return 1;
}

uint32_t s2a_aaudio_space(s2a_aaudio *a) { return s2a_pcm_ring_space(&a->ring); }
uint32_t s2a_aaudio_write(s2a_aaudio *a, const void *pcm, uint32_t bytes) { return s2a_pcm_ring_write(&a->ring, pcm, bytes); }
void s2a_aaudio_set_volume(s2a_aaudio *a, float volume) { s2a_pcm_ring_set_volume(&a->ring, volume); }

void s2a_aaudio_stats(s2a_aaudio *a, struct s2a_aaudio_stats *out) {
    memset(out, 0, sizeof *out);
    out->frames_out = atomic_load(&a->ring.frames_out);
    out->frames_padded = atomic_load(&a->ring.frames_padded);
    out->sample_rate = a->sample_rate;
    out->reopens = a->reopens;
    if (!a->stream) return;
    out->xruns = (uint32_t)AAudioStream_getXRunCount(a->stream);
    out->burst_frames = AAudioStream_getFramesPerBurst(a->stream);
    out->buffer_frames = AAudioStream_getBufferSizeInFrames(a->stream);
    out->sharing = AAudioStream_getSharingMode(a->stream);
}

void s2a_aaudio_close(s2a_aaudio *a) {
    if (!a) return;
    pthread_mutex_lock(&sinks_lock);
    struct s2a_aaudio **p = &sinks;
    while (*p && *p != a) p = &(*p)->next;
    if (*p) *p = a->next;
    pthread_mutex_unlock(&sinks_lock);
    if (a->stream) { AAudioStream_requestStop(a->stream); AAudioStream_close(a->stream); }
    s2a_pcm_ring_free(&a->ring);
    close(a->wake_fd);
    free(a);
}
