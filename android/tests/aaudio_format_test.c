// Host fixture: fake Android API, but exercise the production open and reconnect paths.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include "../native/aaudio_sink.h"
#include "../native/pcm_ring.h"
#include "../native/capacity_affinity.h"
static int affinity_error, use_helper, opens;
int s2a_capacity_call_wide(int (*operation)(void *), void *argument, int *result) {
    if (!use_helper) return affinity_error;
    *result = operation(argument);
    return 1;
}
static struct AAudioStreamStruct { int token; } stream;
static struct AAudioStreamBuilderStruct { int token; } builder;
static int actual_format, actual_rate = 48000, actual_channels = 2, closed, starts, pauses;
static int affinity_warnings;
static void (*error_callback)(AAudioStream *, void *, aaudio_result_t);
static void *error_user;
static aaudio_data_callback_result_t (*data_callback)(AAudioStream *, void *, void *, int32_t);
static void *data_user;
int eventfd(unsigned int v, int flags) { (void)v; (void)flags; return open("/dev/null", O_RDWR); }
int __android_log_print(int level, const char *tag, const char *fmt, ...) {
    (void)tag;
    if (level == ANDROID_LOG_WARN && strstr(fmt, "AAudio affinity helper failed")) affinity_warnings++;
    return 0;
}
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **b) { *b=&builder; return 0; }
#define SETTER(name) void AAudioStreamBuilder_set##name(AAudioStreamBuilder *b, int v) { (void)b; (void)v; }
SETTER(Direction) SETTER(Format) SETTER(SampleRate) SETTER(ChannelCount) SETTER(PerformanceMode) SETTER(SharingMode) SETTER(Usage) SETTER(ContentType)
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *b, aaudio_data_callback_result_t (*cb)(AAudioStream *, void *, void *, int32_t), void *u) { (void)b; data_callback=cb; data_user=u; }
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder *b, void (*cb)(AAudioStream *, void *, aaudio_result_t), void *u) { (void)b; error_callback=cb; error_user=u; }
int AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **s) { (void)b; *s=&stream; opens++; return 0; }
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { (void)b; return AAUDIO_OK; }
#define GETTER(name, value) int AAudioStream_get##name(AAudioStream *s) { (void)s; return (value); }
GETTER(Format, actual_format) GETTER(SampleRate, actual_rate) GETTER(ChannelCount, actual_channels) GETTER(FramesPerBurst, 128) GETTER(XRunCount, 0) GETTER(BufferSizeInFrames, 256) GETTER(SharingMode, 0)
int AAudioStream_close(AAudioStream *s) { (void)s; closed++; return 0; }
int AAudioStream_setBufferSizeInFrames(AAudioStream *s, int n) { (void)s; return n; }
int AAudioStream_requestStart(AAudioStream *s) { (void)s; starts++; return 0; }
int AAudioStream_requestPause(AAudioStream *s) { (void)s; pauses++; return 0; }
int AAudioStream_requestStop(AAudioStream *s) { (void)s; return 0; }
int main(void) {
    s2a_aaudio *a = NULL;
    actual_format=AAUDIO_FORMAT_PCM_FLOAT;
    assert(s2a_aaudio_open(S2A_PCM_I16, 48000, 2, 10, &a)==AAUDIO_ERROR_INVALID_FORMAT);
    assert(a==NULL && closed==1);
    actual_format=AAUDIO_FORMAT_PCM_I16;
    assert(s2a_aaudio_open(S2A_PCM_I16, 48000, 2, 10, &a)==0 && a);
    assert(s2a_aaudio_start(a)==0 && starts==1);
    /* Foreground focus loss mutes but must return queued frames to the guest. */
    s2a_aaudio_set_output_allowed(0);
    int16_t queued[2]={1234,-1234}, actual[2]={1,1};
    assert(s2a_aaudio_write(a,queued,sizeof queued)==sizeof queued);
    struct s2a_aaudio_stats before, after;
    s2a_aaudio_stats(a,&before);
    data_callback(&stream,data_user,actual,1);
    s2a_aaudio_stats(a,&after);
    assert(actual[0]==0 && actual[1]==0);
    assert(after.frames_out==before.frames_out+1);
    s2a_aaudio_service(a);
    assert(pauses==0 && starts==1);
    s2a_aaudio_set_output_allowed(1);
    s2a_aaudio_service(a);
    assert(starts==1);
    /* Background pauses the stream and preserves queued frames until resume. */
    s2a_aaudio_set_foreground(0);
    s2a_aaudio_service(a);
    assert(pauses==1 && starts==1);
    assert(s2a_aaudio_write(a,queued,sizeof queued)==sizeof queued);
    s2a_aaudio_stats(a,&before);
    data_callback(&stream,data_user,actual,1);
    s2a_aaudio_stats(a,&after);
    assert(actual[0]==0 && actual[1]==0 && after.frames_out==before.frames_out);
    s2a_aaudio_set_foreground(1);
    s2a_aaudio_service(a);
    assert(starts==2);
    data_callback(&stream,data_user,actual,1);
    assert(actual[0]==queued[0] && actual[1]==queued[1]);
    starts=1;
    actual_format=AAUDIO_FORMAT_PCM_FLOAT;
    error_callback(&stream,error_user,AAUDIO_ERROR_DISCONNECTED);
    s2a_aaudio_service(a);
    assert(closed==3 && starts==1);
    assert(s2a_aaudio_start(a)==AAUDIO_ERROR_DISCONNECTED);
    actual_format=AAUDIO_FORMAT_PCM_I16;
    s2a_aaudio_service(a);
    assert(starts==2);
    s2a_aaudio_close(a);
    actual_format=AAUDIO_FORMAT_PCM_FLOAT;
    assert(s2a_aaudio_open(S2A_PCM_F32, 48000, 2, 10, &a)==0);
    s2a_aaudio_close(a);
    /* A rejected affinity expansion never invoked the operation. Open once
       on the caller for both initial and reconnect attempts. */
    affinity_error = -EAGAIN;
    const int opens_before = opens, starts_before = starts;
    assert(s2a_aaudio_open(S2A_PCM_F32, 48000, 2, 10, &a)==AAUDIO_OK && a);
    assert(opens == opens_before + 1);
    assert(s2a_aaudio_start(a) == AAUDIO_OK && starts == starts_before + 1);
    error_callback(&stream, error_user, AAUDIO_ERROR_DISCONNECTED);
    s2a_aaudio_service(a);
    assert(opens == opens_before + 2 && starts == starts_before + 2);
    assert(affinity_warnings == 2);
    s2a_aaudio_close(a);
    use_helper = 1;
    assert(s2a_aaudio_open(S2A_PCM_I16, 48000, 2, 10, &a)==AAUDIO_ERROR_INVALID_FORMAT && !a);
    assert(opens == opens_before + 3 && affinity_warnings == 2);
    puts("AAudio format: mismatch rejected and closed on open/reopen; matching i16/f32 accepted");
    puts("AAudio affinity fallback: start and reconnect succeeded after EAGAIN");
}
