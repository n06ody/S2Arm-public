#!/usr/bin/env python3
"""Method-boundary regression, not an Android lifecycle simulator.
Extracts the actual production stop/intent/callback methods unchanged.
The controlled task queue records the 10-second fallback; native stop only records admission.
Expected ordering follows ae9a79bb5 (merged by #907): keep Surface alive for native teardown.
Normal-stop expectations follow a9de257f1: normal completion may clear attempt state, destruction must not.
Only stop/finish control intents are in scope; launch selection is deliberately rejected by the fixture.
Android task scheduling, WSI revoke and real native shutdown require device validation.
"""
import argparse
from pathlib import Path
import subprocess
import shutil
import tempfile


def method(source, signature):
    if source.count(signature) != 1:
        raise ValueError(f'expected exactly one production method: {signature}')
    begin = source.index(signature)
    brace = source.index('{', begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


SCAFFOLD = r'''
import java.util.*;
class ParentActivity {
    boolean finishing;
    android.content.Intent baseIntent = new android.content.Intent();
    static List<String> events = new ArrayList<>();
    void finishAndRemoveTask() { finishing = true; events.add("task_finishing"); }
    void finish() { finishing = true; events.add("activity_finish"); }
    android.content.Intent getIntent() { return baseIntent; }
    protected void onNewIntent(android.content.Intent intent) { }
    protected void onDestroy() { }
}
class android {
    static class os {
        static class Bundle extends HashMap<String,Object> { }
        static class Looper { static Looper getMainLooper() { return new Looper(); } }
        static class Process {
            static int myPid() { return 42; }
            static void killProcess(int pid) { ParentActivity.events.add("process_kill"); }
        }
        static class Handler {
            Handler(Looper looper) { }
            void postDelayed(Runnable action, long delay) {
                ExitOrderFixture.check(delay == 10000, "fallback must wait ten seconds");
                ExitOrderFixture.deferred.add(action); ParentActivity.events.add("fallback_queued");
            }
            void removeCallbacks(Runnable action) { }
        }
    }
    static class view { static class View { static final int VISIBLE=0; } }
    static class content {
        static class Intent {
            final Map<String,Object> extras = new HashMap<>();
            Intent put(String key, Object value) { extras.put(key,value); return this; }
            boolean hasExtra(String key) { return extras.containsKey(key); }
            boolean getBooleanExtra(String key, boolean fallback) {
                return extras.containsKey(key) ? (Boolean)extras.get(key) : fallback;
            }
            int getIntExtra(String key, int fallback) {
                return extras.containsKey(key) ? (Integer)extras.get(key) : fallback;
            }
            String getStringExtra(String key) { return (String)extras.get(key); }
            android.os.Bundle getExtras() {
                android.os.Bundle bundle = new android.os.Bundle(); bundle.putAll(extras); return bundle;
            }
            void removeExtra(String key) { extras.remove(key); }
        }
    }
    static class widget {
        static class Toast {
            static final int LENGTH_LONG=1;
            static Toast makeText(Object activity,String text,int duration) { return new Toast(); }
            void show() { }
        }
    }
}
public class ExitOrderFixture extends ParentActivity {
    static ExitOrderFixture current;
    static List<Runnable> deferred = new ArrayList<>();
    boolean runtimeLoaded=true;
    int coldVulkanChoice;
    static final String VULKAN_DIRECT_EXTRA="vulkan";
    static final String MIRROR_EXTRA="s2arm.mirror";
    static final Set<String> CONTROL_EXTRAS=Set.of("s2arm.finish", DebugEnvironment.EXTRA, GameActivity.ICON_RESUME_EXTRA);
    static class BuildConfig { static final boolean DEBUG=true; }
    static class DebugEnvironment { static final String EXTRA="s2arm.debug_env"; }
    // 调试入口两道门（DebugGate.java）：生产 onCreate / onNewIntent 第一步剥调试参数。本夹具测方法边界，按「门开着」桩成不剥
    static class DebugGate { static int scrub(android.content.Intent intent) { return 0; } }
    Object gameBackCallback;
    Surface surface=new Surface(), panel=new Surface();
    Probe lifecycleProbe=new Probe(); Runnable lifecycleSample=() -> {};
    FrameRate frameRate=new FrameRate(); Keyboard hostKeyboard=new Keyboard(); Keyboard legacyTextInput=new Keyboard();   // #1611 onPause 新加 legacyTextInput.pause()（11-06 main 安卓钩子红，合入运维线补桩）
    GameAudioFocus gameAudioFocus=null;   // #1658 onPause/onResume 新加 gameAudioFocus.pause()/resume()（11-07 main 安卓钩子红，合入运维线补桩）
    android.os.Handler saveAtomicProbe=new android.os.Handler(android.os.Looper.getMainLooper());
    Runnable saveAtomicSample=() -> {};   // 存档原子提交判定：onStop 前 removeCallbacks（#473 补桩）
    DguardNotice dguardNotice=null;       // #473 onDestroy 新加 dguardNotice.pause()（安全网提示桩）
    Library library=new Library();
    static class Surface { void setKeepScreenOn(boolean value) { } void setVisibility(int value) { } }
    static class Probe { void removeCallbacks(Runnable r) { } }
    static class FrameRate { void setResumed(boolean value) { } }
    static class Keyboard { void pause() { } }
    static class GameAudioFocus { void pause() { } void resume() { } }
    static class DguardNotice { void pause() { } void resume() { } void show(String d) { } }
    static class Library { void close() { events.add("library_close"); } }
    static class Build { static class VERSION { static final int SDK_INT=33; } }
    static class Dispatcher { void unregisterOnBackInvokedCallback(Object cb) { } }
    Dispatcher getOnBackInvokedDispatcher() { return new Dispatcher(); }
    static class GameResume {
        static final String EXTRA="resume";
        static void clear(Object directory) throws java.io.IOException { }
    }
    static class RuntimeLibrary {
        static final String EXTRA="runtime";
        static boolean isLaunch(Set<String> keys,Set<String> control) {
            if (!control.containsAll(keys)) throw new AssertionError("fixture only supports control intents");
            return false;
        }
    }
    static class Loaded {
        boolean selectedBy(String requested) { throw new AssertionError("not a launch fixture"); }
        String line() { return "fixture"; }
    }
    Object getCacheDir() { return null; }
    void gameLifecycle(String state) { }
    void maybeExportShaderCache(String reason) { }
    String requestedRuntime(android.content.Intent intent) { return null; }
    void consoleModeFromIntent(android.content.Intent intent,boolean cold) { }
    int vulkanChoice(android.content.Intent intent,int fallback) { return fallback; }
    void updateFrameRate(android.content.Intent intent) { }
    boolean gameMode=true, gameStopping;
    long host=7;
    Session gameSession;
    TouchPad touchPad=new TouchPad();
    Text status=new Text();
    InputManager inputManager=new InputManager();
    static class Spec { String sessionId="fixture"; }
    static class Session { Spec spec=new Spec(); void stop() { events.add("session_stop"); } }
    static class TouchPad { void setGameTouchEnabled(boolean enabled) { } }
    static class Text { String text=""; void setTextColor(int color) { } void setText(String value) { text=value; } }
    static class InputManager { void unregisterInputDeviceListener(Object listener) { } }
    static class Log { static void e(String tag,String text,Throwable error) { } static void e(String tag,String text) { } static void i(String tag,String text) { } }
    static class NativeBridge {
        static Loaded loaded() { return new Loaded(); }
        static int stops,destroys,debugRequests;
        static boolean lastNormal;
        static void stopGame(long host,String why,boolean normal) {
            lastNormal=normal;
            check(!current.finishing, "native teardown must start before task finish");
            ++stops; events.add("native_stop_admitted");
        }
        static void destroy(long host) { ++destroys; }
        static void clearStopListener(Object listener) { }
        static void setReleaseStall(long host,int ms) { }
        static void setStopStall(long host,int ms) { }
        static void debugRequestStop(int reason,String why) { ++debugRequests; }
        static String stopReason(int reason) { return "stop"; }
    }
    void updateTouchPad() { }
    void requestOrientation(android.content.Intent intent) { }
    // EXACT_METHODS
    static ExitOrderFixture fresh() {
        events.clear(); deferred.clear(); NativeBridge.stops=NativeBridge.destroys=NativeBridge.debugRequests=0;
        return current=new ExitOrderFixture();
    }
    static void check(boolean value,String message) {
        if (!value) throw new AssertionError(message+"; events="+events);
    }
    public static void main(String[] args) {
        ExitOrderFixture cold=fresh();
        cold.baseIntent.put("s2arm.finish",true);
        // Actual onCreate calls this same method after host creation, before queued game admission.
        cold.requestDebugLifecycle(cold.getIntent());
        check(events.equals(List.of("native_stop_admitted","fallback_queued")),
            "cold finish must admit native exit before scheduling fallback");
        check(!cold.getIntent().hasExtra("s2arm.finish"),"cold finish extra must be consumed");
        check(cold.gameStopping && cold.host==7 && NativeBridge.lastNormal,"stop must latch admission without freeing host");

        ExitOrderFixture live=fresh(); live.gameSession=new Session();
        android.content.Intent command=new android.content.Intent().put("s2arm.finish",true);
        live.onNewIntent(command);
        check(events.equals(List.of("session_stop","native_stop_admitted","fallback_queued")),
            "live intent must fence session then admit native exit without detaching");
        check(!command.hasExtra("s2arm.finish"),"incoming terminal extra must be consumed");
        live.onNewIntent(new android.content.Intent().put("s2arm.finish",true));
        check(NativeBridge.stops==1 && deferred.size()==1 && NativeBridge.lastNormal, "duplicate command must not re-admit or schedule twice");
        check(!live.finishing, "fallback must not run synchronously");
        deferred.remove(0).run();
        check(live.finishing && events.get(events.size()-1).equals("task_finishing"), "fallback must finish task");
        live.onDestroy(); // Explicit later callback; no assumption that Android invokes it synchronously.
        check(NativeBridge.stops==1 && NativeBridge.destroys==0 && live.host==0 && NativeBridge.lastNormal,
            "later destroy/duplicate command must not repeat cleanup or free native owner");

        ExitOrderFixture envChange=fresh(); envChange.gameSession=new Session();
        envChange.onNewIntent(new android.content.Intent().put(DebugEnvironment.EXTRA,"S2A_TEST=secret"));
        check(envChange.status.text.equals("Debug environment requires a fresh process"),
            "live environment change must be refused without echoing values");
        check(envChange.host==7 && !envChange.gameStopping && events.isEmpty(),
            "refused environment change must not replace or retire the running identity");

        ExitOrderFixture hostStop=fresh(); hostStop.gameSession=new Session();
        hostStop.onSessionStop(3,"fixture");
        check(events.equals(List.of("session_stop","native_stop_admitted","fallback_queued")),
            "HostApi stop callback must use the same native-first ordering");
        check(NativeBridge.lastNormal, "game stop callback is a normal session end");

        ExitOrderFixture absentHost=fresh(); absentHost.host=0;
        absentHost.stopGame("no host", false);
        check(events.equals(List.of("fallback_queued")) && NativeBridge.stops==0,
            "no native owner must still schedule fallback");
        deferred.remove(0).run();
        check(absentHost.finishing, "host-zero fallback completes task");

        ExitOrderFixture destroyed=fresh(); destroyed.gameSession=new Session();
        destroyed.onDestroy();
        check(events.equals(List.of("session_stop","native_stop_admitted","fallback_queued","library_close"))
                && destroyed.host==0 && NativeBridge.destroys==0 && !NativeBridge.lastNormal, "destroy admits native stop before closing library");
        ExitOrderFixture game=fresh(); game=new GameActivity(); current=game; game.host=0;
        game.stopGame("no host game", false);
        check(events.equals(List.of("process_kill","fallback_queued")), "host-zero game process requests termination");

        ExitOrderFixture resume=fresh(); resume.gameSession=new Session();
        resume.onNewIntent(new android.content.Intent().put(GameActivity.ICON_RESUME_EXTRA,true));
        check(events.isEmpty() && NativeBridge.stops==0 && !resume.gameStopping,
            "icon resume control intent must keep the session untouched (#1474: not a launch)");
        ExitOrderFixture diagnostic=fresh(); diagnostic.gameMode=false;
        diagnostic.requestDebugLifecycle(new android.content.Intent().put("s2arm.finish",true));
        check(events.equals(List.of("activity_finish")) && NativeBridge.stops==0,
            "diagnostic finish path must remain an Activity finish");
        System.out.println("PASS: cold/live/callback native-first ordering and ten-second fallback, consumed control, deferred destroy idempotence, host-zero and diagnostic paths");
        System.out.println("BOUNDARY: actual source methods, fake Android task/native boundaries; no Android scheduling or GPU/cleanup proof");
    }
}
class GameActivity extends ExitOrderFixture { static final String TITLE_EXTRA="title"; static final String ICON_RESUME_EXTRA="s2arm.icon_resume"; }
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'app/src/main/java/org/s2arm/android/MainActivity.java')
    p.add_argument('--out',type=Path)
    p.add_argument('--javac',default=shutil.which('javac') or 'javac')
    p.add_argument('--java',default=shutil.which('java') or 'java')
    a=p.parse_args()
    signatures=['private void stopGame(String why, boolean normal)',
        'private void requestDebugLifecycle(android.content.Intent intent)',
        'protected void onNewIntent(android.content.Intent intent)',
        'public void onSessionStop(int reason, String detail)',
        'protected void onDestroy()']
    actual=a.source.read_text()
    methods='\n'.join(method(actual,s) for s in signatures)
    with tempfile.TemporaryDirectory(prefix='s2a-exit-order-') as temp:
        return run(a, methods, a.out or Path(temp))


def run(a, methods, out):
    a.out = out
    a.out.mkdir(parents=True,exist_ok=True)
    harness=a.out/'ExitOrderFixture.java'
    harness.write_text(SCAFFOLD.replace('// EXACT_METHODS',methods))
    subprocess.run([a.javac,'-d',str(a.out),str(harness)],check=True)
    return subprocess.run([a.java,'-cp',str(a.out),'ExitOrderFixture']).returncode


if __name__=='__main__':
    raise SystemExit(main())
