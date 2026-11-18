// 手机当掌机的六轴与震动（#528 第 10 项第一块），接口与规矩见 phone_feedback.h；换算的纯函数在 phone_feedback_policy.h。
//   对齐 iOS ios/phone_motion.m：没有外接手柄六轴时手机陀螺仪 / 加速度当 Switch 掌机六轴，HD Rumble 走手机马达；
//   服务层（ControllerFeedback → Ryujinx MotionInput / UpdateSixAxis）不变，安卓原先只是没导出这几个符号。
// 设置：游戏内四指菜单「手机陀螺仪」「手机震动」（PhoneFeedback.java，SharedPreferences，缺省开，同 iOS 缺省）。
#include "phone_feedback.h"
#include "phone_feedback_policy.h"
#include "phone_axes.h"
#include "phone_rumble_mailbox.h"
#include "motion_pick.h"
#include "s2a_pad.h"
#include <android/log.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default"), used))
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "S2Arm", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "S2Arm", __VA_ARGS__)
#define S2A_PHONE_SESSION_BASE 0x5048000000000000ull   // 手机马达的震动会话号（同 iOS，与 NS2 的小整数会话号不相交）
// 只给体感的会话（同 iOS controller.m MOTION_ONLY_SESSION 的用法）：在前台但没有能震的马达（设置关了 / 没马达 / 接着手柄）时报它，
//   服务层的反馈对象照留（InputPump.RefreshFeedback 见 0 会拆掉整个对象，六轴也跟着没了）；拿它提交的震动一律丢
#define S2A_MOTION_ONLY_SESSION 0x4d4f000000000001ull
enum { SENSOR_PERIOD_US = 5000 };   // 200 Hz，同 iOS；安卓 12+ 不申请 HIGH_SAMPLING_RATE_SENSORS 时的上限正好是 200 Hz

// ---------- 设置与前后台（主线程写，HID / 反馈线程读）----------
static _Atomic int g_gyro_pref = 1, g_rumble_pref = 1, g_has_vibrator, g_gamepad, g_foreground = 1, g_rotation = 1;
static _Atomic uint64_t g_session = S2A_PHONE_SESSION_BASE + 1;   // 手机马达这一代的会话号：退后台、改设置、接拔手柄换号

// ---------- 六轴 ----------
// 传感器线程只在第一次要六轴时起，之后常驻（ALooper 绑在它身上）；开关只调 enable / disable，不拆线程
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ASensorManager *g_manager;
static ASensorEventQueue *g_queue;
static const ASensor *g_accel, *g_gyro;
static char g_package[256];
static int g_want_motion, g_sensors_on, g_thread_state;   // 0 = 没起、1 = 起好、-1 = 起不来（已报红）
static pthread_cond_t g_thread_cv = PTHREAD_COND_INITIALIZER;
static float g_acc[3], g_gyr[3];
static int64_t g_acc_ns, g_gyr_ns;

static int on_sensor(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    ASensorEvent ev[16];
    ssize_t n;
    while ((n = ASensorEventQueue_getEvents(g_queue, ev, 16)) > 0) {
        pthread_mutex_lock(&g_mu);
        for (ssize_t i = 0; i < n; i++) {
            if (ev[i].type == ASENSOR_TYPE_ACCELEROMETER) { memcpy(g_acc, ev[i].acceleration.v, sizeof g_acc); g_acc_ns = ev[i].timestamp; }
            else if (ev[i].type == ASENSOR_TYPE_GYROSCOPE) { memcpy(g_gyr, ev[i].vector.v, sizeof g_gyr); g_gyr_ns = ev[i].timestamp; }
        }
        pthread_mutex_unlock(&g_mu);
    }
    return 1;
}

static void *sensor_thread(void *arg) {
    (void)arg;
    ALooper *looper = ALooper_prepare(0);
    pthread_mutex_lock(&g_mu);
    g_manager = ASensorManager_getInstanceForPackage(g_package[0] ? g_package : NULL);
    g_accel = g_manager ? ASensorManager_getDefaultSensor(g_manager, ASENSOR_TYPE_ACCELEROMETER) : NULL;
    g_gyro = g_manager ? ASensorManager_getDefaultSensor(g_manager, ASENSOR_TYPE_GYROSCOPE) : NULL;
    g_queue = g_accel && g_gyro ? ASensorManager_createEventQueue(g_manager, looper, 1, on_sensor, NULL) : NULL;
    if (!g_queue)
        LOGE("【红】手机六轴：本机%s，游戏的六轴读不到手机（陀螺仪瞄准等不可用）",
             !g_manager ? "拿不到传感器服务" : !g_gyro ? "没有陀螺仪" : !g_accel ? "没有加速度计" : "建不出传感器队列");
    g_thread_state = g_queue ? 1 : -1;
    pthread_cond_broadcast(&g_thread_cv);
    pthread_mutex_unlock(&g_mu);
    if (!g_queue) return NULL;
    for (;;) ALooper_pollOnce(-1, NULL, NULL, NULL);
}

// 持 g_mu。游戏要六轴、设置开着、在前台才开传感器；其余一律关（省电，同 iOS 游戏启用 SixAxis 才开）
static void sensors_apply_locked(void) {
    int on = g_want_motion && atomic_load(&g_gyro_pref) && atomic_load(&g_foreground);
    if (on && !g_thread_state) {
        pthread_t t;
        if (pthread_create(&t, NULL, sensor_thread, NULL) != 0) { LOGE("【红】手机六轴：传感器线程起不来"); g_thread_state = -1; return; }
        pthread_detach(t);
        while (!g_thread_state) pthread_cond_wait(&g_thread_cv, &g_mu);
    }
    if (g_thread_state != 1 || on == g_sensors_on) return;
    if (on) {
        int a = ASensorEventQueue_registerSensor(g_queue, g_accel, SENSOR_PERIOD_US, 0);
        int g = ASensorEventQueue_registerSensor(g_queue, g_gyro, SENSOR_PERIOD_US, 0);
        if (a < 0 || g < 0) {
            LOGE("【红】手机六轴：开传感器失败（加速度计 %d、陀螺仪 %d）", a, g);
            if (!a) ASensorEventQueue_disableSensor(g_queue, g_accel);
            if (!g) ASensorEventQueue_disableSensor(g_queue, g_gyro);
            return;
        }
        LOGI("手机六轴：开（加速度计 %s、陀螺仪 %s，%d Hz，没有外接手柄六轴时当掌机六轴）",
             ASensor_getName(g_accel), ASensor_getName(g_gyro), 1000000 / SENSOR_PERIOD_US);
    } else {
        ASensorEventQueue_disableSensor(g_queue, g_accel);
        ASensorEventQueue_disableSensor(g_queue, g_gyro);
        g_acc_ns = g_gyr_ns = 0;   // 关过就不再把旧样本当新的（再开后两路都到齐才报有样本）
        LOGI("手机六轴：关");
    }
    g_sensors_on = on;
}

EXPORT void s2a_pad_set_motion_enabled(int enabled) {
    pthread_mutex_lock(&g_mu);
    g_want_motion = enabled != 0;
    sensors_apply_locked();
    pthread_mutex_unlock(&g_mu);
}

EXPORT int s2a_pad_read_motion(s2a_pad_motion *out) {
    if (!out) return 0;
    pthread_mutex_lock(&g_mu);
    int known = g_sensors_on && g_acc_ns && g_gyr_ns;
    float acc[3], gyr[3];
    int64_t ts = g_gyr_ns;   // 以陀螺仪的时间为样本时间：服务层按它判新样本、积分姿态
    memcpy(acc, g_acc, sizeof acc);
    memcpy(gyr, g_gyr, sizeof gyr);
    pthread_mutex_unlock(&g_mu);
    // 玩家 1 的六轴来源仲裁（共用规则）：安卓外接手柄的体感还没接（第二块），手柄两路恒为没有样本，有手机样本就用手机
    if (s2a_motion_pick(S2A_PAD_TOUCH, 0, 0, known) != S2A_MOTION_PHONE) return 0;
    double acc_dev[3], gyr_dev[3] = {gyr[0], gyr[1], gyr[2]};
    s2a_android_accel_to_coremotion(acc, acc_dev);
    int right[2], up[2];
    s2a_android_rotation_basis(atomic_load(&g_rotation), right, up);
    float a[3], g[3];
    s2a_phone_axes_basis(right, up, acc_dev, gyr_dev, a, g);
    *out = (s2a_pad_motion){ (uint64_t)ts / 1000u, a[0], a[1], a[2], g[0], g[1], g[2] };
    return 1;
}

// ---------- 震动 ----------
// 提交侧（服务层在 HID 的 _rumbleLock 内同步调）只写 mailbox 就返回；Binder 调 Vibrator 一律在反馈线程上（同 iOS 的理由，phone_rumble_mailbox.h）
static JavaVM *g_vm;
static jclass g_feedback_class;
static jmethodID g_vibrate;
static _Atomic int g_mb_ready;
static s2a_android_rumble_state g_rumble;   // 只由反馈线程读写
static pthread_mutex_t g_pair_mu = PTHREAD_MUTEX_INITIALIZER;
static s2a_pad_vibration g_pair[2];   // s2a_pad_set_rumble 单侧提交时补另一侧

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void java_vibrate(int amplitude) {
    static JNIEnv *env;   // 反馈线程常驻：挂一次 JVM，不再摘
    if (!g_vm || !g_vibrate) return;
    if (!env && (*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) {
        env = NULL;
        static int said; if (!said++) LOGE("【红】手机震动：反馈线程挂不上 JVM");
        return;
    }
    (*env)->CallStaticVoidMethod(env, g_feedback_class, g_vibrate, (jint)amplitude, (jint)S2A_ANDROID_RUMBLE_LEASE_MS);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
}

// 反馈线程：马达的唯一入口
static void rumble_apply(const s2a_pad_vibration v[2], int kind) {
    if (kind == S2A_PHONE_RUMBLE_WARM) return;
    int level = kind == S2A_PHONE_RUMBLE_STOP || !v ? 0 : s2a_android_rumble_level(v);
    int send = s2a_android_rumble_step(&g_rumble, level, now_ns());
    if (send < 0) return;
    java_vibrate(send);
    static int said; if (send > 0 && !said++) LOGI("手机震动：首次 振幅 %d/255", send);
}

// 手机马达只在没接手柄时震（同 iOS：接着外接手柄时手机多半放在一边；手柄自己的马达是第二块）
EXPORT uint64_t s2a_pad_rumble_session(void) {
    if (!atomic_load(&g_foreground)) return 0;
    if (!atomic_load(&g_rumble_pref) || !atomic_load(&g_has_vibrator) || atomic_load(&g_gamepad)) return S2A_MOTION_ONLY_SESSION;
    return atomic_load(&g_session);
}

static int owns(uint64_t session) {
    uint64_t now = s2a_pad_rumble_session();
    return session && session == now && now != S2A_MOTION_ONLY_SESSION;
}

static void rumble_stop(void) {
    if (atomic_load(&g_mb_ready)) s2a_phone_rumble_mailbox_submit_stop();   // STOP 在信箱里压过 PLAY
}

EXPORT void s2a_pad_set_rumble_pair(uint64_t session, const s2a_pad_vibration values[2]) {
    if (!values) return;
    pthread_mutex_lock(&g_pair_mu);
    memcpy(g_pair, values, sizeof g_pair);
    pthread_mutex_unlock(&g_pair_mu);
    if (!owns(session)) return;   // 旧会话（退过后台 / 关了设置）的迟到提交不再震
    if (!atomic_load(&g_mb_ready)) {
        if (!s2a_phone_rumble_mailbox_start(rumble_apply)) { static int said; if (!said++) LOGE("【红】手机震动：反馈线程起不来"); return; }
        atomic_store(&g_mb_ready, 1);
    }
    s2a_phone_rumble_mailbox_submit(values);
}

EXPORT void s2a_pad_set_rumble(uint64_t session, unsigned side, float low_amp, float low_hz, float high_amp, float high_hz) {
    if (side > 1) return;
    s2a_pad_vibration pair[2];
    pthread_mutex_lock(&g_pair_mu);
    g_pair[side] = (s2a_pad_vibration){ low_amp, low_hz, high_amp, high_hz };
    memcpy(pair, g_pair, sizeof pair);
    pthread_mutex_unlock(&g_pair_mu);
    s2a_pad_set_rumble_pair(session, pair);
}

EXPORT void s2a_pad_stop_rumble(uint64_t session) {
    (void)session;   // 停止不看会话：任何一方要停都停（多停一次无害）
    rumble_stop();
}

// ---------- 前后台 / 设置 / 屏幕方向 ----------
void s2a_android_phone_feedback_foreground(int foreground) {
    foreground = foreground != 0;
    if (atomic_exchange(&g_foreground, foreground) == foreground) return;
    if (!foreground) { atomic_fetch_add(&g_session, 1); rumble_stop(); }   // 会话换号：服务层见到变化就清本地震动状态（ControllerFeedback.ObserveSession）
    pthread_mutex_lock(&g_mu);
    sensors_apply_locked();
    pthread_mutex_unlock(&g_mu);
}

// 服务层登记（android/core-runtime/AndroidGameRuntime.cs → InputPump.Feedback，字段次序同 InputPump.HostFeedback）：
//   本库以 RTLD_LOCAL 载入，服务层按名查主程序导出查不到上面这几个函数，改由平台装配把地址交过去
typedef struct { void *rumble_session, *set_rumble_pair, *stop_rumble, *set_motion_enabled, *read_motion; } s2a_android_pad_feedback_hooks;
EXPORT void s2a_android_pad_feedback(s2a_android_pad_feedback_hooks *out) {
    if (!out) return;
    out->rumble_session = (void *)s2a_pad_rumble_session;
    out->set_rumble_pair = (void *)s2a_pad_set_rumble_pair;
    out->stop_rumble = (void *)s2a_pad_stop_rumble;
    out->set_motion_enabled = (void *)s2a_pad_set_motion_enabled;
    out->read_motion = (void *)s2a_pad_read_motion;
}

JNIEXPORT void JNICALL
Java_org_s2arm_android_NativeBridge_configurePhoneFeedback(JNIEnv *env, jclass bridge, jstring package, jboolean gyro, jboolean rumble,
                                                           jboolean has_vibrator, jboolean gamepad) {
    (void)bridge;
    if (!g_vm) (*env)->GetJavaVM(env, &g_vm);
    if (!g_feedback_class) {
        jclass local = (*env)->FindClass(env, "org/s2arm/android/PhoneFeedback");   // 主线程查（反馈线程的类加载器找不到应用类）
        if (local) {
            g_vibrate = (*env)->GetStaticMethodID(env, local, "vibrate", "(II)V");
            g_feedback_class = (*env)->NewGlobalRef(env, local);
            (*env)->DeleteLocalRef(env, local);
        }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        if (!g_vibrate) LOGE("【红】手机震动：PhoneFeedback.vibrate 找不到");
    }
    pthread_mutex_lock(&g_mu);
    if (package && !g_package[0]) {
        const char *p = (*env)->GetStringUTFChars(env, package, NULL);
        if (p) { strncpy(g_package, p, sizeof g_package - 1); (*env)->ReleaseStringUTFChars(env, package, p); }
    }
    pthread_mutex_unlock(&g_mu);
    // 能不能震变了就换会话号：服务层见到变化清掉本地震动状态（ControllerFeedback.ObserveSession），旧会话的迟到提交不再震
    uint64_t before = s2a_pad_rumble_session();
    atomic_store(&g_rumble_pref, rumble != JNI_FALSE);
    atomic_store(&g_has_vibrator, has_vibrator != JNI_FALSE && g_vibrate != NULL);
    atomic_store(&g_gamepad, gamepad != JNI_FALSE);
    if (s2a_pad_rumble_session() != before) {
        if (before != S2A_MOTION_ONLY_SESSION) rumble_stop();
        atomic_fetch_add(&g_session, 1);
    }
    atomic_store(&g_gyro_pref, gyro != JNI_FALSE);
    pthread_mutex_lock(&g_mu);
    sensors_apply_locked();
    pthread_mutex_unlock(&g_mu);
}

JNIEXPORT void JNICALL
Java_org_s2arm_android_NativeBridge_setDisplayRotation(JNIEnv *env, jclass bridge, jint rotation) {
    (void)env; (void)bridge;
    int was = atomic_exchange(&g_rotation, rotation & 3);
    if (was != (rotation & 3)) LOGI("手机六轴：屏幕方向 ROTATION_%d", (rotation & 3) * 90);
}
