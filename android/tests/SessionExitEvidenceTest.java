package org.s2arm.android;

import java.util.List;

public final class SessionExitEvidenceTest {
    private static int checks;
    private static void check(boolean yes, String why) { checks++; if (!yes) throw new AssertionError(why); }
    private static final ProcessBirth BIRTH = new ProcessBirth(42,1234,12000,100000);
    private static final String PROCESS = "fixture:game";
    private static SessionExitEvidence.Exit exit(int pid,long timestamp,String process,byte[] summary,int reason,int status) {
        return new SessionExitEvidence.Exit(pid,timestamp,process,summary,reason,status);
    }
    private static void rejected(SessionExitEvidence.Exit exit,String host,long generation,String why) {
        boolean rejected=false;
        try { SessionExitEvidence.requireClean(BIRTH,exit,PROCESS,host,generation); } catch(AssertionError e) { rejected=true; }
        check(rejected,why);
    }
    public static void main(String[] args) throws Exception {
        String stat="42 (a comm ) with spaces) S" + " 0".repeat(18) + " 1234 0";
        check(ProcessBirth.startTicks(stat,42)==1234,"stat field22 with parentheses/spaces in comm");
        check(ProcessBirth.parse(BIRTH.json()).startTicks==BIRTH.startTicks,"birth JSON round trip");
        ProcessBirth oldBirth = new ProcessBirth(42,1000,10000,90000);
        var old = exit(42,95000,PROCESS,oldBirth.summary(),1,134);
        var fresh = exit(42,101000,PROCESS,BIRTH.summary(),1,0);
        check(SessionExitEvidence.select(BIRTH,PROCESS,List.of(old))==null,"same PID stale record must not end asynchronous wait");
        check(SessionExitEvidence.select(BIRTH,PROCESS,List.of(old,fresh))==fresh,"fresh record after delayed publication wins");
        check(SessionExitEvidence.select(BIRTH,PROCESS,List.of(fresh,old))==fresh,"selection independent of returned order");
        check(SessionExitEvidence.select(BIRTH,PROCESS,List.of(exit(42,101001,PROCESS,oldBirth.summary(),1,0)))==null,"same PID wrong start rejected even if clean/new timestamp");
        check(SessionExitEvidence.select(BIRTH,PROCESS,List.of(exit(42,99999,PROCESS,BIRTH.summary(),1,0)))==null,"pre-birth timestamp rejected");
        check(SessionExitEvidence.select(BIRTH,PROCESS,List.of(exit(43,101001,PROCESS,BIRTH.summary(),1,0)))==null,"wrong PID rejected");
        check(SessionExitEvidence.select(BIRTH,PROCESS,List.of(exit(42,101001,"fixture",BIRTH.summary(),1,0)))==null,"library process rejected");
        check(SessionExitEvidence.select(BIRTH,PROCESS,List.of(exit(42,101001,PROCESS,null,1,0)))==null,"missing identity summary rejected");
        check(SessionExitEvidence.select(BIRTH,PROCESS,List.of(exit(42,101001,PROCESS,BIRTH.summary(),0,0)))==null,"pending classification cannot complete wait");
        String log="[s2a] Android 会话日志：session=1 pid=42 file=/private/host.log\n[s2a] GPU Run 退出：cancel=True\n"
            + "[WSI] destroyed serial=10 gen=5 rev=3 mode=EnabledPresentFence poolCollected=True\n[s2a] 优雅退出完成（0.164 秒）\n";
        SessionExitEvidence.requireClean(BIRTH,fresh,PROCESS,log,5); check(true,"current exit plus final generation retirement passes");
        String logUuid = log.replace("pid=42 file=","pid=42 uuid=9f3e-run file=");
        SessionExitEvidence.requireClean(BIRTH,fresh,PROCESS,logUuid,5); check(true,"uuid 字段的新首行同样认");
        rejected(fresh,logUuid.replace("pid=42 ","pid=43 "),5,"uuid 首行错 PID 照样拒");
        SessionExitEvidence.requireClean(BIRTH,fresh,PROCESS,log.replace("GPU Run 退出：","GPU Run 退出（拆线程）：")
            + "[s2a] GPU Run 退出后：排空 FIFO\n",5); check(true,"threaded exit marker accepted without confusing later timing summary");
        rejected(old,log,5,"old failure cannot be mistaken for current exit");
        rejected(exit(42,101000,PROCESS,BIRTH.summary(),1,125),log,5,"clean host cannot override a failed current exit");
        rejected(exit(42,101000,PROCESS,BIRTH.summary(),4,0),log,5,"status0 without EXIT_SELF rejected");
        rejected(fresh,log.replace("优雅退出完成","收局开始"),5,"missing completion cannot be clean");
        rejected(fresh,log.replace("poolCollected=True","poolCollected=False"),5,"unproven pool retirement rejected");
        rejected(fresh,log,6,"old surface generation retirement insufficient");
        rejected(fresh,log.replace("pid=42 ","pid=43 "),5,"wrong host PID rejected");
        rejected(fresh,log+"[s2a] 进程失败退出\n",5,"terminal failure cannot be overridden by earlier success");
        System.out.println("Session exit evidence: " + checks + " checks PASS");
    }
}
