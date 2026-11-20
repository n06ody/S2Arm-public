"""Mac 公共宿主与只换宿主（11-09 维护者定「升级后只换宿主」；游戏库结构双端统一后宿主多款共用，见 layout.py）。

宿主放 <库>/hosts/mac/<可执行文件摘要前 12 位>/S2ArmGame.app，每款镜像 games/<英文名>/image/mac 不含宿主；game.json 的 hostMac 记这款在用的宿主。
升级 S2Arm 后，新宿主认得旧镜像就直接换用，不用重新准备。判据两项，都对上才换：
  hostAbi    —— 宿主读每款产物的约定号，就是下面的 HOST_ABI（从 runtime/include/s2a_host_abi.h 读，宿主编的是同一个数）。改了宿主读产物的方式（改写器与运行时的约定：跳板 / 虚拟寄存器槽 / 门控表，
                *.fixups / *.sites / *.gates / addr_need.tsv 等表的格式，镜像目录布局）就把它加 1；只修宿主自身的 bug 不动它。
  dmshChain  —— 直连着色器库版本 V（graphics/nvn/gfx/nvn/dmsh_chain.h 的 DMSH_CHAIN，宿主只读 shaders/direct/b-<V>/），从引擎源码机械读出。
镜像的 manifest.json 记准备时的这两项；本字段之前（v0.1.0 起的公开版）出的镜像 hostAbi 按 1，dmshChain 按目录里实有的 b-<V> 认。
试玩：换上新宿主后第一局 TRIAL_OK_SECONDS 秒内异常退出 = 新宿主不认这款，记进 game.json hostFailed、退回原来的宿主（不动任何文件）；
正常退出或玩满这么久 = 通过，hostMac 改成新宿主。60 秒同直连安全网「首帧后前台满 60 秒算成功玩过」（runtime/include/s2a_dguard.h）。
不兼容时照用原来的宿主，catalog 报 hostUpdate = "incompatible"，游戏库提示重新准备。
"""
from __future__ import annotations
import json
import os
import re
import shutil
import stat
import subprocess
import time
import math
import sys
from pathlib import Path

import layout
from core import Failure, contained, digest, read

def _host_abi():
    """宿主读产物的约定号：从 runtime/include/s2a_host_abi.h 机械读（与宿主编进去的是同一个数，不两处各写一份）"""
    here = Path(__file__).resolve()
    # Mac 应用里作业层在 Contents/Resources/Jobs，头文件随包放同目录（launchers/macos/jobs_bundle.py EXTRA）；源码树 / Windows 引擎快照里在 runtime/include
    head = here.parent / "s2a_host_abi.h"
    if not head.is_file():
        head = here.parents[2] / "runtime/include/s2a_host_abi.h"
    text = head.read_text(encoding="utf-8")
    return int(re.search(r"^#define S2A_HOST_ABI (\d+)$", text, re.M).group(1))


HOST_ABI = _host_abi()
TRIAL_OK_SECONDS = 60
APP = "S2ArmGame.app"
EXE = APP + "/Contents/MacOS/s2arm-host"
# iPhone 容器（pack/ship_ext.py container）：s2a_catalogue.json 每款记 host_abi / chain，装机时壳变了但兼容的款原样带进新壳（ipa.assemble）。
#   本字段之前的容器条目按 1 / LEGACY_CHAIN：v0.1.0 起 graphics/nvn/gfx/nvn/dmsh_chain.h 的 DMSH_CHAIN 一直是它（git log v0.1.0.. 该文件无改动）
LEGACY_CHAIN = "0be3f28548fd"


def engine_compat(c):
    """本版引擎（新宿主）的两项判据；读不到直连库版本 → None（不换）"""
    try:
        text = (c.engine / "graphics/nvn/gfx/nvn/dmsh_chain.h").read_text()
    except OSError:
        return None
    m = re.search(r'^#define DMSH_CHAIN "([0-9a-f]+)"', text, re.M)
    return dict(hostAbi=HOST_ABI, dmshChain=m.group(1)) if m else None


HINT_REASONS = ("rewriter", "hostAbi", "shader")   # 改写器更新 / 宿主约定号变了 / 着色器链（直连库）更新


SHADER_FALLBACK = "shader_fallback.json"   # 宿主写的「包里没有、现场首次翻译」的着色器支数（{count, at, app}，只加不减；iOS 在 Documents/games/<号>/，Mac 在游戏库 temp/<号>/）


def built_by(c):
    """出包时记下是哪一版启动器打的（game.json ios / Mac 运行版本清单的 builtBy）：着色器覆盖度判「这包比当前 App 旧」用"""
    from core import release_version, display_version
    return dict(version=release_version(c.engine), display=display_version(c.engine))


def _vtuple(v):
    try:
        return tuple(int(x) for x in str(v).split("."))
    except ValueError:
        return None


def built_older(c, built):
    """这份包是不是比当前启动器早的版本打的：没记 builtBy（这条判据之前出的包）→ 是；当前版本号读不出 → 判不了，按否（不因此标角标）"""
    from core import release_version
    cur = _vtuple(release_version(c.engine))
    if not cur:
        return False
    got = _vtuple((built or {}).get("version")) if built else None
    return got is None or got < cur


def shader_misses(c, gid):
    """这款在各处现场翻译过多少支包里没有的着色器 → {ios: 各台手机里最大的, mac: Mac 局的}（读设备记录与游戏库 temp/<号>/，不读手机）"""
    gid = gid.upper()
    ios = 0
    for _, rec in layout.devices(c):
        if isinstance(rec, dict):
            ios = max(ios, int(((rec.get("shaderFallback") or {}).get(gid) or {}).get("count") or 0))
    try:
        mac = int((json.loads((Path(c.library) / "temp" / gid / SHADER_FALLBACK).read_text(encoding="utf-8")) or {}).get("count") or 0)
    except (OSError, ValueError, AttributeError, TypeError):
        mac = 0
    return dict(ios=ios, mac=mac)


def _mac_built(c, gid):
    try:
        return (json.loads((layout.image_mac(c, gid, create=False) / "manifest.json").read_text(encoding="utf-8")) or {}).get("builtBy")
    except (OSError, ValueError, AttributeError):
        return None


def shader_hint(c, gid, ios_count=None):
    """着色器覆盖度（11-17 维护者定的判据，设备实测不按版本猜）：现场翻译过包里没有的着色器（count > 0）且这份包比当前启动器旧 → 重新出包能补上，
    给 shaderMiss；count > 0 但包已是当前版本打的 → 重出也没用（是出包流程漏了，另由出包流程补），不标。ios_count 给了就只看这一台"""
    got = shader_misses(c, gid)
    n_ios = got["ios"] if ios_count is None else ios_count
    ios = layout.program_info(c, gid) or {}
    if n_ios > 0 and ios and built_older(c, ios.get("builtBy")):
        return True
    return ios_count is None and got["mac"] > 0 and built_older(c, _mac_built(c, gid))


# 直连库内容代数（同 sgrade_bf.BF_GEN 的做法）：库里多出旧包没有、设备又要用的内容时 +1；出包时记进 game.json ios.libGen / Mac 运行版本 manifest.json libGen。
#   1 = 库带客体输入（库容器种类 56）：设备上现做补齐 / 采样模拟 / 任务阶段变体从包里取（不再拷源快照）；旧包没有，游戏回收控制段后这些变体做不了（那一对绘制跳过、不崩），
#   重出这一款就补齐，所以旧代数的款在「建议更新」里列出（原因 libGen，前端用着色器那条文案）
#   2 = 着色器键第 2 代（graphics/nvn/gfx/nvn/nvn_key.h）+ 包内键别名表 key_alias.tsv：旧包靠宿主先新后旧兜底照常能用，重出后库按新键去重、首用现编与读档换键都对上新键
LIB_GEN = 2


def lib_stale(gen):
    return int(gen or 0) < LIB_GEN


def update_hints(c):
    """0.6.0 通用壳（开关后面）：启动器新版本打开时的快速比对（《文件与程序管理体系》⑦ ③，11-17 维护者定）。每款只读程序兼容记录
    （game.json 的 ios，没有读专用文件夹那份，layout.program_info，几 KB），比对改写器指纹 / 宿主约定号 / 着色器链与本版启动器的值；
    对不上的款给出「建议更新」与原因，玩家点了才做。不读游戏大文件（game.img、资源包、程序件）、不排后台重活、不改任何东西。
    没打过 iPhone 包的款没有记录，不标。开关关着 → {enabled: False, games: []}"""
    want = engine_compat(c) or {}
    import rewrite_refresh
    try:
        rid = rewrite_refresh.rewriter_id(c)
    except (Failure, OSError, subprocess.SubprocessError):
        rid = None   # 取不到改写器指纹：这一项不判（不因此给所有款标「建议更新」）
    games = []
    for g in c.catalog()["games"]:
        if not layout.program_info(c, g["id"]):
            continue   # #1712：没打过 iPhone 包（只在 Mac 出包 / 玩）的款不标——「更新这款」就是推到 iPhone，Mac 那份旧不该弹手机提示
        why = _game_hint(c, g["id"], rid, want)
        if shader_hint(c, g["id"]):
            why.append("shaderMiss")
        mac = layout.image_mac(c, g["id"], create=False) / "manifest.json"
        if "libGen" not in why and mac.is_file() and lib_stale(read(mac, {}).get("libGen")):
            why.append("libGen")   # Mac 运行版本的库是旧代数（iPhone 那份在 _game_hint 判）
        if why:
            games.append(dict(gameId=g["id"].upper(), reasons=why))
    return dict(games=games)


SIGN_LIFETIME = dict(free=7 * 86400, paid=365 * 86400)   # 读不到真实到期时按苹果规则估：免费账号描述文件 7 天、付费 / 证书一年（手机上 App 自己读描述文件给准数）


def _game_hint(c, gid, rid, want):
    """一款的程序兼容记录对本版启动器的原因（同 update_hints）"""
    ios = layout.program_info(c, gid)
    if not ios:
        return []
    why = []
    if rid and ios.get("rewriterId") != rid:
        why.append("rewriter")
    if want and ios.get("hostAbi", 1) != want["hostAbi"]:
        why.append("hostAbi")
    if want and ios.get("dmshChain", LEGACY_CHAIN) != want["dmshChain"]:
        why.append("shader")
    if lib_stale(ios.get("libGen")):
        why.append("libGen")
    return why


def _online(c, names=None):
    """现在连着的手机 → {UDID: 'usb' | 'network'}；读不到 → {}（probe 时才调，可能要几秒）。names（dict）顺带填 {UDID: 设备名}（Mac 读得到；Windows 给不出）"""
    out = {}
    try:
        if os.name == "nt":
            import install
            for u, kind, _ in install.win_install_mod(c).mux_devices():
                if u:
                    out[u] = "network" if str(kind).lower().startswith("net") else "usb"
        else:
            from diag import devicectl
            rc, data, _ = devicectl(c, "list", "devices", timeout=30)
            for d in data.get("result", {}).get("devices", []) if rc == 0 else []:
                u = (d.get("hardwareProperties") or {}).get("udid")
                cp = d.get("connectionProperties") or {}
                if u and cp.get("tunnelState") != "unavailable":
                    out[u] = "network" if cp.get("transportType") == "localNetwork" else "usb"
                    if names is not None:
                        names[u] = (d.get("deviceProperties") or {}).get("name") or ""
    except (Failure, OSError, ValueError, subprocess.SubprocessError):
        return {}
    return out


def _has_game_img(c, gid):
    try:
        return (Path(layout.ios_pack(c, gid)) / "game.img").is_file()
    except (Failure, OSError):
        return False


def _sign_mode(c, rec, signed):
    """这台的 S2Arm 是怎么签的：appleId | devCert（我们签的，记在 signed.mode）| external（推过游戏、却没有我们的签名记录 = 玩家用别的工具装的）| None（判不出）。
    signed.mode 之前的旧记录（只有 free）：按这台电脑现在的签名方式算（证书模式是玩家在设置里选的，平时不换）"""
    if signed:
        mode = signed.get("mode")
        if mode in ("appleId", "devCert"):
            return mode
        import devcert
        return "devCert" if devcert.active(c) else "appleId"
    return "external" if rec.get("games") or rec.get("shellId") else None


def _can_sign(rec):
    """这台手机上的 S2Arm 有没有能自己签程序的身份（完成页「未签名导出」要不要补「导入后要回电脑更新」）：
    设备记录里有交过的身份（install.deliver_identity 写的 identity：Windows Apple ID 的私钥或证书用户的证书）→ True；
    我们装过 App（有壳号）却没交过 → False（换了团队 = 换包名，install.forget_other_app 会把旧身份一并清掉，同样落到 False）；没有这台的记录 → None"""
    if (rec.get("identity") or {}).get("cert_sha1"):
        return True
    return False if rec.get("shellId") else None


# 别的工具（AltStore / SideStore …）装的 S2Arm 那台手机在总控台的按钮（11-19 维护者定的动作名）：
#   exportShellIpa = 导出通用 S2Arm.ipa（玩家拿去自己签、覆盖装新版）；pushS2arm = 把游戏推进它的「放这里导入」（推到 iPhone，未签名、手机签）；
#   exportS2arm = 导出 .s2arm；handOverIdentity = 把签名身份交给手机（作业 phoneIdentity，按团队对得上的那个包名）。没有「安装 S2Arm」
EXTERNAL_PHONE_ACTIONS = ("exportShellIpa", "pushS2arm", "exportS2arm", "handOverIdentity")


def phone_overview(c):
    """0.6.0 通用壳（开关后面）：「iPhone 总控台」的只读总览。每台见过的手机（设备记录）：名字、App 装没装、壳是不是本版、签名时刻 /
    免费账号与否 / 按苹果规则估的到期时刻 / 上次连上（lastSeen = 设备记录最后一次写入：装 App、推游戏时连着才会写）；每款状态 latest（最新）|
    suggested（建议更新，附 reasons，同 update_hints）| notPushed（游戏库里有、这台没推过）| needsData（程序推过、资源没推）；notPushed / needsData 带 dataOnComputer =
    游戏库里还有没有 game.img；signMode 见 _sign_mode。缺省只读记录（几 KB，秒回）；请求带 probe: true 才查现在连着的手机（可能要几秒），
    结果里 online = 'usb' | 'network' | None（没查时没有这个键）。不改任何东西、不排后台活。开关关着 → {enabled: False, phones: []}"""
    import ipa as packer
    import rewrite_refresh
    want = engine_compat(c) or {}
    try:
        rid = rewrite_refresh.rewriter_id(c)
    except (Failure, OSError, subprocess.SubprocessError):
        rid = None
    try:
        sid = packer.current_shell_id(c)
    except (Failure, OSError, subprocess.SubprocessError):
        sid = None
    library = [g["id"].upper() for g in c.catalog()["games"]]
    hints = {g: _game_hint(c, g, rid, want) for g in library}
    names = {}
    online = _online(c, names) if c.r.get("probe") is True else None
    rows = [(k, r) for k, r in layout.devices(c) if isinstance(r, dict)]
    if online:   # 结构 5：从「连着的手机上实际装了什么」出发——连着、却没有设备记录的手机（玩家用 AltStore / SideStore 装的 S2Arm）也列出来
        known = {k for k, _ in rows}
        rows += [(u, {}) for u in online if u not in known]
    phones = []
    for key, rec in rows:
        signed = rec.get("signed") or {}
        at = signed.get("at") if isinstance(signed.get("at"), (int, float)) else None
        free = bool(signed.get("free", True))
        # 到期：装机时从签好的包里读到的真实值（signed.expires，#1772）；没有（旧记录、Xcode 钥匙串签的）才按苹果规则估
        known = isinstance(signed.get("expires"), (int, float))
        until = signed["expires"] if known else (at + SIGN_LIFETIME["free" if free else "paid"]) if at else None
        recs = {str(g).upper(): v for g, v in (rec.get("games") or {}).items() if isinstance(v, dict)}
        games = []
        for g in library:
            r = recs.get(g)
            if r is None or not (r.get("program") or r.get("pack")):
                games.append(dict(gameId=g, state="notPushed", dataOnComputer=_has_game_img(c, g)))
            elif not r.get("pack"):   # 电脑上还有没有资源本体（只看文件在不在，不打开）：界面分「点推到 iPhone 就行」和「要原件」
                games.append(dict(gameId=g, state="needsData", dataOnComputer=_has_game_img(c, g)))
            elif not r.get("program"):   # 资源推过、手机上没有这款的程序包（0.5.x 整包 App 装的款：装上通用 App 后程序不在了）→ 点「推到 iPhone」只补程序
                games.append(dict(gameId=g, state="suggested", reasons=["missing"]))
            else:
                why = list(hints[g])
                prog = r.get("program") or {}
                if want and prog and prog.get("host_abi", want["hostAbi"]) != want["hostAbi"] and "hostAbi" not in why:
                    why.append("hostAbi")
                if rid and prog and prog.get("rewriter_id") not in (None, rid) and "rewriter" not in why:
                    why.append("rewriter")
                if shader_hint(c, g, ios_count=int(((rec.get("shaderFallback") or {}).get(g) or {}).get("count") or 0)) and "shaderMiss" not in why:
                    why.append("shaderMiss")   # 这台手机上现场翻译过包里没有的着色器、包又比当前启动器旧：重新出包能补上
                games.append(dict(gameId=g, state="suggested", reasons=why) if why else dict(gameId=g, state="latest"))
        # 谁装的 S2Arm：连着就现查（install.s2arm_owner），列不出 / 没连着就按设备记录（启动器装过 = 有壳号；别的工具装的 = 推过时记下的 externalBundleId）
        who, ext, cands = (None, None, None)
        if online and online.get(key):
            import install
            who, ext = install.s2arm_owner(c, key, rec)
            if who == "choose" or rec.get("chosenBundleId"):   # 装着不止一个：列候选让玩家选（选过的也带上，可以换）
                cands = install.s2arm_candidates(c, key, rec)
        if who is None:
            who = "launcher" if rec.get("shellId") else "external" if rec.get("externalBundleId") else None
            ext = rec.get("externalBundleId") if who == "external" else None
        external = who == "external"
        choose = who == "choose"
        phone = dict(device=key, name=signed.get("device") or names.get(key) or "", installed=who in ("launcher", "external", "choose") if who else bool(rec.get("shellId")),
                     installedBy=who if who in ("launcher", "external") else None, bundleId=ext if external else rec.get("bundleId") or "",
                     shellCurrent=None if external else bool(sid) and rec.get("shellId") == sid, signedAt=at, free=free,
                     signMode="external" if external else _sign_mode(c, rec, signed),
                     expiresAtEstimate=until, remainingDays=max(0, math.ceil((until - time.time()) / 86400)) if until else None, expiresKnown=known,
                     lastSeen=rec.get("at") if isinstance(rec.get("at"), (int, float)) else None, canSign=_can_sign(rec), games=games)
        if external:   # 别的工具装的：给它自己的一套按钮，不出「安装 S2Arm」（会新建证书、顶掉玩家 AltStore 的证书）
            phone["actions"] = list(EXTERNAL_PHONE_ACTIONS)
        if choose:   # 装着不止一个、玩家还没选：只给「选用哪个 S2Arm」，不替玩家挑、不出安装
            phone.update(needsChoice=True, bundleId="", signMode=None, shellCurrent=None, actions=["chooseApp"])
        if cands and len(cands) > 1:
            phone["candidates"] = cands   # 前端据此列候选（要选时）或给「换一个」入口（选过了）；不往 actions 里加，启动器装的手机按钮照旧由前端算
        if online is not None:
            phone["online"] = online.get(key)
        phones.append(phone)
    return dict(phones=phones)


def compatible(c, entry):
    """iPhone 容器里的一款能不能原样放进本版宿主"""
    want = engine_compat(c)
    return bool(want) and dict(hostAbi=entry.get("host_abi", 1), dmshChain=entry.get("chain", LEGACY_CHAIN)) == want


def image_compat(image, manifest):
    chain = manifest.get("dmshChain")
    if not chain:
        libs = [p.name[2:] for p in (Path(image) / "shaders/direct").glob("b-*") if p.is_dir()]
        chain = libs[0] if len(libs) == 1 else None
    return dict(hostAbi=manifest.get("hostAbi", 1), dmshChain=chain)


LSREGISTER = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
# 系统应用登记里所有 S2ArmGame.app 记录逐条注销（含早已删掉的路径：系统照样给它们出启动台图标）。$1 = lsregister
FORGET_SCRIPT = ('"$1" -dump 2>/dev/null | grep "path:.*/S2ArmGame\\.app" | sed "s/.*path: *//; s/ (0x[0-9a-f]*)$//" | sort -u'
                 ' | while IFS= read -r p; do "$1" -u "$p" 2>/dev/null; done')


def forget_registrations(lsregister=None, wait=False, app=None):
    """#1126：Mac 宿主是直接执行 Contents/MacOS/s2arm-host 起局的，系统每次起它都会把这个 .app 登记进应用登记库，
    启动台 / 应用程序里就多出一个灰色「S2ArmGame」；引擎更新、换库后旧路径的登记还留着（玩家实测 43 条、34 条指向已删路径）。
    宿主不需要登记（启动器不经启动服务打开它）：每局结束后把全部 S2ArmGame.app 登记注销。
    后台脱离进程做（导出整张登记表约 3 秒），不拖慢回游戏库；失败不影响游戏，只是图标还在。wait=True 给单测
    app：这局起的宿主包。系统只登记带 Contents/Info.plist 的真 App 包——不是真包（单测的假宿主只有一个可执行文件）就没有可注销的，
      不碰系统登记库（11-15 门禁机上单测起局打到真 lsregister、38 个 -dump 同时跑）。
    同一时间只跑一个（锁随子进程持有到它退出）：已有一个在跑就跳过，下一局再注销，不叠"""
    if app is not None and not (Path(app) / "Contents" / "Info.plist").is_file():
        return None
    lsregister = lsregister or os.environ.get("S2A_LSREGISTER") or LSREGISTER   # 单测改指（jobcase：不碰本机真登记）
    if sys.platform != "darwin" or not os.path.exists(lsregister):
        return None
    import fcntl
    import hashlib
    import tempfile
    lock = Path(tempfile.gettempdir()) / f"s2a-lsregister-forget-{hashlib.sha1(lsregister.encode()).hexdigest()[:12]}.lock"
    fd = os.open(lock, os.O_RDWR | os.O_CREAT, 0o666)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return None
        p = subprocess.Popen(["/bin/bash", "-c", FORGET_SCRIPT, "forget", lsregister], stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True, pass_fds=(fd,))
    finally:
        os.close(fd)   # 子进程继承的那份还拿着锁，退出才放
    if wait:
        p.wait()
    return p


def host_exe(c, sha):
    return contained(c.library, layout.hosts_mac(c) / sha / EXE)


def install_host(c, app, sha=None):
    """把一份 S2ArmGame.app 放进 hosts/mac/<摘要>（已有就不动）→ 摘要。sha：调用方已知的摘要前 12 位（内置宿主取 engine.json 清单里的，
    起局不再每次现算 49 MB 的 SHA-256，11-19）；不给才现算"""
    from build import clone_tree
    sha = sha or digest(Path(app) / "Contents/MacOS/s2arm-host")[:12]
    dest = layout.hosts_mac(c) / sha
    if not _same_files(Path(app), dest / APP):   # 已有的一份被外部改过（#304 同类：Info.plist 大小变了）也重放，不只看可执行文件在不在
        temporary = dest.with_name(sha + ".partial")
        shutil.rmtree(temporary, ignore_errors=True)
        temporary.mkdir(parents=True)
        try:
            clone_tree(c, Path(app), temporary / APP)
            if dest.exists():
                shutil.rmtree(dest)
            temporary.rename(dest)
        finally:
            shutil.rmtree(temporary, ignore_errors=True)
    return sha


def _same_files(source, copy):
    """copy 与 source 的普通文件一一对应、大小相同（逐文件 lstat，宿主几十个文件；摘要已由 embedded.artifact 对 source 核过）"""
    def files(root):
        out = {}
        for d, _, names in os.walk(root):
            for n in names:
                p = os.path.join(d, n)
                st = os.lstat(p)
                out[os.path.relpath(p, root)] = st.st_size if stat.S_ISREG(st.st_mode) else -1
        return out
    try:
        return copy.is_dir() and files(copy) == files(source)
    except OSError:
        return False


def bundled_sha(c):
    """应用内置宿主的摘要前 12 位：engine.json 的文件清单里就有，不用现算（catalog 每款都要问）；开发模式没有 → None"""
    entry = read(c.engine / "engine.json", {}).get("files", {}).get("Binaries/" + EXE)
    return entry[1][:12] if isinstance(entry, list) and len(entry) == 2 else None


def status(c, gid, image, manifest):
    """catalog 用：none（开发模式 / 没准备好）| current（在用的就是本版宿主）| swap（下次起局自动换）| incompatible | failed（换过没起来、已退回）"""
    new, info = bundled_sha(c), layout.game_info(c, gid)
    if not new or not manifest:
        return "none"
    if info.get("hostMac") == new:
        return "current"
    if new in info.get("hostFailed", []):
        return "failed"
    return "swap" if image_compat(image, manifest) == engine_compat(c) else "incompatible"


def choose(c, gid, image, manifest):
    """起局用哪个宿主 → (摘要, 是不是新宿主的试玩)。兼容就换上本版内置宿主（放进 hosts/mac）；否则用这款原来的"""
    info = layout.game_info(c, gid)
    current = info.get("hostMac")
    if status(c, gid, image, manifest) in ("swap", "current"):   # current 也过一遍：hosts/mac 那份被外部改过就从内置宿主重放（#304），没变是几十次 lstat
        from embedded import artifact
        sha = install_host(c, artifact(c, APP), bundled_sha(c))
        return sha, sha != current
    if not current or not host_exe(c, current).is_file():
        raise Failure("这款游戏的 Mac 宿主不在了，请重新准备这款游戏。", code="runtime_invalid")
    return current, False


def settle(c, gid, sha, ok):
    """试玩结论：ok → 这款改用新宿主；否则记进 hostFailed，下次照用原来的宿主"""
    info = layout.game_info(c, gid)
    if ok:
        layout.update_info(c, gid, hostMac=sha)
        tidy(c, "清理不再用的旧运行层", prune, c)   # 清旧宿主只是省盘：出错不能把玩过的这一局判失败
    else:
        layout.update_info(c, gid, hostFailed=sorted(set(info.get("hostFailed", [])) | {sha}))
        c.emit("message", "新版运行层这款没能正常启动，下次会用原来的版本启动；再点一次开始即可。")


def tidy(c, what, fn, *a):
    """收尾里只为省盘 / 刷新显示的步骤：出错只提示一句、不抛（v0.2.3 清理：#343 出包已成功，收尾清旧宿主抛错把整次出包判成失败）"""
    try:
        return fn(*a)
    except Exception as e:   # noqa: BLE001 — 收尾步骤任何错误都不改变主结论
        c.emit("message", f"{what}没做完（不影响这次结果）：{e}")
        return None


def prune(c):
    """hosts/mac 里没有任何一款在用、也不是本版内置的宿主删掉（.partial 残留一并清）。
    要扫所有存放位置的游戏目录；有位置没连上（移动硬盘没插）就读不全，这次不清，宁可多留不误删"""
    root = layout.hosts_mac(c)
    if not root.is_dir():
        return
    keep = {bundled_sha(c)}
    # 扫每个位置的 games/ 而不是只读索引：索引不保证覆盖每个文件夹（layout.state 对不上时也要回头扫），这里要「宁可多留不误删」。
    #   只认是目录的条目；.DS_Store 这类文件读 .DS_Store/game.json 由 core.read 当「没有」（#343）
    for _, place, connected in layout.locations(c):
        if not connected:
            return
        games = layout.games_root(c, place)
        for d in games.iterdir() if games.is_dir() else []:
            if d.is_dir():
                keep.add(read(d / layout.GAME_JSON, {}).get("hostMac"))
    for d in root.iterdir():
        if d.is_dir() and not d.is_symlink() and d.name not in keep:
            shutil.rmtree(d, ignore_errors=True)
