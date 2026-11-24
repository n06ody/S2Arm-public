#!/usr/bin/env python3
"""推资源包（Mac / Windows 共用，只此一份；11-06 维护者拍板、维护者「玩家流程做在共用后端、两端都验」）：
把 <资源包目录> 增量推到已装好的 App，只传机上没有 / 内容变了的文件，App 下次启动时自己解开（S2A_PACK_APPLY_ONLY=1 只解包不跑游戏）。
原先 Mac 是 tools/push_pack.sh（bash + devicectl）、Windows 是 pack/orch/push_win.py（s2a-devpush），同一套契约各写一遍，
残留批覆盖、.pushed 记账这类坑两边各踩一次；现在两边都调本模块，tools/push_pack.sh 只剩 dev_run 的回调外壳。
放在 tools/ 而不是 pack/jobs/：调度把 tools/ 整份单独拷到 gate-host 跑 dev_run（tools/sched.sh git archive tools），tools 树要自成一体（11-06 列车 sched_test / ssh_argv_test 退回）。

口径（手机端 runtime/src/pack_apply.c）：
  ① 机上已有什么按 <资源包根>/index.applied.tsv 认（App 解包成功时写；老 App 没有它才退回 index.tsv）。
     资源包根：单款 App 缺省 <文档根>/资源包 = Documents/资源包；多款容器由调用方 --pack-root 给（pack/jobs/install.py pick_pack_root：
     Library/Application Support/S2Arm/games/<游戏号>/资源包，过渡期新位置不在而旧位置 Documents/games/<游戏号>/资源包 在时给旧位置）；
     解包日志总在 <文档根>/unpack.log（文档根缺省 Documents，多款容器每款 Documents/games/<游戏号>）
  ② .incoming 里本工具推的包（<时刻>_<三位序号>.tar）逐个核完整性（s2a-devpush tar-ok：512 整倍、末尾 1024 字节全零）：
     完整的留着（推完、还没起 App 解的批：出包重叠早推的、上次推完没起 App 的），其里的文件按 .pushed 算机上已有；
     不完整或核不了（Mac 没有 s2a-devpush / UDID）的用同名空 tar（1024 字节全零）盖掉，不信 .pushed、按已应用索引补推（10-26 截短残批）
  ③ 没有待解批 = 账已由已应用索引接管，.pushed 清空（10-20 换回旧包时 .pushed 旧账把被新包删掉的文件判成「机上已有」）
  ④ 上次收推送撞到截短包（unpack.log 末行）：不信 .pushed，也不退回 index.tsv（它是上次推的目标，文件没落地）
  ⑤ 推前抽查机上实况：直连库顶层表 + b-<链>/lib.idx 全核、同 sha 小文件按 sha 序取 5 个拉回比 sha256；
     任何一个不符：拉机上实际清单（路径 + 大小）核账，只留在且大小对得上的行；清单拉不回才整包重推（10-25 马车8、11-02 测试设备1 奥德赛）
  ⑥ ≤1 GB 一批（单个超 1 GB 的文件自成一批）；每批 s2a-devpush put-tar 边读边打 pax tar 直接写进 .incoming（本机盘不落临时 tar）；
     Mac 上没工具 / 不知道 UDID / 这批流式写失败 → 这批打 tar + devicectl copy（同名覆盖半截包）
  ⑦ 每推完一批把「路径 \\t sha」追加进 .pushed 并推上机（中断续推 / 出包重叠早推后补差量）；开推先把机上 manifest.json 置空，最后推 index.tsv 与 manifest.json
  ⑧ 推之前核剩余空间：待推量 + 最大一批 + 每文件 4 KB + S2A_PUSH_SPARE_GB（缺省 1）GB（need_bytes）；读不到照推
  ⑨ 压缩传输（v0.5.0）：game.img 按款量一次 zstd 压缩率，省到 15% 的款切成 1 GiB 一块、每块一个 zstd 帧推进 .incoming，App 解开时流式解回 game.img（zstd_plan / send_zst；
     手机端 runtime/src/pack_apply.c img_unzst）；S2A_PUSH_ZSTD=0 一律原样直写。导出 / 推到「放这里导入」的 .img 不压（手机按文件头认领、原样挪位）

设备操作两种（同一个 Dev 接口）：
  Devpush：s2a-devpush 走 usbmuxd + house_arrest + AFC（Windows 只有它；Mac 上手机连 usbmuxd 时也能用）
  Devicectl：Mac 的 xcrun devicectl（S2A_DEVICECTL 可换假的）；put-tar / tar-ok 有 s2a-devpush + UDID 时借它

用法：
  python3 tools/push_pack.py run   <设备> <包名> <资源包目录> [--full] [--udid U] [--devpush P] [--work 目录] [--apply]   一条龙（启动器 / Windows / 手动装机）；
      --apply：推完起一次 App 只解包并等解完（.applied = 本包且解包日志本次新增完成行）
  各子命令都收 --docs <设备侧文档根>（缺省 Documents；多款容器每款 Documents/games/<游戏号>，解包日志在它下面）、
      --pack-root <设备侧资源包根>（缺省 <文档根>/资源包；多款容器每款 Library/Application Support/S2Arm/games/<游戏号>/资源包）
      与 --launch-ctx <值>（多款容器：推完正常拉起 App 到游戏库、由库解开这款（值本身已不再传给 App））
  python3 tools/push_pack.py plan  <设备> <包名> <资源包目录> --work O [--full] [--udid U]   算待推批次（dev_run 用：之后它按推送量放大看门、腾空间）
  python3 tools/push_pack.py free  <设备> <包名> <资源包目录> --work O [--udid U]           机上可写字节（读不到不打印）
  python3 tools/push_pack.py send  <设备> <包名> <资源包目录> --work O [--udid U] [--devpush P]   推 plan 算好的批次 + 索引 + 清单
  plan / send 把机读结果写进 <O>/push.state（PLAN 批数 文件数 字节数 / SENT 批数 / FAIL 原因），日志行打到标准输出（不带时刻，调用方自己加）
  <设备>：Mac 是 devicectl 的设备标识或名字；Windows（或 --via devpush）是手机 UDID
退出码：0 成功；1 失败（标准输出末行【红】说明）；2 用法"""
from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

R = Path(__file__).resolve().parents[1]
LIM = 1 << 30
IMG = re.compile(r"(^|.*/)romfs\.img$|game\.img")   # 单文件镜像（旧形态 romfs.img / v0.3.0 单一 IMG game.img，pack/img.py）：直接写到最终路径，不进 tar、不用首启解开
def is_direct(rel): return bool(IMG.fullmatch(rel))   # 资源包里哪些文件直写最终路径（整名匹配：game.imgmap 这类旁带文件进 tar、App 解开才就位）；推和核对（pack/jobs/tvos.py verify）共用这一条
# 流式写失败后还能交给 devicectl 整份拷的上限（缺省 2 GiB，S2A_DC_FALLBACK_MAX 可改）：devicectl device copy to 把整个文件读进
#   CoreDeviceService 内存（#641 实测 12.2 GiB 的资源包：服务涨到约 20 GB、整机换页 10 GB 并卡死，下一次流式写在 0 字节就失败）。
#   批次 tar（≤1 GB）照旧可以退回
DC_FALLBACK_MAX = 2 << 30
DENIED = "Permission denied"   # afc 打开写被拒的原文（s2a-devpush「打不开 <远端>：afc error: Permission denied」）
REINSTALL_STEP = "先试这两步：①在 iPhone 上打开一次 S2Arm，等游戏库界面出来；②回电脑对这款游戏再点一次「装到 iPhone」，会接着已传的部分继续，不用从头来。还是写不进，就要删掉 iPhone 上的 S2Arm App 再重装：先在游戏菜单「存档管理」里导出存档，存到 S2Arm 文件夹以外（比如 iCloud 云盘）；再在 iPhone 桌面长按 S2Arm，删除 App；然后在电脑上的 S2Arm 里对这款游戏再点一次「装到 iPhone」；装好后在「存档管理」里导回存档。电脑上的游戏和 S2Arm 都不用删。"   # 机上目录 App 写不进时唯一走得通的下一步（同失败码 ios_install_dir_unwritable）


def tv(env=None):   # 推进 Apple TV（S2A_PUSH_TV=1：pack/jobs/install.py push_pack tv=True，或 pack/jobs/tvos.py TvClient 建 Devicectl 时给的环境）：提示按电视说
    return (env or os.environ).get("S2A_PUSH_TV") == "1"


def push_reason(text):
    """推送失败的原文 → 玩家一眼看得懂的一类（摘要第一条要能看出是哪种，11-12 v0.3.1 Mac 玩家「romfs.img 没传完」看不出原因）：
    USB 看不到手机 / 手机锁屏 / 连接断了；认不出就空串（照原文报）。按 s2a-devpush 与 devicectl 的报错特征认，不看英文全文"""
    t = str(text or "")
    if nospace(t): return "手机空间写满了"
    if "找不到设备" in t or "No device" in t or "usbmuxd" in t.lower() and "connect" in t.lower(): return "USB 看不到手机（没插数据线，或手机只在无线连接上）"
    if DENIED in t: return "手机上这个文件 App 写不进（Permission denied）"
    if "锁屏" in t or "locked" in t.lower() or "passcode" in t.lower(): return "手机锁屏了"
    if "没有任何进展" in t or "socket io failed" in t or "Broken pipe" in t or "Connection reset" in t or "disconnected" in t.lower(): return "和手机的连接断了（数据线松了 / Wi-Fi 断开）"
    return ""


def avail_mem():
    """本机现在能腾出来的内存（字节）：Mac 读 vm_stat（空闲 + 不活跃 + 可回收），读不到 0"""
    try:
        out = subprocess.run(["vm_stat"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    page = int(re.search(r"page size of (\d+)", out).group(1)) if re.search(r"page size of (\d+)", out) else 16384
    n = sum(int(m) for k in ("Pages free", "Pages inactive", "Pages speculative") for m in re.findall(rf"{k}:\s+(\d+)", out))
    return n * page
IMG_PAGE = 16384   # 单一 IMG 的页（pack/img.py PAGE）：第 0 页是头，续传按页向下取整接着写
IMG_PACK_ID = slice(32, 96)   # 头里的资源包号（ASCII，可空）：空 = 认不出是不是同一份，不续传
DOCS = "Documents"   # 设备侧文档根（单款 App）；多款容器每款 Documents/games/<游戏号>（pack/ship_ext.py 容器布局），设备对象的 docs 属性


def rp(dev): return dev.pack or f"{dev.docs}/资源包"   # 设备侧资源包根：设备对象的 pack 属性（--pack-root），没给按文档根推（单款 App）
def inc(dev): return rp(dev) + "/.incoming"
def unpack_log(dev): return f"{dev.docs}/unpack.log"   # runtime/src/pack_apply.c：<文档根>/unpack.log
STALE = re.compile(r"\d+_\d{3}\.tar")
EXE = "s2a-devpush.exe" if os.name == "nt" else "s2a-devpush"


NOT_FOUND = "找不到设备"   # s2a-devpush 的原话（pack/devpush/src/main.rs：usbmuxd 列不出这台 / 连不上 usbmuxd）  # 非玩家句


class Fail(Exception):
    """推包失败：reason 是给调用方 fin / 启动器看的一句话"""


class DeviceLocked(Fail):
    """推之前写探针就写不进（多半是锁屏 / 数据线松了 / 隧道断了）：调用方报 device_locked，不推（#333）"""
    code = "device_locked"


class DeveloperModeOff(Fail):
    """devicectl 原话「Developer Mode is disabled」（CoreDeviceError 10005）：手机没开开发者模式，重试 / 插稳数据线都没用（D-MHM5：
    原先只报「devicectl 退出码 1：确认手机解锁、数据线插稳」，把玩家往错的方向引）。调用方按码 developer_mode_required 收尾，原因句照做即过"""
    code = "developer_mode_required"


DEV_MODE_OFF = re.compile(r"Developer\s*Mode\s*(?:is\s*)?(?:disabled|not enabled)|CoreDeviceError error 10005")


class LowSpace(Fail):
    """推之前估算手机空间不够（估算值偏小：AFC 报的空闲不含 iOS 随时能清掉的空间，「设置 → 储存空间」里的「可用」含它）：
    认得出的风险让玩家确认——调用方报 device_space_low，界面「仍然继续」带 acceptLowSpace 重来就照推，真写满了才停"""
    code = "device_space_low"


class SpaceFull(Fail):
    """推的时候手机真写满了（推失败后再量，空闲已近 0）：说清这次要多少、手机此刻还剩多少"""
    code = "ios_install_no_space"


FULL_LEFT = 256 << 20   # 推失败后手机空闲低于它 = 写满了（按写满报，不报成传输失败）
# 手机写满的报错，同一族的说法都认（11-17：原先只认 devicectl 的 POSIX 28，s2a-devpush 的 AFC「No space left」被当成连接中断，
#   还改走 devicectl 白等两分钟，最后报「devicectl 退出码 1，确认手机解锁、数据线插稳」）。实测原文：s2a-devpush「afc error: No space left」；
#   devicectl「POSIX error code 28」「NSPOSIXErrorDomain error 28」「No space left on device」；libimobiledevice「AFC_E_NO_SPACE_LEFT」；ENOSPC
NOSPACE = re.compile(r"No space left|ENOSPC|AFC_E_NO_SPACE_LEFT|POSIX error code 28(?!\d)|NSPOSIXErrorDomain error 28(?!\d)")


def nospace(text):
    """这段报错说的是手机写满了吗（同一族各种说法）"""
    return bool(NOSPACE.search(text or ""))


def gb1(n): return f"{n / 2**30:.1f}"


class DirUnwritable(Fail):
    """机上资源包所在的目录 App 写不进（afc Permission denied，删掉重写也一样；部分机上这条目录整条归 root）：重试、续传、改走 devicectl 都没用，
    唯一走得通的是导出存档后删除重装 S2Arm——调用方报 ios_install_dir_unwritable，不再说「重新装会续传」"""
    code = "ios_install_dir_unwritable"


class NeedsPlayer(Fail):
    """资源已传完，只差玩家在手机上打开 App 解开（电脑拉不起 App，#192）：不是传输失败，调用方按「需要玩家操作」收尾"""


def devpush_path(given=None, env=None):
    """s2a-devpush：显式给的 > S2A_DEVPUSH > 启动器发版内置（S2A_PREBUILT_TOOLS/devpush/）> 仓库编出的 build/devpush/release > PATH"""
    env = os.environ if env is None else env
    pre = env.get("S2A_PREBUILT_TOOLS")
    for p in (given, env.get("S2A_DEVPUSH"), pre and str(Path(pre) / "devpush" / EXE), str(R / "build/devpush/release" / EXE), shutil.which(EXE)):
        if p and Path(p).is_file() and (os.name == "nt" or os.access(p, os.X_OK)):
            return str(p)
    return None


class Devpush:
    """s2a-devpush（pack/devpush）：经 usbmuxd（USBMUXD_SOCKET_ADDRESS 可指别的机器）连手机，house_arrest 进 App 容器、AFC 读写"""
    kind = "devpush"
    docs, ctx, pack = DOCS, None, None
    progress = None   # 当前文件 / 批次的有效载荷字节；不代表 .pushed 已提交

    def __init__(self, exe, udid, bid, work, env=None):
        self.exe, self.udid, self.bid, self.work, self.env = exe, udid, bid, Path(work), env

    def call(self, *args):
        try:
            cmd = [self.exe, self.udid, self.bid, *map(str, args)]
            if self.progress is not None and args[0] in ("put", "put-tar", "put-at"):
                env = dict(os.environ if self.env is None else self.env, S2A_DEVPUSH_PROGRESS="1")
                tail = deque(maxlen=20)
                # 合并 stderr 持续排空，避免任一管道写满；老工具忽略环境变量，照常只报批末进度。
                with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", env=env) as p:
                    try:
                        for line in p.stdout:
                            m = re.fullmatch(r"S2A_DEVPUSH_PROGRESS (\d+)\s*", line)
                            if m: self.progress(int(m[1]))
                            else: tail.append(line)
                        p.wait()
                    except BaseException:
                        p.kill(); p.wait()   # 回调失败 / 取消时与 subprocess.run 一样回收本次子进程
                        raise
                out = "".join(tail).strip()
            else:
                p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=self.env)
                out = (p.stdout + p.stderr).strip()
        except OSError as e:
            return 1, str(e)
        with open(self.work / "devpush.log", "a", encoding="utf-8") as f:
            f.write(f"[{time.strftime('%H:%M:%S')}] {args[0]} rc={p.returncode} {out[-300:]}\n")
        return p.returncode, out

    def get(self, rel, dst, t=None):
        Path(dst).unlink(missing_ok=True)
        rc, _ = self.call("get", rel, dst)
        return rc == 0 and Path(dst).is_file()

    fallback = None   # Mac：usbmuxd 看不到这台（s2a-devpush「找不到设备」）时整份改走的另一条路 (src, rel) → None（install.MacPhone.client 给 devicectl）

    def put(self, src, rel):
        rc, out = self.call("put", src, rel)
        if rc and DENIED in out: rc, out = self.rewrite(src, rel, out)
        if rc and self.fallback and NOT_FOUND in out:   # #1756：手机在 devicectl 那边连着、usbmuxd 那一刻列不出（无线隧道 / 间歇断开），推 .s2arm 次次失败
            with open(self.work / "devpush.log", "a", encoding="utf-8") as f:
                f.write(f"[{time.strftime('%H:%M:%S')}] put {rel}：s2a-devpush 找不到设备，改走 devicectl\n")
            return self.fallback(src, rel)
        if rc:
            raise Fail(f"资源包写进 iPhone 时中断（{out.splitlines()[-1] if out else f'退出码 {rc}'}）：确认手机解锁、数据线插稳后再点一次")   # 哪个文件写进 devpush.log

    def rewrite(self, src, rel, first):
        """机上这个文件打不开写（afc「Permission denied」，#074d88 三台不同设备：手机上 game.img 写不进，重试 / 续传次次一样）：
        同一处的旧文件删掉、从头写一份新的（AFC 新建的文件归 App、可写）。删不掉、或删了新建照样被拒 = 它所在的目录 App 写不进：
        抛 DirUnwritable（手机原话 + 删除重装那一步，重试 / 续传 / 改走 devicectl 都没用）。→ (rc, 输出)"""
        ok, why = self.rm(rel)
        if not ok:
            raise DirUnwritable(f"iPhone 上的 S2Arm 写不进 {rel}，也删不掉它（{first.splitlines()[-1] if first else DENIED}；删除：{why.splitlines()[-1] if why else '失败'}）。" + REINSTALL_STEP)
        rc, out = self.call("put", src, rel)
        if rc and DENIED in out:   # 删掉了（或本来就不在）、新建照样被拒 = 它所在的目录 App 写不进，同删不掉（D-CY3B：原先报「重新装会续传」，重装次次一样）
            raise DirUnwritable(f"iPhone 上的 S2Arm 写不进 {rel}：删掉旧文件重写也被系统拒绝（{out.splitlines()[-1] if out else DENIED}），它所在的文件夹 S2Arm 写不进。" + REINSTALL_STEP)
        return rc, out

    def ls(self, rel):
        rc, out = self.call("ls", rel)
        return out.splitlines() if rc == 0 else None

    def apps(self):   # 手机上装的用户 App → [(包名, 可执行文件名, 团队号, 版本)]（团队号 / 版本读不到为 ""，旧工具只有前三列）；工具太旧（不认 apps）/ 断开 → None
        rc, out = self.call("apps")
        if rc:
            return None
        return [tuple((line.split("\t") + ["", "", ""])[:4]) for line in out.splitlines() if line.strip() and "\t" in line]

    def du(self, *rels):   # 量这几个目录的占用（只读文件信息）→ {目录: (字节, 文件数) | None（不在）}；工具太旧（没有 du）/ 断开 / 锁屏 → None
        rc, out = self.call("du", *rels)
        if rc:
            return None
        got = {}
        for line in out.splitlines():
            m = re.fullmatch(r"du (-?\d+) (\d+) (.+)", line.strip())
            if m:
                got[m[3]] = None if m[1] == "-1" else (int(m[1]), int(m[2]))
        return got

    def get_tree(self, rel, dst):   # 整个目录拉回本机；目录不在 = 成功且 0 个文件。→ (成功?, 输出)
        rc, out = self.call("get-tree", rel, dst)
        return rc == 0, out

    def rm(self, rel):   # 删机上一个文件或整个目录（已不在算成功）。→ (成功?, 输出)
        rc, out = self.call("rm", rel)
        return rc == 0, out

    def mv(self, src, dst):   # 容器内改名（目标在就覆盖；通用壳程序包 catalogue.json 先写临时名再改名就位）。→ (成功?, 输出)
        rc, out = self.call("mv", src, dst)
        return rc == 0, out

    put_big = put

    def head(self, rel, n):   # 机上文件的（长度, 开头至多 n 字节）；不在 → (-1, b"")；工具太旧不认 head / 读不了 → None（调用方不续传）
        dst = self.work / "head.bin"; dst.unlink(missing_ok=True)
        rc, out = self.call("head", rel, n, dst)
        m = re.search(r"^head (-?\d+)$", out, re.M) if rc == 0 else None
        if not m: return None
        size = int(m[1])
        return (size, dst.read_bytes() if size >= 0 and dst.is_file() else b"")

    def mkdir(self, rel):
        """以 App 身份（AFC）逐级建 rel 并核实末级可写。→ None 建好可写；写不进的那一级（相对路径）；工具太旧不认 mkdir / 连不上 → False"""
        rc, out = self.call("mkdir", rel)
        if rc == 0: return None
        m = re.search(r"^S2A_DIR_UNWRITABLE (.*)$", out, re.M)
        return m.group(1).strip() if m else False

    def put_at(self, src, rel, off):   # 本地从 off 起接着写进机上同一文件的 off 处（不截短）
        rc, out = self.call("put-at", src, rel, off)
        if rc and DENIED in out: rc, out = self.rewrite(src, rel, out)   # 已传的那段打不开写：续不了，删掉整份重传
        if rc:
            raise Fail(f"资源包写进 iPhone 时中断（{out.splitlines()[-1] if out else f'退出码 {rc}'}）：确认手机解锁、数据线插稳后再点一次")

    def tar_ok(self, rel):
        rc, out = self.call("tar-ok", rel)
        return rc == 0 and out.strip().endswith("ok")

    def put_tar(self, lst, root, rel):
        rc, out = self.call("put-tar", lst, root, rel)
        if rc:
            raise Fail(f"资源包写进 iPhone 时中断（{out.splitlines()[-1] if out else f'退出码 {rc}'}）：确认手机解锁、数据线插稳后再点一次")   # 哪个文件写进 devpush.log
        return "stream"

    def listing(self):   # 机上整个资源包目录的「路径 → 大小」：AFC 列表不带大小，读不到
        return None

    def free(self):   # 机上可写字节（AFC 设备信息）；工具太旧或读不到 → None，调用方照推
        rc, out = self.call("free", "x")
        return int(out.strip()) if rc == 0 and re.fullmatch(r"\d+", out.strip()) else None

    def nospc(self):
        return False

    def launch(self, env):   # 起 App（s2a-devpush launch：CoreDevice app_service，iOS 17+；旧工具没有 launch → 失败）
        rc, out = self.call("launch", *[f"{k}={v}" for k, v in env.items()])
        if rc: raise Fail(f"起 App 失败：{out.splitlines()[-1] if out else rc}")


def launch_reason(out, rc):
    """devicectl 拉 App 失败时挑最说明问题的一行：最后一条 NSLocalizedFailureReason（锁屏时是「…because the device was not, or could not be,
    unlocked」，apply 据此判锁屏、等玩家解锁）；没有才取末行。11-10 测试设备锁屏实测：末行是「See 'devicectl device process launch --help'…」，
    只取末行把锁屏原因丢了，玩家看到的是「没开开发者模式」那套说明、也不等解锁"""
    lines = [l.strip() for l in (out or "").strip().splitlines() if l.strip()]
    reasons = [l.split("=", 1)[1].strip() for l in lines if l.startswith("NSLocalizedFailureReason =")]
    return ((reasons or lines or [str(rc)])[-1])[:300]


class Devicectl:
    """Mac：xcrun devicectl（口径同 tools/dev_run.sh：一律用 devicectl 自带 --timeout，外层 timeout 只在它自己卡死时兜底；拷贝按大小定超时、挂了重试一次）。
    有 s2a-devpush + UDID 时流式写批次、核待解批完整性借它（手机只在 CoreDevice 无线隧道上、usbmuxd 看不到时那批退回 devicectl）"""
    kind = "devicectl"
    docs, ctx, pack = DOCS, None, None
    say = staticmethod(lambda m: print(m, flush=True))   # device() 换成调用方的；直接构造（单测）时打到标准输出

    def __init__(self, device, bid, work, udid=None, devpush=None, env=None):
        self.device, self.bid, self.work, self.udid, self.env = device, bid, Path(work), udid, env
        self.dc = shlex.split((env or os.environ).get("S2A_DEVICECTL") or "xcrun devicectl")
        self.dp = Devpush(devpush, udid, bid, work, env) if devpush and udid else None
        self.made = set()   # 已以 App 身份建好的机上目录（put 前建一次）

    def dcp(self, t, *args, log=None):
        s0 = time.time(); margin = int((self.env or os.environ).get("S2A_DC_MARGIN", "30"))
        try:
            p = subprocess.run([*self.dc, *map(str, args), "--timeout", str(t)], capture_output=True, text=True, errors="replace", timeout=t + margin, env=self.env)
            rc, out = p.returncode, p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            rc, out = 124, ""
        if log:
            with open(self.work / log, "a", encoding="utf-8") as f: f.write(out)
        if rc != 0 and time.time() - s0 >= t: rc = 124
        return rc, out

    def slow(self, what, s0, rc):
        if time.time() - s0 >= 20:
            with open(self.work / "slow_devicectl.log", "a", encoding="utf-8") as f:
                f.write(f"[{time.strftime('%H:%M:%S')}] 慢：{what} 用了 {time.time() - s0:.0f} 秒（rc={rc}）\n")

    def container(self):
        return ["--device", self.device, "--domain-type", "appDataContainer", "--domain-identifier", self.bid]

    def get(self, rel, dst, t=None):   # 调用方没给 t 时超时 120 秒、挂了重试一次
        s0 = time.time(); Path(dst).unlink(missing_ok=True)
        for _ in range(1 if t else 2):
            rc, _ = self.dcp(t or 120, "device", "copy", "from", *self.container(), "--source", rel, "--destination", dst, log="cpf.log")
            if rc != 124: break
        self.slow(f"拉 {rel}", s0, rc)
        return rc == 0 and Path(dst).is_file()

    def put(self, src, rel):   # 超时 = S2A_CP_T0（缺省 60）秒 + 每 10 MB 1 秒（实测推送约 33 MB/s，留 3 倍余量）
        # devicectl 会代建缺的父目录，iOS 26.x 上代建的归 root、App 再也写不进（a48655）：有 s2a-devpush 时先以 App 身份把父目录建好（每个目录一次）
        d = rel.rsplit("/", 1)[0] if "/" in rel else ""
        if hasattr(self.dp, "mkdir") and d and d not in self.made:
            self.made.add(d); self.dp.mkdir(d)
        s0 = time.time(); t = int((self.env or os.environ).get("S2A_CP_T0", "60")) + Path(src).stat().st_size // (10 << 20)
        for i in (1, 2):
            rc, out = self.dcp(t, "device", "copy", "to", *self.container(), "--source", src, "--destination", rel, log="cp.log")
            if rc != 124: break
            with open(self.work / "slow_devicectl.log", "a", encoding="utf-8") as f: f.write(f"[{time.strftime('%H:%M:%S')}] 挂：推 {rel} 超 {t} 秒（第 {i} 次）\n")
        self.slow(f"推 {rel}", s0, rc)
        if rc and DEV_MODE_OFF.search(out or ""):
            dev = "Apple TV" if tv(self.env) else "iPhone"
            raise DeveloperModeOff(f"{dev} 没开开发者模式，资源包推不进去：在 {dev} 上打开「设置 → 隐私与安全性 → 开发者模式」，按提示重启，解锁后再点一次")
        if rc:
            if tv(self.env):   # Apple TV（#1696）：没有数据线，同网经 devicectl 推
                raise Fail("资源包写进 Apple TV 时中断（" + ("Apple TV 空间不够" if self.nospc() else f"devicectl 退出码 {rc}") + "）：确认 Apple TV 开着、没有休眠、和这台电脑在同一个网络、空间够后再点一次")
            raise Fail("资源包写进 iPhone 时中断（" + ("iPhone 空间不够" if self.nospc() else f"devicectl 退出码 {rc}") + "）：确认手机解锁、数据线插稳、空间够后再点一次")   # 哪个文件写进 cp.log

    def put_big(self, src, rel):   # 大文件优先走 s2a-devpush 流式（usbmuxd 能到时快且不落临时文件），不行退回 devicectl（大于上限的不退，见 DC_FALLBACK_MAX）
        if self.dp:
            try:
                try:
                    return self.dp.put(src, rel)
                except DirUnwritable:
                    raise
                except Fail as first:   # 流式断一次多半是瞬时的（插拔 / 同网切线）：隔 3 秒再流一次，再不行才考虑 devicectl（预检清单 #77）
                    if nospace(str(first)): raise   # 手机写满了：重试、换 devicectl 都写不进，直接交给 run 按写满报
                    self.say(f"流式写 {Path(rel).name} 断了（{str(first)[-160:]}），3 秒后再试一次")
                    time.sleep(3)
                    return self.dp.put(src, rel)
            except DirUnwritable:
                self.say(self.owner_note(rel))   # 机上那条目录归谁、什么权限：定是「App 自己的、只是缺写权限」还是「归别的用户」（D-A76N）
                raise
            except Fail as e:
                if nospace(str(e)): raise
                # devicectl 会把整个文件读进系统设备服务的内存（#641）：本机可用内存的一半装得下才交给它（至少放行 S2A_DC_FALLBACK_MAX / 2 GiB）
                size, mem = Path(src).stat().st_size, avail_mem()
                cap = max(int((self.env or os.environ).get("S2A_DC_FALLBACK_MAX") or DC_FALLBACK_MAX), mem // 2)
                if size > cap:
                    raise Fail(f"流式写 {Path(rel).name} 中断（{str(e)[-160:]}）。这份有 {size / 2**30:.1f} GB，本机可用内存 {mem / 2**30:.1f} GB，"
                               "交给 devicectl 会把整份读进内存、拖垮系统的设备服务，不改走 devicectl") from None
                self.say(f"流式写 {Path(rel).name} 没成（{str(e)[-160:]}），改走 devicectl")
        self.put(src, rel)

    def owner_note(self, rel):
        """写不进时读机上「文件所在目录」与文件本身的属主 / 权限（devicectl device info files --no-recurse --json-output，只列一层）→ 一句非玩家读数。
        D-A76N：afc 删掉重写也被拒、devicectl 却写得进。测试设备复现过一种成因：目录是 App 自己的（uid 501）但权限 0555——afc 建 / 删都拒、devicectl 照写；
        另一种是 #569 归别的用户（root）。两种修法不同（前者 App 能自己补权限，后者只能重装），所以把读数记下来"""
        d, name = rel.rsplit("/", 1) if "/" in rel else ("", rel)
        parent, dname = d.rsplit("/", 1) if "/" in d else ("", d)
        notes = []
        for sub, want in ((parent, dname), (d, name)):
            js = self.work / "owner.json"; js.unlink(missing_ok=True)
            rc, _ = self.dcp(30, "device", "info", "files", *self.container(), *(["--subdirectory", sub] if sub else []), "--no-recurse", "--json-output", js)
            try:
                files = json.loads(js.read_text(encoding="utf-8"))["result"]["files"]
                m = next(f["metadata"] for f in files if f.get("relativePath", f.get("name")) == want or f.get("name") == want)
                notes.append(f"{sub + '/' if sub else ''}{want}：属主 uid {m.get('ownerUid')}、权限 {int(m.get('permissions', 0)):o}")
            except (OSError, ValueError, KeyError, TypeError, StopIteration):
                notes.append(f"{sub + '/' if sub else ''}{want}：读不到（devicectl rc={rc}）")
        return "写不进的位置（机上属主 / 权限，非玩家句）：" + "；".join(notes)

    def head(self, rel, n):   # 读开头 n 字节只走 s2a-devpush（devicectl 读不了文件的一段）；只要长度（n=0）时 devicectl 列一层父目录也答得出
        if self.dp: return self.dp.head(rel, n)
        return self.size(rel) if n == 0 else None

    def size(self, rel):   # 机上一个文件的长度（devicectl 列父目录一层，--json-output）→ (长度, b"") / 不在 (-1, b"") / 列不出 None。
        #   Apple TV 没有 s2a-devpush（#1696 不走数据线通道）：原先 head 一律 None，压缩块断了每次整份重推、推完也核不了这一块（D-GZHR）
        d, name = rel.rsplit("/", 1) if "/" in rel else ("", rel)
        js = self.work / "size.json"; js.unlink(missing_ok=True)
        rc, _ = self.dcp(60, "device", "info", "files", *self.container(), *(["--subdirectory", d] if d else []), "--no-recurse", "--json-output", js)
        try:
            files = json.loads(js.read_text(encoding="utf-8"))["result"]["files"]
        except (OSError, ValueError, KeyError, TypeError):
            return (-1, b"") if rc == 0 else None
        for f in files:
            if isinstance(f, dict) and (f.get("relativePath") or f.get("name") or "").rsplit("/", 1)[-1] == name and not (f.get("resources") or {}).get("isDirectory"):
                return int((f.get("metadata") or {}).get("size") or 0), b""
        return -1, b""

    def put_at(self, src, rel, off):
        if not self.dp: raise Fail("没有 s2a-devpush，续传不了")
        return self.dp.put_at(src, rel, off)

    def ls(self, rel):
        rc, out = self.dcp(60, "device", "info", "files", *self.container(), "--subdirectory", rel, "--no-recurse")
        return out.splitlines() if rc == 0 else None

    def tar_ok(self, rel):   # 没有 s2a-devpush / UDID：判不了（None），调用方当不完整盖掉
        return self.dp.tar_ok(rel) if self.dp else None

    def put_tar(self, lst, root, rel):
        if self.dp:
            try:
                return self.dp.put_tar(lst, root, rel)
            except Fail as e:
                self.say(f"流式推 {Path(rel).name} 没成（{str(e)[-160:]}），这批改走 devicectl")
        tf = self.work / "push.tar"
        try:
            names = [l for l in Path(lst).read_text(encoding="utf-8").splitlines() if l]
            with tarfile.open(tf, "w", format=tarfile.PAX_FORMAT) as t:
                for n in names:
                    ti = t.gettarinfo(str(Path(root) / n), arcname=n); ti.uid = ti.gid = 0; ti.uname = ti.gname = ""
                    with open(Path(root) / n, "rb") as f: t.addfile(ti, f)
            self.put(tf, rel)
        finally:
            tf.unlink(missing_ok=True)   # 失败也删：10-29 局目录里留下 10.7 GB 的 push.tar
        return "devicectl"

    def listing(self):
        js = self.work / "r.list.json"
        rc, _ = self.dcp(300, "device", "info", "files", *self.container(), "--subdirectory", rp(self), "--json-output", js)
        if rc: return None
        try:
            return {f["relativePath"]: f["metadata"]["size"] for f in json.loads(js.read_text())["result"]["files"] if not f.get("resources", {}).get("isDirectory")}
        except (OSError, ValueError, KeyError, TypeError):
            return None

    def free(self):   # 机上可写字节：有 s2a-devpush 先问它（AFC 设备信息），再退 ideviceinfo AmountDataAvailable；UDID 没给就按设备标识从 devicectl 设备列表换；读不到 None
        # 11-17：原先只问 ideviceinfo，玩家机上多半没装 libimobiledevice，推之前一律「读不到 iPhone 剩余空间」照推，推到一半写满才停
        if self.dp:
            v = self.dp.free()
            if v is not None: return v
        ud = self.udid
        if not ud:
            js = self.work / "devlist.json"
            self.dcp(60, "list", "devices", "--json-output", js)
            try:
                for d in json.loads(js.read_text())["result"]["devices"]:
                    u = d.get("hardwareProperties", {}).get("udid", "")
                    if self.device in (d.get("identifier"), u): ud = u; break
            except (OSError, ValueError, KeyError, TypeError):
                pass
        if not ud or not shutil.which("ideviceinfo"): return None
        try:
            out = subprocess.run(["ideviceinfo", "-u", ud, "-q", "com.apple.disk_usage", "-k", "AmountDataAvailable"], capture_output=True, text=True, timeout=20).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            return None
        return int(out) if re.fullmatch(r"\d+", out) else None

    def launch(self, env):
        # 没有环境变量就不带 --environment-variables：devicectl 收到 '{}' 直接拒（「Unable to decode environment variables」，11-10 测试设备实测），
        #   多款容器拉起到游戏库（env 为空）在 Mac 上因此从来拉不起，不论锁没锁屏
        envs = ["--environment-variables", json.dumps(env)] if env else []
        rc, out = self.dcp(120, "device", "process", "launch", "--device", self.device, "--terminate-existing", *envs, self.bid)
        if rc: raise Fail(f"起 App 失败：{launch_reason(out, rc)}")

    def lock_state(self):   # 手机此刻锁着没有（devicectl device info lockState：passcodeRequired）→ True / False；问不到 None
        js = self.work / "lockstate.json"; js.unlink(missing_ok=True)
        rc, _ = self.dcp(30, "device", "info", "lockState", "--device", self.device, "--json-output", js)
        try:
            v = json.loads(js.read_text())["result"]["passcodeRequired"] if rc == 0 else None
        except (OSError, ValueError, KeyError, TypeError):
            v = None
        return v if isinstance(v, bool) else None

    def nospc(self):   # 刚失败的那次拷贝是不是空间满（cp.log 末尾有 POSIX 28）
        try:
            tail = (self.work / "cp.log").read_text(errors="replace").splitlines()[-40:]
        except OSError:
            return False
        return any(nospace(l) for l in tail)

    say = staticmethod(lambda m: None)


def device(dev, bid, work, udid=None, devpush=None, via=None, env=None, say=print, docs=DOCS, ctx=None, pack=None):
    """按平台挑设备操作：Windows（没有 devicectl）或 via=devpush → s2a-devpush（dev 就是 UDID）；Mac → devicectl（有工具 + UDID 时借 s2a-devpush 流式）"""
    env = dict(os.environ) if env is None else env
    udid = udid or env.get("S2A_PUSH_UDID") or env.get("SW_IOS_UDID") or None
    exe = devpush_path(devpush, env)
    if via == "devpush" or (via is None and os.name == "nt"):
        if not exe: raise Fail("没有 s2a-devpush（cargo build --release --manifest-path pack/devpush/Cargo.toml --target-dir build/devpush）")
        d = Devpush(exe, udid or dev, bid, work, env); d.why = f"工具 {exe}"; d.docs, d.ctx, d.pack = docs, ctx, pack; return d
    d = Devicectl(dev, bid, work, udid, exe, env); d.say = say; d.docs, d.ctx, d.pack = docs, ctx, pack
    d.why = "没有 s2a-devpush" if not exe else "不知道手机 UDID（S2A_PUSH_UDID）" if not udid else f"工具 {exe}"
    return d


def rows(text, n):   # 制表符分隔、恰 n 列的行 → {路径: 末列}
    out = {}
    for l in (text or "").splitlines():
        w = l.split("\t")
        if len(w) == n: out[w[0]] = w[-1]
    return out


def read(dev, rel, work, name):   # 机上一个文本文件拉回 <work>/<name>；拉不到 → None
    p = Path(work) / name
    return p.read_text(encoding="utf-8", errors="replace") if dev.get(rel, p) else None


def jver(text):
    try: return json.loads(text or "").get("version")
    except (ValueError, AttributeError): return None


def batches_of(pack, todo):
    """→ [[行…]…] 每批 ≤1 GB（按 index.tsv 顺序，单个超 1 GB 的文件自成一批）；行 = [路径, 大小, sha]"""
    out, cur, sz = [], [], 0
    for w in todo:
        n = int(w[1])
        if cur and sz + n > LIM: out.append(cur); cur, sz = [], 0
        cur.append(w); sz += n
    if cur: out.append(cur)
    return out


def plan(dev, pack, work, full=False, say=print):
    """算待推批次：写 <work>/push_NNN.tsv（路径 \\t 大小 \\t sha）与 <work>/pushed（续推账）；→ (批数, 文件数, 字节数)"""
    pack, work = Path(pack), Path(work)
    for f in work.glob("push_*.tsv"): f.unlink()
    ver = jver((pack / "manifest.json").read_text(encoding="utf-8") if (pack / "manifest.json").is_file() else "")
    if ver not in (2, 3):   # 3 = v0.3.0 单一 IMG（pack/img.py convert：game.img + 照旧按文件的着色器库 / 覆盖层）
        # 维护者 11-17：「校验不过就整个拒绝」一律撤掉——版本不认只打黄、照常按清单推；真推不了（缺 index.tsv 等）在后面按原因报
        say(f"【黄】资源包清单版本 {ver} 这个工具不认（只认 2、3：{pack}/manifest.json），照常推送")
    if full:
        have, pushed, keep = {}, {}, []
        say("资源包：S2A_PUSH_FULL=1，整包推送")
    else:
        names = sorted({m for l in (dev.ls(inc(dev)) or []) for m in STALE.findall(l)})
        keep, bad = [], []
        for n in names: (keep if dev.tar_ok(f"{inc(dev)}/{n}") else bad).append(n)
        if bad:
            e = work / "empty.tar"; e.write_bytes(b"\0" * 1024)
            try:
                for n in bad:
                    try: dev.put(e, f"{inc(dev)}/{n}")
                    except Fail: say(f"盖掉机上残留推送包 {n} 失败"); raise Fail("盖掉残留推送包失败")
            finally:
                e.unlink(missing_ok=True)
            say(f"机上留着上次没解完的推送包 {len(bad)} 个（{' '.join(bad)[:80]}）：已用空包盖掉，不信 .pushed，按已应用索引补推")
        unpack = read(dev, unpack_log(dev), work, "r.unpack.pre")
        cut = bool(bad) or "截短包" in ((unpack or "").strip().splitlines() or [""])[-1]
        if cut and not bad: say(f"上次收推送撞到截短包（{(unpack or '').strip().splitlines()[-1][:60]}…）：不信 .pushed，按已应用索引补推")
        idx = None
        mf = read(dev, f"{rp(dev)}/manifest.json", work, "r.manifest")
        # v3（单一 IMG）机上同样有 index.applied.tsv：原先只认 v2，IMG 款每次装机都把镜像外的文件整批重推、game.img 再核一遍、App 再解一次包（11-13 实测：同一个包第二次装仍推 37 个文件）
        if jver(mf) in (2, 3) or (mf is not None and not mf.strip()):   # 空清单 = 推送开头置空、没推完（send），已应用索引照信，只补推缺的（不整包重推）
            idx = read(dev, f"{rp(dev)}/index.applied.tsv", work, "r.index")
            if not idx and not cut: idx = read(dev, f"{rp(dev)}/index.tsv", work, "r.index")
        have = rows(idx, 3)
        # .pushed 只在机上还有推完整、没解开的批时才算数（推到一半那批的文件不会在里面：一批 put-tar 成功后才追加）
        pushed = rows(read(dev, f"{rp(dev)}/.pushed", work, "r.pushed"), 2) if keep and not cut else {}   # 有残批 / 上次撞截短包：不信 .pushed
        if keep: say(f"机上有推完、还没解开的推送包 {len(keep)} 个：其中 {len(pushed)} 个文件算已推，不重推")
        if have: have = spot_check(dev, pack, work, have, say)
        cut_img = [r for r in have if is_direct(r)]   # 账上有镜像才去机上核：没有镜像的包不多一次设备往返
        if cut_img and not (read(dev, f"{rp(dev)}/.applied", work, "r.applied.pre") or "").strip():   # send 写镜像前把 .applied 清空：空 = 上次传镜像被打断，机上那份不完整，账不能信
            # 例外：.pushed 里记着这份镜像 = 上次已整份传完（put_big 成功后才记账），.applied 空只是后面解包没成（#457：原先照样打「被打断，这次重传」，
            #   实际按 .pushed 不重传、推送 0 个文件，日志自相矛盾，把排查带偏）。这时如实说镜像已传完、问题在解包
            done = [r for r in cut_img if pushed.get(r) == have[r]]
            if done: say(f"{done[0]} 上次已整份传完（推送账上有），不重传；上次是机上解包没成，见手机上的 unpack.log")
            redo = [r for r in cut_img if r not in done]
            if redo: say(f"上次传 {redo[0]} 被打断，这次重传")
            have = {r: h for r, h in have.items() if r not in redo}
        elif cut_img and getattr(dev, "head", None):   # .applied 不空也要核镜像在机上、长度对（#2161：账上记着 game.img、机上没了，抽查不看大文件，
            #   原先这里不核，每次推 0 个 / 只推小文件，App 解完核对缺 game.img 报 -8、不改账，循环下去）。判据同 App 解包核对（index_check_core：在且长度 = index.tsv）；
            #   读不了长度（工具太旧）照旧信账
            size = {w[0]: w[1] for w in (l.split("\t") for l in (pack / "index.tsv").read_text(encoding="utf-8").splitlines()) if len(w) == 3}
            for r in cut_img:
                got = dev.head(f"{rp(dev)}/{r}", 0)
                if got is None or str(got[0]) == size.get(r): continue
                say(f"已应用索引记着 {r}，机上{'没有' if got[0] < 0 else f'长度 {got[0]} 字节、应为 {size.get(r)} 字节'}：这次重传")
                have.pop(r, None)
    want =[w for w in (l.split("\t") for l in (pack / "index.tsv").read_text(encoding="utf-8").splitlines()) if len(w) == 3]
    acct = {**have, **pushed}
    todo = [w for w in want if acct.get(w[0]) != w[2]]
    direct = [w for w in todo if is_direct(w[0])]
    (work / "direct_img.tsv").write_text("".join("\t".join(w) + "\n" for w in direct), encoding="utf-8")
    bs = batches_of(pack, [w for w in todo if not is_direct(w[0])])
    for i, b in enumerate(bs):
        (work / f"push_{i:03d}.tsv").write_text("".join("\t".join(w) + "\n" for w in b), encoding="utf-8")
    (work / "pushed").write_text("".join(f"{r}\t{h}\n" for r, h in pushed.items()), encoding="utf-8")
    return len(bs), len(todo), sum(int(w[1]) for w in todo)


def spot_check(dev, pack, work, have, say):
    """推送前核机上实况（已应用索引是 App 写的账，不等于机上真有）→ 核过的账（可能删掉了对不上的行，或整个清空）"""
    want = {w[0]: w for w in (l.split("\t") for l in (Path(pack) / "index.tsv").read_text(encoding="utf-8").splitlines()) if len(w) == 3}
    same = [w for r, w in want.items() if have.get(r) == w[2]]
    pick = [w for w in same if w[0].startswith("shaders/direct/") and ("/" not in w[0][15:] or re.fullmatch(r"shaders/direct/b-[^/]+/lib\.idx", w[0]))]
    # 游戏库起局必需的小文件（同 runtime/src/pack_apply.c s2a_pack_core_path：exefs/、ctl/control.nacp）每次都核：账上记着、机上没了就补推。
    #   #1059 D-JAT7：已应用索引记着 exefs/main.npdm（0.3.x 时 pack_apply 只按 index 记账），机上实际没有；抽查没抽到它，
    #   推送当「已在」跳过，App 解开后核程序描述不在、每次重装都一样
    pick += [w for w in same if (w[0].startswith("exefs/") or w[0] == "ctl/control.nacp") and int(w[1]) < 4 << 20 and w not in pick]
    pick += sorted((w for w in same if int(w[1]) < 4 << 20 and w not in pick), key=lambda w: w[2])[:5]
    chk = Path(work) / "chk"; chk.mkdir(exist_ok=True); bad = []
    for i, w in enumerate(pick, 1):
        f = chk / str(i)
        if (dev.get(f"{rp(dev)}/{w[0]}", f, t=30) or dev.get(f"{rp(dev)}/{w[0]}", f, t=60)) and hashlib.sha256(f.read_bytes()).hexdigest() == w[2]:
            continue
        bad.append(w[0])
        if len(bad) == 1: say(f"资源包：机上实况与已应用索引不符：{w[0]} 拉不回或 sha256 不对")
    if not bad:
        say(f"资源包：机上实况抽查 {len(pick)} 个文件，与已应用索引一致"); return have
    lst = dev.listing()
    if lst is None:
        say(f"资源包：抽查 {len(pick)} 个、{len(bad)} 个不符，机上清单拉不回，不信已应用索引，整包重推"); return {}
    rws = [l.split("\t") for l in (Path(work) / "r.index").read_text(encoding="utf-8", errors="replace").splitlines()] if (Path(work) / "r.index").is_file() else []
    rws = [w for w in rws if len(w) == 3]
    keep = {w[0]: w[2] for w in rws if w[0] not in bad and str(lst.get(w[0], -1)) == w[1]}
    say(f"资源包：抽查 {len(pick)} 个、{len(bad)} 个不符；按机上实际清单核账：{len(keep)}/{len(rws)} 行在且大小对得上留下，其余补推")
    return keep


def need_bytes(work, nbytes, env=None):
    """机上要的可写字节 = 待推量 + 最大一批（解开时这批的 tar 和解开的文件同时在，解完一批删一批）+ 每文件 4 KB（块取整）+ S2A_PUSH_SPARE_GB（缺省 1）GB。
    实测（11-08 测试设备2）：动森 11.37 GB、10.5 万文件，峰值 12.40 GB，本式 13.95 GB；原先按待推量 × 2 要 23.8 GB，多拦了空间够的玩家"""
    spare = int((env or os.environ).get("S2A_PUSH_SPARE_GB", "1"))
    biggest = files = 0
    for f in Path(work).glob("push_*.tsv"):
        rows = [l.split("\t") for l in f.read_text(encoding="utf-8").splitlines() if l]
        biggest = max(biggest, sum(int(w[1]) for w in rows)); files += len(rows)
    return nbytes + biggest + files * 4096 + spare * (1 << 30)


PHONE_FREE = "Documents/s2a_free.tsv"   # 手机上的 App 每次启动记的可用空间（ios/app.m free_note）：重要用途 \t 可选用途 \t statfs 空闲 \t 写入时刻


def phone_free(dev, work):
    """手机上的 App 上次启动时记的「重要用途」可用字节（含系统随时能清掉的空间，同「设置 → 通用 → iPhone 储存空间」的「可用」）和它记了多少秒：
    电脑端经 AFC / lockdown 读到的空闲不含可清除空间（玩家反馈：「设置」里剩 70 多 GB，一键安装说只剩 2 GB）。没装过 / 没打开过 / 读不到 → (None, None)"""
    dst = Path(work) / "s2a_free.tsv"
    try:
        if not dev.get(PHONE_FREE, str(dst), 30): return None, None
        f = dst.read_text().split()
        imp, at = int(f[0]), int(f[3])
    except (OSError, ValueError, IndexError, Fail):
        return None, None
    return (imp, max(0, int(time.time()) - at)) if imp > 0 else (None, None)


def space(dev, nbytes, work, env=None, say=print):
    """→ (可写字节或 None, 要的字节, 是否手机记的重要用途口径)：AFC / lockdown 报的空闲与手机上的 App 记的「重要用途」可用取大的。
    手机记的是 App 上次启动那一刻的：之后又装了东西会偏大——只用来决定要不要先问玩家，真写满了推送照样停下说明（SpaceFull）"""
    fb = dev.free(); imp, age = phone_free(dev, work)
    if imp is not None and (fb is None or imp > fb):
        say(f"手机可用空间按 App 记的「重要用途」口径算：{gb1(imp)} GB（{age // 60} 分钟前记的；电脑端读到的空闲 {gb1(fb) if fb is not None else '读不到'} GB，不含可清除空间）")
        return imp, need_bytes(work, nbytes, env), True
    return fb, need_bytes(work, nbytes, env), False


PROGRESS_PREFIX = "S2A_PUSH_PROGRESS"   # --progress-lines 打的机器读行：S2A_PUSH_PROGRESS <已传字节> <本次待传总字节> <已完成批次> <总批次>


def resume_offset(dev, src, rel, size):
    """单一 IMG（game.img）断点续传的起点：机上已有这份的前一部分 → 按页向下取整的字节数；从头写 → 0。
    同一份的判据：机上开头一整页（IMG 头：游戏号、资源包号、各段偏移、整个文件长）与本地逐字节相同，且头里的资源包号不空；
    机上比本地长、读不了机上（工具太旧 / devicectl 退路）、旧形态 romfs.img（没有这种头）都从头写"""
    if Path(src).name != "game.img" or not hasattr(dev, "head"): return 0
    got = dev.head(rel, IMG_PAGE)
    if not got or got[0] < 2 * IMG_PAGE or got[0] > size: return 0
    with open(src, "rb") as f: mine = f.read(IMG_PAGE)
    if got[1] != mine or not mine[IMG_PACK_ID].strip(b"\0"): return 0
    return min(got[0] // IMG_PAGE * IMG_PAGE, size)


# 压缩传输（v0.5.0）：game.img 按 ZCHUNK 原始字节切块，每块一个独立 zstd 帧（带内容校验），推成 <资源包根>/.incoming/game.img.<资源包号前 16 位>.<块号 4 位>.zst（资源包号 = index.tsv 里 game.img 那行第三列，pack/img.py）；
#   手机上的 App 解开时按块号顺序流式解进 game.img 的块号 × ZCHUNK 处、解完一块删一块（runtime/src/pack_apply.c img_unzst），机上仍存未压缩的 game.img，运行时零改动。
#   机上峰值 ≤ 未压缩 game.img + 一块：推的时候压缩块总和 < 原大小，解的时候已解的部分 + 剩下的压缩块单调从压缩总量涨到原大小。
#   只压 game.img：单一 IMG 形态里它占 99% 以上（11-16 丝之歌 6704 / 6728 MB），旁边的着色器库 / 覆盖层照旧按 tar 批推。
#   按款判压不压：整份 game.img 按推送同一套参数（zstd 9 档多线程，同 pack/jobs/programs.py）逐块压一遍量出每块压缩后字节，省不到 ZSAVE 不压；
#   量数按资源包号记在资源包目录旁的 <资源包目录名>.zstd.json（不放进资源包目录：导出按目录里全部文件算来源戳），同一份包只量一次
ZCHUNK = 1 << 30
ZLEVEL = 9
ZSAVE = 0.15   # 维护者定：按款实测省不到 15% 的款原样推
ZNAME = re.compile(r"game\.img\.([0-9a-f]{16})\.(\d{4})\.zst")


# 流式压（D-DVFJ：7 GB 内存的 Windows 机器、此刻可用 0.1 GB，推第一块就 MemoryError）：原先每块 1 GiB 整块读进内存、整块压、压出的字节也整块攥着，
#   再加 zstd 按核数开的工作线程各自的缓冲，一块峰值好几 GB。现在每块仍是一个独立 zstd 帧（块大小 ZCHUNK 与块名是两端契约，pack_apply.c img_unzst 按块号 × ZCHUNK 解回原处），
#   但按 ZREAD 分段读、喂流式压缩器、压出的边写边丢（推送写进这块的临时文件，量数只累计长度）；帧头照写原文长度（pledged size），解压端本就流式解。
#   工作线程固定 ZWORKERS 个（#1942：zstd 压出的帧随线程数变——Windows 随包 zstandard 0.25 实测 1 / 2 / 24 线程同一块差几个字节，
#   量数与推送线程数不同块长度就对不上；固定 2 个，内存约 ZBASE_MB + 2 × ZWORKER_MB，1 GB 一块约 2 秒）。
#   流式帧与原先一次性压的帧差几个头字节：量数缓存带 fmt，旧缓存作废重量，机上旧格式的块长度对不上就重推
ZREAD = 8 << 20
ZBASE_MB, ZWORKER_MB = 160, 80
ZWORKERS = 2
ZFMT = 3   # 3：固定线程数（#1942）后的帧，旧量数作废重量


def zstd_workers():
    """zstd 工作线程数：固定 ZWORKERS（#1942）。量压缩率时记下的每块大小要和推的时候压出来的逐字节一样长（续传判「这块推完了没」、
    Apple TV 推完核对都比它），而 zstd 压出的帧随线程数变（Windows 随包 zstandard 0.25 实测 1 GB 一块：1 / 2 / 24 线程分别差几个字节），
    原先线程数随此刻可用内存变（D-DVFJ），量和推之间一变就对不上。固定 2 线程：实测每 GB 约 2 秒，内存约 ZBASE_MB + 2 × ZWORKER_MB"""
    return ZWORKERS


def zstd_compressor():
    """→ 压一块的函数 z(路径, 块号, out=None) → 压缩后字节数：把文件第 k 块（ZCHUNK 原始字节，末块到文件尾）压成一个 zstd 帧（带内容校验与原文长度），
    out 给了就边压边写进去；本机 Python 两样都没有 → None（照原样推）。
    Python 3.14 用 compression.zstd，随包 Python 3.12 用 zstandard（launchers/macos/requirements-runtime.txt、Windows 同钉版）"""
    try:
        from compression import zstd
        P = zstd.CompressionParameter

        def stream(raw):
            c = zstd.ZstdCompressor(options={P.compression_level: ZLEVEL, P.nb_workers: zstd_workers(), P.checksum_flag: 1})
            c.set_pledged_input_size(raw)
            return c.compress, lambda: c.flush(zstd.ZstdCompressor.FLUSH_FRAME)
    except ImportError:
        try:
            import zstandard
        except ImportError:
            return None

        import io

        class Sink(io.RawIOBase):   # 收 stream_writer 吐出的压缩字节，feed / finish 每次取走
            def __init__(self): super().__init__(); self.parts = []
            def writable(self): return True
            def write(self, b): self.parts.append(bytes(b)); return len(b)
            def take(self): out = b"".join(self.parts); self.parts.clear(); return out

        # 用 stream_writer，不用 compressobj（#1942，Windows 随包 zstandard 0.25 实测 1 GB 一块：compressobj 1 线程 19.6 秒、24 线程 260 秒，
        #   stream_writer 1 线程 4.5 秒、24 线程 1.7 秒，压出的字节数相同）。原先「测压缩率」一块就停十几分钟，界面一直 0%，玩家当成挂住
        def stream(raw):
            sink = Sink()
            w = zstandard.ZstdCompressor(level=ZLEVEL, threads=zstd_workers(), write_checksum=True).stream_writer(sink, size=raw, closefd=False)
            def feed(b): w.write(b); return sink.take()
            def finish(): w.flush(zstandard.FLUSH_FRAME); return sink.take()
            return feed, finish

    def z(path, k, out=None, tick=None):   # tick(这块已读进的原始字节)：每读一段报一次（测压缩率按字节报进度，#1942）
        with open(path, "rb") as f:
            f.seek(0, 2); raw = max(0, min(ZCHUNK, f.tell() - k * ZCHUNK)); f.seek(k * ZCHUNK)
            feed, finish = stream(raw); n, left = 0, raw
            while left:
                b = f.read(min(ZREAD, left))
                if not b: raise OSError(f"{path} 第 {k} 块读到 {raw - left} 字节就到了文件尾（应 {raw}）")
                left -= len(b); o = feed(b); n += len(o)
                if out is not None and o: out.write(o)
                if tick: tick(raw - left)
            o = finish(); n += len(o)
            if out is not None and o: out.write(o)
        return n
    return z


def zname(sha, k): return f"game.img.{sha[:16]}.{k:04d}.zst"


def zstd_plan(pack, row, say=print, stage=None, env=None, comp=None):
    """这份 game.img（index.tsv 一行：路径、字节、sha256）压不压 → 每块压缩后字节的列表（压）或 None（原样推）"""
    if row[0] != "game.img" or (env or os.environ).get("S2A_PUSH_ZSTD") == "0": return None
    comp = comp or zstd_compressor()
    if comp is None:
        say("压缩传输：本机 Python 没有 zstd 模块（compression.zstd / zstandard），原样推"); return None
    pack = Path(pack); raw, sha = int(row[1]), row[2]
    cache = pack.parent / f"{pack.name}.zstd.json"
    try:
        c = json.loads(cache.read_text(encoding="utf-8"))
        sizes = c["z"] if (c.get("sha"), c.get("level"), c.get("chunk"), c.get("fmt")) == (sha, ZLEVEL, ZCHUNK, ZFMT) else None
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        sizes = None
    if sizes is None:
        t0, n, sizes = time.time(), (raw + ZCHUNK - 1) // ZCHUNK, []
        for k in range(n):
            if stage: stage(STAGE_ZSTD, k * ZCHUNK, raw)
            # 块内按读进的字节报进度（#1942：原先只在每块 1 GB 开头报一次，Windows 上一块压十几分钟时界面一直 0%，玩家当成挂住）
            tick = (lambda d, base=k * ZCHUNK: stage(STAGE_ZSTD, base + d, raw)) if stage else None
            sizes.append(comp(pack / row[0], k, tick=tick) if tick else comp(pack / row[0], k))
        try:
            tmp = cache.with_name(cache.name + ".tmp")
            tmp.write_text(json.dumps(dict(sha=sha, level=ZLEVEL, chunk=ZCHUNK, fmt=ZFMT, raw=raw, z=sizes)), encoding="utf-8"); os.replace(tmp, cache)
        except OSError as e:
            say(f"压缩传输：量数记不下（{e}），下次推还会再量")
        say(f"压缩传输：量了 {row[0]} {n} 块，用时 {time.time() - t0:.0f} 秒")
    z = sum(sizes); saved = 1 - z / max(raw, 1)
    yes = saved >= ZSAVE
    say(f"压缩传输：{row[0]} {raw / 1e9:.2f} GB → {z / 1e9:.2f} GB（省 {saved:.0%}）" + ("，压缩推" if yes else f"，省不到 {ZSAVE:.0%}，原样推"))
    return sizes if yes else None


def _send(dev, pack, work, say=print, progress=None, zp=None):
    """推 plan 写好的批次 → 推了几批；每批写完才把它的文件记进 .pushed 并推上机；最后推索引与清单。
    zp：{镜像相对路径: 每块压缩后字节}（zstd_plan 判要压的），这些镜像改推压缩块（send_zst），不直写。
    progress(已传字节, 本次待传总字节, 已完成批次, 总批次)：流式工具按秒报文件 / 批内进度，旧工具与 devicectl 退路仍按批报（#266）。
    计数和手机蒙版同一口径：分母 = 已在手机上的 + 本次待传（plan 算好的差量），首装就是本次待传量；批内最多报到 size-1，成功记账后才报满。"""
    pack, work = Path(pack), Path(work)
    files = sorted(work.glob("push_*.tsv"))
    led = work / "pushed"; ledger = led.read_text(encoding="utf-8") if led.is_file() else ""
    nt = nb = ns = nd = done = 0; ts = int(time.time()); t0 = time.time()
    dl = work / "direct_img.tsv"
    direct = [l.split("\t") for l in dl.read_text(encoding="utf-8").splitlines() if l] if dl.is_file() else []
    batches = [[l.split("\t") for l in bf.read_text(encoding="utf-8").splitlines() if l] for bf in files]
    total, steps, step = sum(int(w[1]) for w in direct) + sum(int(w[1]) for b in batches for w in b), len(direct) + len(files), 0
    zp = zp or {}
    def transfer(method, *args, size, scale=1.0):   # scale：压缩块按已传压缩字节 × 原始 / 压缩折回原始字节报（计数口径仍是原始字节）
        stream = getattr(dev, "dp", None) or dev
        old = getattr(stream, "progress", None); seen = 0
        def report(n):
            nonlocal seen
            n = max(0, min(int(n * scale), size - 1))
            if n > seen:
                seen = n
                progress(done + n, total, step, steps)
        stream.progress = report if progress else None
        try: return method(*args)
        finally: stream.progress = old
    # base = 这款已在手机上的字节（整包 index.tsv 合计 − 本次要传）：电脑端计数和手机蒙版同一口径（已有 + 本次已到 / 整款），
    #   续传 / 重装时两边显示同一个百分比，也不因这次只传剩下的而从 0 涨起
    whole = sum(int(r.split("\t")[1]) for r in (pack / "index.tsv").read_text(encoding="utf-8").splitlines() if r.count("\t") >= 2 and r.split("\t")[1].isdigit()) if total else 0
    base = max(0, whole - total)
    if progress and base:
        report_raw = progress
        progress = lambda d, t, k, n: report_raw(base + d, base + t, k, n)
    if progress: progress(0, total, 0, steps)
    if direct or batches:   # 传输清单：手机上的游戏库按它画装机蒙版（各文件已到多少字节 / 本次总字节，S2ArmShellHostModel），只写这一次——
        #   手机自己看正在写的文件，不依赖电脑端上报，也不跟推包抢 USB
        #   压缩推的镜像列它的各压缩块（手机上看的是块文件长到多少）
        tr = work / "transfer.json"
        listed = [f for w in direct for f in ([[f".incoming/{zname(w[2], k)}", z] for k, z in enumerate(zp[w[0]])] if w[0] in zp else [[w[0], int(w[1])]])] + \
                 [[f".incoming/{ts}_{k:03d}.tar", sum(int(w[1]) for w in b)] for k, b in enumerate(batches)]
        tr.write_text(json.dumps(dict(schema=1, total=sum(f[1] for f in listed), base=base, started=ts, files=listed)), encoding="utf-8")
        try: dev.put(tr, f"{rp(dev)}/.transfer")
        finally: tr.unlink(missing_ok=True)
    if direct or batches:   # 开推先把机上清单置空（11-09 根修 Windows 装完游戏库空）：清单最后写，「清单读得出」= 这次推送完整。
        #   原先只靠下面把 .applied 清空表示「传到一半」，App 分不清「传完了只等解开」和「传断了」，Windows 起 App 解包没成时游戏库把这款整个藏掉
        e = work / "manifest_pending"; e.write_bytes(b"")
        try: dev.put(e, f"{rp(dev)}/manifest.json")
        finally: e.unlink(missing_ok=True)
    if direct:   # 先把「已应用」作废：镜像写到一半时机上不能判成就绪（App 看到长度不对会提示重新传）
        e = work / "not_applied"; e.write_bytes(b"")
        try: dev.put(e, f"{rp(dev)}/.applied")
        finally: e.unlink(missing_ok=True)
    def send_zst(w, sizes):
        """一份镜像改推压缩块：机上旧镜像截空 → 别的版本留下的块删掉 → 机上已有、长度对的块不重推（续传）→ 其余边压边推（压下一块和推这一块并行）"""
        nonlocal done
        size, n = int(w[1]), len(sizes)
        names = [zname(w[2], k) for k in range(n)]
        e = work / "img_empty"; e.write_bytes(b"")   # 旧镜像要被块整份重写，留着白占机上空间（直写时 put 一上来同样截断旧文件）
        try: dev.put(e, f"{rp(dev)}/{w[0]}")
        finally: e.unlink(missing_ok=True)
        seen = {m.group(0) for l in (dev.ls(inc(dev)) or []) for m in ZNAME.finditer(l)}
        rm = getattr(dev, "rm", None) or getattr(getattr(dev, "dp", None), "rm", None)
        for x in sorted(seen - set(names)):   # 别的版本的块：删掉；删不了（devicectl 没有删除）盖成空文件，App 解包时连同它删掉
            if not (rm and rm(f"{inc(dev)}/{x}")[0]):
                e.write_bytes(b"")
                try: dev.put(e, f"{inc(dev)}/{x}")
                finally: e.unlink(missing_ok=True)
        todo = []
        for k, x in enumerate(names):   # 续传：机上这块长度 = 这次量出的压缩字节才算传完（长度对、内容不同的块 App 按帧校验判坏、整份重推）
            got = dev.head(f"{inc(dev)}/{x}", 0) if x in seen and hasattr(dev, "head") else None
            if not (got and got[0] == sizes[k]):
                todo.append(k)
                if got and got[0] >= 0:   # 机上有这块、长度对不上：记一行（诊断里能数白重推了多少，#1942 线程数随内存变时会出现）
                    say(f"  续传 {w[0]}：第 {k + 1}/{n} 块机上 {got[0]} 字节，量数记的是 {sizes[k]} 字节，对不上，这块重推")
        if len(todo) < n:
            say(f"  续传 {w[0]}：机上已有 {n - len(todo)}/{n} 个压缩块，接着推剩下的")
            done += sum(min(ZCHUNK, size - k * ZCHUNK) for k in range(n) if k not in todo)
        comp = zstd_compressor()
        check = not getattr(dev, "dp", None) and hasattr(dev, "size")   # 只走 devicectl（Apple TV）：每块推完列一次长度核对；流式通道自己按字节写完才返回，不另核
        def make(k):
            p = work / f"zchunk_{k:04d}"
            with open(p, "wb") as o: comp(pack / w[0], k, o)
            return p
        from concurrent.futures import ThreadPoolExecutor
        try:
            with ThreadPoolExecutor(1) as ex:
                fut = ex.submit(make, todo[0]) if todo else None
                for i, k in enumerate(todo):
                    p = fut.result()
                    fut = ex.submit(make, todo[i + 1]) if i + 1 < len(todo) else None
                    raw = min(ZCHUNK, size - k * ZCHUNK)
                    for attempt in (1, 2, 3):   # 一块断了原地再推（同直写）；三次都不成才停，已推完的块下次续传
                        try:
                            transfer(dev.put_big, p, f"{inc(dev)}/{names[k]}", size=raw, scale=raw / max(p.stat().st_size, 1))
                            got = dev.head(f"{inc(dev)}/{names[k]}", 0) if check else None
                            if got is not None and got[0] != sizes[k]:   # devicectl 报成功、机上这块却缺 / 短（D-GZHR：断一次重推后核对报 1/4 块不完整）
                                raise Fail(f"推完核对：机上这块 {'没有' if got[0] < 0 else f'{got[0]} 字节'}，应为 {sizes[k]} 字节")
                            break
                        except Fail as e2:
                            if dev.nospc() or attempt == 3: raise
                            say(f"【黄】推 {w[0]} 第 {k + 1}/{n} 块没推成（{str(e2)[-200:]}），原地再试（第 {attempt + 1} 次）")
                    p.unlink(missing_ok=True); done += raw
        except OSError as e2:
            raise Fail(f"读 {w[0]} 压缩失败：{e2}") from None
        finally:
            for p in work.glob("zchunk_*"): p.unlink(missing_ok=True)
    for w in direct:
      if w[0] in zp:
        t1 = time.time()
        try:
            send_zst(w, zp[w[0]])
        except DirUnwritable:
            raise
        except Fail as e:
            if dev.nospc(): raise Fail(f"测试设备空间不足（推 {w[0]} 时 POSIX 28）")
            say(f"【红】传 {w[0]} 中断（{e}）；机上这份不完整，不会被当成可用，重新装机会接着已传的部分续传")
            raise Fail(f"资源传输中断（{push_reason(e) or str(e)[-160:]}）：{w[0]} 没传完，重新装到 iPhone 会接着已传的部分续传")
        nt += 1; ledger += f"{w[0]}\t{w[2]}\n"; led.write_text(ledger, encoding="utf-8"); dev.put(led, f"{rp(dev)}/.pushed")
        say(f"  压缩推 {w[0]}：{int(w[1]) / 1e9:.2f} GB（压缩后 {sum(zp[w[0]]) / 1e9:.2f} GB），{time.time() - t1:.0f} 秒，折合 {int(w[1]) / 1e6 / max(time.time() - t1, 1e-3):.1f} MB/s")
        step += 1
        if progress: progress(done, total, step, steps)
        continue
      for attempt in (1, 2):   # 直写失败先原地再试一次（线缆抖一下、隧道重连）：game.img 接着已传的续传，旧形态从头写；两次都不成才停
        t1 = time.time(); size, rel = int(w[1]), f"{rp(dev)}/{w[0]}"
        off = resume_offset(dev, pack / w[0], rel, size)
        try:
            if off:
                say(f"  续传 {w[0]}：机上已有同一份的前 {off / 1e9:.2f} GB，从这里接着写")
                done += off
                try:
                    transfer(dev.put_at, pack / w[0], rel, off, size=size - off)
                except Fail as e:
                    say(f"  续传 {w[0]} 没成（{str(e)[-160:]}），改为整份重写"); done -= off; off = 0
                    transfer(dev.put_big, pack / w[0], rel, size=size)
            else:
                transfer(dev.put_big, pack / w[0], rel, size=size)
            if w[0] == "game.img":   # 写完（整份或续传）都核机上总长，对不上不记账（App 也按头里整长判没传完）；续传读不到长度算没核过，整份写读不到不算错
                got = (dev.head(rel, 0) or (None,))[0]
                if (got != size) if off else (got is not None and got != size):
                    raise Fail(f"传完后机上 {w[0]} 长度 {got} 字节，应为 {size} 字节")
        except DirUnwritable as e:
            say(f"【红】传 {w[0]} 写不进：{e}")
            raise
        except Fail as e:
            if dev.nospc(): raise Fail(f"测试设备空间不足（推 {w[0]} 时 POSIX 28）")
            if attempt == 1:
                say(f"【黄】传 {w[0]} 中断（{str(e)[-200:]}），原地再试一次"); done -= off; continue
            why = push_reason(e)
            again = "接着已传的部分续传" if w[0] == "game.img" else "从头重传"
            say(f"【红】传 {w[0]} 中断（{e}）；机上这份不完整，不会被当成可用，重新装机会{again}")
            # 旧形态 romfs.img 没有 IMG 头，续传不了（resume_offset）；就地转 game.img 要原始 romfs（产物根），沿用的旧包一般已没有：说清新版能续传、怎么换。
            #   句式在启动器中英对照表（launchers/macos/adapter/.../Localizable.json）里有登记，改字要两边一起改
            reason = why or str(e)[-160:]
            if w[0] == "game.img":
                raise Fail(f"资源传输中断（{reason}）：{w[0]} 没传完，重新装到 iPhone 会接着已传的部分续传")
            raise Fail(f"资源传输中断（{reason}）：{w[0]} 没传完。这款是旧版本出的包，断了只能从头传。新版已支持单一文件包和断点续传：在启动器里重新出一次这款的包，以后断了能接着传。")
        break
      done += size - off; nt += 1; ledger += f"{w[0]}\t{w[2]}\n"; led.write_text(ledger, encoding="utf-8"); dev.put(led, f"{rp(dev)}/.pushed")
      say(f"  直写 {w[0]}：{int(w[1]) / 1e9:.2f} GB，{time.time() - t1:.0f} 秒，{int(w[1]) / 1e6 / max(time.time() - t1, 1e-3):.1f} MB/s")
      step += 1
      if progress: progress(done, total, step, steps)
    for k, b in enumerate(batches):
        lst = work / "push.lst"; lst.write_text("".join(w[0] + "\n" for w in b), encoding="utf-8")
        tn = f"{ts}_{k:03d}.tar"
        try:
            how = transfer(dev.put_tar, lst, pack, f"{inc(dev)}/{tn}", size=sum(int(w[1]) for w in b))
        except Fail:
            if dev.nospc():
                say(f"【红】推 {tn} 时测试设备空间满（POSIX 28）"); raise Fail(f"测试设备空间不足（推 {tn} 时 POSIX 28）")
            say(f"推 {tn} 失败"); raise   # 底层已是人话原因（不带内部文件名，#333）；文件名留在上面这行日志
        finally:
            lst.unlink(missing_ok=True)
        ns += how == "stream"; nd += how != "stream"
        sz = sum(int(w[1]) for w in b); done += sz; nt += len(b); nb += 1
        ledger += "".join(f"{w[0]}\t{w[2]}\n" for w in b); led.write_text(ledger, encoding="utf-8"); dev.put(led, f"{rp(dev)}/.pushed")
        if len(files) > 1:
            say(f"  批 {k + 1}/{len(files)}：{sz >> 20} MB，累计 {done / 1e9:.2f} GB，{done / 1e6 / max(time.time() - t0, 1e-3):.1f} MB/s")
        step += 1
        if progress: progress(done, total, step, steps)
    for f in ("index.tsv", "manifest.json"):
        try:
            dev.put(pack / f, f"{rp(dev)}/{f}")
        except Fail:
            if dev.nospc(): raise Fail(f"测试设备空间不足（推 {f} 时 POSIX 28）")
            say(f"推 {f} 失败"); raise
    say(f"资源包：推 {nt} 个文件 {done >> 20} MB，分 {nb} 批（流式 s2a-devpush {ns} 批 / devicectl {nd} 批；{dev.why}）+ 索引 + 清单（{time.time() - t0:.0f} 秒）")
    return nb


# 等机上解包的判据（11-07 根修）：不按假定写速算总上限，按解包日志的进度行看——包数还在涨就一直等，连续 STALL_S 秒没有新进展才判失败。
#   原先「上限 = 字节 ÷ 50 MB/秒、至少 5 分钟」：11-07 测试设备1 01007EF00011E000（17 包 48895 个文件 16483 MB）机上实测 463 秒解完（约 36 MB/秒），
#   330 秒上限先到，装机作业报「解包没在规定时间内完成」（rc=1）而 App 其实在正常解——玩家看到假失败。
#   STALL_S = 180 的依据：同一次实测 unpack.log 单包（≤1 GB）最慢 43.5 秒（第 14 包 7728 个文件 1023 MB），App 冷起到第一行进度 38 秒（含第 1 包 33.1 秒）；
#   取单包最慢的约 4 倍，比任何一包正常耗时都长，又不至于真卡住时让玩家干等十几分钟
STALL_S = 180
OPEN_BY_HAND = "在 iPhone 上打开 S2Arm，选这款游戏，保持在前台别锁屏，等它导入完就能玩"
LOCKED_WAIT = "iPhone 锁屏了：请解锁手机、保持亮屏，解锁后会自动接着解开"
LOCKED_WHY = "iPhone 一直锁着屏，电脑拉不起 App。解锁后"
CANT_OPEN_WHY = ("电脑拉不起手机上的 App，常见原因：iPhone 没开开发者模式（设置 → 隐私与安全性 → 开发者模式），"
                 "或免费 Apple ID 还没信任开发者（设置 → 通用 → VPN 与设备管理）。处理好后")
NO_DEVMODE_WHY = "电脑连不上手机的开发服务：请在 iPhone 上打开开发者模式（设置 → 隐私与安全性 → 开发者模式，要 iOS 17 以上）。处理好后"
LOCKED_SIGNS = ("XPC response missing CoreDevice.output", "locked", "Locked")   # 锁屏时拉 App 的报错（Windows s2a-devpush 实测原文；devicectl 带 locked 字样）
DEVMODE_SIGNS = ("CoreDeviceProxy",)   # s2a-devpush 建不了隧道（没开开发者模式 / iOS 17 以下）
# 手机拒绝打开 App（devicectl 原文「…invalid code signature, inadequate entitlements or its profile has not been explicitly trusted by the user」）：
#   一句里包了三种原因——还没信任开发者、签名 / 描述文件过期、换了团队。照着做的两步都写上；装机时读到的证书名（S2A_TRUST_NAME，install.py profile_trust_name）填进要点的那一项
UNTRUSTED_SIGNS = ("explicitly trusted", "invalid code signature")
def untrusted_why(name=None):
    name = os.environ.get("S2A_TRUST_NAME", "") if name is None else name
    item = f"点「{name}」" if name else "点你的开发者 App"
    return (f"iPhone 拒绝打开 S2Arm：第一次装或换了 Apple ID，要在 iPhone 上：设置 → 通用 → VPN 与设备管理 → {item} → 信任；"
            "如果签名已经过期，在启动器点「续签 S2Arm」。处理好后")
RELAUNCH_S = 10   # 拉不起时每隔几秒再拉一次（拉一次约 1 秒；等待总长仍按 STALL_S 无进展判）
PROGRESS = re.compile(r"进度：解开 \S+（(\d+)/(\d+)）")
FAILED = re.compile(r"解包失败：(.+)")   # App 解开失败时写进解包日志的那一行（runtime/src/pack_apply.c s2a_pack_apply 收尾）：原因原样带回


def send(dev, pack, work, say=print, progress=None, stage=None):
    """推（plan 之后）：要推的单文件镜像库里没有实体（romfs.files/ 库内形态，#329）先现拼，推完（含失败）就删，库里只留逐文件那一份。
    run 与 dev_run 的两步路（tools/push_pack.sh：plan → send）都经这里：原先只有 run 现拼，dev_run 推库里的包 FileNotFoundError: romfs.img"""
    made = materialize_images(pack, work, say, stage)
    try:
        dl = Path(work) / "direct_img.tsv"   # 压不压按款量（zstd_plan，同一份包只量一次）：在现拼之后，库内形态的镜像这时才有实体
        rows = [l.split("\t") for l in dl.read_text(encoding="utf-8").splitlines() if l] if dl.is_file() else []
        zp = {w[0]: z for w in rows for z in [zstd_plan(pack, w, say, stage)] if z}
        return _send(dev, pack, work, say, progress, zp)
    finally:
        for f in made: Path(f).unlink(missing_ok=True)


def apply(dev, pack, work, nb, say=print, sleep=time.sleep, now=time.time, stall=STALL_S):
    """推完起一次 App 只解包（S2A_PACK_APPLY_ONLY=1，runtime/src/pack_apply.c）并等它解完：机上 .applied = 本包 index_sha256，且解包日志
    本次新增完成行「解开 N 个包 … .applied = <本包>」（只看 .applied 不够：同包重推时推之前就对得上，11-06 对照实测）。
    等待按进度：本次新增的进度行「进度：解开 <包>（k/n）」k 涨了就重新计时、打一行「机上解包 k/n」（装机作业据此发进度事件，pack/jobs/install.py）；
    连续 stall 秒 k 没涨 → 抛 NeedsPlayer（传输成功、提醒玩家打开 App 接着装）。没推东西且 .applied 已是本包 → 不起 App。机上解开失败抛 Fail"""
    want = json.loads((Path(pack) / "manifest.json").read_text(encoding="utf-8")).get("index_sha256")
    def applied(): return (read(dev, f"{rp(dev)}/.applied", work, "r.applied") or "").strip()
    if not nb and applied() == want:
        return False
    u0 = len((read(dev, unpack_log(dev), work, "r.unpack0") or "").splitlines())
    done = re.compile(r"解开 \d+ 个包 .*\.applied = " + re.escape(want or "-"))
    partial = re.compile(r"解开 \d+ 个包 .*缺 (\d+) 个非必需文件.*部分可玩 = " + re.escape(want or "-"))   # 手机上只缺非核心文件（v0.3.1 缺件照玩，pack_apply.c）
    # 多款容器：正常拉起到游戏库，由库在后台解开传完的这款（图块显示「安装中」和进度，S2ArmShellHostModel）。只解包模式是整屏导入页、解完进程自己退出，
    #   玩家正开着库时 App 会突然消失；拉不起时玩家手动打开也走库这同一条路。单款包照旧只解包模式
    say("拉起 App 到游戏库，由它解开这款" if dev.ctx else "起一次 App 解开推送包（S2A_PACK_APPLY_ONLY=1）")
    env = {} if dev.ctx else {"S2A_PACK_APPLY_ONLY": "1"}
    def try_launch():   # → None 起成了；否则失败原文（拉起靠电脑侧的设备服务，玩家机上常常没有，#192）
        try: dev.launch(env); return None
        except Fail as e: return str(e)
    def locked(err):   # 拉不起时按报错原文判是不是锁屏（11-09 测试设备实测：锁屏时写文件照样成功、只有拉 App 失败，Windows 回「XPC response missing CoreDevice.output」，解锁后同一调用成功）
        return any(k in err for k in LOCKED_SIGNS)
    cant_open = try_launch(); was_locked = False; tried = now()
    if cant_open:
        was_locked = locked(cant_open)
        say(f"资源包已传完。{LOCKED_WAIT if was_locked else OPEN_BY_HAND}")
    seen, last = 0, now()
    while True:
        if cant_open and now() - tried >= RELAUNCH_S:   # 推几分钟间手机自动锁屏是常态：每隔一会儿再拉一次，玩家一解锁就接着自动解开，不用自己去点
            tried = now(); cant_open = try_launch()
            if cant_open: was_locked = locked(cant_open)
            else: say("已拉起 App，接着解开推送包"); last = now()
        new = (read(dev, unpack_log(dev), work, "r.unpack1") or "").splitlines()[u0:]
        if applied() == want and any(done.search(l) for l in new):
            say("资源包：机上已解开（.applied = 本包）"); return True
        part = [m for m in map(partial.search, new) if m]
        if part:   # 解开了，但手机上缺几个非核心文件：能玩，提醒补（同「还差最后一步」口径，装机按传输成功 + 提醒收尾）
            raise NeedsPlayer(f"资源包已传完，还差最后一步：手机上还缺 {part[-1].group(1)} 个文件，可以先玩，用到它们时可能会停；再点一次「装到 iPhone」会把缺的补上")
        failed = [m.group(1).strip() for m in map(FAILED.search, new) if m]
        if failed:   # App 已经起来解过、失败了：按它写的原因当场报（原先只看进度行，第一包前就失败时干等 stall 秒、再报「解包没有开始」，原因丢了）
            raise Fail(f"资源包推进去了，但手机上解开失败：{failed[-1][:600]}")
        got = [(int(m.group(1)), int(m.group(2))) for m in map(PROGRESS.search, new) if m]
        if got and got[-1][0] > seen:
            seen, last = got[-1][0], now(); say(f"机上解包 {seen}/{got[-1][1]}")
        elif now() - last > stall:
            break
        sleep(3)
    if cant_open:
        why = (LOCKED_WHY if was_locked else NO_DEVMODE_WHY if any(k in cant_open for k in DEVMODE_SIGNS)
               else untrusted_why() if any(k in cant_open for k in UNTRUSTED_SIGNS) else CANT_OPEN_WHY)
        raise NeedsPlayer(f"资源包已传完，还差最后一步：{why}{OPEN_BY_HAND}（电脑这边打不开手机上的 App：{cant_open}）")
    # 解包停住（装机原则 11-11）：资源已经全在手机上，玩家打开 App 会接着装——和拉不起 App 同一口径（传输成功 + 提醒），不报装机失败
    raise NeedsPlayer(f"资源包已传完，还差最后一步：机上解包 {stall} 秒没有新进展（停在第 {seen} 包）。{OPEN_BY_HAND}" if seen else
                      f"资源包已传完，还差最后一步：起 App 后 {stall} 秒机上解包没有开始。{OPEN_BY_HAND}")


PROBE = ".s2a-write-probe"   # 推之前写的几字节探针（资源包目录下，下次覆盖）


LOCKED_MSG = "iPhone 写不进资源包：手机锁屏了。解锁手机、保持亮屏，再点一次。"


FIXED_DIRS = {"", "Documents", "Library", "Library/Application Support", "Library/Application Support/S2Arm"}   # 容器骨架：写不进也不挪


def app_dirs(dev, rel, say=print):
    """推之前以 App 身份（s2a-devpush / AFC）建好 rel（资源包 / .incoming），不让 devicectl 去代建（a48655：iOS 26.2 上 devicectl 代建的目录归 root、
    权限 755，AFC 和 App 都写不进，原先只能删除重装）。已经归了别的用户的那一级：在原父目录里改名挪开（.s2a-foreign-<名>-<时刻>，同目录改名只要父目录可写），
    再以 App 身份重建；挪开的那份 App 删不掉，删除重装时随容器一起清。没有 s2a-devpush（只有无线隧道）/ 工具太旧：不做，照旧推"""
    dp = dev if isinstance(dev, Devpush) else getattr(dev, "dp", None)
    if not hasattr(dp, "mkdir"): return   # 别的设备对象（假设备、没有 AFC 通道的）：不做
    for _ in range(4):
        bad = dp.mkdir(rel)
        if bad is None or bad is False: return
        d = bad
        while True:
            if d in FIXED_DIRS or "/" not in d:
                raise DirUnwritable(f"iPhone 上的 S2Arm 写不进 {bad}，也没法把它挪开。" + REINSTALL_STEP)
            parent, name = d.rsplit("/", 1)
            aside = f"{parent}/.s2a-foreign-{name}-{int(time.time())}"
            ok, out = dp.mv(d, aside)
            if ok:
                say(f"🟡 手机上 {d} 归别的用户、S2Arm 写不进（多半是旧版推送时 devicectl 代建的）：已挪到 {aside}，以 S2Arm 自己的身份重建后接着装，这款要整份重推")
                break
            d = parent
    raise DirUnwritable(f"iPhone 上的 S2Arm 写不进 {rel}：挪开写不进的目录后还是建不出来。" + REINSTALL_STEP)


def probe(dev, work, sleep=None):
    """推之前先往机上写一个几字节的文件：断开时这里就写不进，立刻停（原先要等核账、推到几分钟后才失败，前面推的全作废，#333）。
    写不进不等于锁屏（11-13 测试设备实测：手机亮着、没锁，devicectl 写这几字节连挂两次超时，被报成锁屏；随后同一路径 1 秒写成；
    11-09 实测锁屏时写文件照样成功）。所以：先问手机是不是真锁着（Mac devicectl lockState）→ 锁着才报锁屏 device_locked；
    没锁 / 判不了 → 隔 2 秒再写一次（优先走推包本身用的 s2a-devpush 通道）→ 还不行按原文报连接问题，不冒充锁屏"""
    src = Path(work) / PROBE
    src.write_bytes(b"ok\n")
    rel = f"{rp(dev)}/{PROBE}"
    try:
        return dev.put(str(src), rel)
    except Fail as e:
        first = e
    if tv():   # Apple TV（D-NCDE）：没有锁屏、没有数据线，不问锁屏状态、不说手机；隔 2 秒再写一次，还不行按原文报网络连接
        (sleep or time.sleep)(2)
        try:
            return dev.put(str(src), rel)
        except Fail as e:
            why = str(e).splitlines()[-1][:200] if str(e) else type(e).__name__
            raise Fail(f"Apple TV 写不进资源包（{why}）。让 Apple TV 开着、别让它休眠，和这台电脑连同一个网络，再点一次。") from e
    locked = getattr(dev, "lock_state", lambda: None)()   # True 锁着 / False 没锁 / None 判不了（Windows、假设备）
    # 锁着也照样再写一次（预检清单 #72：锁屏时写文件照样成功，见上）；两次都写不进才报
    (sleep or time.sleep)(2)
    try:
        return (getattr(dev, "dp", None) or dev).put(str(src), rel)
    except Fail as e:
        second = e
    why = str(second).splitlines()[-1][:200] if str(second) else type(second).__name__
    if locked or (locked is None and any(k in str(first) + str(second) for k in LOCKED_SIGNS)):   # 锁着、或判不了时按报错原文认
        raise DeviceLocked(LOCKED_MSG) from second
    raise Fail(f"iPhone 写不进资源包（{why}）。" + ("手机没有锁屏，是电脑和手机的连接出了问题：" if locked is False else "")
               + "拔下数据线重新插好（无线连接的换成数据线），保持手机亮屏，再点一次。") from second


def run(dev, pack, work, full=False, say=print, env=None, on_plan=None, progress=None, stage=None, accept_low_space=False):
    """一条龙：探针（写不进就停）→ 算 → 核空间（估算不够：没确认过就停下问玩家 LowSpace，确认过 accept_low_space / S2A_PUSH_ACCEPT_LOW_SPACE=1 打黄照推；
    读不到提示后照推：探针已证明手机可写）→ 推（真写满了停下 SpaceFull，说清缺多少）→ (推了几批, 文件数, 字节数)。
    stage(名字, 已做, 总量)：推之前的准备步骤报给装机页（核对手机上的文件 / 检查手机空间 / 准备资源（拼镜像）按字节），每一刻都看得到在做什么"""
    stage = stage or (lambda name, done=None, total=None: None)
    stage(STAGE_CHECK)
    app_dirs(dev, inc(dev), say)
    probe(dev, work)
    nb, nt, nbytes = plan(dev, pack, work, full, say)
    if on_plan: on_plan(nb, nt, nbytes)
    if nbytes:
        stage(STAGE_SPACE)
        fb, need, imp = space(dev, nbytes, work, env, say)
        accept = accept_low_space or (env or os.environ).get("S2A_PUSH_ACCEPT_LOW_SPACE") == "1"
        if fb is None: say(f"🟡 读不到 Apple TV 剩余空间，这次要传 {max(1, nbytes >> 30)} GB：空间不够时会中途停下，请确认 Apple TV 上留有足够空间" if tv() else
                           f"🟡 读不到 iPhone 剩余空间，这次要传 {max(1, nbytes >> 30)} GB：空间不够时会中途停下，请确认手机上留有足够空间")
        elif fb < need:
            # 手机报的空闲（AFC）不含系统随时能清掉的空间，比「设置 → 储存空间」里的「可用」小，常差好几 GB：估算不够不等于放不下
            # 「估算」写进句子（11-17 维护者）：手机报的空闲偏小，能读到空间以后提示变多，玩家要知道这是估的、可以照传
            head = (f"按估算，iPhone 剩余空间可能不够：这次要传约 {gb1(nbytes)} GB，连同解开时临时多占的约要 {gb1(need)} GB，"
                    + (f"手机可用约 {gb1(fb)} GB（含系统随时能清掉的空间）" if imp else f"手机报的空闲约 {gb1(fb)} GB")
                    + f"，还差约 {gb1(need - fb)} GB。")
            if not accept:
                raise LowSpace(head + ("可以先在「设置 → 通用 → iPhone 储存空间」腾出一些，或者" if imp else
                                       "「设置 → 通用 → iPhone 储存空间」里的「可用」还包括系统随时能清掉的空间，实际多半放得下；")
                               + "点「仍然继续」照传，真写满了会停下并说明还差多少。")
            say("🟡 " + head + "已按你的选择仍然继续；真写满了会停下并说明还差多少")
    try:
        send(dev, pack, work, say, progress, stage)
    except (DeviceLocked, NeedsPlayer):
        raise
    except Fail as e:
        left = dev.free() if nbytes else None
        # 报错原文就说写满了（AFC「No space left」等，见 NOSPACE），或推完再量空闲已近 0：都按写满报，不报成连接中断
        if nospace(str(e)) or (left is not None and left < FULL_LEFT):
            raise SpaceFull((f"iPhone 空间写满了，传到一半停下：这次要传约 {gb1(nbytes)} GB，手机现在只剩 {left >> 20} MB。" if left is not None else
                             f"iPhone 空间写满了，传到一半停下：这次要传约 {gb1(nbytes)} GB。")   # 两句整句进对照表（BackendText 按整句 / 句式翻）
                            + "在「设置 → 通用 → iPhone 储存空间」腾出空间后再装一次（已传完的文件不用重传）。") from e
        raise
    return nb, nt, nbytes


CODE_PREFIX = "S2A_PUSH_CODE"   # 失败的机器读行：S2A_PUSH_CODE <码>\t<给玩家的话>（device_space_low / ios_install_no_space / device_locked / ios_install_dir_unwritable）
STAGE_CHECK, STAGE_SPACE, STAGE_IMAGE, STAGE_ZSTD = "核对手机上的文件", "检查手机空间", "准备资源（拼镜像）", "准备资源（测压缩率）"   # 装机页的准备子阶段（作业层原样上屏）
STAGE_PREFIX = "S2A_PUSH_STAGE"   # --progress-lines 打的机器读行：S2A_PUSH_STAGE <名字>\t<已做字节>\t<总字节>（没有字节的两栏为空）


def materialize_images(pack, work, say=print, stage=None):
    """这次要推的单文件镜像（plan 写的 direct_img.tsv）包里没有、但有 romfs.files/（pack/romfs_image.py v2 的库内形态）→ 现拼到包里，
    返回现拼的路径（run 推完删）。本机盘放不下就报红不推：拼一份要镜像那么大的空间，推完即还"""
    dl = Path(work) / "direct_img.tsv"
    rows = [l.split("\t") for l in dl.read_text(encoding="utf-8").splitlines() if l] if dl.is_file() else []
    todo = [w for w in rows if not (Path(pack) / w[0]).is_file()]
    if not todo: return []
    sys.path.insert(0, str(R / "pack"))
    import romfs_image
    need = sum(int(w[1]) for w in todo)   # 只按要写的确切字节判（不加估的余量）：确实放不下才停，免得写到一半（不许堵门：估出来的余量不当门）
    free = shutil.disk_usage(pack).free
    if free < need:
        say(f"【红】本机盘放不下现拼的镜像：要 {need / 1e9:.1f} GB，可用 {free / 1e9:.1f} GB（{pack}）；腾出空间再装")
        raise Fail("本机盘空间不足，拼不出要推的镜像")
    made, base, whole = [], 0, sum(int(w[1]) for w in todo)
    try:
        for w in todo:
            t0 = time.time()
            if stage: stage(STAGE_IMAGE, base, whole)
            made.append(romfs_image.materialize(pack, w[0], int(w[1]), w[2], say=say,
                                                progress=(lambda n, b=base: stage(STAGE_IMAGE, b + n, whole)) if stage else None))
            base += int(w[1])
            say(f"  现拼 {w[0]} 用时 {time.time() - t0:.0f} 秒")
    except (romfs_image.Red, OSError) as e:
        for f in made: Path(f).unlink(missing_ok=True)
        say(f"【红】{e}"); raise Fail(f"拼不出要推的镜像：{e}")
    return made


def main(argv=None):
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, "reconfigure"): s.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description="推资源包（Mac / Windows 共用）")
    ap.add_argument("op", choices=("run", "plan", "free", "send", "need")); ap.add_argument("device"); ap.add_argument("bundle_id"); ap.add_argument("pack")
    ap.add_argument("--work"); ap.add_argument("--full", action="store_true"); ap.add_argument("--udid"); ap.add_argument("--devpush")
    ap.add_argument("--via", choices=("devpush", "devicectl")); ap.add_argument("--apply", action="store_true")
    ap.add_argument("--progress-lines", action="store_true")   # 推送进度另打机器读的行「PROGRESS_PREFIX 已传 总量 批 总批」（Mac 装机作业 pack/jobs/install.py 解析成作业计数）
    ap.add_argument("--docs", default=DOCS); ap.add_argument("--pack-root"); ap.add_argument("--launch-ctx")   # 多款容器：本款设备侧文档根 Documents/games/<游戏号>、推完正常拉起 App 到游戏库由它解开   # run 推完等机上解开（s2armctl push-pack / Windows 装机）
    a = ap.parse_args(argv)
    pack = Path(a.pack)
    if not (pack / "index.tsv").is_file():
        print(f"【红】资源包目录不在或不是 v2（缺 index.tsv）：{pack}"); return 1
    stamp = a.op == "run"
    def say(m): print((f"[{time.strftime('%H:%M:%S')}] " if stamp else "") + m, flush=True)
    tmp = None
    if a.work: work = Path(a.work); work.mkdir(parents=True, exist_ok=True)
    elif stamp: tmp = tempfile.TemporaryDirectory(dir=os.environ.get("S2A_PUSH_TMP")); work = Path(tmp.name)
    else: ap.error(f"{a.op} 要 --work")
    state = work / "push.state"
    try:
        dev = device(a.device, a.bundle_id, work, a.udid, a.devpush, a.via, say=say, docs=a.docs.rstrip("/"), ctx=a.launch_ctx, pack=a.pack_root.rstrip("/") if a.pack_root else None)
        if a.op == "free":
            fb = dev.free(); print(fb if fb is not None else "", flush=True); return 0
        if a.op == "need":   # plan 之后：机上要的可写字节（tools/push_pack.sh）
            print(need_bytes(work, int((state.read_text().split() + ["0"] * 4)[3])), flush=True); return 0
        if a.op == "plan":
            nb, nt, nbytes = plan(dev, pack, work, a.full, say); state.write_text(f"PLAN {nb} {nt} {nbytes}\n"); return 0
        if a.op == "send":
            state.write_text(f"SENT {send(dev, pack, work, say)}\n"); return 0
        say(f"推资源包：设备 {a.device} 包 {a.bundle_id} 目录 {pack}" + ("（--full 整包重推）" if a.full else ""))
        lines = (lambda d, t, k, n: print(f"{PROGRESS_PREFIX} {d} {t} {k} {n}", flush=True)) if a.progress_lines else None
        stages = (lambda name, d=None, t=None: print(f"{STAGE_PREFIX} {name}\t{'' if d is None else d}\t{'' if t is None else t}", flush=True)) if a.progress_lines else None
        nb, _, _ = run(dev, pack, work, a.full, say, progress=lines, stage=stages)
        if a.apply: apply(dev, pack, work, nb, say)
        else: say(f"资源包：推完 {nb} 批；起一次 App 让它解开 .incoming（S2A_PACK_APPLY_ONLY=1 只解包不跑游戏）" if nb else "资源包：机上已是最新，不用推")
        return 0
    except Fail as e:
        state.write_text(f"FAIL {e}\n")
        if stamp:
            print(f"【红】{e}"); print(f"RESULT=push out= reason={e}")
            if getattr(e, "code", None): print(f"{CODE_PREFIX} {e.code}\t{e}", flush=True)   # 装机作业按码收尾（pack/jobs/install.py push_pack）
        return 1
    finally:
        if tmp: tmp.cleanup()


if __name__ == "__main__":
    sys.exit(main())
