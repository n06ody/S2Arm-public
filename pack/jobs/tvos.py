"""装到 Apple TV（tvOS 版，11-15 维护者拍板照开源项目 TVPort（brad-n/TVPort，MIT）的做法实现；组里没有 Apple TV，没在真机上验证过）。

装机（作业 tvInstall）：
  ① 设备：devicectl 设备表里 deviceType / platform 是 Apple TV 的（diag.is_tv，不写死型号）；配对靠 Xcode 自己完成（Apple TV 没有数据线，同网配对一次）
  ② 通用壳（0.6.0 起，同 iPhone）：电视上的 S2Arm 只有壳（本版 tvOS 预编壳，pack/ship_ext.py container --shell-only），本版、同包名、签名还新鲜就不重装；
     各款程序包（image/tvos/kit 出的 tvOS 版，program_pack.sign(platform="tvos")）打成一个 .s2arm，经 devicectl 写进电视上 App 的导入文件夹
     （install.push_program，同 iPhone；TvClient），App 用导入同一条代码解开到 Library/Caches/S2Arm/games/<号>/program/、换上清单，宿主按清单加载（runtime/src/launchctx.c program_paths）
  ③ 签名：pack/prov 的申请工程改成 tvOS 目标，xcodebuild -allowProvisioningUpdates 让 Xcode 申请 tvOS 开发描述文件（同 TVPort backend.py:1081 自动签名），
     再用 pack/dev_sign.py 签；权限按款（entitlements_for）：「提高内存上限」照带（免费账号在 tvOS 上被系统丢掉，不挡），「扩展虚拟寻址」只给 Switch 2 款（只有付费账号签得了）。包名 io.s2arm.tv.<团队号小写>：和 iPhone 的容器分开（开发描述文件按平台出，同一包名 dev_sign 会挑到 iOS 的那份）
  ④ 覆盖装之前把电视上各款的存档备份到 Mac（同下面的自动备份）；devicectl device install app（TVPort :1368）
  ⑤ tools/push_pack.py 按内容增量推资源包到 App 数据容器 Library/Caches/S2Arm/games/<程序号>/资源包（TVPort :1353 送进 Library/Caches）
     → 列电视上的文件、逐个核对大小（TVPort :1353 核两边大小）
  devicectl 报 3002 / 超时 / 网络 / 隧道这类临时错误时最多试 3 次（TVPort :1331）；别的错误一次就停，报 devicectl 原文。不做装机前的预检拦截。
存档自动备份（作业 tvSaveBackup，启动器连着 Apple TV 时每 30 分钟一轮；TVPort backend.py:735 backup_saves / :1546 cmd_autobackup）：
  每款一个槽（存档目录 games/<程序号>/S2A/ryujinx/bis/user/save）：先列文件 → 内容和最新一份的清单一样就不拉 → 逐个拉进临时目录、核大小 →
  再列一次，两次不一致（拉的时候游戏正好在存档）这份作废重来（最多 3 次）→ 拉回的和最新一份逐字节相同就不新建 → 否则改名成
  游戏库 games/<款>/save_backup/tv-<时刻>/（同 iPhone 覆盖装前备份的目录），每款只留最新 10 份 tv-*（不碰 iPhone 装机前的那些）。
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from core import Failure, atomic, read
import install
import layout

PLATFORM = "tvos"   # 游戏库 image/<平台>、game.json 里本平台出包记录的字段名（同 iPhone 的 "ios"）
TV_DOCS = "Library/Caches/S2Arm"   # 电视上宿主的文档根（iOS 的 Documents 换到这里：tvOS App 只能长期写 Library/Caches）；同 diag.TV_DOCS
TV_RECORDS = "devices_tv"   # 默认位置根下每台电视一份记录 <设备>.json（和 iPhone 的 devices/ 分开：「更新 iPhone 上的 S2Arm」只读 devices/）
KIND = "Apple TV"


def tv_docs(gid):
    """电视上这款的文档根（存档在它下面与 iOS 同样的相对路径）"""
    return f"{TV_DOCS}/games/{gid.upper()}"


def tv_pack_root(gid):
    return f"{tv_docs(gid)}/资源包"  # 非玩家句


def tv_save_dir(gid):
    return f"{tv_docs(gid)}/{install.SAVE_REL}"


def tv_image(c, gid):
    return layout.path(c, gid, "image", PLATFORM, create=False)


def tv_kit(c, gid):
    return tv_image(c, gid) / layout.IOS_KIT


def tv_pack(c, gid):
    return tv_image(c, gid) / layout.IOS_PACK


def tv_ready(c, gid):
    """这款的 Apple TV 版已出好（容器件 + 资源包都在）：游戏库 catalog 的 appleTvReady"""
    return (tv_kit(c, gid) / "main.npdm").is_file() and (tv_pack(c, gid) / "manifest.json").is_file()


def tv_bundle_id(team):
    return f"io.s2arm.tv.{team.lower()}"


# ---- 设备 ----

S_NO_TV = ("没看到 Apple TV：让 Apple TV 开着、和这台 Mac 连同一个网络，并在 Xcode 的「Devices and Simulators」窗口里和它配对过一次"
           "（Apple TV 上打开「设置 → 遥控器与设备 → 遥控器 App 与设备」会显示配对码），再试一次。")
S_MANY_TV = "这台 Mac 看得到不止一台 Apple TV：{0}。在装机页选要装的那一台再装。"


def tv_list(c, retry=True):
    """devicectl 设备表里的 Apple TV → (设备列表, 读不到的原因)。真装机时一次没读到再读（最多 3 次，同 iPhone 装机：设备表会瞬时空）"""
    import target
    from diag import is_tv
    tvs, why = [], ""
    for k in range(install.LINK_READS if retry else 1):
        devices, why, _, _ = target.device_list(c, timeout=60)
        tvs = [d for d in devices if is_tv(d)]
        if tvs or why or k + 1 >= (install.LINK_READS if retry else 1):
            break
        time.sleep(install.LINK_RETRY_WAIT)
    return tvs, why


def tv_name(d):
    return (d.get("deviceProperties", {}).get("name") or "").strip()


def tv_key(d):
    return d.get("hardwareProperties", {}).get("udid") or d.get("identifier") or ""


def row(d):
    from diag import reachable
    h, p = d.get("hardwareProperties", {}), d.get("deviceProperties", {})
    return dict(id=d.get("identifier", ""), udid=h.get("udid", ""), name=tv_name(d), model=h.get("marketingName") or h.get("productType") or "",
                os=p.get("osVersionNumber") or "", kind="appleTV", online=reachable(d))


def pick(c, want=""):
    """要装 / 备份的那台：给了 want（玩家选的那台）列着就用它；没列着但只有一台就用那一台；一台都没有 / 多台对不上 → 报原因"""
    tvs, why = tv_list(c)
    if why:
        raise Failure(why, code="device_not_connected")
    if want:
        hit = [d for d in tvs if want in (d.get("identifier"), d.get("hardwareProperties", {}).get("udid"), tv_name(d))]
        if hit:
            return hit[0]
    if len(tvs) == 1:
        return tvs[0]
    if not tvs:
        raise Failure(S_NO_TV, code="device_not_connected")
    from diag import reachable
    online = [d for d in tvs if reachable(d)]
    if len(online) == 1:   # 两台已配对、只有一台在线：直接用在线的那台，别让玩家先在一台关机的电视和一台开机的电视里选
        return online[0]
    raise Failure(S_MANY_TV.replace("{0}", "、".join("「" + (tv_name(d) or KIND) + "」" for d in (online or tvs))), code="device_required")


def devices_job(c):
    """作业 tvDevices：连着（配对过）的 Apple TV → {devices: [{id, udid, name, model, os, kind, online}]}；读不到 / 没有 = 空表，不报错"""
    tvs, _ = tv_list(c, retry=False)
    return dict(devices=[row(d) for d in tvs])


# ---- 临时错误重试（TVPort backend.py:1331 with_retries）----

TRANSIENT = re.compile(r"3002|coordinat|locate|time[ds]? ?out|connection|network|unreachable|tunnel|IXErrorDomain", re.I)   # 底下网络抖一下时 devicectl 的说法
ATTEMPTS = 3
RETRY_DELAYS = (5, 15)   # 第 2、3 次之前等几秒（单测置 0）


def transient(rc, reason):
    from diag import DEVICECTL_TIMEOUT
    return rc == DEVICECTL_TIMEOUT or bool(TRANSIENT.search(reason or ""))


def retried(c, what, attempt):
    """attempt() → None 成了，或 (退出码, 原因)。像临时错误的再试，最多 ATTEMPTS 次；别的错误立刻返回原因。→ None 或最后一次的 (退出码, 原因)"""
    for n in range(1, ATTEMPTS + 1):
        got = attempt()
        if got is None:
            return None
        rc, reason = got
        if n == ATTEMPTS or not transient(rc, reason):
            return got
        delay = RETRY_DELAYS[min(n - 1, len(RETRY_DELAYS) - 1)]
        tail = (reason or "").strip().splitlines()[-1:] or ["没有给原因"]  # 非玩家句
        install.note(c, f"{what}没成（第 {n} 次，共试 {ATTEMPTS} 次）：{tail[0][:200]}；{delay} 秒后再试")  # 非玩家句
        time.sleep(delay)
    return got


# ---- 记录（默认位置根 devices_tv/<设备>.json）----

def rec_path(c, key):
    return c.library / TV_RECORDS / (layout.device_key(key) + ".json")


def load_rec(c, key):
    rec = read(rec_path(c, key), {})
    return dict(dict(schema=1, games={}, shellId="", bundleId=""), **rec) if isinstance(rec, dict) else dict(schema=1, games={}, shellId="", bundleId="")


def save_rec(c, key, rec):
    p = rec_path(c, key)
    p.parent.mkdir(parents=True, exist_ok=True)
    atomic(p, dict(rec, schema=1))


# ---- 组装 ----

def tv_shell(c):
    """tvOS 预编壳 → (目录, 壳号)：请求给的 tvShellDir > 发版应用资源 Shell-tvOS/ > 开发树本机缓存 <S2A_RUN_ROOT>/s2arm/cache/shell-tvos/<壳编号>
    （同 iPhone 的 Shell/ 与 cache/shell/<壳编号>）。壳号取壳 manifest.json 的 details.shellId，没写按本引擎壳编号"""
    import ipa as packer
    if c.r.get("tvShellDir"):
        d = Path(c.r["tvShellDir"])
    elif c.r.get("engineBundle"):
        d = Path(c.r.get("resources") or c.private) / "Shell-tvOS"
    else:
        d = Path(c.env().get("S2A_RUN_ROOT") or Path.home() / ".s2arm/run") / "s2arm/cache/shell-tvos" / tv_shell_id(c)
    manifest = d / "manifest.json"
    if not manifest.is_file():
        raise Failure("这台电脑上还没有 Apple TV 版的预编壳：先给这款出一次 Apple TV 包，再装到 Apple TV。", code="tool_missing", internal=str(d))
    return d, ((read(manifest, {}) or {}).get("details") or {}).get("shellId") or tv_shell_id(c)


ASTC_PACK = "astc.pack"   # 资源包里的 BC → ASTC 预转包（pack/astc_pack.py 出；宿主在电视上见到就设 NXGAL_ASTC_PACK，ios/tv_host.m）


def clone_tree(src, dst):
    """整棵目录拷一份：Mac 用 cp -cR（APFS 克隆，不占空间；不同卷时 cp 自动退成整份拷贝），Windows 没有 cp，整份拷贝（NTFS 没有写时复制，
    不用硬链接：之后改 tvOS 这份不能动到 iPhone 那份）"""
    if sys.platform == "darwin":
        r = subprocess.run(["cp", "-cR", str(src), str(dst)], capture_output=True, text=True)
        if r.returncode:
            raise OSError((r.stderr.strip().splitlines() or ["cp 失败"])[-1])  # 非玩家句
        return
    shutil.copytree(src, dst)


def _clone_file(src, dst):
    """单个文件：Mac cp -c（APFS 克隆，不同卷自动退成拷贝），别的平台整份拷"""
    if sys.platform == "darwin":
        r = subprocess.run(["cp", "-c", str(src), str(dst)], capture_output=True, text=True)
        if r.returncode:
            raise OSError((r.stderr.strip().splitlines() or ["cp 失败"])[-1])  # 非玩家句
        return dst
    return shutil.copy2(src, dst)


def clone_pack(c, src, dst):
    """iPhone 资源包派生给 Apple TV（体系文档 L）：game.img 用硬链接引用 iPhone 那份（同一个文件，Windows NTFS 也不再实拷几 GB），
    其余小件照拷（电视这份要改清单、加 astc.pack，都是写新文件替换，不动 game.img）。建不了硬链接（跨盘 / exFAT）→ game.img 照旧拷。
    game.img 不在或是链接：整棵照旧（clone_tree）"""
    img = Path(src) / "game.img"
    if not img.is_file() or img.is_symlink():
        return clone_tree(src, dst)
    shutil.copytree(src, dst, ignore=lambda d, names: ["game.img"] if Path(d) == Path(src) else [], copy_function=_clone_file)
    try:
        os.link(img, Path(dst) / "game.img")
    except OSError:
        _clone_file(img, Path(dst) / "game.img")


def derive(c, gid, extraction):
    """出 Apple TV 包（ipa 作业带 platform = "tvos"，iPhone 版刚交付、解包目录还没清时调）：电视与 iPhone 跑同一份改写产物，差别只在
    组装时镜像包标的平台（ios/embed.py 按 S2A_APPLE_OS 写）、tvOS 壳、和多一个 ASTC 预转包（A15 不支持 BC 贴图，不转会在 CPU 上整张解成未压缩、
    贴图内存涨 4–8 倍，电视约 2 GB 就被系统结束）。所以：
      ① image/tvos/kit、image/tvos/.pack 从 image/ios 拷一份（clone_tree：Mac APFS 克隆不占空间，Windows 整份拷贝；资源包同一份，电视上按 Library/Caches 推；
         0.6.0 起 .pack/game.img 是 iPhone 那份的硬链接，clone_pack）
      ② 解开的 romfs → image/tvos/.pack/astc.pack（只给电视推，iPhone 资源包不带）
      ③ 开发树本机还没有本版 tvOS 壳就出一个（发版应用自带 Shell-tvOS/）
      ④ game.json 记 tvos：壳号、兼容判据、资源包号（usable_kit 按它判）"""
    import host_update
    pack = layout.image_ios(c, gid, create=False) / layout.IOS_PACK
    if not layout.has_ios_kit(c, gid) or not (pack / "manifest.json").is_file():
        raise Failure("iPhone 版没出好，Apple TV 包从它派生：先看上面 iPhone 那一步的报错。", code="package_failed")
    kit = layout.ios_kit(c, gid)   # iPhone 程序件：0.5.0 起常驻在库根 .programs + 缓存（pack/jobs/programs.py kit_dir），不在 image/ios/kit 了
    if not (kit / "main.npdm").is_file():
        raise Failure("iPhone 版没出好，Apple TV 包从它派生：先看上面 iPhone 那一步的报错。", code="package_failed")
    dst = tv_image(c, gid)
    dst.mkdir(parents=True, exist_ok=True)
    for name, src in ((layout.IOS_KIT, kit), (layout.IOS_PACK, pack)):
        tmp = dst / (name + ".partial")
        shutil.rmtree(tmp, ignore_errors=True)
        try:
            (clone_pack(c, src, tmp) if name == layout.IOS_PACK else clone_tree(src, tmp))
        except (OSError, shutil.Error) as e:
            shutil.rmtree(tmp, ignore_errors=True)
            raise Failure("Apple TV 包的文件拷不出来：" + (str(e).strip().splitlines() or ["?"])[-1], code="package_failed") from e  # 非玩家句
        shutil.rmtree(dst / name, ignore_errors=True)
        os.replace(tmp, dst / name)
    romfs = Path(extraction) / "romfs"
    if romfs.is_dir():
        c.begin("把贴图预转成 Apple TV 能直接用的格式（BC → ASTC，只做一次）")
        log = c.logs / (c.task + "-astc.log")
        c.run([sys.executable, "-X", "utf8", "-B", c.engine / "pack/astc_pack.py", "build", romfs, dst / layout.IOS_PACK / ASTC_PACK], log, c.env())
        # 进资源包清单（推送按 index.tsv 推、机上按 index_sha256 认装好没有）：只改 tvOS 这份（克隆出来的，os.replace 写新文件，不动 iPhone 那份）
        from sgrade_bf import add_to_pack
        add_to_pack(dst / layout.IOS_PACK, ASTC_PACK, [ASTC_PACK])
    else:   # 没有 romfs 的款（只有程序）：不用预转
        c.emit("message", "这款没有 RomFS，Apple TV 包不用预转贴图")
    sid = tv_shell_id(c)
    shell = Path(c.env().get("S2A_RUN_ROOT") or Path.home() / ".s2arm/run") / "s2arm/cache/shell-tvos" / sid
    try:   # 有没有壳按装机 / 组装同一个取法判（tv_shell：tvShellDir > 发版应用 <resources>/Shell-tvOS > 开发树缓存）；原先看 <引擎>/Shell-tvOS，发版应用放在 Resources/Shell-tvOS 判成没有
        tv_shell(c)
        have = True
    except Failure:
        have = False
    if not have:
        if sys.platform != "darwin" or c.r.get("engineBundle"):   # 现出壳要 Xcode 和开发树：Windows 与发版应用只能用安装包带的 Shell-tvOS（launchers/macos/build_app.py、launchers/windows/bundle_win.py --tv-shell）
            raise Failure("这个 S2Arm 安装包没带 Apple TV 预编壳，这台电脑出不了 Apple TV 包：请更新到带 Apple TV 支持的 S2Arm 版本。", code="engine_incomplete")
        c.begin("出 Apple TV 预编壳（本版第一次）")
        tmp = shell.parent / (".tmp." + sid + "." + str(os.getpid()))
        shutil.rmtree(tmp, ignore_errors=True); shell.parent.mkdir(parents=True, exist_ok=True)
        c.run(["bash", c.engine / "pack/shell_build.sh", tmp], c.logs / (c.task + "-tvshell.log"), tv_env(c))
        os.replace(tmp, shell)
    want = read(dst / layout.IOS_PACK / "manifest.json", {}).get("index_sha256") or ""
    layout.update_info(c, gid, **{PLATFORM: dict(shellId=sid, pack=want, astc=(dst / layout.IOS_PACK / ASTC_PACK).is_file(), at=time.time(),
                                                  **(host_update.engine_compat(c) or {}))})


def tv_env(c):
    """组装 / 出壳子进程的环境：S2A_APPLE_OS=tvos（ios/embed.py 按 tvOS 写镜像包平台，pack/shell/manifest.py 按 tvOS 算壳编号）"""
    return dict(c.env(), S2A_APPLE_OS="tvos")


def tv_shell_id(c):
    """本引擎该用的 tvOS 壳编号（同 ipa.current_shell_id，按 tvOS 算）"""
    r = subprocess.run([sys.executable, "-X", "utf8", "-c", "from pack.shell.manifest import current_id; print(current_id('tvos'))"], cwd=c.engine, env=c.env(),
                       capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        raise Failure("算不出本引擎的 Apple TV 壳编号：" + ((r.stderr or "").strip().splitlines() or ["?"])[-1], code="tool_missing")  # 非玩家句
    return r.stdout.strip()


def usable_kit(c, gid, sid):
    """这款的 tvOS 容器件能不能放进本版壳 → 路径 | None（同 ipa.usable_kit：本壳出的直接用；别的壳出的、新宿主认得它也用）"""
    import host_update
    from core import resolve_path
    kit = tv_kit(c, gid)
    if not (kit / "main.npdm").is_file():
        return None
    info = layout.game_info(c, gid).get(PLATFORM) or {}
    if info.get("shellId") == sid:
        return resolve_path(kit)
    want = host_update.engine_compat(c)
    have = dict(hostAbi=info.get("hostAbi", 1), dmshChain=info.get("dmshChain", host_update.LEGACY_CHAIN))
    return resolve_path(kit) if want and have == want else None


S_DROPPED = "{0}这次没放进 Apple TV 上的 S2Arm（要按新版重新出 Apple TV 包）；电视上它们的存档和资源都还在，重新出包再装就回来。"


def assemble(c, gid, others, shell, sid):
    """现组电视上的容器 App（这款 + others）→ (临时目录, S2Arm.app, 全部款)。别的款放不进本壳（旧版出的 / 游戏库删了）就这次不带、记黄字：
    它们的存档和资源包还在电视上，重新出 Apple TV 包再装就回来。这款自己放不进 → 停"""
    import host_update
    import ipa as packer
    roots = {gid: usable_kit(c, gid, sid)}
    if roots[gid] is None:
        raise Failure("这款的 Apple TV 版不是这一版 S2Arm 出的（或还没出）：先出 Apple TV 包，再装到 Apple TV。", code="container_stale")
    catalog = c.catalog()["games"]
    names = {g["id"].upper(): g.get("name") or g["id"] for g in catalog}
    in_library = {g["id"].upper() for g in catalog if not g.get("hidden")}
    dropped = []
    for g in sorted({o.upper() for o in others} - {gid}):
        kit = usable_kit(c, g, sid) if g in in_library else None
        if kit is None:
            dropped.append(names.get(g, g))
        else:
            roots[g] = kit
    if dropped:
        c.diagnostic(S_DROPPED.replace("{0}", "「" + "」「".join(dropped) + "」"), code="container_stale", severity="warning")
    icon = c.engine / "launchers/macos/Branding/AppIcon-iOS.png"
    if not icon.is_file():
        raise Failure("这个 S2Arm 安装包不完整（缺应用图标），请重新下载 S2Arm。", code="engine_incomplete")
    out = Path(tempfile.mkdtemp(prefix=".tv-container-", dir=c.library))
    app, log = out / "S2Arm.app", c.logs / (c.task + "-container.log")
    (out / "custom.json").write_text(json.dumps(packer.container_custom(c), ensure_ascii=False), encoding="utf-8")
    banners = [x for g in roots for b in [layout.path(c, g, "banner.jpg", create=False)] if b.is_file() for x in ("--banner", f"{g}={b}")]
    try:
        c.run([sys.executable, "-X", "utf8", "-B", c.engine / "pack/ship_ext.py", "container", "--shell", shell, "--out", app, "--bundle-id", packer.CONTAINER_BUNDLE,
               "--icon", icon, "--sign", "none", "--custom", out / "custom.json", "--host-abi", str(host_update.HOST_ABI),
               "--version", packer.release_version(c.engine), "--display-version", packer.display_version(c.engine),
               *[x for r in roots.values() for x in ("--game", r)], *banners], log, tv_env(c))
    except Failure as e:
        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        shutil.rmtree(out, ignore_errors=True)
        if e.code == "tool_exit" and "地址空间类型不一致" in text:  # 非玩家句
            raise Failure(packer.incompatible_message(text, gid, names), code="container_incompatible") from e
        raise
    packer.sign_container(c, app, log)
    games = [g["title_id"] for g in json.loads((app / "s2a_catalogue.json").read_text(encoding="utf-8"))["games"]]
    try:   # 同 ipa.assemble：程序号和游戏包号对不上时按程序号放、资源取回游戏包号那款（认不出是哪个才停）
        packer.title_mismatch(c, gid, list(others), games)
    except Failure:
        shutil.rmtree(out, ignore_errors=True)
        raise
    return out, app, games


# ---- 签名 ----

PROV_SWAPS = (("SDKROOT = iphoneos;", "SDKROOT = appletvos;"), ('TARGETED_DEVICE_FAMILY = "1,2";', "TARGETED_DEVICE_FAMILY = 3;"),
              ("IPHONEOS_DEPLOYMENT_TARGET = 17.0;", "TVOS_DEPLOYMENT_TARGET = 17.0;"))   # pack/prov 模板（iOS 17 的 SwiftUI 空工程）改成 tvOS 目标


def prov_project(engine, dest, bid, team, ents):
    """pack/prov 申请工程拷一份、改成 tvOS 目标并填包名 / 团队号（同 pack/ensure_profile.sh 的替换）；模板改过、对不上就停，不悄悄申请成 iOS 的"""
    shutil.copytree(Path(engine) / "pack/prov", dest, dirs_exist_ok=True)
    shutil.copyfile(ents, Path(dest) / "app.entitlements")
    pbx = Path(dest) / "Prov.xcodeproj/project.pbxproj"
    text = pbx.read_text(encoding="utf-8")
    for old, new in PROV_SWAPS + (('"S2ABID"', f'"{bid}"'), ('"S2ATEAM"', f'"{team}"')):
        if old not in text:
            raise Failure(f"描述文件申请工程模板（pack/prov）里没有「{old}」，改不成 tvOS 目标", code="job_failed")  # 非玩家句
        text = text.replace(old, new)
    pbx.write_text(text, encoding="utf-8")
    return pbx


ENTITLEMENTS = "pack/app_tvos.entitlements"   # tvOS 版权限底本：只有「提高内存上限」（iOS 用 pack/app.entitlements）；扩展虚拟寻址按款加（entitlements_for）
IML = "com.apple.developer.kernel.increased-memory-limit"
EVA = "com.apple.developer.kernel.extended-virtual-addressing"


def is_switch2(npdm):
    """main.npdm 是不是 Switch 2 程序：ACID 用 RSA-3072 签名，魔数在 ACID +0x300（Switch 1 在 +0x200；同 runtime/src/svc_mem.c npdm_read）。读不出按否"""
    try:
        b = Path(npdm).read_bytes()
    except OSError:
        return False
    if b[:4] != b"META" or len(b) < 0x80:
        return False
    acid = int.from_bytes(b[0x78:0x7C], "little")
    return b[acid + 0x300:acid + 0x304] == b"ACID"


def entitlements_for(c, app, dest, npdms=None):
    """这个 App 要的权限 → 写到 dest（plist）。底本「提高内存上限」照带（免费账号的描述文件没有这项时 dev_sign 不带、不挡）；
    「扩展虚拟寻址」只在包里有 Switch 2 款时带：Switch 2 客体要约 12.5 GB 地址，tvOS / iOS 不给这项权限就不让占（TVPort build_ipa.sh:206 同判据：
    超过 6 GB 才带），Switch 1 款不要，免费账号也就签得了。npdms = 要放上电视的各款 main.npdm（通用壳的 App 本身不带游戏，按这些判；
    缺省看 App 包里的 Games/*/main.npdm，导出单款安装包用）。→ (路径, 带没带扩展虚拟寻址)"""
    import plistlib
    base = c.engine / ENTITLEMENTS
    if not base.is_file():
        raise Failure("这个 S2Arm 安装包不完整（缺 Apple TV 签名权限文件），请重新下载 S2Arm。", code="engine_incomplete", internal=str(base))
    ents = plistlib.loads(base.read_bytes())
    eva = any(is_switch2(n) for n in (npdms if npdms is not None else Path(app).glob("Games/*/main.npdm")))
    if eva:
        ents[EVA] = True
    Path(dest).write_bytes(plistlib.dumps(ents))
    return Path(dest), eva


# Switch 2 款要的「扩展虚拟寻址」只有付费开发者账号的描述文件允许：申请 / 签名失败的原文里认出是这一项，就照实说（只在真带了这一项时判）
EVA_SIGNS = re.compile(r"Personal development teams[^\n]{0,160}Extended Virtual Addressing|描述文件不允许这些权限[^\n]{0,200}extended-virtual-addressing|"  # 非玩家句
                       r"签名里没有[^\n]{0,80}extended-virtual-addressing", re.I)
S_EVA = ("这款是 Switch 2 游戏，要「扩展虚拟寻址」权限才能在 Apple TV 上占到够用的地址，这项权限只有付费的 Apple 开发者账号签得了。"
         "在 Xcode 的「设置 → 账户」里登录付费开发者账号（或在团队里选付费团队）再装；Switch 1 游戏用免费 Apple ID 就能装。")


def sign(c, app, team, bid, udid, log, npdms=None):
    """申请 tvOS 开发描述文件（带这台电视的 UDID：登记设备）→ pack/dev_sign.py 签（S2A_SIGN_PLATFORM=tvos：只挑 tvOS 描述文件）。
    权限按款（entitlements_for）。失败按日志原文归码，同 iPhone 装机；带了扩展虚拟寻址（Switch 2 款）而账号签不了这一项 → tvos_eva_required，照实说"""
    from errors import recovery, tool_reason
    c.begin("申请 Apple TV 开发描述文件（第一次要几十秒）", 3, 6, keychain=True)
    try:
        with tempfile.TemporaryDirectory(dir=c.private) as d:
            ents, eva = entitlements_for(c, app, Path(d) / "tv.entitlements", npdms)
            prov = Path(d) / "prov"
            prov_project(c.engine, prov, bid, team["id"], ents)
            dest = ["-destination", f"platform=tvOS,id={udid}", "-allowProvisioningDeviceRegistration", "-destination-timeout", "120"] if udid else \
                ["-destination", "generic/platform=tvOS"]
            c.run(["xcodebuild", "-project", "Prov.xcodeproj", "-scheme", "Prov", "-configuration", "Debug", *dest, "-derivedDataPath", "dd",
                   "-allowProvisioningUpdates", "build"], log, cwd=prov, timeout=600)
            c.begin("用你的 Apple ID 签名", 4, 6, keychain=True)
            c.run([sys.executable, c.engine / "pack/dev_sign.py", app, bid, team["id"], ents], log, env=dict(c.env(), S2A_SIGN_PLATFORM=PLATFORM))
    except Failure as e:
        if e.code != "tool_exit":
            raise
        text = log.read_text(errors="replace")[-8000:] if log.is_file() else ""
        if eva and EVA_SIGNS.search(text):
            raise Failure(S_EVA, code="tvos_eva_required", internal=tool_reason(log)) from e
        code = install.classify(text)
        raise Failure("签名没成：" + tool_reason(log) + "；" + recovery(code, packaged=bool(c.r.get("engineBundle"))), code=code) from e  # 非玩家句


# ---- 推资源包与核对 ----

def push(c, dev_id, bid, gid, pack, udid):
    """推这款的资源包（install.push_pack → tools/push_pack.py，按内容增量），临时错误再试（推包本身增量，再试只补没到的）。→ (文件数, 字节数)"""
    got = []
    def attempt():
        try:
            got.append(install.push_pack(c, dev_id, bid, pack, docs=tv_docs(gid), pack_root=tv_pack_root(gid), tv=True))
            return None
        except Failure as e:
            if e.code in NO_RETRY:   # 空间不够 / 写满：再试也一样，原样报
                raise Failure(str(e).replace("iPhone", KIND), code=e.code) from e
            got.append(e)
            return 1, str(e)
    if retried(c, "推资源包", attempt) is None:  # 非玩家句
        return got[-1]
    e = got[-1]
    raise Failure(str(e).replace("iPhone", KIND), code=e.code or "resource_transfer_failed") from e


NO_RETRY = ("device_space_low", "ios_install_no_space")


IMG = re.compile(r"(^|.*/)romfs\.img$|game\.img")   # 直接写到最终路径的单文件镜像（同 tools/push_pack.py IMG）


def zst_chunks(c, pack):
    """这份 game.img 这次推送是不是走压缩分块（同 tools/push_pack.py zstd_plan 的判法：index.tsv 有 game.img 那行、推送环境没关压缩、本机有 zstd）
    → [(资源包根下的块相对路径 .incoming/game.img.<号>.<块>.zst, 压缩字节)]；直推 → None"""
    sys.path.insert(0, str(Path(c.engine) / "tools"))
    import push_pack as P
    row = next((l.split("\t") for l in (Path(pack) / "index.tsv").read_text(encoding="utf-8", errors="replace").splitlines()
                if l.split("\t")[0] == "game.img" and len(l.split("\t")) >= 3), None) if (Path(pack) / "index.tsv").is_file() else None
    if not row:
        return None
    sizes = P.zstd_plan(Path(pack), row, say=lambda *a, **k: None, env=c.env())
    return None if sizes is None else [(".incoming/" + P.zname(row[2], k), n) for k, n in enumerate(sizes)]


def verify(c, dev_id, bid, gid, pack):
    """推完列电视上这款的资源包根，逐个核对大小（TVPort 推 romfs.img 后核两边大小）：索引、清单、单文件镜像。分批推的小文件在 .incoming 里由 App 打开时解开，不在这里核"""
    from diag import devicectl
    pack = Path(pack)
    rc, data, err = devicectl(c, "device", "info", "files", "--device", dev_id, "--domain-type", "appDataContainer", "--domain-identifier", bid,
                              "--subdirectory", tv_pack_root(gid), timeout=300)
    if rc:
        raise Failure("核对不了 Apple TV 上的资源包（列不出电视上的文件）：" + (err.splitlines()[-1] if err else f"devicectl 退出 {rc}"), code="resource_transfer_failed")  # 非玩家句
    there = {(f.get("relativePath") or f.get("path")): (f.get("metadata") or {}).get("size")
             for f in ((data.get("result") or {}).get("files") or []) if isinstance(f, dict) and not (f.get("resources") or {}).get("isDirectory")}
    want = ["index.tsv", "manifest.json"] + sorted(p.relative_to(pack).as_posix() for p in pack.rglob("*") if p.is_file() and not layout.os_junk(p.name) and IMG.search(p.relative_to(pack).as_posix()))
    chunks = zst_chunks(c, pack)
    for rel in want:
        local = pack / rel
        if not local.is_file():
            continue
        if rel == "game.img" and chunks is not None and there.get(rel) != local.stat().st_size:
            # 压缩分块推的（tools/push_pack.py send_zst）：电视上的 game.img 先被截空，等 App 打开解包（pack_apply.c img_unzst）才拼回原样（#1460：
            #   原先照最终大小核，推完必报「电视上是 0 字节」、重装也一样）。核每一块：在 .incoming 里且字节对；App 已在解的块解完即删，game.img 已有内容时允许不在
            bad = [x for x, n in chunks if there.get(x) not in (n, None)]
            gone = [x for x, n in chunks if there.get(x) is None]
            if not bad and (not gone or (there.get(rel) or 0) > 0):
                continue
            raise Failure(f"资源包没有完整传到 Apple TV：game.img 的压缩块 {len(bad) + len(gone)}/{len(chunks)} 个缺或不完整。"  # 非玩家句
                          "再点一次「装到 Apple TV」会补传。", code="resource_transfer_failed")
        if there.get(rel) != local.stat().st_size:
            got = there.get(rel)
            raise Failure(f"资源包没有完整传到 Apple TV：{rel} 电视上{'没有' if got is None else f'是 {got} 字节'}，这台 Mac 上是 {local.stat().st_size} 字节。"  # 非玩家句
                          "再点一次「装到 Apple TV」会补传。", code="resource_transfer_failed")
    return len(want)


# ---- 存档备份 ----

BACKUP_PREFIX = "tv-"
KEEP = 10   # 每款（每槽）留最新几份自动备份（TVPort save_import.prune_automatic_backups 留 10 份）
LISTING = ".tv-listing.json"   # 备份里记着拉它时电视上的文件清单：清单一样下一轮就不拉


def save_listing(c, dev_id, bid, gid):
    """电视上这款的存档文件 → {相对存档目录的路径: [大小, 修改时刻]}；电视回答了但没有这个 App / 目录 → {}；没回答（断开 / 超时）→ None"""
    from diag import devicectl
    rc, data, _ = devicectl(c, "device", "info", "files", "--device", dev_id, "--domain-type", "appDataContainer", "--domain-identifier", bid,
                            "--subdirectory", tv_save_dir(gid), timeout=120)
    if rc:   # CoreDevice.ActionError = 电视回答了：没有这个 App 或这个目录（TVPort save_listing）
        return {} if ((data or {}).get("error") or {}).get("domain") == "CoreDevice.ActionError" else None
    out = {}
    for f in ((data.get("result") or {}).get("files") or []):
        if not isinstance(f, dict) or (f.get("resources") or {}).get("isDirectory"):
            continue
        rel = f.get("relativePath") or f.get("path") or ""
        if not rel or rel.rsplit("/", 1)[-1].startswith("."):
            continue
        meta = f.get("metadata") or {}
        out[rel] = [meta.get("size"), meta.get("lastModDate")]
    return out


def backups(place):
    return sorted(p for p in place.iterdir() if p.is_dir() and p.name.startswith(BACKUP_PREFIX)) if place.is_dir() else []


def same_files(one, other):
    def names(root):
        return sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file() and p.name != LISTING)
    return names(one) == names(other) and all((one / n).read_bytes() == (other / n).read_bytes() for n in names(one))


def prune(place):
    for old in backups(place)[:-KEEP]:
        shutil.rmtree(old, ignore_errors=True)


def backup_game(c, dev_id, bid, gid):
    """一款一轮：→ {state: backed-up / unchanged / none（电视上没有存档）/ unavailable（电视没回答）/ busy（三次都赶上游戏在存档）, files, folder}。
    拉失败抛 Failure（save_backup_failed），已有的备份不动"""
    from diag import devicectl
    import fcntl
    place = layout.path(c, gid, "save_backup")
    place.mkdir(parents=True, exist_ok=True)
    with open(place / ".tv-backup.lock", "w") as lock:   # 自动备份与装机前的备份可能撞上
        fcntl.flock(lock, fcntl.LOCK_EX)
        have = backups(place)
        newest = have[-1] if have else None
        listing = None
        for _ in range(ATTEMPTS):
            listing = save_listing(c, dev_id, bid, gid)
            if listing is None:
                return dict(state="unavailable", files=0, folder=str(newest) if newest else "")
            if not listing:
                return dict(state="none", files=0, folder=str(newest) if newest else "")
            if newest and read(newest / LISTING, None) == listing:
                return dict(state="unchanged", files=len(listing), folder=str(newest))
            incoming = Path(tempfile.mkdtemp(prefix=".tv-incoming-", dir=place))   # 在备份之外：拉到一半失败的不会被当成一份备份
            try:
                for rel, (size, _) in sorted(listing.items()):
                    if Path(rel).is_absolute() or ".." in Path(rel).parts:
                        raise Failure(f"Apple TV 返回的存档路径不对（{rel}），没有备份", code="save_backup_failed")  # 非玩家句
                    dest = incoming / rel
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    rc, _, err = devicectl(c, "device", "copy", "from", "--device", dev_id, "--domain-type", "appDataContainer", "--domain-identifier", bid,
                                           "--source", f"{tv_save_dir(gid)}/{rel}", "--destination", str(dest), timeout=120)
                    if rc or not dest.is_file() or (size is not None and dest.stat().st_size != size):
                        raise Failure(f"从 Apple TV 拉存档没成（{rel}）：" + (err.splitlines()[-1] if err else f"devicectl 退出 {rc}" if rc else "大小对不上"),  # 非玩家句
                                      code="save_backup_failed")
                if save_listing(c, dev_id, bid, gid) != listing:
                    continue   # 拉的时候游戏正好在存档：这份可能新旧混着，作废重来
                if newest and same_files(incoming, newest):
                    atomic(newest / LISTING, listing)   # 内容没变、只是重写过：记下这份清单，下一轮不用再拉
                    return dict(state="unchanged", files=len(listing), folder=str(newest))
                atomic(incoming / LISTING, listing)
                stamp, n = BACKUP_PREFIX + time.strftime("%Y%m%d-%H%M%S"), 1
                target = place / stamp
                while target.exists():
                    n += 1
                    target = place / f"{stamp}-{n}"
                os.rename(incoming, target)
                prune(place)
                return dict(state="backed-up", files=len(listing), folder=str(target))
            finally:
                shutil.rmtree(incoming, ignore_errors=True)
        return dict(state="busy", files=len(listing or {}), folder=str(newest) if newest else "")


def autobackup(c):
    """作业 tvSaveBackup（启动器连着 Apple TV 时每 30 分钟调一次）：每台电视上这台 Mac 装过的各款各备份一轮。
    → {state: done / partial（有款失败）/ unreachable（没看到电视）, devices: [{device, name, games: {款: 结果}}]}；没连电视不算错"""
    tvs, _ = tv_list(c, retry=False)
    want = (c.r.get("device") or "").strip()
    if want:
        tvs = [d for d in tvs if want in (d.get("identifier"), d.get("hardwareProperties", {}).get("udid"), tv_name(d))]
    from diag import reachable
    out = dict(state="done" if tvs else "unreachable", devices=[])
    for d in tvs:
        rec = load_rec(c, tv_key(d))
        games, bid = sorted(rec.get("games") or {}), rec.get("bundleId") or ""
        entry = dict(device=tv_key(d), name=tv_name(d), games={})
        out["devices"].append(entry)
        if not reachable(d):
            entry["state"] = "unreachable"
            continue
        for g in games if bid else []:
            try:
                entry["games"][g] = backup_game(c, d["identifier"], bid, g)
            except Failure as e:
                entry["games"][g] = dict(state="failed", message=str(e))
                out["state"] = "partial"
    return out


# ---- 装机 ----

def require_pack(c, gid):
    if not (tv_kit(c, gid) / "main.npdm").is_file():
        raise Failure("这款游戏还没出 Apple TV 包：先在出包页勾上「Apple TV」出一次包。", code="job_failed")
    pack = tv_pack(c, gid)
    if not (pack / "manifest.json").is_file():
        raise Failure(f"没找到这款的 Apple TV 资源包（{pack}），没有装。请重新出 Apple TV 包后再装。", code="pack_missing")
    return pack


# ---- 导出 Apple TV 安装包（.ipa，11-16 维护者：玩家用自己的工具签名、自己装，Mac / Windows 同一套；没上电视验过）----
#   电视没有「文件」App、没有文件共享，资源推不进去 → 这款的资源包整份打进 App 本体 S2Arm.app/Games/<号>/资源包/（同电视上推好、解好的样子：
#   index.tsv 列的文件 + index.tsv / manifest.json + index.applied.tsv + .applied = 清单 index_sha256），电视宿主先找包内、没有再找 Library/Caches
#   （runtime/include/s2a_docs.h s2a_pack_root）。资源包不先拷进 App：组好不带资源的容器 → 打成 ipa → 资源文件从游戏库原地流式追加进 zip，
#   盘上只多出 ipa 本身（不复制 2–3 倍）。单款一个 App，包名 io.s2arm.tv.<程序号小写>（几款能同时装在电视上；玩家的签名工具一般会自己改包名）。
#   主程序 ad-hoc 签名里带权限（同 TVPort build_ipa.sh:253：侧载工具换签名时保留权限）：提高内存上限照带，Switch 2 款再带扩展虚拟寻址。
#   单个文件或整包超过 4 GB 时只能写成 zip64：这类大 ipa 有的侧载工具读不了（pack/app_bundle.py write_ipa 的口径），结果里照实标出
TV_EXPORT_DIRS = {"zh": "S2Arm Apple TV 导出", "en": "S2Arm Apple TV Export"}  # 非玩家句
ZIP32_MAX = 0xFFFFFFFF


def tv_export_bundle_id(gid):
    return f"io.s2arm.tv.{gid.lower()}"


def pack_entries(pack):
    """电视上推好、解好的资源包里有的文件 → [(包内相对路径, 本地文件)]：index.tsv 列的每个文件 + index.tsv / manifest.json（同 tools/push_pack.py 推的口径）；
    列着的文件不在 → 停（导出的包缺资源，电视上起局才报，不如这里说清）"""
    rows = [l.split("\t") for l in (pack / "index.tsv").read_text(encoding="utf-8").splitlines() if l.count("\t") == 2]
    out = []
    for rel, size, _ in rows:
        f = pack / rel
        if not f.is_file() or f.stat().st_size != int(size):
            raise Failure(f"这款的 Apple TV 资源包不完整（缺 {rel} 或大小不对）：请重新出 Apple TV 包后再导出。", code="pack_missing", internal=str(f))
        out.append((rel, f))
    return out + [("index.tsv", pack / "index.tsv"), ("manifest.json", pack / "manifest.json")]


def adhoc_entitle(c, app, ents, log):
    """主程序 ad-hoc 签名换成这款要的权限（只在要加扩展虚拟寻址时调）：Mac 用 codesign 整包重签，Windows 用随包 rcodesign 签主程序"""
    import plistlib
    exe = app / plistlib.loads((app / "Info.plist").read_bytes())["CFBundleExecutable"]
    if sys.platform == "darwin":
        c.run(["codesign", "--force", "--sign", "-", "--entitlements", ents, app], log)
    else:
        import devcert
        c.run([devcert.rcodesign_path(c), "sign", "--entitlements-xml-path", ents, exe], log)


def write_tv_ipa(c, app, gid, pack, ipa, log):
    """容器 App（不带资源）→ ipa（pack/app_bundle.py write_ipa，同 iPhone 口径）→ 追加这款资源包（流式、不压缩、包内路径 Payload/S2Arm.app/Games/<号>/资源包/）
    → (总字节, 是不是 zip64)"""
    import zipfile
    import ipa as packer
    packer.write_container_ipa(c, app, ipa, log)
    entries = pack_entries(pack)
    sha = (read(pack / "manifest.json", {}) or {}).get("index_sha256") or ""
    if not sha:
        raise Failure("这款的 Apple TV 资源包清单没写资源包号：请重新出 Apple TV 包后再导出。", code="pack_missing")
    root = f"Payload/S2Arm.app/Games/{gid}/资源包/"  # 非玩家句
    total = sum(f.stat().st_size for _, f in entries)
    done = 0
    with zipfile.ZipFile(ipa, "a", compression=zipfile.ZIP_STORED, allowZip64=True) as z:
        for rel, f in entries:
            z.write(f, root + rel)   # 按块读写，不整份读进内存
            done += f.stat().st_size
            c.progress(done, total)   # 也核取消
        z.writestr(root + "index.applied.tsv", (pack / "index.tsv").read_bytes())
        z.writestr(root + ".applied", sha + "\n")
    size = Path(ipa).stat().st_size
    big = size > ZIP32_MAX or any(f.stat().st_size > ZIP32_MAX for _, f in entries)
    return size, big


def export_tv(c):
    """作业 tvExport：导出这款的 Apple TV 安装包（未签名 .ipa，资源包在 App 本体里），到游戏库根「S2Arm Apple TV 导出」。不连电视、不签名"""
    import ipa as packer
    gid = (c.r.get("gameId") or "").upper()
    c.begin("核对这款的 Apple TV 包", 1, 4)
    pack = require_pack(c, gid)
    shell, sid = tv_shell(c)
    with packer.program_locks(c, [gid]):   # 读这款的程序件与资源包：整段持有程序锁，出包交付换件时等它写完（同 iPhone 自签导出 install.export_phone）
        dest, size, big, eva = _export_tv(c, gid, pack, shell, sid)
    c.begin("导出完成", 4, 4)
    return dict(outputPath=str(dest), ipa=str(dest), ipaBytes=size, bundleId=tv_export_bundle_id(gid), zip64=big, eva=eva, deviceKind=KIND)


def _export_tv(c, gid, pack, shell, sid):
    import plistlib
    import ipa as packer
    from install import _BAD_NAME, ui_lang
    names = {g["id"].upper(): g.get("name") or g["id"] for g in c.catalog()["games"]}
    c.begin("组装 Apple TV 版 S2Arm（未签名）", 2, 4)
    out, app, _ = assemble(c, gid, [], shell, sid)
    log = c.logs / (c.task + "-tvexport.log")
    dest_dir = c.library / TV_EXPORT_DIRS[ui_lang(c)]
    tmp = dest_dir / (".partial-" + gid + ".ipa")
    try:
        info = app / "Info.plist"
        pl = plistlib.loads(info.read_bytes()); pl["CFBundleIdentifier"] = tv_export_bundle_id(gid); info.write_bytes(plistlib.dumps(pl))
        ctx = app / "Games" / gid / "launch.ctx"   # 资源包在 App 本体里：按启动上下文直接起这一款时（S2A_LAUNCH_CTX）也读包内（游戏库选游戏走 s2a_pack_root，本来就先找包内）
        if ctx.is_file():
            line = f"game_data\t@bundle/Games/{gid}/资源包\n"  # 非玩家句
            ctx.write_text("".join(line if l.startswith("game_data\t") else l for l in ctx.read_text(encoding="utf-8").splitlines(keepends=True)),
                           encoding="utf-8", newline="\n")
        if sys.platform == "darwin":
            packer.sign_container(c, app, log)   # 改了 Info.plist：Mac 重签 ad-hoc
        ents, eva = entitlements_for(c, app, out / "tv.entitlements")
        if eva:
            adhoc_entitle(c, app, ents, log)
        c.begin("把游戏资源打进安装包（.ipa）", 3, 4)
        dest_dir.mkdir(parents=True, exist_ok=True)
        base = _BAD_NAME.sub(" ", names.get(gid, gid)).strip().strip(".") or gid
        dest = dest_dir / f"{base} Apple TV.ipa"
        tmp.unlink(missing_ok=True)
        size, big = write_tv_ipa(c, app, gid, pack, tmp, log)
        os.replace(tmp, dest)
    finally:
        shutil.rmtree(out, ignore_errors=True)
        tmp.unlink(missing_ok=True)   # 成功时已换名，不在
    return dest, size, big, eva


def install_app(c, dev_id, app):
    from diag import devicectl
    def attempt():
        rc, _, err = devicectl(c, "device", "install", "app", "--device", dev_id, str(app), timeout=1800)
        return None if rc == 0 else (rc, err.strip()[-2000:] or f"devicectl 退出 {rc}（未提供错误详情）")  # 非玩家句
    failed = retried(c, "装 App", attempt)  # 非玩家句
    if failed:
        raise Failure("装不上 Apple TV：" + failed[1], code=install.classify(failed[1], default="device_install_failed"))  # 非玩家句


SHELL_FRESH = (3 * 86400, 30 * 86400)   # 电视上的壳签名多新就不重签重装：免费账号描述文件 7 天到期，签了不到 3 天才跳过；付费 30 天（同 iPhone install.SAME_APP_FRESH）


def shell_fresh(c, rec, bid, sid, eva=False):
    """电视上已是本版壳、同一个包名、签名离到期还早、请求没要求重装、要「扩展虚拟寻址」时现装的带着 → 不重签重装 App，只推这款"""
    signed = rec.get("signed") or {}
    if c.r.get("forceInstall") or rec.get("bundleId") != bid or rec.get("shellId") != sid or not isinstance(signed.get("at"), (int, float)):
        return False
    if eva and not rec.get("eva"):
        return False
    return time.time() - signed["at"] < SHELL_FRESH[0 if signed.get("free", True) else 1]


def assemble_shell(c, shell):
    """通用壳（11-19 Apple TV 也走它）：只有壳的 tvOS 容器（s2a_catalogue.json 的 games 为空），各款程序包另推进电视 program/。→ (临时目录, S2Arm.app)"""
    import host_update
    import ipa as packer
    icon = c.engine / "launchers/macos/Branding/AppIcon-iOS.png"
    if not icon.is_file():
        raise Failure("这个 S2Arm 安装包不完整（缺应用图标），请重新下载 S2Arm。", code="engine_incomplete")
    out = Path(tempfile.mkdtemp(prefix=".tv-shell-", dir=c.library))
    app, log = out / "S2Arm.app", c.logs / (c.task + "-container.log")
    try:
        c.run([sys.executable, "-X", "utf8", "-B", c.engine / "pack/ship_ext.py", "container", "--shell-only", "--shell", shell, "--out", app,
               "--bundle-id", packer.CONTAINER_BUNDLE, "--icon", icon, "--sign", "none", "--host-abi", str(host_update.HOST_ABI),
               "--version", packer.release_version(c.engine), "--display-version", packer.display_version(c.engine)], log, tv_env(c))
        packer.sign_container(c, app, log)   # 0.6.0 起 ipa.finish_container 已并进 sign_container（68e5887f1），这里漏改：每次装 Apple TV 都 AttributeError（#1758）
    except BaseException:
        shutil.rmtree(out, ignore_errors=True)
        raise
    return out, app


def tv_signer(c):
    """电视上的程序包签名：钥匙串开发身份（pack/dev_sign.py --dylib，按这个包名的 tvOS 开发描述文件挑身份，同签 App 的那一个）。
    同一团队才过宿主加载镜像包时的库校验"""
    def run(dylib, team, bundle):
        c.run([sys.executable, Path(c.engine) / "pack/dev_sign.py", "--dylib", dylib, bundle, team], c.logs / (c.task + "-program-sign.log"),
              env=dict(c.env(), S2A_SIGN_PLATFORM=PLATFORM))
    run.kind = "xcode-tvos"
    return run


class TvClient:
    """install.push_program 要的设备客户端，走 devicectl（电视没有数据线、没有 AFC）：只用 put（一个 .s2arm 写进电视上 App 的导入文件夹，App 自己导入）"""

    def __init__(self, c, dev_id, bid, work, udid=""):
        sys.path.insert(0, str(Path(c.engine) / "tools"))
        import push_pack as P
        self.P, self.work = P, Path(work)
        self.dc = P.Devicectl(dev_id, bid, work, udid=udid or None, env=dict(c.env(), S2A_PUSH_TV="1"))   # 中断提示按电视说（#1696）

    def put(self, src, rel):
        try:
            self.dc.put(src, rel)
        except self.P.Fail as e:
            raise Failure(str(e).replace("iPhone", KIND).replace("手机", "电视"), code="resource_transfer_failed") from e  # 非玩家句（换字；原句是 push_pack 的玩家句）


def push_programs(c, dev_id, bid, games, rec, team, udid, work, sid):
    """各款程序包（tvOS 版，program_pack.sign(platform="tvos")，程序件取 image/tvos/kit）按设备记录判要不要推、要推的打成 .s2arm 推进电视上 App 的导入文件夹
    （install.push_program，同 iPhone）。→ {游戏号: program 记录}。这款（games[0]）的程序件放不进本版壳 / 出不成照抛；
    别的款放不进（旧版出的）/ 出不成记黄字、这次不带（它们的资源包与存档还在电视上，重新出 Apple TV 包再装就回来）"""
    import program_pack
    import ipa as packer
    signer, cl = tv_signer(c), TvClient(c, dev_id, bid, work, udid)
    looks = packer.container_custom(c)
    out, dropped = {}, []
    names = {g["id"].upper(): g.get("name") or g["id"] for g in c.catalog()["games"]}
    for i, g in enumerate(games):
        old = (rec.get("games", {}).get(g) or {}).get("program")
        kit = usable_kit(c, g, sid)
        if kit is None and i == 0:
            raise Failure("这款的 Apple TV 版不是这一版 S2Arm 出的（或还没出）：先出 Apple TV 包，再装到 Apple TV。", code="container_stale")
        try:
            if kit is None:
                raise Failure("", code="container_stale")
            pkg = program_pack.sign(c, g, team, bid, signer, platform=PLATFORM, kit=kit)
        except Failure:
            if i == 0:
                raise
            dropped.append(names.get(g, g))
            continue
        custom = next((v for k, v in looks.items() if k.upper() == g.upper()), None)
        want = dict(json.loads((Path(pkg) / "catalogue.json").read_text(encoding="utf-8")), look=install.look_of(custom))
        if not program_pack.need(c, g, old, team, want=want):
            out[g] = old
            continue
        out[g] = install.push_program(cl, g, pkg, install.import_inbox(cl, TV_DOCS), custom=custom)   # 一个 .s2arm 进电视上 App 的导入文件夹，App 自己导入（同 iPhone）
    if dropped:
        c.diagnostic(S_DROPPED.replace("{0}", "「" + "」「".join(dropped) + "」"), code="container_stale", severity="warning")
    return out


def install_tv(c):
    """作业 tvInstall（0.6.0 起 Apple TV 也走通用壳）：电视上的 S2Arm 只装一次壳（本版、同包名、签名还新鲜就不重装），
    这款的程序包与资源包推进电视；电视上已有的别的款缺程序包 / 程序包旧了一并补推（0.5.x 整包装过的老款、换了壳的）。见本文件开头"""
    import ipa as packer
    gid = (c.r.get("gameId") or "").upper()
    pack = require_pack(c, gid)
    c.begin("找签名账号与 Apple TV", 1, 6)
    team = install.pick_team(install.xcode_teams(c.env()), c.r.get("teamId"))
    tid = team["id"]
    dev = pick(c, (c.r.get("device") or "").strip())
    key, name, udid = tv_key(dev), tv_name(dev), dev.get("hardwareProperties", {}).get("udid") or ""
    bid = tv_bundle_id(tid)
    rec = load_rec(c, key)
    shell, sid = tv_shell(c)
    moved = rec.get("bundleId") not in (None, "", bid)
    if moved:   # 换了团队 = 换包名 = 电视上另一个 App：原来那个 App 里的各款这里都没有，按没推过算（原来的 App 和存档留在电视上不动）
        rec = {k: v for k, v in rec.items() if k != "games"}
    in_library = {g["id"].upper() for g in c.catalog()["games"] if not g.get("hidden")}
    games = [gid] + sorted(g for g in (rec.get("games") or {}) if g != gid and g in in_library)
    kits = {g: usable_kit(c, g, sid) for g in games}
    if kits[gid] is None:
        raise Failure("这款的 Apple TV 版不是这一版 S2Arm 出的（或还没出）：先出 Apple TV 包，再装到 Apple TV。", code="container_stale")
    npdms = [Path(k) / "main.npdm" for k in kits.values() if k is not None]
    eva = any(is_switch2(n) for n in npdms)
    installed = False
    if not shell_fresh(c, rec, bid, sid, eva):
        c.begin("组 Apple TV 上的 S2Arm（不带游戏）", 2, 6)
        out, app = assemble_shell(c, shell)
        log = c.logs / (c.task + "-install.log")
        try:
            import plistlib
            info = app / "Info.plist"
            pl = plistlib.loads(info.read_bytes()); pl["CFBundleIdentifier"] = bid
            info.write_bytes(plistlib.dumps(pl))
            sign(c, app, team, bid, udid, log, npdms)
            c.begin("装到 Apple TV", 5, 6)
            if rec.get("bundleId") == bid:   # 覆盖装前把电视上各款的存档拉回 Mac（装 App 本身不动数据容器，备份是防万一）；备份不了只记黄字、照装
                for g in sorted(rec.get("games") or {}):
                    try:
                        backup_game(c, dev["identifier"], bid, g)
                    except Failure as e:
                        c.diagnostic(f"装机前没能备份 Apple TV 上这款的存档（{e}），照常装了；装 App 不会动电视上的存档。", code="save_backup", severity="warning")  # 非玩家句
            install_app(c, dev["identifier"], app)
        finally:
            shutil.rmtree(out, ignore_errors=True)
        installed = True
        rec = dict(rec, shellId=sid, bundleId=bid, eva=eva, signed=dict(at=time.time(), free=bool(team.get("free")), device=name))
        save_rec(c, key, dict(rec, at=time.time()))
    c.begin("把游戏的程序推进 Apple TV", 6, 6)
    progs = {}
    with packer.program_locks(c, games), tempfile.TemporaryDirectory(dir=c.private) as w:
        try:
            progs = push_programs(c, dev["identifier"], bid, games, rec, tid, udid, w, sid)
        finally:   # 推到一半失败：已推好的记下，没推的下次照推
            had = rec.get("games") or {}
            save_rec(c, key, dict(rec, games={g: dict(had.get(g) or {}, **({"program": progs[g]} if g in progs else {})) for g in set(had) | set(progs)},
                                  at=time.time()))
    c.begin("把游戏资源推进 Apple TV（只推变了的文件；第一次大游戏要几分钟）", 6, 6)
    # 这款照推（push_pack 按内容只补缺的 / 变了的）；别的款资源包号变了（重新出过 Apple TV 包）也补推，同 0.5.x 整包装机
    stale = [gid] + [g for g in progs if g != gid and (tv_pack(c, g) / "manifest.json").is_file()
                     and install.pack_id(tv_pack(c, g)) != ((rec.get("games") or {}).get(g) or {}).get("pack")]
    files = size = 0
    for g in stale:
        p = pack if g == gid else tv_pack(c, g)
        f, n = push(c, dev["identifier"], bid, g, p, udid)
        verify(c, dev["identifier"], bid, g, p)
        files += f; size += n
        rec = load_rec(c, key)
        save_rec(c, key, dict(rec, games=dict(rec.get("games") or {}, **{g: dict((rec.get("games") or {}).get(g) or {}, pack=install.pack_id(p))}), at=time.time()))
    games = sorted(g for g in (load_rec(c, key).get("games") or {}) if g in progs)
    return dict(outputPath="", bundleId=bid, games=games, packFiles=files, packBytes=size, device=name, deviceName=name, deviceKind=KIND,
                team=team.get("name") or "", free=bool(team.get("free")), installed=installed, openToFinish=True)
