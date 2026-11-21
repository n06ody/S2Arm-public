# SPDX-License-Identifier: Apache-2.0
# 门禁手动：当前跑红（适配层与 thread.c 结构体字段 pins / paused_ack 重复），修后接线；见 #1309
# Copyright 2026 S2Arm contributors
"""Darwin pthread 协议适配器，不是 Android/Bionic 执行。
直接编译真实 Android 普通上下文的暂停/SVC/条件等待函数体；
宿主 ABI、信号入口及 VM park 是明确的夹具边界。"""
import argparse, json, os, pathlib, platform, shutil, subprocess, re
ROOT=pathlib.Path(__file__).resolve().parents[3]
FAULT=pathlib.Path('android/native/runtime/fault')
def function(text,signature):
    lo=text.index(signature); start=text.index('{',lo); depth=1; end=start+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[lo:end]
ANDROID_MEMBERS=[('paused_ack','_Atomic int paused_ack'),('vm_ack','_Atomic unsigned vm_ack'),('vm_wait_safe','int vm_wait_safe'),
    ('pins','int pins'),('trace_pause_conditions','unsigned trace_pause_conditions'),('pause_sig_seen','_Atomic unsigned pause_sig_seen'),
    ('pause_sig_native','_Atomic unsigned pause_sig_native'),('trace_pause','uint64_t trace_pause')]
def must(text,old,new):
    # every rewrite has to land: a pattern that stopped matching silently compiled the Apple body instead and tested nothing (#1309)
    if old not in text: raise SystemExit(f'adapter: pattern no longer in thread.c: {old[:70]!r}')
    return text.replace(old,new,1)
def apple_members(text):
    # thr_t body as an Apple build sees it: drop `#if !defined(__APPLE__)` blocks and the `#else` half of `#if defined(__APPLE__)` (the only two forms it uses)
    body=text[text.index('typedef struct thr {'):text.index('} thr_t;')]; keep=[]; stack=[]
    for line in body.splitlines():
        d=line.strip()
        if d.startswith('#if'): stack.append(not ('!defined(__APPLE__)' in d)); continue
        if d.startswith('#else'): stack[-1]=not stack[-1]; continue
        if d.startswith('#endif'): stack.pop(); continue
        if all(stack): keep.append(line.split('//')[0])
    return set(re.findall(r'[A-Za-z_]\w*',' '.join(keep)))
def adapt(root,out):
    tree=out/'adapter'; tree.mkdir(parents=True,exist_ok=True)
    files=[*root.joinpath('runtime/include').glob('*.h'),*root.joinpath('runtime/src').glob('*.h'),
           root/'runtime/src/thread.c',root/'runtime/src/thread_wait_diag.inc',
           root/'runtime/tests/t_thread.c',root/'platform/plat.h',root/'graphics/track/s2a_track.h',
           root/FAULT/'tests/pause_wait_test.inc']
    for p in files:
        if not p.exists():
            alternative=ROOT/p.relative_to(root)
            if alternative.exists():p=alternative
            else:continue
        q=tree/(p.relative_to(root) if root in p.parents else p.relative_to(ROOT));q.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,q)
    inc=(root/FAULT/'thread_android.inc').read_text(); text=(root/'runtime/src/thread.c').read_text()
    android_condition=function(inc,'static int android_condition_wait(')
    android_barrier=function(inc,'static void android_vm_barrier(')
    if 'static void android_vm_barrier_poll(' in inc:
        android_barrier=function(inc,'static void android_vm_barrier_poll(')+'\n'+android_barrier
    extras=''
    if 'static void android_wait_pause_init(' in inc:
        extras='static pthread_once_t android_wait_pause_once=PTHREAD_ONCE_INIT;\nstatic int android_wait_pause_enabled;\n'
        for signature in ['static void android_wait_pause_init(', 'static int android_wait_pause_on(', 'static void android_wait_pause_locked(']: extras+=function(inc,signature)+'\n'
        if 'static void android_wait_pause_poll(' in inc: extras+=function(inc,'static void android_wait_pause_poll(')+'\n'
    # Android-side members the adapted bodies use; add only the ones thr_t does not already declare on Apple
    #   (thread.c gained pins and an Apple paused_ack later: re-declaring them failed to compile, #1309)
    visible=apple_members(text)
    field=''.join(f'    {decl};\n' for name,decl in ANDROID_MEMBERS if name not in visible)
    text=must(text,'    mach_port_t mport;','    mach_port_t mport;\n'+field)
    text=must(text,'#if !defined(__APPLE__)\nstatic void pause_diag_boundary_locked',
                      '#if 1\nstatic void pause_diag_boundary_locked')
    start=text.index('#if !defined(__APPLE__)\n#include "../../android/native/runtime/fault/thread_android.inc"')
    end=text.index('#endif',start)+len('#endif')
    bridge='enum { ANDROID_VM_MAX_THREADS=1024 };\nstatic _Atomic unsigned android_vm_epoch;\nstatic _Atomic(thr_t*) android_vm_owner;\nstatic _Atomic unsigned host_barrier_hits;\nstatic void core_release(thr_t*),core_acquire(thr_t*),thr_free(thr_t*);\nstatic pthread_cond_t g_pause_cv;\nstatic _Atomic int g_frz;\nuint64_t plat_now_ns(void);\nstatic void android_vm_park(thr_t *t) {\n    (void)t; atomic_fetch_add(&host_barrier_hits,1);\n    while (atomic_load(&android_vm_epoch)) usleep(100);\n}\n'
    bridge+='static void android_vm_park_poll(thr_t *t,void (*poll)(thr_t *)) { atomic_fetch_add(&host_barrier_hits,1); do { if(poll)poll(t);if(!atomic_load(&android_vm_epoch))break;usleep(100); } while(1); }\nstatic void android_futex_wake(_Atomic unsigned *word) { (void)word; }\n'
    text=text[:start]+bridge+extras+android_barrier+'\n'+android_condition+text[end:]
    old=function(text,'static int thread_condition_wait(')
    text=must(text,old,'static int thread_condition_wait(thr_t *t,pthread_cond_t *c,const struct timespec *d) { return android_condition_wait(t,c,d); }')
    # the Apple thr_pause: its `#if defined(__APPLE__)` is the nearest one above (a comment block may sit in between since the P1-49 rework)
    at=text.index('static uint32_t thr_pause('); guard=text.rindex('#if defined(__APPLE__)\n',0,at)
    if '#endif' in text[guard:at] or '#else' in text[guard:at]: raise SystemExit('adapter: Apple thr_pause guard not found')
    text=text[:guard]+'#if 0\n'+text[guard+len('#if defined(__APPLE__)\n'):]
    text=must(text,'#if !defined(__APPLE__)\n    case 0x32:', '#if 1\n    case 0x32:')
    # Android timed waits sleep in thread_condition_wait (= android_condition_wait here); the Apple kqueue timer path (d31766f46) never sets vm_wait_safe
    text=must(text,'#if defined(__APPLE__)\n    // 带超时：在本线程 kqueue','#if 0\n    // 带超时：在本线程 kqueue')
    free=function(text,'static void thr_free(thr_t *t) {')
    text=text.replace(free,free.replace('#if !defined(__APPLE__)','#if 1'))
    park=function(text,'static void pause_park(thr_t *t) {')
    text=text.replace(park,park.replace('#if !defined(__APPLE__)','#if 1'))
    (tree/'runtime/src/thread.c').write_text(text)
    p=tree/'runtime/src/thread_wait_diag.inc'
    if p.exists():p.write_text(p.read_text().replace('#if !defined(__APPLE__)','#if 1'))
    unit='#define S2A_UNIT_TEST\n#include "runtime/src/thread.c"\n#define main unused_thread_main\n#include "runtime/tests/t_thread.c"\n#undef main\nuint64_t plat_now_ns(void) { return now_ns(); }\nstatic void host_native_signal(int s) {\n    (void)s; thr_t *t=pthread_getspecific(g_keythr); if (!t) return;\n    atomic_fetch_add(&t->pause_sig_seen,1); atomic_fetch_add(&t->pause_sig_native,1);\n    atomic_store(&t->preempt_req,1); atomic_store(&t->preempting,0);\n}\n#include "android/native/runtime/fault/tests/pause_wait_test.inc"\nint main(int argc,char **argv) {\n    if (argc>1 && !strcmp(argv[1],"on")) setenv("S2A_WAIT_DIAG","1",1);\n    else unsetenv("S2A_WAIT_DIAG");\n    setenv("S2A_CORES","0",1);\n    pthread_key_create(&g_key18,NULL);pthread_key_create(&g_keytls,NULL);\n    pthread_key_create(&g_keytlr,NULL);pthread_key_create(&g_keygate,NULL);\n    s2a_thread_main_init(); signal(SIGURG,host_native_signal);\n    return pause_wait_suite();\n}\n'
    unit=unit.replace('#include "runtime/src/thread.c"','#include <pthread.h>\nstatic int pw_pthread_cond_signal(pthread_cond_t *);\nstatic int pw_pthread_mutex_lock(pthread_mutex_t *);\nstatic int pw_pthread_cond_wait(pthread_cond_t *,pthread_mutex_t *);\nstatic int pw_pthread_cond_timedwait(pthread_cond_t *,pthread_mutex_t *,const struct timespec *);\n#define pthread_mutex_lock pw_pthread_mutex_lock\n#define pthread_cond_signal pw_pthread_cond_signal\n#define pthread_cond_wait pw_pthread_cond_wait\n#define pthread_cond_timedwait pw_pthread_cond_timedwait\n#include "runtime/src/thread.c"\n#undef pthread_mutex_lock\n#undef pthread_cond_signal\n#undef pthread_cond_wait\n#undef pthread_cond_timedwait\n',1)
    unit=unit.replace('#include \"android/native/runtime/fault/tests/pause_wait_test.inc\"', 'static int pw_epoch_parked(thr_t *t) { (void)t;return atomic_load(&host_barrier_hits)!=0; }\nstatic int pw_epoch_begin(void) { atomic_store(&host_barrier_hits,0);atomic_store(&android_vm_owner,cur()); atomic_store(&android_vm_epoch,1); return 0; }\nstatic void pw_epoch_end(void) { atomic_store(&android_vm_epoch,0); }\n#include \"android/native/runtime/fault/tests/pause_wait_test.inc\"')
    unit=unit.replace('return pause_wait_suite();',
        'if (argc>1 && !strcmp(argv[1],"scope")) return pw_finish("deadline-scope",pw_scope_suite(),0);\n'
        '    if (argc>1 && !strcmp(argv[1],"watchdog")) { pthread_t wd;\n'
        '        setenv("S2A_SCHED_DIAG","1",1);   /* 测的是空闲期按时醒（调度诊断开时才定时醒；玩家局缺省关时空闲期纯等等核者叫醒，#1842） */\n'
        '        pw_check(!pthread_create(&wd,NULL,preempt_watchdog,NULL),"actual watchdog starts");\n'
        '        while(pw_watchdog_calls<4)usleep(100);   /* 按事件等满 4 轮（不设墙钟：负载高时 1 秒转不满 4 轮，11-02 假红）；挂死交 run_pause_wait_checks.run() 的 40 秒 */\n'
        '        pw_check(pw_watchdog_calls>=4 && pw_watchdog_changes>=3,"independent watchdog deadlines may change");\n'
        '        return pw_finish("watchdog",0,0); }\n'
        '    return pw_finish("pause-waits",pause_wait_suite(),0);')
    if (root/'runtime/src/thread_wait_diag.inc').exists():unit=unit.replace(
        'return pw_finish("pause-waits",pause_wait_suite(),0);',
        'int result=pause_wait_suite(); s2a_wait_diag_report(); return pw_finish("pause-waits",result,0);')
    (tree/'test.c').write_text(unit)
    return tree

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--enable-wait-pause',action='store_true');p.add_argument('--source-root',type=pathlib.Path,default=ROOT);a=p.parse_args()
    if platform.system()!='Darwin' or platform.machine() not in ('arm64','aarch64'):p.error('Darwin ARM64 protocol adapter only; use real Android/Linux fixture on that platform')
    if ROOT==a.out.resolve() or ROOT in a.out.resolve().parents:p.error('output must be outside checkout')
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);tree=adapt(a.source_root.resolve(),out)
    env=dict(os.environ,TMPDIR=str(out),TMP=str(out),TEMP=str(out),CLANG_MODULE_CACHE_PATH=str(out/'clang'),PYTHONDONTWRITEBYTECODE='1')
    if a.enable_wait_pause: env['S2A_ANDROID_WAIT_PAUSE']='1'
    else: env.pop('S2A_ANDROID_WAIT_PAUSE',None)
    rows=[]
    for name,cmd in [('compile',['/usr/bin/clang','-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-function','-I'+str(tree/'runtime/include'),str(tree/'test.c'),'-o',str(out/'test')]),
                     ('off',[str(out/'test'),'off']),('on',[str(out/'test'),'on'])]:
        c=subprocess.run(cmd,cwd=tree,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=45)
        (out/(name+'.log')).write_bytes(c.stdout);rows.append(dict(name=name,exit=c.returncode,command=cmd,fail_rows=len(re.findall(rb'\bFAIL\b',c.stdout))))
        print(name,c.returncode,flush=True)
        if name=='compile' and c.returncode:print(c.stdout.decode());break
    (out/'results.json').write_text(json.dumps(dict(platform='Darwin adapter, not Bionic runtime',results=rows),indent=2)+'\n')
    return 0 if len(rows)==3 and all(x['exit']==0 and not x['fail_rows'] for x in rows) else 1
if __name__=='__main__':raise SystemExit(main())
