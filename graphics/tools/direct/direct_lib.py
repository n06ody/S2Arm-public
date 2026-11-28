#!/usr/bin/env python3
"""直连着色器库出包（双轨 §12.3，着色器线 10-24）：游戏原始输入 → 两边抽取合并 → 直连离线链出直连库，布局照直连离线链原样。
用法：python3 graphics/tools/direct/direct_lib.py <产物根（含 romfs/ exefs/main）> <输出目录>
  输出目录 = <资源包>/shaders/direct：b-<链版本>/lib.idx + lib.NNN.pak（iOS 库容器，lib_pack.py；内含各键 _b.metallib / _b.json / _bf / _bv / _bg*）、shader_map.tsv（并集）、
  shader_facts.tsv、shader_pairs.tsv、bf_want.tsv、pso_manifest.tsv（经 pack/stage_direct.sh 时只有配置档维护过的，零配置的由 pack/stage_direct_seed.sh 另写）、c1_full.tsv、
  cb_extent.tsv、keys.tsv（实际出成的键，一行一个，并集里出库失败的不列）；另出 ps_inputs.tsv（片元键 → 读的 user(locn)，兜底现编补顶点输出用）与 key_map.tsv
  （S2Arm 键 → 直连键，pack/stage_direct.sh 出 seen 用的客体代码表时换键）、key_alias.tsv（第 1 代 → 第 2 代着色器键，宿主读档与读本机管线记录时换键）与 direct_lib.json（读数）。
为什么是 Python（11-20）：原先是 bash 脚本，Windows 出包机用随包的 Git Bash（MSYS2）跑；MSYS2 的 fork 模拟在部分 Windows 环境（强制 ASLR、注入、DLL 基址冲突）下
  起第一个子进程就失败（dofork … 0xC0000142），整单失败、出包机这边修不了。现在 Mac / Windows / 安卓出包都由本文件直接起原生进程（抽取器、编译器、Python 子工具），不经 shell。
步骤：① 直连抽取 shader_split：在配置档 graphics/nvn/games/<程序号> 的副本里重出包目录与着色器表（原配置档不动）② S2Arm nxsh_extract
  ③ merge_extract：S2Arm 独有键补进直连包目录与表 ④ dsh_batch build（C# 一个进程：翻译、逐键收尾、变体、编批库、两张表、发布打包）出 Mac + iOS 库；Mac 那份只留缓存
  ⑤ 收表、出 keys.tsv。
缓存：S2A_DIRECT_CACHE（缺省 <运行根>/s2arm/cache/direct/<程序号>）只放两样：内容寻址仓 store/（translator/dmsh_store.py：256 个分片文件，
  着色器翻译结果、批库、变体、Metal 编译结果都在里面，整份出库结束只留本次用到的条目）与上次出库的日志 logs/。10-31 出包线深修：原先按键落小文件
  （mac/b-<链>、ios/b-<链> 两代 + cas/shader、index、variant…），马车8 一款 66 万个，出库时盘每秒上万次读写、CPU 闲着。
逐键产物只在本次出库的临时目录里（Mac 库 / work 写 <名>.stage.<进程号>，输出目录写 <输出>.stage.<进程号>）：出完核验齐全、打成容器，
  只把容器原子换进输出目录（renamex_np RENAME_SWAP）；中途停下输出目录逐字节不动，临时目录下次起跑时按进程号已不在清掉
S2A_DIRECT_TARGETS=ios：只出 iOS 库（Mac 的 metal -c / metallib 一律不跑，Mac 那份只有 _b.metal / _b.json）；不设 = 两份照出；mac = 只出 Mac 库（Mac 版 App）。
  运行时不读 Mac 那份（Mac 局 / Mac 版 App 读的都是随包的 shaders/direct），pack/stage_direct.sh 出 iOS 包缺省设 ios，出 Mac 版 App 缺省设 mac。
S2A_DIRECT_TARGETS=android（#783，发布目标 S2A_DIRECT_PUBLISH=android）：只出离线 Vulkan 库 <输出目录>/b-<链>-vk/lib.idx + lib.NNN.pak（graphics/tools/direct/vklib 的 dsh_vklib 出），
  不编 Metal、不出 Metal 专用的变体请求表 / 管线种子表。抽取只用产物根里抽取阶段的 extract/，没有就按内容缺口退 3；Vulkan / OpenGL 款安卓运行时自己编，退 0 不出库
配置由 game.py dir 选择，支持既有 DM_GAMES_DIR/DM_GAMES_FIRST；缺档明确失败。
退出码（10-25 维护者，同抽取阶段的原则）：0 = 出成（可部分出库，末行黄、列没出成的类别与计数）；3 = 内容缺口（某支着色器翻不了、本款用了直连还没实现的结构），
  本款这次出不了直连库，打「🟡 直连缺口：<类别> <计数>」；1 = 基础设施出错（缺文件、工具 / 编译器起不来、脚本异常、超时、写盘失败）与判不清的。
环境变量：S2A_DIRECT_TARGETS / S2A_DIRECT_PUBLISH / S2A_DIRECT_CACHE / S2A_RUN_ROOT / J、S2A_CPU_BAKE、S2A_JOBS / S2A_DIRECT_KEEP / S2A_PSO_SEED_STAGE /
  S2A_RUN_KNOWLEDGE / S2A_NOSTALL_S / DSX_LIB / GLSLANG / S2A_PYTHON、S2A_PY（子工具用的 Python，缺省本解释器）/ S2A_PROGRESS / S2A_DISK_FREE_G（单测注入余量）
"""
import ctypes, ctypes.util, collections, fnmatch, glob, json, os, re, shutil, signal, subprocess, sys, time

WIN = os.name == 'nt'
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'tools'))
from progress import Ticker  # noqa: E402
import pool_workers  # noqa: E402

T_START = time.monotonic()


def secs():   # 同 bash SECONDS：本进程起跑以来的整秒
    return int(time.monotonic() - T_START)


def fwd(p):   # Windows 一律写 R:/a/b（原生程序与日志都认，同原先 cygpath -m）
    return p.replace('\\', '/') if WIN else p


def say(msg, tag='直连库'):
    print(f'[{tag}] {msg}', flush=True)


class Exit(Exception):
    def __init__(self, code): super().__init__(code); self.code = code


def die(msg=None, code=1):
    if msg is not None: say(msg)
    raise Exit(code)


RED_LINES, RED_BYTES = 20, 1600   # 报红行里带的子命令原文：至多 20 行、按字节封顶（UTF-8 按字节截、不截半个字）


def tail_of(text, n=RED_LINES, cap=RED_BYTES):
    """子命令输出末尾的非空行（至多 n 行）连成一行（行间「 ⏎ 」），超过 cap 字节只留最后 cap 字节"""
    ls = [l.rstrip() for l in (text or '').replace('\r\n', '\n').replace('\r', '\n').split('\n') if l.strip()][-n:]
    t = ' ⏎ '.join(ls)
    b = t.encode('utf-8')
    return t if len(b) <= cap else '…' + b[-cap:].decode('utf-8', errors='ignore')


def cmd_name(cmd):
    """命令的名字：跳过解释器与无进展看门狗（nostall.py … -- <命令>），取脚本名（.py）或可执行文件名"""
    cmd = [str(c) for c in cmd]
    if any(os.path.basename(c) == 'nostall.py' for c in cmd) and '--' in cmd: cmd = cmd[cmd.index('--') + 1:]
    if len(cmd) > 2 and cmd[1] == '-c': return os.path.basename(cmd[0]) + ' -c'
    for c in cmd[1:] if len(cmd) > 1 and os.path.basename(cmd[0]).lower().startswith('python') else cmd[:1]:
        if c.endswith('.py') or c == cmd[0]: return os.path.basename(c)
    return os.path.basename(cmd[0]) if cmd else '?'


def red(msg, cmd=None, rc=None, text='', log=None):
    """报红退出（退 1）：一行带上失败的子命令名、退出码和它的输出末尾原文（玩家诊断只收这一行：原文在路径前，脱敏把路径一直抹到行尾）"""
    line = '🔴 ' + msg
    if cmd is not None: line += f'：{cmd_name(cmd)} 退出码 {rc}'
    t = tail_of(text)
    if t: line += '：' + t
    if log: line += f'（{log}）'
    die(line)


def log_text(p):
    try: return read_text(p)
    except OSError as e: return f'（日志读不了：{e}）'


def read_text(p):
    with open(p, 'rb') as f: return f.read().decode('utf-8', errors='replace')


def lines_of(p):
    """文件的行（不带换行符；同 grep / awk 的行：末尾没有换行的半行也算一行）；读不了 → None"""
    try: t = read_text(p)
    except OSError: return None
    if t.endswith('\n'): t = t[:-1]
    return t.split('\n') if t else []


def last_line(p):   # = $(tail -1 <文件>)
    ls = lines_of(p)
    return ls[-1] if ls else ''


def wc_l(p):   # = wc -l < <文件>：数换行符
    with open(p, 'rb') as f: return f.read().count(b'\n')


def grep_last(p, rx):   # = grep -a <rx> <文件> | tail -1（没有 = ''）
    r = re.compile(rx)
    hit = ''
    for l in lines_of(p) or []:
        if r.search(l): hit = l
    return hit


def rmtree(p):
    """= rm -rf：只读位（Windows）先去掉再删；删不掉的照原先 rm -rf 一样不停（收尾清理）"""
    def unlock(fn, q, _):
        try: os.chmod(q, 0o700 if os.path.isdir(q) else 0o600); fn(q)
        except OSError: pass
    if os.path.isdir(p) and not os.path.islink(p): shutil.rmtree(p, onerror=unlock)
    elif os.path.lexists(p):
        try: os.unlink(p)
        except OSError: pass


def rm_bg(p):
    """后台删（不等）：同原先 nohup rm -rf … &。起一个脱离本进程的 Python，本进程退出也删完"""
    code = 'import shutil, sys; shutil.rmtree(sys.argv[1], ignore_errors=True)'
    kw = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if WIN: kw['creationflags'] = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    else: kw['start_new_session'] = True
    subprocess.Popen([sys.executable, '-c', code, p], **kw)


def alive(pid):
    """进程还在？（Windows 上 os.kill(pid, 0) 是发 Ctrl+C，不能拿来探活：按句柄读退出码）"""
    if pid <= 0: return False
    if WIN:
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        k.OpenProcess.restype = ctypes.c_void_p
        k.CloseHandle.argtypes = [ctypes.c_void_p]
        k.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        h = k.OpenProcess(0x1000, False, pid)
        if not h: return ctypes.get_last_error() == 5   # 拒绝访问 = 在
        code = ctypes.c_ulong()
        ok = k.GetExitCodeProcess(h, ctypes.byref(code)); k.CloseHandle(h)
        return bool(ok) and code.value == 259   # STILL_ACTIVE
    try: os.kill(pid, 0)
    except ProcessLookupError: return False
    except PermissionError: return True
    except OverflowError: return False
    return True


def run(cmd, log=None, mode='w', env=None, cwd=None, capture=False, err=None):
    """起原生进程。log 给了：标准输出 + 错误都进它（同 > log 2>&1）；capture：取标准输出（同 $(…)，去掉末尾换行），返回 (rc, 文本)；否则继承本进程的输出"""
    sys.stdout.flush(); sys.stderr.flush()
    cmd = [str(c) for c in cmd]
    try:
        if log is not None:
            with open(log, mode + 'b') as f:
                return subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=cwd).returncode
        if capture:
            ef = open(err, 'wb') if err else None
            try: p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=ef or subprocess.PIPE, env=env, cwd=cwd)
            finally:
                if ef: ef.close()
            run.err = '' if ef else p.stderr.decode('utf-8', errors='replace')   # 标准错误照转出去，另留一份给报红行带原文（red）
            if run.err: sys.stderr.write(run.err); sys.stderr.flush()
            return p.returncode, p.stdout.decode('utf-8', errors='replace').replace('\r\n', '\n').rstrip('\n')   # Windows 原生程序按 CRLF 写标准输出（同 Git Bash 的 $(…) 去掉 CR）
        return subprocess.run(cmd, env=env, cwd=cwd).returncode
    except OSError as e:   # 起不来（同 shell 的 126 / 127）
        print(f'{cmd[0]}: {e}', file=sys.stderr, flush=True)
        run.err = f'{cmd[0]}: {e}'
        if log is not None:
            try:
                with open(log, 'ab') as f: f.write((run.err + '\n').encode('utf-8'))
            except OSError: pass
        return (127, '') if capture else 127


run.err = ''


def which(name):
    return shutil.which(name) if name else None


# ---- 失败分类（原 gap / rc127_note / fail_why；单测直接调这几个函数）----
GAP_RED = re.compile(r'^\[RED\] ([0-9a-f]{16}: (vertex shader of shader type 0 without a VertexB part|compute code area)|vertex shader without a main0 main function)')


def gap(log, step):
    """只认确定的内容缺口形状，认出就打黄、退 3，认不出就返回（调用方照旧报红）：
    · 有 Python 异常栈 → 判不清（脚本异常）
    · NVN 出库（dsh_batch build）是部分出库：单支翻不了 / 编不过记进 _b_build.json 与 direct_fail.log，库照出；只有三种本链不支持的 NVN 结构整份停住（Build.cs 收尾那三行）
    · Vulkan 离线翻译（vk_shaders.py）v0.3.1 起也逐支剔除、照出其余；仍整份停的只剩带那支路径的红行「[RED] /<路径>: <原因>」"""
    ls = lines_of(log) or []
    if any(l.startswith('Traceback ') for l in ls): return
    hit = [l for l in ls if GAP_RED.search(l)]
    l = hit[-1] if hit else ''
    if not l:
        red = [x for x in ls if x.startswith('[RED] ')]
        l = red[-1] if red else ''
        if not re.match(r'\[RED\] /[^:]+: .+', l): l = ''
    if not l: return
    l = l[len('[RED] '):]
    why = l.split(': ', 1)[1] if ': ' in l else l
    k = '-' if why == l else l.split(': ', 1)[0]
    if ' (' in why: why = why.split(' (', 1)[0]
    say(f'🟡 直连缺口：{step} 翻不了 1 支（{k.rsplit("/", 1)[-1]}：{why[:120]}），现有出库不支持部分出库，本款这次走通用')
    raise Exit(3)


DISK_FULL_TEXT = re.compile(r'not enough space on the disk|磁盘空间不足|No space left on device|ENOSPC|0x80070070|ERROR_DISK_FULL', re.I)


def free_gb(d):
    v = os.environ.get('S2A_DISK_FREE_G')
    if v: return v
    try: return '%.1f' % (shutil.disk_usage(d).free / 1073741824)
    except OSError: return ''


def rc127_note(rc, log, dirs, win=WIN):
    """出库退出码 127 怎么说（只在 Windows 说）：Windows 进程退出码 0xC000007F（STATUS_DISK_FULL）的低 8 位是 127，出库程序自己异常退出 127 也是 127
    （11-09 维护方测试树诊断替换写错，被一律译成盘满，照着提示误删 37 GB 缓存）。判据：build.log 里有盘满原文 → 盘满；否则看临时目录 / 缓存所在盘余量，
    任一 < 2 GB 才说多半是盘满；余量都够 → 明说不是盘满、看 build.log 末尾；有盘量不出余量 → 两种都说、不下结论。
    原生进程拿到的是完整退出码（不再经 Git Bash 截成低 8 位），0xC000007F 与 127 同样按这条判"""
    if not win or rc & 0xFF != 127: return ''
    try: hit = DISK_FULL_TEXT.search(read_text(log))
    except OSError: hit = None
    if hit: return f'；127 = 盘满（build.log 里有盘满原文，STATUS_DISK_FULL），看 {" ".join(dirs)} 所在盘'
    gs, low, unk = '', False, False
    for d in dirs:
        g = free_gb(d)
        gs += f'{d} 剩 {g or "?"} GB、'
        if not g: unk = True
        else:
            try: low = low or float(g) < 2
            except ValueError: pass
    gs = gs[:-1]
    if unk and not low: return f'；rc=127 可能是盘满（Windows 盘满码的低 8 位），也可能是出库程序异常退出：余量量不全（{gs}），先看下面 build.log 末尾'
    if low: return f'；127 多半是盘满（STATUS_DISK_FULL 0xC000007F 低 8 位）：{gs}'
    return f'；rc=127 不是盘满（{gs}）：出库程序异常退出，看下面 build.log 末尾'


def fail_why(log):
    """出库失败的一句原因（诊断#3d03f4）：.NET 未处理异常 / 断言 / 致命错误取第一行，否则取最后一行非缩进、非本脚本进度的输出；放在路径前面——脱敏把路径一直抹到行尾"""
    ls = lines_of(log) or []
    l = next((x for x in ls if re.search(r'Unhandled exception|Assertion failed|[Ff]atal error', x)), None)
    if l is None:
        rest = [x for x in ls if x and not x[0].isspace() and not x.startswith('[直连库]')]
        l = rest[-1] if rest else ''
    return l.replace('\r', '')[:160]


# ---- 换进位（原 swap_out 里那段 Python；单测直接调 swap）----
class SwapError(Exception): pass


def swap(a, b, win=WIN):
    """临时输出目录 a 与正式输出目录 b 原子互换（macOS renamex_np RENAME_SWAP，Linux renameat2 RENAME_EXCHANGE；正式的还没有就直接改名）；
    Windows / 不支持原子互换的卷（exFAT）三步改名：旧的先挪成 <正式>.swapold（不是临时目录的名字，起跑清扫不会当垃圾删），第二步失败把它改回正式名；
    换下的旧版本最后落在临时名、由退出时的清理删（审计 P1-173）"""
    if not os.path.lexists(b): os.rename(a, b); return
    def three():
        old = b + '.swapold'
        os.rename(b, old)
        try: os.rename(a, b)
        except OSError:
            os.rename(old, b)
            raise
        os.rename(old, a)
    if win: three(); return
    c = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True)
    sw = getattr(c, 'renamex_np', None)
    if (sw(os.fsencode(a), os.fsencode(b), ctypes.c_uint(2)) if sw else c.renameat2(-100, os.fsencode(a), -100, os.fsencode(b), ctypes.c_uint(2))):
        e = ctypes.get_errno()
        if e not in (45, 95, 102): raise SwapError(f'{a} ⇄ {b}: {os.strerror(e)}')
        three()   # exFAT 等外接盘不支持原子互换（ENOTSUP / EOPNOTSUPP）


# 随库发布的配置档副本表（有就收进输出目录）：fps_l3_results.tsv = L3 判定结果表（名单线产出，宿主 host_adapt.m fps_l3_override 读）
TABLES = ('game.tsv', 'fps_l3_results.tsv', 'shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'bf_want.tsv', 'pso_manifest.tsv', 'c1_full.tsv', 'cb_extent.tsv',
          'shader_map.tsv.conflicts.tsv')

TMP_SYSTEM = ('/var/folders/*', '/private/var/folders/*', '/tmp', '/tmp/*', '/private/tmp', '/private/tmp/*',
              '*/AppData/Local/Temp', '*/AppData/Local/Temp/*', '*\\AppData\\Local\\Temp*')


def build_env(py, tr, w, v):
    """出库前核工具 / 链号（translator/dmsh_chain.py --build-env）→ (链号, 程序号, 输入摘要)。stdout 只收这三样；编译器版本提示走 stderr（照转到本进程 stderr），
    合流会把 note: 误读成链号。工具退非 0 → 报红；链号与开头取的不一 → 报红"""
    cmd = [py, 'dmsh_chain.py', '--build-env', w + '/profile_rows.txt']
    rc, be = run(cmd, capture=True, err=w + '/build_env.log', cwd=tr)
    if rc != 0:
        sys.stderr.write(log_text(w + '/build_env.log')); sys.stderr.flush()
        red('出库前核工具 / 链号失败', cmd, rc, log_text(w + '/build_env.log') + '\n' + be, w + '/build_env.log')
    sys.stderr.write(read_text(w + '/build_env.log')); sys.stderr.flush()
    f = (be.split('\n')[0] if be else '').split(None, 2)
    bv, bg, bi = (f + ['', '', ''])[:3]
    if bv != v: die(f'🔴 链号前后不一（{v} / {bv}）')
    return bv, bg, bi.strip()


def batch_tmp(td, w, pid):
    """dsh_batch 的临时工作目录 → (目录, 退出时要删的目录或 '')：调用方指到工作盘的沿用、建本次子目录；系统缺省临时目录 / 没设 → <工作目录>/tmp"""
    if td == '' or any(fnmatch.fnmatchcase(td, p) for p in TMP_SYSTEM): return w + '/tmp', ''
    bt = (td[:-1] if td.endswith('/') else td) + f'/dsh_batch.{pid}'
    return bt, bt


def recover(c, out):
    """起跑清扫。审计 P1-173：三步改名停在第一步之后，正式目录在 <输出>.swapold：先改回来，再清上次被停留下的临时目录（进程号已不在的）"""
    if not os.path.lexists(out) and os.path.isdir(out + '.swapold'):
        os.rename(out + '.swapold', out); say('上次换进位停在半路：已把上一份直连库改回正式位置')
    for d in sorted(glob.glob(glob.escape(c) + '/work.stage.*')) + sorted(glob.glob(glob.escape(c) + '/mac.stage.*')) + sorted(glob.glob(glob.escape(out) + '.stage.*')):
        p = d.rsplit('.', 1)[-1]
        if os.path.isdir(d) and not (p.isdigit() and alive(int(p))): rmtree(d)


def comm23(a, b):
    """= comm -23 a b（两份按行归并，只在 a 里的行）；输入未排序时与 comm 同样按归并走"""
    out, i, j = [], 0, 0
    while i < len(a):
        if j >= len(b) or a[i] < b[j]: out.append(a[i]); i += 1
        elif a[i] > b[j]: j += 1
        else: i += 1; j += 1
    return out


class Lib:
    def __init__(self, argv):
        if len(argv) != 2: die('用法：direct_lib.py <产物根（含 romfs/ exefs/main）> <输出目录>')
        if not os.path.isdir(argv[0]): die(f'🔴 {argv[0]}：产物根不是目录')
        # Windows 工作区走短根联接：不展开（同原先 pwd -L）；别处同 pwd -P
        self.ROOT = fwd(os.path.abspath(argv[0]) if WIN else os.path.realpath(argv[0]))
        self.OUT = fwd(os.path.abspath(argv[1])) if WIN else argv[1]
        self.R = fwd(os.path.normpath(os.path.join(HERE, '..', '..', '..')))
        self.NX = self.R + '/graphics/nvn'
        self.TR = self.NX + '/../../translator'
        self.PY = os.environ.get('S2A_PYTHON') or os.environ.get('S2A_PY') or sys.executable
        self.cleanup = []   # 退出时按序做
        self.ticker = None; self.ps = 0

    # 进度：出包进度协议（tools/progress.py Ticker，写 S2A_PROGRESS）：阶段「直连库」= 第几步 / 5 步；日志里每步另打一行「[直连库] 第 k/5 步 …」给人看
    def prog(self, msg):
        self.ps += 1
        say(f'第 {self.ps}/5 步 {msg}')
        if self.ticker is None: self.ticker = Ticker('直连库', 5, every=0)
        else: self.ticker.step()

    def prog_done(self):
        if self.ticker is None: self.ticker = Ticker('直连库', 5, every=0)
        self.ticker.step(5)

    def deg(self, cls, n, hint, note):
        """降级汇总（v0.3.1 拿不准不停）：离线翻译剔掉的支数写进产物根 degrade.direct.tsv（阶段 / 类别 / 处数 / 玩家提示 / 说明；出包作业收尾读它上屏、诊断包收它）"""
        new = not (os.path.isfile(self.DEG) and os.path.getsize(self.DEG) > 0)
        with open(self.DEG, 'a', encoding='utf-8', newline='\n') as f:
            if new: f.write('# 阶段\t类别\t处数\t玩家提示\t说明\n')
            f.write(f'direct\t{cls}\t{n}\t{hint}\t{note}\n')

    def nostall(self, watch, cmd, log, env=None):
        w = []
        for d in watch: w += ['--watch', d]
        return run([self.PY, self.R + '/tools/nostall.py', *w, os.environ.get('S2A_NOSTALL_S') or '600', '--', *cmd], log=log, env=env)

    def main(self):
        R, NX, PY, ROOT = self.R, self.NX, self.PY, self.ROOT
        self.DEG = ROOT + '/degrade.direct.tsv'; rmtree(self.DEG)
        if WIN:
            os.environ.update(PYTHONUTF8='1')   # Windows 的 Python 缺省按系统代码页（GBK）写标准输出，中文 / 🟢 写不出就抛错；日志一律 UTF-8，同 Mac
            # Windows 出包机（#1100 W6）：没有 Homebrew 的 spirv-cross，PATH 上没有就用仓库同一份源码（= 链锁版本）现编命令行、放进 PATH（translator/spvc_inproc.py cli）
            if not which('spirv-cross'):
                cmd = [PY, self.TR + '/spvc_inproc.py', 'cli']; rc, sc = run(cmd, capture=True)
                if rc != 0 or not sc: red('Windows 上编不出 spirv-cross 命令行（translator/spvc_inproc.py cli）', cmd, rc, run.err + '\n' + sc)
                os.environ['PATH'] = os.path.dirname(sc) + os.pathsep + os.environ.get('PATH', '')
        # 并行路数（10-25 维护者「多核要适配机器的核心数」）：J / S2A_CPU_BAKE = 本阶段分到的路数，都没给才用 S2A_JOBS / 本机核数；导出给下面每个工具
        self.J = str(pool_workers.env_jobs('J', 'S2A_CPU_BAKE', 'S2A_JOBS'))   # 都没给 → 本机核数（tools/pool_workers.cores，认 S2A_NCPU）
        os.environ['J'] = self.J
        if not ((os.path.isdir(ROOT + '/romfs') or os.path.isfile(ROOT + '/romfs.std')) and os.path.isfile(ROOT + '/exefs/main')):
            die(f'🔴 {ROOT} 缺 romfs/（或镜像 romfs.std）或 exefs/main')   # RomFS 目录或镜像都行（romfs_list.py image_of）
        self.TID = self.prog_id()
        if not self.TID: die('🔴 取不到程序号')
        # 出包分流的唯一判法（graphics/nvn/tools/gfx_api.py）
        cmd = [PY, NX + '/tools/gfx_api.py', ROOT + '/exefs', ROOT + '/romfs']; rc, API = run(cmd, capture=True)
        if rc != 0: red('判图形接口失败（graphics/nvn/tools/gfx_api.py）', cmd, rc, API.split(chr(9), 1)[-1] + '\n' + run.err)
        self.api = API.split('\t', 1)[0]
        tg = os.environ.get('S2A_DIRECT_TARGETS', '')
        if tg == 'android' and self.api in ('vulkan', 'gl'):
            say(f'🟡 {self.TID} 是 {self.api} 款：着色器是游戏自带的 SPIR-V / GLSL，安卓运行时自己编，不出直连库'); raise Exit(0)
        if self.api == 'vulkan': self.vulkan()
        if self.api == 'gl': self.opengl()
        self.nvn()

    def prog_id(self):
        """= pack/srcs.sh prog_id：unpack.tsv 的 title_id（大写）；没有就读 exefs/main.npdm（META +0x70 指的 ACI0 +0x10，tools/addr_need.py npdm_program）"""
        ids = [l.split('\t')[1].upper() for l in (lines_of(self.ROOT + '/unpack.tsv') or []) if l.split('\t')[0] == 'title_id' and '\t' in l]
        if ids: return '\n'.join(ids)
        try:
            import addr_need
            return addr_need.npdm_program(self.ROOT + '/exefs/main.npdm').upper()
        except Exception:
            return ''

    def cache_dir(self):
        return fwd(os.environ.get('S2A_DIRECT_CACHE') or ((os.environ.get('S2A_RUN_ROOT') or os.path.expanduser('~') + '/.s2arm/run') + '/s2arm/cache/direct/' + self.TID))

    def aocs(self):
        return [a[:-1] for a in sorted(glob.glob(glob.escape(self.ROOT) + '/aoc/*/')) if os.path.isdir(a)]

    # Vulkan 款（main 直接导入 vk*：哈迪斯、超级马力欧银河 1 / 2）：直连走直连层的 Vulkan 层（graphics/nvn/gfx/vk/hle_vk.m），着色器是 RomFS 里的 SPIR-V，
    #   出包时用 vk_shaders.py（spirv-cross）离线翻成一个库：<输出目录>/vk/vk_shaders.metallib + vk_shaders.tsv（运行时 <DM_MSL_DIR>/vk/）。
    #   不要配置档，按魔数扫 RomFS + 追加内容；keys.tsv 列 SPIR-V 键；运行时生成 SPIR-V 的款（银河）出包不起局，只出表头、全部设备首遇现编
    def vulkan(self):
        R, NX, PY, ROOT, OUT, TID = self.R, self.NX, self.PY, self.ROOT, self.OUT, self.TID
        if not which('spirv-cross'): die('🔴 Vulkan 款要 spirv-cross（brew install spirv-cross）')
        C = self.cache_dir(); os.makedirs(C, exist_ok=True); os.makedirs(OUT, exist_ok=True); s = secs()
        # 出库期间在缓存目录放占用标记 vk.stage.<本进程号>（清理器 stage_live 认：进程活着就不删这款缓存）
        mark = f'{C}/vk.stage.{os.getpid()}'; open(mark, 'w').close(); self.cleanup.append(lambda: rmtree(mark))
        DIRS = [ROOT + '/romfs'] + self.aocs()
        # RomFS / 追加内容里一个 SPIR-V 都没有 = 游戏运行时才生成（银河 1 / 2）；文件头取 RomFS 清单（graphics/nvn/tools/romfs_list.py，仓库里唯一的 RomFS 遍历）
        sys.path.insert(0, NX + '/tools')
        import romfs_list
        nospv = not any(e.error is None and e.head[:4] == b'\x03\x02\x23\x07' for d in DIRS for e in romfs_list.heads(d).files())
        # 跑局学到的 SPIR-V（设备首遇现编时存下、收局拉回 pso_seed/<程序号>/vk_spv/，经跑局知识快照 vk_spv/）：照 RomFS 一起翻进离线库（11-05）
        KN = os.environ.get('S2A_RUN_KNOWLEDGE', '')
        if not KN:
            KN = C + '/knowledge'
            cmd = [PY, R + '/graphics/tools/direct/run_knowledge.py', KN, TID]; rc = run(cmd, log=C + '/run_knowledge.log')
            if rc != 0: red('跑局知识快照出不来', cmd, rc, log_text(C + '/run_knowledge.log'), C + '/run_knowledge.log')
        try: nrt = sum(1 for n in os.listdir(KN + '/vk_spv') if not n.startswith('.') and n.endswith('.spv'))
        except OSError: nrt = 0
        if nrt: DIRS.append(KN + '/vk_spv')
        vlog = C + '/vk_shaders.log'
        if nospv and not nrt:
            # 出包不起局（11-05 维护者）：只出表头、没有库，keys.tsv 为空，全部交设备首遇现编（hle_vk.m → vk_rt.inc 后台编、编好持久化）
            cmd = [PY, self.TR + '/vk_shaders.py', '--header-only', C + '/vkmac', '--ios', OUT]; rc = run(cmd, log=vlog)
            if rc != 0: red('Vulkan 表头没写出', cmd, rc, log_text(vlog), vlog)
            if not nonempty(OUT + '/vk/vk_shaders.tsv'): red(f'没出 {OUT}/vk/vk_shaders.tsv（vk_shaders.py 退 0）', text=log_text(vlog), log=vlog)
            open(OUT + '/keys.tsv', 'w').close()
            say(f'🟡 Vulkan 款 {TID}：RomFS / 追加内容里没有 SPIR-V（游戏运行时生成），跑局知识里也没有，出包不起局，只出表头、全部设备首遇现编（vk/vk_shaders.tsv），{secs() - s} 秒')
            raise Exit(0)
        if nrt: say(f'Vulkan 款 {TID}：跑局知识里有设备现编存下的 SPIR-V {nrt} 支，照 RomFS 一起翻进离线库')
        cmd = [PY, self.TR + '/vk_shaders.py', *DIRS, C + '/vkmac', '--ios', OUT]; vrc = self.nostall([C + '/vkmac', OUT], cmd, vlog)
        if vrc != 0:
            if vrc == 1: gap(vlog, 'Vulkan 着色器')
            red(f'Vulkan 着色器离线翻译失败 rc={vrc}', cmd, vrc, log_text(vlog), vlog)
        # 单支翻不了 / Metal 编不过的已剔掉（vk_shaders.py，v0.3.1），汇总行「Vulkan 汇总：成功 N、失败 M」：M > 0 记降级；N = 0 只出表头、全部设备首遇现编
        ok, bad = summary(vlog, 'Vulkan')
        if bad is not None and bad > 0:
            self.deg('vk_failed', bad, f'这款有 {bad} 个着色器没能提前编好，用到时会现场编译、可能卡一下，遇到请反馈', f'Vulkan 离线翻译剔掉的支（{vlog}）')
        if ok == 0:
            if not nonempty(OUT + '/vk/vk_shaders.tsv'): red(f'没出 {OUT}/vk/vk_shaders.tsv（vk_shaders.py 退 0）', text=log_text(vlog), log=vlog)
            open(OUT + '/keys.tsv', 'w').close()
            say(f'🟡 Vulkan 款 {TID}：离线一支没编成（失败 {"" if bad is None else bad}），只出表头、全部设备首遇现编，{secs() - s} 秒'); raise Exit(0)
        # 两个目标各剔各的（审计 P1-176）：别的目标编成、这个目标一支不剩时只有表头没有库，照出、keys.tsv 空
        n = self.keys_from(OUT + '/vk/vk_shaders.tsv')
        if n and not nonempty(OUT + '/vk/vk_shaders.metallib'): red(f'表里有 {n} 支却没出 {OUT}/vk/vk_shaders.metallib', text=log_text(vlog), log=vlog)
        say(f'Vulkan 款 {TID}：SPIR-V {n} 支 → vk/vk_shaders.metallib {du_h(OUT + "/vk/vk_shaders.metallib")}，{secs() - s} 秒（{last_line(vlog)[:120]}）')
        raise Exit(0)

    def keys_from(self, tsv):   # = grep -v '^#' <表> | cut -f1 | sort -u > keys.tsv；返回行数
        ks = sorted({l.split('\t', 1)[0] for l in lines_of(tsv) or [] if not l.startswith('#')})
        with open(self.OUT + '/keys.tsv', 'w', encoding='utf-8', newline='\n') as f: f.write(''.join(k + '\n' for k in ks))
        return len(ks)

    # OpenGL 款（main 导入 eglCreateWindowSurface、取 glShaderSource、带 GLSL：数码宝贝物语）：直连走 GL 层（graphics/nvn/gfx/gl），着色器是游戏运行时编的 GLSL 源码，
    #   出包时用 gl_shaders.py（glslang → SPIR-V → spirv-cross）把 RomFS（含 MVGL 归档）、追加内容与 main 里能找到的源码离线翻成一个库：<输出目录>/gl/gl_shaders.metallib + .tsv
    #   （运行时 <DM_MSL_DIR>/gl/）。不要配置档；keys.tsv 列 GLSL 键；运行时才拼出来的源码：宿主首遇现场翻译（gl_prog.m）
    def opengl(self):
        R, PY, ROOT, OUT, TID = self.R, self.PY, self.ROOT, self.OUT, self.TID
        # 与 gl_shaders.py 的 os.environ.get 一致：只有未设置 GLSLANG 才回退 PATH，空值不能回退
        if not which(os.environ.get('GLSLANG', 'glslangValidator')): die('🔴 OpenGL 款要 glslangValidator（brew install glslang spirv-cross）')
        if not which('spirv-cross'): die('🔴 OpenGL 款要 spirv-cross（brew install glslang spirv-cross）')
        C = self.cache_dir(); os.makedirs(C, exist_ok=True); os.makedirs(OUT, exist_ok=True); s = secs()
        mark = f'{C}/gl.stage.{os.getpid()}'; open(mark, 'w').close(); self.cleanup.append(lambda: rmtree(mark))   # 同 Vulkan 分支：出库期间的占用标记
        INS = [ROOT + '/romfs', ROOT + '/exefs/main'] + self.aocs()
        glog = C + '/gl_shaders.log'
        gcmd = [PY, self.TR + '/gl_shaders.py', *INS, C + '/glmac', '--ios', OUT]; grc = self.nostall([C + '/glmac', OUT], gcmd, glog)
        # gl_shaders.py 翻成的照写库、有翻不了的退 1，汇总行「GLSL 汇总：成功 N、失败 M」：N > 0 = 部分出库（照常出、末行黄）；成功 0 或别的退出码 = 判不清，报红
        ok, bad = summary(glog, 'GLSL'); miss = 0
        if grc == 0 or (grc == 1 and ok is not None and ok > 0): miss = bad if bad is not None and bad >= 0 else 0
        else: red(f'GLSL 着色器离线翻译失败 rc={grc}', gcmd, grc, log_text(glog), glog)
        # 离线一支没有（GameMaker 等引擎运行时才拼 GLSL，#518）：照出，只有表头、没有库，keys.tsv 为空；宿主库里没有的源码首遇时现场翻译
        if miss > 0:
            self.deg('gl_failed', miss, f'这款有 {miss} 个着色器没能提前编好，用到时会现场编译、可能卡一下，遇到请反馈', f'GLSL 离线翻译译不过的支（{glog}）')
        if grc == 0 and ok == 0:   # 成功 0：离线一支没有（失败 0）或一支都没译成（失败 > 0，v0.3.1 起照出空表）
            if not nonempty(OUT + '/gl/gl_shaders.tsv'): red(f'没出 {OUT}/gl/gl_shaders.tsv', text=log_text(glog), log=glog)
            open(OUT + '/keys.tsv', 'w').close()
            if miss > 0: say(f'🟡 OpenGL 款 {TID}：离线一支没编成（编译失败 {miss} 支），只出表头、全部运行时首遇现编（gl/gl_shaders.tsv），{secs() - s} 秒')
            else: say(f'🟡 OpenGL 款 {TID}：离线 0 支 GLSL（运行时拼源码的引擎；没有 main 的片段记跳过），只出表头、全部运行时首遇现编（gl/gl_shaders.tsv），{secs() - s} 秒（{last_line(glog)[:120]}）')
            raise Exit(0)
        if not nonempty(OUT + '/gl/gl_shaders.tsv'): red(f'没出 {OUT}/gl/gl_shaders.tsv', text=log_text(glog), log=glog)
        n = self.keys_from(OUT + '/gl/gl_shaders.tsv')
        if n and not nonempty(OUT + '/gl/gl_shaders.metallib'): red(f'表里有 {n} 支却没出 {OUT}/gl/gl_shaders.metallib', text=log_text(glog), log=glog)   # 同 Vulkan：这个目标一支不剩时只有表头
        pre = '' if miss == 0 else f'🟡 直连缺口（部分出库）：GLSL 翻不了 {miss} 支（运行时走现编 / 兜底）；'
        say(f'{pre}OpenGL 款 {TID}：GLSL {n} 支 → gl/gl_shaders.metallib {du_h(OUT + "/gl/gl_shaders.metallib")}，{secs() - s} 秒（{last_line(glog)[:120]}）')
        raise Exit(0)

    def swap_out(self):
        """⑥ 换进位（苹果 / 安卓共用）：临时输出目录与正式输出目录原子互换，换下来的旧版本由退出时的清理删；日志留进 <缓存>/logs/"""
        try: swap(self.OS, self.OUT)
        except (OSError, SwapError) as e: red('换进位失败', text=str(e))
        C, W = self.C, self.W
        try:
            rmtree(C + '/logs.new'); os.makedirs(C + '/logs.new')
            for n in sorted(os.listdir(W)):
                if fnmatch.fnmatchcase(n, '*.log') and os.path.isfile(W + '/' + n): shutil.copy(W + '/' + n, C + '/logs.new/')
            rmtree(C + '/logs'); os.rename(C + '/logs.new', C + '/logs')
        except OSError as e:
            print(f'日志没拷全（{C}/logs）：{e}', file=sys.stderr, flush=True)

    def nvn(self):
        R, NX, PY, ROOT, TID = self.R, self.NX, self.PY, self.ROOT, self.TID
        cmd = [PY, NX + '/games/game.py', 'dir', TID]; rc, PROF = run(cmd, capture=True)
        if rc != 0: red(f'{TID} 没有可用配置档（先运行 pack/stage_direct_profile.sh）', cmd, rc, run.err + '\n' + PROF)
        if not os.path.isfile(PROF + '/game.tsv'): red(f'{TID} 配置档缺 game.tsv（game.py dir 给的是「{PROF}」）', cmd, rc, run.err)
        tg = os.environ.get('S2A_DIRECT_TARGETS', '')
        VK = 0; CK = ''
        if tg == '': NOMAC, NOIOS = 0, 0
        elif tg == 'ios': NOMAC, NOIOS = 1, 0
        elif tg == 'mac': NOMAC, NOIOS = 0, 1
        elif tg == 'android': NOMAC, NOIOS, VK = 1, 1, 1
        else: die(f'🔴 S2A_DIRECT_TARGETS={tg} 不认识（不设 = Mac + iOS 两份，ios = 只出 iOS，mac = 只出 Mac，android = 只出安卓 Vulkan 库）')
        # 发布哪一份进输出目录（#897）：S2A_DIRECT_PUBLISH=ios（缺省）/ mac（Mac 版 App；pack/stage_direct.sh 按 S2A_PACK_PLAT 定）。mac 要 Mac 那份真编了库
        PUB = os.environ.get('S2A_DIRECT_PUBLISH') or 'ios'
        if PUB == 'ios':
            if NOIOS: die(f'🔴 发布 iOS 库却没出 iOS 那份（S2A_DIRECT_TARGETS={tg}）')
        elif PUB == 'mac':
            if NOMAC: die(f'🔴 发布 Mac 库却没出 Mac 那份（S2A_DIRECT_TARGETS={tg}）')
        elif PUB == 'android':
            if not VK: die(f'🔴 发布安卓库却没出安卓那份（S2A_DIRECT_TARGETS={tg}）')
        else: die(f'🔴 S2A_DIRECT_PUBLISH={PUB} 不认识（ios / mac / android）')
        if VK and PUB != 'android': die(f'🔴 只出安卓 Vulkan 库（S2A_DIRECT_TARGETS=android）却要发布 {PUB}')
        self.C = C = self.cache_dir()
        try: os.makedirs(self.OUT, exist_ok=True)
        except OSError as e: die(f'🔴 建不了输出目录 {self.OUT}：{e}')
        self.OUT = OUT = fwd(os.path.abspath(self.OUT) if WIN else os.path.realpath(self.OUT))
        cmd = [PY, '-c', 'import dmsh_chain; print(dmsh_chain.version())']; rc, V = run(cmd, capture=True, cwd=self.TR)
        if rc != 0: red('取不到链版本', cmd, rc, run.err + '\n' + V)
        pid = os.getpid()
        # 缓存只剩内容寻址仓 $C/store 与上次的日志 $C/logs/；旧布局（mac/ ios/ work/）第一次碰到整个删掉。改名挪开是瞬间的，真删放后台
        #   （旧布局一款几十万个小文件，同步删把出库卡住半小时）。上次被停在半路留下的 .old-* 同样后台删
        for d in (C + '/mac', C + '/ios', C + '/work'):
            if os.path.lexists(d):
                os.rename(d, f'{C}/.old-{pid}-{os.path.basename(d)}'); say(f'旧布局缓存 {os.path.basename(d)}/ 已挪开、后台删（改存内容寻址仓 {C}/store）')
        for d in sorted(glob.glob(glob.escape(C) + '/.old-*')): rm_bg(d)
        os.makedirs(C + '/store', exist_ok=True); os.makedirs(C + '/logs', exist_ok=True)
        recover(C, OUT)
        self.W = W = f'{C}/work.stage.{pid}'; MS = f'{C}/mac.stage.{pid}'; self.OS = OS = f'{OUT}.stage.{pid}'
        done = [False]; self.done = done; btx = []
        # 成功：临时目录换进来的是旧版本，删；失败：只删库，work 留着看日志（下次起跑时清）
        def fin():
            rmtree(MS); rmtree(OS)
            for b in btx: rmtree(b)
            if done[0]: rmtree(W)
        self.cleanup.append(fin)
        # 输出目录里原有的东西（旧链版本的容器、表）克隆进临时输出目录；逐键产物不跨次保留。跳过同链号的旧容器 b-${V}：dsh_batch 要全量重打这份容器
        cmd = [PY, R + '/tools/clonedir.py', OUT, OS, 'b-' + V]; rc, o = run(cmd, capture=True)
        if o: print(o, flush=True)
        if rc != 0: red('建临时输出目录失败', cmd, rc, run.err + '\n' + o)
        P = f'{W}/profile/{TID}'
        os.makedirs(P); os.makedirs(W + '/pkg'); os.makedirs(MS)
        try:
            bad = []
            for n in sorted(os.listdir(PROF)):
                if n.startswith('.'): continue
                if os.path.isdir(PROF + '/' + n): bad.append(n); continue
                shutil.copy(PROF + '/' + n, P + '/')
            if bad: raise OSError(f'{"、".join(bad)} 是目录')
        except OSError as e: red('配置档复制失败', text=str(e))
        # L3 判定结果表（fps_l3_results.tsv，名单线产出）拷进配置档，随包带给宿主运行时（host_adapt.m fps_l3_override）
        if os.path.isfile(NX + '/games/fps_l3_results.tsv'): copy_soft(NX + '/games/fps_l3_results.tsv', P + '/fps_l3_results.tsv')
        # 零源跳过事实（graphics/nvn/games/zero_skip.tsv，按程序号逐款登记）写成副本 game.tsv 的 zero_skip 键，随包带给宿主（nvn_mtl.m zskip_facts_load）
        zl = lines_of(NX + '/games/zero_skip.tsv')
        if zl is None: die('🔴 读零源跳过事实表失败')
        zs = []
        for l in zl:
            c = l.split('\t')
            if c[0].upper() == TID.upper(): zs.append(':'.join((c + [''] * 5)[1:5]))
        if zs:
            with open(P + '/game.tsv', 'a', encoding='utf-8', newline='\n') as f:
                f.write('\n# 零源跳过事实（出库时取自 graphics/nvn/games/zero_skip.tsv）\nzero_skip\t' + ' '.join(zs) + '\n')
        # 只有 game.tsv 的配置档：缺的两张表补成只有表头的空表（表头照上游马力欧 010015100B514000 那份）；维护过的配置档原样不动
        if not os.path.isfile(P + '/shader_map.tsv.conflicts.tsv'):
            put(P + '/shader_map.tsv.conflicts.tsv', "# kind: evidence  control-section key conflict rulings: this game's keys already include the code area (dm/gfx/tools/shader_split.py), so one key never has two different codes; empty\ndm_ctl_sha16\tchosen_pc\tclass\tcandidates\trecords\n")
        if not os.path.isfile(P + '/bf_want.tsv'): put(P + '/bf_want.tsv', '# generated\n')
        # 管线种子表：维护过的配置档原样带；没有的零配置生成（经 pack/stage_direct.sh 时归轻阶段 pack/stage_direct_seed.sh）。「维护过」= 表里有数据行
        self.PSO_GEN = not any(l.strip() and not l.startswith('#') for l in (lines_of(P + '/pso_manifest.tsv') or []))
        os.environ.update(DM_GAMES_DIR=W + '/profile', DM_GAMES_FIRST='1', DM_GAME=TID, DM_ROOT=C, IOSOUT=OS, DMSH_NO_MAC_LIB=str(NOMAC), DMSH_NO_IOS_LIB=str(NOIOS))
        # ①② 抽取（10-25 维护者「抽着色器只做一次」）：共享抽取阶段（pack/stage_extract.sh）已经出好的 <产物根>/extract/（有 ok）就整份克隆进工作目录，不重抽；
        #   没有（单独调用 / Windows 出包 / 单测）就当场跑同一份 graphics/tools/direct/extract.py。追加内容（aoc/<序号>/）两边抽取都并进来
        AOCS = self.aocs()
        T0 = secs()
        if os.path.isfile(ROOT + '/extract/ok'):
            X = ROOT + '/extract'; self.prog(f'抽取着色器：用抽取阶段的（{read_text(X + "/ok").replace(chr(9), "=").replace(chr(10), " ")}）')
        elif VK:
            die('🟡 直连缺口：产物根里没有抽取阶段的 extract/（安卓刷新在直连库之前跑抽取阶段），这里不另抽着色器', 3)   # 文案里不写脚本路径：阶段输入闭包扫描（tools/stage_inputs.py）按字面路径认依赖
        else:
            X = W + '/x'; self.prog(f'抽取着色器（{self.J} 路）')
            xrc = self.extract(X, W + '/extract.log')
            # 退 3 = 只有直连用的那几份抽不出（运行时现编的 GLSL 没抓到输出、认不出着色器容器……）：出运行时现编空直连库、照常换进输出目录、退 0
            #   （11-06 Windows 出包机疯狂兔子 / 马力欧高尔夫）。一个 Python 进程做完清目录、出空库、换进位、拷日志
            if xrc == 3:
                cmd = [PY, R + '/graphics/tools/direct/runtime_only_lib.py', OS, V, TID, 'auto', W + '/direct_gap.txt', PROF + '/game.tsv', '--gap-from', W + '/extract.log',
                       '--swap-to', OUT, '--logs-from', W, '--logs-to', C]
                rc, o = run(cmd, capture=True)
                if o: print(o, flush=True)
                if rc != 0: red('出运行时现编空直连库失败', cmd, rc, run.err + '\n' + o)
                g = (lines_of(W + '/direct_gap.txt') or [''])[0].strip()
                done[0] = True; say(f'🟡 {TID} 离线 0 支、运行时首遇现编（{g[:200]}）'); raise Exit(0)
            if xrc != 0: red(f'抽取失败（{X}）', [self.PY, self.R + '/graphics/tools/direct/extract.py'], xrc, log_text(W + '/extract.log'), W + '/extract.log')
        # 翻译编译开工前复核盘（pack/jobs/space_rule.py，#265）：预估不够只打黄、照常开编（space_rule.py 恒退 0，不许堵门）；真写满时写盘报 ENOSPC，那才是物理停（disk_full）
        src = run([PY, R + '/pack/jobs/space_rule.py', 'direct-check', ROOT, C, OUT])
        if src != 0: say(f'🟡 直连库开工前的空间复核没跑成（pack/jobs/space_rule.py 退 {src}），照常开编；真写满时会按盘满停')
        try:
            rmtree(W + '/pkg')
            for n in ('pkg', 'split', 's2x'): copy_tree(X + '/' + n, W + '/' + n)
            for n in ('split.log', 's2x.log'): copy_file(X + '/' + n, W + '/' + n)
        except OSError as e: red(f'抽取结果克隆进工作目录失败（{X} → {W}）', text=str(e))
        # 抽取的表写临时目录，不盖配置档：着色器表用抽取的；答案表 = 配置档 + 抽取里配置档没有的键；配对表 = 两边并集。配置档里有、本次抽不到的行只报 🟡 计数
        try: PROF_ONLY = merge_tables(W + '/split', P)
        except Exception as e: red('合表失败', text=f'{type(e).__name__}: {e}')
        if PROF_ONLY: say(f'🟡 配置档里有、本次抽不到的着色器 {PROF_ONLY} 个（多为没解的追加内容，游戏没挂那份数据就用不到）')
        okv = dict(l.split('\t', 1) for l in lines_of(X + '/ok') or [] if '\t' in l)
        tx, ts = int(okv.get('s2a_extract', '0').split('\t')[0] or 0), int(okv.get('split', '0').split('\t')[0] or 0)
        if X == ROOT + '/extract': ts = tx = 0   # 用现成抽取 = 本次没花时间
        s = secs()
        self.prog('合并两边抽取')
        # ③ 并集表放回配置档副本（dmsh_build 按配置档读表、按包目录读记录）
        cmd = [PY, R + '/graphics/tools/direct/merge_extract.py', W + '/s2x', W + '/pkg', P + '/shader_map.tsv', W + '/merged']; rc = run(cmd, log=W + '/merge.log')
        if rc != 0: red('合并抽取失败', cmd, rc, log_text(W + '/merge.log'), W + '/merge.log')
        copy_soft(W + '/merged/shader_map.tsv', P + '/shader_map.tsv')
        copy_soft(W + '/merged/shader_facts.tsv', P + '/shader_facts.tsv')   # 答案表：计算着色器补齐线程组 / 共享内存（merge_extract.py）
        # 真机转储的着色器（产物根 rt_shaders/：游戏文件里抽不到、真机首遇只能现编的，DM_NVN_SHDUMP 存的 <键>.ctl / .code）：并进合并后的包与表、补进并集键清单（11-05 时之笛）
        if glob.glob(glob.escape(ROOT) + '/rt_shaders/*.code'):
            n0 = sum(1 for l in lines_of(P + '/shader_map.tsv') or [] if l)
            cmd = [PY, self.TR + '/rt_dump_merge.py', ROOT + '/rt_shaders', W + '/merged/pkg', P + '/shader_map.tsv', P + '/shader_facts.tsv']; rc = run(cmd, log=W + '/rt_dump.log')
            if rc != 0: red('真机转储着色器并不进库', cmd, rc, log_text(W + '/rt_dump.log'), W + '/rt_dump.log')
            add = [(l.split('\t')[3] if l.count('\t') >= 3 else ('' if '\t' in l else l)) for l in (lines_of(P + '/shader_map.tsv') or [])[n0:]]
            with open(W + '/merged/union_keys.tsv', 'a', encoding='utf-8', newline='\n') as f: f.write(''.join(k + '\n' for k in add))
            say(last_line(W + '/rt_dump.log'))
        # 包内键别名表（第 1 代 → 第 2 代，translator/nvn_key.py）：表与包都定型（含真机转储）之后出
        if run([PY, R + '/graphics/tools/direct/merge_extract.py', '--alias', W + '/merged/pkg', P + '/shader_map.tsv', W + '/merged/key_alias.tsv'], log=W + '/alias.log') != 0:
            say(f'🟡 键别名表没出成，随包空表照常出包（只影响旧版本存档 / 管线记录换键；{last_line(W + "/alias.log")}；{W}/alias.log）'); open(W + '/merged/key_alias.tsv', 'w').close()
        # 跑局知识（审计 #1534 P1-54 B1）：只读快照 KN，不读活文件。pack/stage_direct.sh 算键前出好一份、经 S2A_RUN_KNOWLEDGE 交来；单独调用时这里按同一工具现出
        KN = os.environ.get('S2A_RUN_KNOWLEDGE', '')
        if not KN:
            KN = W + '/knowledge'
            cmd = [PY, R + '/graphics/tools/direct/run_knowledge.py', KN, TID]; rc = run(cmd, log=W + '/run_knowledge.log')
            if rc != 0: red('跑局知识快照出不来', cmd, rc, log_text(W + '/run_knowledge.log'), W + '/run_knowledge.log')
            say(last_line(W + '/run_knowledge.log'))
        for f in ('pairs', 'fout_want', 'se_want'):
            if not os.path.isfile(f'{KN}/{f}.tsv'): die(f'🔴 跑局知识快照缺 {f}.tsv（{KN}）')
        # 跑局里见到、出包没配上的「顶点 + 片元」对并进配对表，④ 的 vs_pad 给顶点着色器补上片元要读的输出（run_pairs.py）
        cmd = [PY, R + '/graphics/tools/direct/run_pairs.py', P + '/shader_pairs.tsv', W + '/merged/union_keys.tsv', KN + '/pairs.tsv']; rc = run(cmd, log=W + '/run_pairs.log')
        if rc != 0: red('跑局配对并不进配对表', cmd, rc, log_text(W + '/run_pairs.log'), W + '/run_pairs.log')
        say(last_line(W + '/run_pairs.log'))
        # 片元输出类型变体请求表供两端消费（Metal _ft、Vulkan _vf）；采样模拟表只给 Metal。
        try: shutil.copy(KN + '/fout_want.tsv', P + '/fout_want.tsv')
        except OSError as e: red(f'输出类型变体请求表拷不进配置档副本（{KN}/fout_want.tsv）', text=str(e))
        say(f'输出类型变体请求：{sum(1 for l in lines_of(P + "/fout_want.tsv") or [] if not l.startswith("#"))} 条（跑局知识快照）')
        if not VK:
            cmd = [PY, R + '/graphics/tools/direct/se_want.py', P + '/se_want.tsv', W + '/merged/union_keys.tsv', '--keys', KN + '/se_want.tsv']; rc = run(cmd, log=W + '/se_want.log')
            if rc != 0: red('采样模拟变体请求表出不来', cmd, rc, log_text(W + '/se_want.log'), W + '/se_want.log')
            say(last_line(W + '/se_want.log'))
        tm = secs() - s; s = secs()
        st = dict(V=V, P=P, MS=MS, PROF_ONLY=PROF_ONLY, AOCS=len(AOCS), ts=ts, tx=tx, tm=tm, s=s, T0=T0, NOMAC=NOMAC, NOIOS=NOIOS, PUB=PUB, KN=KN, btx=btx)
        if VK: self.android(st)
        self.metal(st)

    def extract(self, X, log):
        """当场抽取（graphics/tools/direct/extract.py）：输出原样进 <工作目录>/extract.log，屏上每行缩进两格"""
        sys.stdout.flush()
        try: p = subprocess.Popen([self.PY, self.R + '/graphics/tools/direct/extract.py', self.ROOT, X], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except OSError as e:
            print(e, file=sys.stderr, flush=True); return 127
        out = sys.stdout.buffer
        with open(log, 'wb') as f:
            for line in p.stdout:
                f.write(line); f.flush()
                out.write(b'  ' + line); out.flush()
        return p.wait()

    # ④⑤ 安卓（VK=1，#783）：graphics/tools/direct/vklib/dsh_vklib.py 按需编 dsh_vklib 再调：逐键出 Vulkan 产物，把成功批次打成 LibPack 容器 <临时输出>/b-<链>-vk/；
    #   收表、出 keys.tsv（只列出成的键）、vk_build.tsv 副本与 direct_lib.json，换进位。不编 Metal，不出 ps_inputs / c1_full / cb_extent / 管线种子表
    def android(self, st):
        R, PY, TID, W, OS, P = self.R, self.PY, self.TID, self.W, self.OS, st['P']
        V = st['V']
        self.prog('翻译成 Vulkan')
        L = f'{OS}/b-{V}-vk'; K = W + '/vk/keys'; rmtree(L); rmtree(K); os.makedirs(W + '/vk', exist_ok=True)
        brc = self.nostall([W + '/vk', OS], vcmd := [PY, R + '/graphics/tools/direct/vklib/dsh_vklib.py', 'build', '--pkg', W + '/merged/pkg', '--profile', P, '--out', K,
                                             '--lib', L, '--chain', V, '--dsx', os.environ.get('DSX_LIB') or R + '/build/dsx/libdsx.dylib', '--work', W + '/vk/work',
                                             '--cas', self.cache_dir() + '/store', '--jobs', self.J], W + '/build.log')
        if not (brc == 0 and os.path.isfile(K + '/vk_build.tsv') and nonempty(L + '/lib.idx')):
            red(f'安卓 Vulkan 库出库失败 rc={brc}', vcmd, brc, log_text(W + '/build.log'), W + '/build.log')
        try: shutil.copy(K + '/vk_build.tsv', OS + '/vk_build.tsv')
        except OSError as e: red('vk_build.tsv 拷不进输出目录', text=str(e))
        rows = [l.split('\t') for l in lines_of(K + '/vk_build.tsv') or []]
        f3 = lambda c, i: c[i] if len(c) > i else ''
        put(OS + '/keys.tsv', ''.join(k + '\n' for k in sorted(c[0] for c in rows if f3(c, 2) == 'ok')))
        cnt = collections.Counter(f3(c, 2) for c in rows if f3(c, 2) != 'ok')
        GAPS = '、'.join(f'{k} {v} 支' for k, v in cnt.items())
        put(W + '/direct_fail.log', '键\t类别\t错误\n' + ''.join(f'{c[0]}\t{f3(c, 2)}\t{f3(c, 3)[:200]}\n' for c in rows if f3(c, 2) != 'ok'))
        tb = secs() - st['s']
        self.prog('收表')
        self.collect()
        nu, nk, miss = self.counts()
        m = json.load(open(W + '/merged/merge.json', encoding='utf-8'))
        put_json(OS + '/direct_lib.json', {'title': TID, 'chain': V, 'target': 'android', 'union': nu, 'built': nk, 'missing': miss, 'profile_only': st['PROF_ONLY'], 'aoc_dirs': st['AOCS'], 'merge': m,
                                           'jobs': int(self.J), 'wall_s': {'split': st['ts'], 's2a_extract': st['tx'], 'merge': st['tm'], 'build': tb, 'total': secs() - st['T0']}})
        self.prog('换进位')
        self.swap_out()
        self.done[0] = True
        self.prog_done()
        head = '🟢' if miss == 0 else f'🟡 直连缺口（部分出库）：{GAPS or f"没出成 {miss} 支"}（运行时走现编）；'
        say(f'{head} {TID} 安卓 Vulkan 库 b-{V}-vk：并集 {nu}，出库 {nk}，没出成 {miss}；用时 抽取 {st["ts"]}+{st["tx"]} / 合并 {st["tm"]} / 出库 {tb} / 合计 {secs() - st["T0"]} 秒（{last_line(W + "/build.log")[:80]}）')
        raise Exit(0)

    def collect(self):   # ⑤ 收表（苹果 / 安卓同一张清单 TABLES）
        P = f'{self.W}/profile/{self.TID}'
        for f in TABLES:
            if os.path.isfile(P + '/' + f): copy_soft(P + '/' + f, self.OS + '/' + f)
        for f in ('key_map.tsv', 'key_alias.tsv'): copy_soft(f'{self.W}/merged/{f}', self.OS + '/' + f)

    def counts(self):
        u = self.W + '/merged/union_keys.tsv'; k = self.OS + '/keys.tsv'
        return wc_l(u), wc_l(k), len(comm23(lines_of(u) or [], lines_of(k) or []))

    # ④ 翻译 + 编库 + 发布：内容寻址仓 $C/store 里有的翻译产物 / Metal 库按内容取，其余现做；发布那一侧（S2A_DIRECT_PUBLISH）直接写成 $OS/b-<链>/lib.NNN.pak + lib.idx，
    #   出 keys.tsv / ps_inputs.tsv / c1_full / cb_extent / _b_build.json；进度按 S2A_PROGRESS 协议写（Build.cs Ticker）
    def metal(self, st):
        R, NX, PY, ROOT, TID, W, OS, C, OUT = self.R, self.NX, self.PY, self.ROOT, self.TID, self.W, self.OS, self.C, self.OUT
        V, P, MS, KN = st['V'], st['P'], st['MS'], st['KN']
        self.prog(f'翻译 + 编库（{self.J} 路）')
        # S2A_DIRECT_KEEP=1：整份出库另把 Mac 那份逐键产物导出到 $C/keep/mac/b-<链>（门禁考卷冻结 t_dsh.sh freeze / 全量对拍 DSH_EXAM 读它）
        KM = ''
        if os.environ.get('S2A_DIRECT_KEEP', '0') == '1':
            rmtree(C + '/keep'); os.makedirs(C + '/keep/mac'); KM = f'{C}/keep/mac/b-{V}'
        # 出库链整条在 C# 一个进程里（graphics/tools/direct/dsh/batch/Build.cs）；链号、配置档输入摘要先由 dmsh_chain.py 核过工具再给。
        #   stdout 只收链号 / 程序号 / 输入摘要；编译器版本提示走 stderr，合流会把 note: 误读成链号
        BV, BG, BI = build_env(PY, self.TR, W, V)
        cmd = [PY, '-c', 'import spvc_inproc; print(spvc_inproc.lib_path())']; rc, SPVC = run(cmd, capture=True, cwd=self.TR)
        if rc != 0: red('spirv-cross 进程内库编不出', cmd, rc, run.err + '\n' + SPVC)
        BA = ['--pkg', W + '/merged/pkg', '--profile', P, '--game', BG, '--chain', V, '--inputs', BI, '--profile-rows', W + '/profile_rows.txt', '--out', MS, '--iosout', OS, '--publish', st['PUB'],
              '--cas', C + '/store', '--dsx', os.environ.get('DSX_LIB') or R + '/build/dsx/libdsx.dylib', '--spvc', SPVC, '-j', self.J, '--lock', self.TR + '/dmsh_chain.lock']
        if st['NOMAC']: BA.append('--no-mac')
        if st['NOIOS']: BA.append('--no-ios')
        if KM: BA += ['--export-mac', KM]
        # dsh_batch 的临时工作目录（spill.dat / side_*.dat，大款几十 GB）：调用方已把 TMP / TEMP / TMPDIR 指到本单工作盘（pack/orch/oneclick_win.py）就沿用、在下面建本次的子目录；
        #   是系统缺省临时目录（Mac /var/folders、/tmp，Windows 用户 AppData\Local\Temp）才落 $W/tmp（不让内置盘被写爆，11-02 本机内置盘 15 分钟掉 61 GB）
        BT, BTX = batch_tmp(os.environ.get('TMPDIR', ''), W, os.getpid())
        if BTX: st['btx'].append(BTX)
        os.makedirs(BT, exist_ok=True)
        log = W + '/build.log'
        bcmd = [PY, self.TR + '/dsh_batch.py', 'build', *BA]; brc = self.nostall([MS, OUT], bcmd, log, env=dict(os.environ, TMPDIR=BT))
        DF = rc127_note(brc, log, [BT, C])
        if brc != 0:
            if brc == 1: gap(log, 'NVN 着色器')
            why = fail_why(log)
            red(f'出库失败 rc={brc}{"：" + why if why else ""}{DF}', bcmd, brc, log_text(log), log)
        # 核产物出自本树的翻译器（11-06 时之笛第 49 版：DSH_BATCH_CMD 指到按旧树编的 dsh_batch.exe，41971 支全命中旧缓存、出库照报绿）：
        #   dsh_batch 把自己的翻译器指纹 xfp 与 --lock 文件摘要写进库印章 inputs.tsv，这里拿本树算的逐项比，不等 / 印章没这两行（旧程序）报红
        sys.stdout.flush()
        cmd = [PY, 'dmsh_chain.py', '--check-stamp', f'{OS}/b-{V}']
        p = subprocess.run(cmd, cwd=self.TR, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if p.returncode != 0: red(f'直连库不是本树的翻译器出的（DSH_BATCH_CMD={os.environ.get("DSH_BATCH_CMD") or "未设"}）', cmd, p.returncode, p.stdout.decode('utf-8', 'replace'))
        # 没出成的按类别计数（_b_build.json 的 st），末行黄行带上，普查按它归类；编不过的键清单（11-01 维护者）随 *.log 收进 $C/logs：出库成功就写（没有编不过的也写表头）
        GAPS = ''
        try:
            rs = json.load(open(f'{MS}/b-{V}/_b_build.json', encoding='utf-8'))
            GAPS = '、'.join(f'{k} {v} 支' for k, v in collections.Counter(r['st'] for r in rs if r['st'] != 'ok').most_common())
            put(W + '/direct_fail.log', '键\t类别\t错误\n' + ''.join(f'{r["key"]}\t{r["st"]}\t{" ".join(str(r.get("err") or "").split())[:200]}\n' for r in rs if r['st'] != 'ok'))
        except Exception:
            rmtree(W + '/direct_fail.log')
        tb = secs() - st['s']
        self.prog('收表')
        # ⑤ 收表、出 keys.tsv（以实际出成的 iOS 库为准）
        if not os.path.isdir(f'{OS}/b-{V}'): red(f'没出 b-{V}（出库退 0 却没写库目录）', bcmd, brc, log_text(log), f'{OS}/b-{V}；{log}')
        self.collect()
        # 管线种子表（配置档没有时，direct_lib.json 记 pso_seed=gen；配置档维护过的记 profile、上面收表时原样带了）：经 pack/stage_direct.sh 出库（S2A_PSO_SEED_STAGE=1）不在这里生成，
        #   归轻阶段 pack/stage_direct_seed.sh；这里只不发布配置档那份只有表头的空表。单独调用（Windows 出包 pack/orch/oneclick_win.py、单测）照旧在这里按快照的 seen.tsv 生成
        if self.PSO_GEN and os.environ.get('S2A_PSO_SEED_STAGE', '0') == '1': rmtree(OS + '/pso_manifest.tsv')
        elif self.PSO_GEN:
            # 种子表是锦上添花（审计 #1592 P1-100）：出不来只打【黄】、放只有表头的空表，库照常换进位
            def seedsoft(why):
                say(f'🟡 {TID} 管线种子表这次没生成：{why}；库照常出，缺的是首局管线预建（首遇现编）；包里那张空表的表头记着同一原因')
                put(OS + '/pso_manifest.tsv', f'# Prewarm seed list: empty (generation failed at packing time: {why.replace(chr(9), "").replace(chr(10), "")[:200]}); pipelines compile at their first use\n')
            if not os.path.isfile(KN + '/seen.tsv'): seedsoft(f'跑局知识快照缺 seen.tsv（{KN}）')
            else:
                rc, SK = run([PY, R + '/graphics/tools/direct/pso_layout.py', 'skey-size'], capture=True, err=W + '/pso_seed.log')
                if rc != 0: seedsoft(f'量不出管线键字节数（{W}/pso_seed.log）')
                elif run([PY, R + '/graphics/tools/direct/pso_seed.py', OS + '/pso_manifest.tsv', OS + '/keys.tsv', W + '/merged/shader_map.tsv', SK, KN + '/seen.tsv'], log=W + '/pso_seed.log', mode='a') != 0:
                    seedsoft(f'pso_seed.py 失败：{last_line(W + "/pso_seed.log")[:200]}')
                else: say(last_line(W + '/pso_seed.log')[:300])
        nu, nk, miss = self.counts()
        m = json.load(open(W + '/merged/merge.json', encoding='utf-8'))
        put_json(OS + '/direct_lib.json', {'title': TID, 'chain': V, 'union': nu, 'built': nk, 'missing': miss, 'profile_only': st['PROF_ONLY'], 'aoc_dirs': st['AOCS'], 'merge': m,
                                           'pso_seed': 'gen' if self.PSO_GEN else 'profile', 'jobs': int(self.J),
                                           'wall_s': {'split': st['ts'], 's2a_extract': st['tx'], 'merge': st['tm'], 'build': tb, 'total': secs() - st['T0']}})
        GLMSG, GLX = self.gl_also()
        self.prog('换进位')
        # ⑥ 核验齐全、发布哪一份、反射 -sdk 与目标相符、打成 lib.NNN.pak + lib.idx 已在整份出库里从内存做完，这里只看结果
        for l in ('核验齐全', '发布 ', '打包：'):
            x = grep_last(log, r'\[直连库\] .*' + re.escape(l))
            if x: say(x[len('[直连库] '):] if x.startswith('[直连库] ') else x)
        if not nonempty(f'{OS}/b-{V}/lib.idx'): red(f'整份出库没写出 b-{V}/lib.idx', bcmd, brc, log_text(log), log)
        # 只把打好的容器换进输出目录；逐键的 Mac / iOS 产物与工作目录是这次出库的临时品，日志留进 $C/logs/，其余退出时删
        self.swap_out()
        # S2A_DIRECT_KEEP=1：这次的工作目录与 Mac 那份逐键产物留到 $C/keep/{work,mac}（门禁考卷冻结 / 全量对拍读它们；平时不留）
        if os.environ.get('S2A_DIRECT_KEEP', '0') == '1':
            try: os.rename(W, C + '/keep/work')
            except OSError: say(f'🟡 S2A_DIRECT_KEEP：没留全（{C}/keep）')
        self.done[0] = True   # 临时目录（逐键产物、工作目录）退出时删
        self.prog_done()
        head = '🟢' if miss == 0 else f'🟡 直连缺口（部分出库）：{GAPS or f"没出成 {miss} 支"}（运行时走现编 / 兜底）；'
        say(f'{head} {TID} 直连库 b-{V}{"（只出 iOS）" if st["NOMAC"] else ""}：并集 {nu}，出库 {nk}，没出成 {miss}；{self.J} 路并行，用时 抽取 {st["ts"]}+{st["tx"]} / 合并 {st["tm"]} / 出库 {tb} / 合计 {secs() - st["T0"]} 秒（{last_line(C + "/logs/build.log")[:80]}）')
        if GLMSG: say(f'{GLMSG}（{GLX.split(chr(9), 1)[-1]}）')
        raise Exit(0)
    def gl_also(self):
        """NVN 款另带 GL → (给人看的一行或空串, gfx_api.py --gl-also 的输出)"""
        R, NX, PY, ROOT, W, OS, C = self.R, self.NX, self.PY, self.ROOT, self.W, self.OS, self.C
        # NVN 款另带 GL（gfx_api.py --gl-also：真建 GL 上下文、有 GL 入口、带 GLSL；11-03 火影忍者 终极风暴羁绊的界面中间件 Scaleform 走 GL）：同 OpenGL 款分支再出一份 GL 着色器库，
        #   写进临时输出目录 $OS/gl，随 NVN 库一起原子换进位。GL 这份翻不全或出不来只打黄、不挡 NVN 库发布
        rc, GLX = run([PY, NX + '/tools/gfx_api.py', '--gl-also', ROOT + '/exefs', ROOT + '/romfs'], capture=True)
        if rc != 0: GLX = '0\t判不了（gfx_api.py --gl-also 出错）'
        GLMSG = ''
        if GLX.split('\t', 1)[0] == '1':
            rmtree(OS + '/gl'); s = secs()
            if not which(os.environ.get('GLSLANG', 'glslangValidator')) or not which('spirv-cross'):
                GLMSG = '🟡 另带 GL 的着色器库没出：缺 glslangValidator / spirv-cross（brew install glslang spirv-cross）'
            else:
                INS = [ROOT + '/romfs', ROOT + '/exefs/main'] + self.aocs()
                glog = W + '/gl_shaders.log'
                grc = self.nostall([C + '/glmac', OS], [PY, self.TR + '/gl_shaders.py', *INS, C + '/glmac', '--ios', OS], glog)
                ok, bad = summary(glog, 'GLSL')
                if grc == 0 and (ok, bad) == (0, 0) and nonempty(OS + '/gl/gl_shaders.tsv'):
                    GLMSG = f'🟡 另带 GL 离线 0 支 GLSL：只出表头、界面那部分运行时首遇现编，{secs() - s} 秒'
                elif (grc == 0 or (grc == 1 and ok is not None and ok > 0)) and nonempty(OS + '/gl/gl_shaders.metallib') and nonempty(OS + '/gl/gl_shaders.tsv'):
                    GN = sum(1 for l in lines_of(OS + '/gl/gl_shaders.tsv') or [] if not l.startswith('#'))
                    GMISS = bad if grc == 1 else 0
                    GLMSG = ('另带 GL' if not GMISS else f'🟡 另带 GL 部分出库（GLSL 翻不了 {GMISS} 支）') + f'：GLSL {GN} 支 → gl/gl_shaders.metallib，{secs() - s} 秒'
                else:
                    rmtree(OS + '/gl'); GLMSG = f'🟡 另带 GL 的着色器库没出（rc={grc}，{C}/logs/gl_shaders.log）：界面那部分运行时报红'
        return GLMSG, GLX


def nonempty(p):
    try: return os.path.getsize(p) > 0
    except OSError: return False


def put(p, text):
    with open(p, 'w', encoding='utf-8', newline='\n') as f: f.write(text)


def put_json(p, obj):
    with open(p, 'w', encoding='utf-8', newline='\n') as f: json.dump(obj, f, ensure_ascii=False, indent=1)


def du_h(p):   # 给人看的大小（同 du -h 的量级写法）
    try: n = os.path.getsize(p)
    except OSError: return '?'
    for u in ('B', 'K', 'M', 'G'):
        if n < 1024 or u == 'G': return f'{n:.0f}{u}' if u == 'B' else f'{n:.1f}{u}'
        n /= 1024


def summary(log, kind):
    """汇总行「<kind> 汇总：成功 N、失败 M」（取最后一行）→ (N, M)；没有 → (None, None)"""
    hit = None
    for l in lines_of(log) or []:
        m = re.match(kind + r' 汇总：成功 ([0-9]*)、失败 ([0-9]*)', l)
        if m: hit = m
    if not hit: return None, None
    conv = lambda x: int(x) if x else None
    return conv(hit.group(1)), conv(hit.group(2))


def copy_tree(a, b):
    """抽取结果克隆进工作目录：macOS cp -c（APFS 克隆，瞬间、不占盘），Linux GNU cp，Windows 逐文件拷"""
    if sys.platform == 'darwin': cmd = ['cp', '-c', '-R', a, b]
    elif not WIN: cmd = ['cp', '-R', a, b]
    else: shutil.copytree(a, b, dirs_exist_ok=True); return
    if subprocess.run(cmd).returncode != 0: raise OSError(f'{" ".join(cmd)} 失败')


def copy_soft(a, b):   # 拷不成只记一句、照常往下（原先这几处 cp 不查退出码，行为不变）
    try: shutil.copy(a, b)
    except OSError as e: print(f'没拷成 {a} → {b}：{e}', file=sys.stderr, flush=True)


def copy_file(a, b):
    if sys.platform == 'darwin':
        if subprocess.run(['cp', '-c', a, b]).returncode != 0: raise OSError(f'cp -c {a} {b} 失败')
    else: shutil.copy(a, b)


def merge_tables(sp, pf):
    """抽取的三张表并进配置档副本，返回「配置档有、抽取没有」的着色器键数"""
    def data(p): return [l for l in open(p, encoding='utf-8') if l.strip() and not l.startswith('#') and not l.startswith('package\t')] if os.path.exists(p) else []
    def head(p): return [l for l in open(p, encoding='utf-8') if l.startswith('#') or l.startswith('package\t')] if os.path.exists(p) else []
    key = lambda l: l.split('\t', 1)[0]
    # 着色器表：抽取的为准；数出配置档有、抽取没有的键
    pk = {l.split('\t')[3] for l in data(os.path.join(pf, 'shader_map.tsv')) if len(l.split('\t')) > 4}
    sk = {l.split('\t')[3] for l in data(os.path.join(sp, 'shader_map.tsv')) if len(l.split('\t')) > 4}
    open(os.path.join(pf, 'shader_map.tsv'), 'w', encoding='utf-8', newline='\n').writelines(head(os.path.join(pf, 'shader_map.tsv')) + data(os.path.join(sp, 'shader_map.tsv')))
    # 答案表：配置档的行原样，再补抽取里配置档没有的键
    f0 = data(os.path.join(pf, 'shader_facts.tsv')); have = {key(l) for l in f0}
    f1 = [l for l in data(os.path.join(sp, 'shader_facts.tsv')) if key(l) not in have]
    open(os.path.join(pf, 'shader_facts.tsv'), 'w', encoding='utf-8', newline='\n').writelines(head(os.path.join(pf, 'shader_facts.tsv')) + f0 + f1)
    # 配对表：并集（去重保序）
    p0 = data(os.path.join(pf, 'shader_pairs.tsv')); seen = set(p0)
    open(os.path.join(pf, 'shader_pairs.tsv'), 'w', encoding='utf-8', newline='\n').writelines(head(os.path.join(pf, 'shader_pairs.tsv')) + p0 + [l for l in data(os.path.join(sp, 'shader_pairs.tsv')) if not (l in seen or seen.add(l))])
    return len(pk - sk)


def main(argv):
    if WIN:
        for s in (sys.stdout, sys.stderr): s.reconfigure(encoding='utf-8')
    def term(sig, _): raise Exit(128 + sig)
    signal.signal(signal.SIGTERM, term)
    lib = None
    try:
        lib = Lib(argv)
        lib.main()
        return 0
    except Exit as e:
        return e.code
    except Exception:
        import traceback; tb = traceback.format_exc(); sys.stderr.write(tb)
        try: red('直连库脚本异常', text=tb)
        except Exit as e: return e.code
    finally:
        sys.stdout.flush()
        for f in reversed(lib.cleanup if lib else []):
            try: f()
            except Exception as e: print(f'收尾清理没做完：{e}', file=sys.stderr)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
