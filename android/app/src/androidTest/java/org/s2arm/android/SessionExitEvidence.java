package org.s2arm.android;

import java.util.Arrays;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Exit selection is independent of Android so old-PID and delayed-record cases run on the host. */
final class SessionExitEvidence {
    static final class Exit {
        final int pid, reason, status;
        final long timestamp;
        final String process;
        final byte[] summary;
        Exit(int pid, long timestamp, String process, byte[] summary, int reason, int status) {
            this.pid = pid; this.timestamp = timestamp; this.process = process;
            this.summary = summary; this.reason = reason; this.status = status;
        }
    }

    static Exit select(ProcessBirth birth, String process, List<Exit> records) {
        Exit selected = null;
        for (Exit exit : records) {
            if (exit.pid != birth.pid || !process.equals(exit.process) || exit.timestamp < birth.startWallMs
                    || !Arrays.equals(birth.summary(),exit.summary) || exit.reason == 0) continue;
            if (selected == null || exit.timestamp > selected.timestamp) selected = exit;
        }
        return selected;
    }

    static void requireClean(ProcessBirth birth, Exit exit, String process, String log, long generation) {
        if (exit == null || select(birth,process,java.util.Collections.singletonList(exit)) != exit
                || exit.reason != 1 || exit.status != 0)
            throw new AssertionError("current process exit is not EXIT_SELF/status0: " + birth.json()
                + (exit == null ? " no matching record" : " reason=" + exit.reason + " status=" + exit.status + " timestamp=" + exit.timestamp));
        if (generation <= 0) throw new AssertionError("no final surface generation");
        Pattern header = Pattern.compile("(?m)^\\[s2a\\] Android 会话日志：session=[0-9]+ pid=" + birth.pid + "(?: uuid=\\S+)? file=");
        if (!header.matcher(log).find()) throw new AssertionError("host log PID mismatch");
        Matcher gpuExit = Pattern.compile("(?m)^\\[s2a\\] GPU Run 退出(?:（拆线程）)?：").matcher(log);
        int gpu = -1;
        while (gpuExit.find()) gpu = gpuExit.start();
        int done = log.lastIndexOf("[s2a] 优雅退出完成（");
        Matcher destroyed = Pattern.compile("(?m)^\\[WSI\\] destroyed serial=[0-9]+ gen=" + generation
            + " .*poolCollected=True(?:\\r?$)").matcher(log);
        int retired = -1;
        while (destroyed.find()) retired = destroyed.start();
        if (gpu < 0 || retired < gpu || done < retired) throw new AssertionError("host does not prove final GPU/WSI retirement and clean completion");
        if (log.contains("Android fatal signal") || log.contains("FailFast") || log.contains("Unhandled exception")
                || log.contains("保留服务资源并失败退出") || log.contains("进程失败退出"))
            throw new AssertionError("host contains a terminal failure");
    }
}
