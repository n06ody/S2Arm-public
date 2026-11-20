"""装到 iPhone（10-26 维护者 定：Xcode 做主路、AltStore 备选）：打 iPhone 包出的是自签 IPA，iPhone 不认；
用玩家在 Xcode 里登录的 Apple ID（免费的个人团队就行）重签后，用数据线直接装上，再把资源包拷进去、逐文件核对。

  ① 团队：优先读 Xcode 团队缓存；账号存在但缓存缺失时核本机开发描述文件与可用签名身份。请求给了 teamId 就用它，否则优先付费团队；找不到不当作未登录
  ② 设备：连着线、已信任的那台 iPhone / iPad（恰好一台；请求可给 device 指定）
  ③ 包名：io.s2arm.games.<团队号小写>（所有游戏进同一个容器 App，主页亮点「一个 App 装下整个游戏库」，维护者 11-08）——包名在 Apple 那边按团队注册，别的团队用过的同名包名注册不了
  ④ pack/ensure_profile.sh（xcodebuild -allowProvisioningUpdates，带设备号时顺带登记这台设备）拿开发描述文件 → pack/dev_sign.py 重签（核「提高内存上限」）
  ⑤ devicectl 装 App → tools/push_pack.py 按内容增量推资源包（同 dev_run、Windows 一键装机）→ 起 App 只解包，机上 .applied 对上本包索引才算装好
10-26 本机实测（免费个人团队、测试设备2）：描述文件 27 秒、签名 7 秒、安装 3 秒。免费账号签的 App 7 天过期，到期连上再装一次就行（资源包已在机上的不重拷）。"""
from __future__ import annotations

import argparse
import base64
import contextlib
import json
import datetime
import errno
import hashlib
import io
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from core import Failure, atomic, contained, read
from errors import recovery
import devcert
import layout


def profile_teams(env=None):
    """从未过期开发描述文件及可用私钥身份发现团队；最终账号权限仍由 Xcode 签名验证。"""
    deadline = time.monotonic() + 8
    try:
        p = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"], capture_output=True, text=True, env=env, timeout=5)
        identities = set(re.findall(r"\b[0-9A-Fa-f]{40}\b", p.stdout.upper())) if p.returncode == 0 else set()
    except (OSError, subprocess.TimeoutExpired):
        return []
    if not identities:
        return []
    home = Path((env or os.environ).get("HOME", str(Path.home())))
    folders = (home / "Library/Developer/Xcode/UserData/Provisioning Profiles", home / "Library/MobileDevice/Provisioning Profiles")
    out, seen = [], set()
    now = datetime.datetime.now(datetime.timezone.utc)
    for folder in folders:
        for path in sorted(p for p in folder.glob("*.mobileprovision") if not layout.os_junk(p.name)):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return out
            try:
                p = subprocess.run(["security", "cms", "-D", "-i", str(path)], capture_output=True, env=env, timeout=min(2, remaining))
                if p.returncode:
                    continue
                profile = plistlib.loads(p.stdout)
                expiry = profile.get("ExpirationDate")
                if not isinstance(expiry, datetime.datetime) or expiry.replace(tzinfo=datetime.timezone.utc) <= now:
                    continue
                ent = profile.get("Entitlements", {})
                teams = profile.get("TeamIdentifier", [])
                if len(teams) != 1 or not isinstance(teams[0], str) or not re.fullmatch(r"[A-Z0-9]{10}", teams[0]):
                    continue
                tid = teams[0]
                if ent.get("get-task-allow") is not True or not profile.get("ProvisionedDevices") or ent.get("com.apple.developer.team-identifier") != tid:
                    continue
                if not any(hashlib.sha1(cert).hexdigest().upper() in identities for cert in profile.get("DeveloperCertificates", [])):
                    continue
                if tid not in seen:
                    seen.add(tid)
                    # 缺少个人团队标记时保守按免费处理，不据缺字段断言付费资格。
                    out.append(dict(id=tid, name=profile.get("TeamName") or "", free=profile.get("LocalProvision") is not False))
            except (OSError, subprocess.TimeoutExpired, ValueError, TypeError, AttributeError):
                continue
    return out


# 玩家话术（环境检查、装机清单、失败提示同一份）。Xcode 里加完 Apple ID，团队要等账户页把团队列表显示出来才写进缓存（11-08 MacBook Air 实走）：
#   判据是团队号真的出现，不是账号存在
ACCOUNT_NONE = ("还没有登录 Apple ID：打开 Xcode，点菜单 Xcode › Settings › Apple Accounts，点 + 添加你的 Apple ID（免费的就行），"
                "选中账号，等团队列表显示出来，再回来重新检查。装了多个 Xcode 也只要登一个，账号是共用的。")
TEAM_PENDING = ("Apple ID 已经登录，但这台 Mac 还没有签名团队：点「解决办法」打开现成的引导项目，在 Xcode 里选 Personal Team，"
                "连上手机点运行一次，回来会自动打勾。也可以先到 Xcode › Settings › Apple Accounts 看团队列表有没有显示。")


def _security_text(args, env):
    try:
        p = subprocess.run(["security", *args], capture_output=True, text=True, env=env, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return p.stdout if p.returncode == 0 else ""


def development_identities(env=None):
    """钥匙串里带私钥、能用的开发证书的 SHA-1。"""
    text = _security_text(["find-identity", "-v", "-p", "codesigning"], env)
    return {h.upper() for h in re.findall(r'\b([0-9A-Fa-f]{40})\s+"(?:Apple Development|iPhone Developer):', text)}


def keychain_teams(env=None):
    """钥匙串开发证书的 OU 就是团队号，不依赖 Xcode 偏好缓存。证书看不出是否付费，按免费处理（只影响到期提示）；最终权限由 Xcode 签名验证。"""
    identities = development_identities(env)
    if not identities:
        return []
    text = _security_text(["find-certificate", "-a", "-Z", "-p"], env)
    out, seen = [], set()
    for sha, pem in re.findall(r"SHA-1 hash: ([0-9A-Fa-f]{40})\s+(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)", text, re.S):
        if sha.upper() not in identities:
            continue
        try:
            subject = subprocess.run(["/usr/bin/openssl", "x509", "-noout", "-subject"], input=pem, capture_output=True, text=True, env=env, timeout=5).stdout
        except (OSError, subprocess.TimeoutExpired):
            continue
        match = re.search(r"\bOU\s*=\s*([A-Z0-9]{10})\b", subject)
        if match and match[1] not in seen:
            seen.add(match[1])
            out.append(dict(id=match[1], name="", free=True))
    return out


PREFS_UNREADABLE = "S2ArmPrefsUnreadable"


def _xcode_prefs(env):
    p = subprocess.run(["defaults", "export", "com.apple.dt.Xcode", "-"], capture_output=True, env=env, timeout=20)
    if p.returncode:
        return {}
    # 第三方 Xcode 扩展的快捷键键名可能带控制字符（如 ESC），defaults 原样导出，XML 不允许，解析会失败：先去掉（#144）
    text = re.sub(rb"[\x00-\x08\x0b\x0c\x0e-\x1f]", b"", p.stdout)
    try:
        return plistlib.loads(text)
    except Exception:
        return {PREFS_UNREADABLE: True}


def signing_summary(env=None):
    """给诊断用的签名现状：只有版本和数量，不含账号和团队名。"""
    if os.name == "nt":
        return ""
    try:
        prefs = _xcode_prefs(env)
        version = subprocess.run(["xcodebuild", "-version"], capture_output=True, text=True, env=env, timeout=20).stdout.splitlines()[:1]
    except (OSError, subprocess.TimeoutExpired):
        prefs, version = {}, []
    accounts = prefs.get("DVTDeveloperAccountManagerAppleIDLists") or {}
    count = sum(len(xs) for xs in accounts.values() if isinstance(xs, list)) if isinstance(accounts, dict) else 0
    return (f"{version[0] if version else 'Xcode 未知'}；账号 {count} 个；团队缓存键{'读不了偏好' if PREFS_UNREADABLE in prefs else '有' if 'IDEProvisioningTeamByIdentifier' in prefs else '无'}；"  # 非玩家句
            f"可用团队 {len(xcode_account(env)[1])} 个；开发证书 {len(development_identities(env))} 张")


def xcode_account(env=None):
    """(有没有登录 Apple ID, 团队列表)：依次看 Xcode 团队缓存、本机描述文件、钥匙串开发证书，定位不到才算没有。"""
    if os.name == "nt":   # Windows 没有 Xcode 账号（也没有 defaults 命令）：签名走 Impactor
        return False, []
    prefs = _xcode_prefs(env)
    out, seen = [], set()
    for teams in (prefs.get("IDEProvisioningTeamByIdentifier") or {}).values():
        for t in teams or []:
            tid = t.get("teamID")
            if tid and tid not in seen:
                seen.add(tid)
                out.append(dict(id=tid, name=t.get("teamName") or "", free=bool(t.get("isFreeProvisioningTeam"))))
    accounts = prefs.get("DVTDeveloperAccountManagerAppleIDLists") or {}
    logged_in = isinstance(accounts, dict) and any(isinstance(xs, list) and any(isinstance(a, dict) and a.get("identifier") for a in xs) for xs in accounts.values())
    return logged_in, out or (profile_teams(env) if logged_in else []) or keychain_teams(env)


SIGNING_PBXPROJ = """// !$*UTF8*$!
{ archiveVersion = 1; classes = {}; objectVersion = 56; objects = {
A1000000000000000000000A = {isa = PBXBuildFile; fileRef = A1000000000000000000000B; };
A1000000000000000000000B = {isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = App.swift; sourceTree = "<group>"; };
A1000000000000000000000C = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = S2ArmSigning.app; sourceTree = BUILT_PRODUCTS_DIR; };
A1000000000000000000000D = {isa = PBXGroup; children = (A1000000000000000000000B, A1000000000000000000000E); sourceTree = "<group>"; };
A1000000000000000000000E = {isa = PBXGroup; children = (A1000000000000000000000C); name = Products; sourceTree = "<group>"; };
A1000000000000000000000F = {isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (A1000000000000000000000A); runOnlyForDeploymentPostprocessing = 0; };
A10000000000000000000010 = {isa = PBXNativeTarget; buildConfigurationList = A10000000000000000000013; buildPhases = (A1000000000000000000000F); buildRules = (); dependencies = (); name = S2ArmSigning; productName = S2ArmSigning; productReference = A1000000000000000000000C; productType = "com.apple.product-type.application"; };
A10000000000000000000011 = {isa = PBXProject; buildConfigurationList = A10000000000000000000012; compatibilityVersion = "Xcode 14.0"; developmentRegion = en; hasScannedForEncodings = 0; knownRegions = (en, Base); mainGroup = A1000000000000000000000D; productRefGroup = A1000000000000000000000E; projectDirPath = ""; projectRoot = ""; targets = (A10000000000000000000010); };
A10000000000000000000012 = {isa = XCConfigurationList; buildConfigurations = (A10000000000000000000014); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
A10000000000000000000013 = {isa = XCConfigurationList; buildConfigurations = (A10000000000000000000015); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
A10000000000000000000014 = {isa = XCBuildConfiguration; buildSettings = {IPHONEOS_DEPLOYMENT_TARGET = 16.0; SDKROOT = iphoneos; SWIFT_VERSION = 5.0; ONLY_ACTIVE_ARCH = YES; }; name = Debug; };
A10000000000000000000015 = {isa = XCBuildConfiguration; buildSettings = {CODE_SIGN_STYLE = Automatic; GENERATE_INFOPLIST_FILE = YES; INFOPLIST_KEY_UILaunchScreen_Generation = YES; PRODUCT_BUNDLE_IDENTIFIER = "@BUNDLE@"; PRODUCT_NAME = "$(TARGET_NAME)"; TARGETED_DEVICE_FAMILY = "1,2"; }; name = Debug; };
}; rootObject = A10000000000000000000011; }
"""
SIGNING_APP = """import SwiftUI  # 非玩家句

@main struct S2ArmSigningApp: App {
    var body: some Scene { WindowGroup { Text("S2Arm 签名已就绪，可以回到电脑继续") } }
}
"""


def signing_project(c):
    """没有团队的玩家用的引导项目：放在私有目录，玩家在 Xcode 选 Personal Team、连手机运行一次，Xcode 就会生成证书和描述文件。"""
    root = c.private / "signing-helper"
    project = root / "S2ArmSigning.xcodeproj"
    project.mkdir(parents=True, exist_ok=True)
    bundle = "io.s2arm.signing.h" + hashlib.sha256(str(c.private).encode()).hexdigest()[:10]   # 免费账号的包名要全局唯一：按这台机的数据目录算，重开不变
    (project / "project.pbxproj").write_text(SIGNING_PBXPROJ.replace("@BUNDLE@", bundle), encoding="utf-8")
    (root / "App.swift").write_text(SIGNING_APP, encoding="utf-8")
    return dict(path=str(project))


def xcode_teams(env=None):
    return xcode_account(env)[1]


def no_team_failure(env=None):
    """没有可用团队时的失败：还没登录，和登录了但团队没同步，是两件事。"""
    return Failure(TEAM_PENDING, code="xcode_team_pending") if xcode_account(env)[0] else Failure(ACCOUNT_NONE, code="xcode_account_missing")


def pick_team(teams, want=None):
    if want:
        t = next((t for t in teams if t["id"] == want), None)
        if t:
            return t
        if not re.fullmatch(r"[A-Z0-9]{10}", want):
            raise Failure("Team ID 要是 10 位大写字母或数字，可以在 Xcode 的账户页或 Apple 开发者网站查到。", code="xcode_account_missing")
        return dict(id=want, name="", free=True)   # 玩家手填的：本机查不到也试，能不能用由 Xcode 签名给真实结果
    if not teams:
        raise no_team_failure()
    paid = [t for t in teams if not t["free"]]
    return (paid or teams)[0]


# 多款容器（启动器线 11-07，一个 App 装多款游戏；组装见 pack/ship_ext.py container、pack/jobs/ipa.py container）：
#   包名固定 io.s2arm.games.<团队号小写>：本来就按团队分（不会被别的团队占），不用 NX2Apple 那样「缺省包名被占 → 团队变体 → 落盘沿用」；
#     加款 / 换款都是同一个包名覆盖安装（不卸载，Documents 保留），一个团队只占一个 App ID（免费账号 7 天 10 个的额度）。换团队 = 换包名 = 新沙盒，旧存档要迁
#   设备侧每款文档根 Documents/games/<游戏号>（存档 / 直连见过的键 / unpack.log 在它下面），资源包根 Library/Application Support/S2Arm/games/<游戏号>/资源包
#     （11-11 挪出 Documents：几万个文件让「文件」App 与文件选择器「最近项目」一直「正在加载」；旧位置 Documents/games/<游戏号>/资源包 过渡期还认，见 pack_listing），
#   起这一款的启动上下文 @bundle/Games/<游戏号>/launch.ctx——与 ship_ext 容器布局同口径


def container_bundle_id(team, c=None):
    """S2A_CONTAINER_SUFFIX（开发用：同一台手机上同时装几份测试包）：包名加后缀 io.s2arm.games.<团队>.<后缀>，几份并存互不覆盖；
    只收小写字母 / 数字 / 连字符，不设时与玩家完全一样。付费团队没有免费账号每周 10 个包名的限制。
    给了 c 且这个团队的包名曾被别处占用、自动换过（bundle_switched）→ 用换过的那个，之后每次装机、推资源包都对同一个 App"""
    suffix = re.sub(r"[^a-z0-9-]", "", os.environ.get("S2A_CONTAINER_SUFFIX", "").lower())[:40]
    base = f"io.s2arm.games.{team.lower()}" + (f".{suffix}" if suffix else "")
    return (read(_bundle_switch_file(c), {}).get(team.lower()) or base) if c is not None else base


# 包名被别处占用（Apple 9401：玩家先用自己的签名工具签 S2Arm 时沿用了 io.s2arm.games.<团队> 这个包名，它就注册到了别的 Apple ID 下；
#   之后 Mac 一键装按同一个包名向 Xcode 账号注册被拒——玩家改了签名里的包名才装上，11-11 反馈）：自动换成 <包名>.s2 再签一次，记在私有目录
ALT_BUNDLE_SUFFIX = ".s2"


def _bundle_switch_file(c):
    return c.private / "bundle-switch.json"


def bundle_switched(c, team, bid):
    m = read(_bundle_switch_file(c), {})
    atomic(_bundle_switch_file(c), dict(m if isinstance(m, dict) else {}, **{team.lower(): bid}))


def container_display_name(app):
    """S2A_CONTAINER_NAME（开发用，与 S2A_CONTAINER_SUFFIX 配套）：主屏显示名，连各语言 InfoPlist.strings 里的一起换；不设时不动"""
    name = os.environ.get("S2A_CONTAINER_NAME", "").strip()
    if not name:
        return None
    for strings in Path(app).glob("*.lproj/InfoPlist.strings"):
        text = strings.read_text(encoding="utf-8", errors="replace")
        strings.write_text(re.sub(r'"CFBundleDisplayName"\s*=\s*".*?";', '"CFBundleDisplayName" = "%s";' % name.replace('"', ''), text), encoding="utf-8")
    return name




def sgrade_pending(c, games):
    """这几款里有没有要补 S 级优化 / 按新版改写器重做的（rewrite_refresh.pending_ios）；取不到改写器指纹 = 没法判，按「没有」（不因此重组容器）"""
    import rewrite_refresh
    try:
        rid = rewrite_refresh.rewriter_id(c)
    except (Failure, OSError, subprocess.SubprocessError):
        return False
    if any(rewrite_refresh.kept_todo(c, g, layout.ios_kit(c, g)) for g in games if g):   # 运行时留下、还没进 iPhone 件的 NRO（#8be819）：要重组容器补上
        return True
    pend = rewrite_refresh.pending_ios(c, [g for g in games if g], rid, ignore_rewriter=True)   # 改写器指纹单独变了不因此重组容器（11-17 维护者定）
    # 这一版记号下已经试过、缺原件补不上的不算（不然每次「更新 iPhone」都白重装一遍）
    if any((layout.game_info(c, g).get("ios") or {}).get("sgradeMissing") != (rewrite_refresh.ios_key(c, g, rid) or "") for g in pend):
        return True
    import sgrade_bf   # 老包直连库缺可读瓦片版着色器（_bf）：取不到原料、这一代已试过的不算
    return any(sgrade_bf.due(c, g) for g in games if g)

def patches_results(c):
    import patches
    return patches.results(c)

def game_docs(gid):
    return f"Documents/games/{gid.upper()}"


DOCS_GAMES = "Documents/games"
PACK_GAMES = "Library/Application Support/S2Arm/games"   # 每款资源包放 <PACK_GAMES>/<游戏号>/资源包（设备侧宿主同口径）
# 备用位置（v0.3.3）：iOS 27 玩家机上 Library/Application Support/S2Arm 整条归 root、App 写不进，当时的壳把资源包搬到 Library/S2Arm/games
#   （那段迁移 11-15 已撤，存量手机上可能还留着这个目录）；手机上有它就一律推它（宿主同一判据：s2a_docs.h s2a_pack_base_rel 看 Library/S2Arm/games 在不在）
PACK_GAMES_ALT = "Library/S2Arm/games"


def game_pack(gid, games=PACK_GAMES):
    """本款设备侧资源包根（新位置，不在 Documents 里；games = PACK_GAMES_ALT 时是备用位置）"""
    return f"{games}/{gid.upper()}/资源包"  # 非玩家句


def game_pack_old(gid):
    """本款资源包旧位置（11-11 之前的宿主放在文档根下）：过渡期只读、只在新位置不在时推它（App 下次启动整个挪到新位置）"""
    return f"{game_docs(gid)}/资源包"  # 非玩家句


def game_ctx(gid):
    return f"@bundle/Games/{gid.upper()}/launch.ctx"


S_NO_USB = "这台 Mac 的 USB 上没看到 iPhone / iPad：换一根能传数据的线（充电线不行）、换个接口直连 Mac，并解锁手机。"
S_NOT_LISTED = "这台 Mac 看到手机插着，但设备服务还没列出它：解锁手机，在弹出的框里点「信任」；还不行就拔掉重插一次。"
S_MANY_WIRED = "连着不止一台 iPhone / iPad：只留要装的那一台。"
S_NETWORK = "能装，但网络传资源包慢、容易断：插上数据线再装更稳。"
S_MANY_NETWORK = "同一网络上有多台配对过的 iPhone / iPad：用数据线连上要装的那一台。"
# 装到哪台由玩家选、作业照办，不替玩家换一台（维护者 11-11：续签时玩家选的 iPhone 拔了线，旁边同网的 iPad 被装上了）
S_DEVICE_REQUIRED = "没说要装到哪台设备：在装机页选好 iPhone / iPad 再装。"
S_OFFLINE = "{0}「{1}」现在不在线：插上数据线，或让它和这台电脑连同一个 Wi-Fi 并解锁，再试一次。"
S_PICK_LISTED = "要装的那台（{0}）这次没列出来；这台电脑现在看得到：{1}——在装机页选其中一台再装。"
S_TRUST = "在 iPhone 上点「信任」，再输入手机的锁屏密码。"
S_DEV_MODE = "在 iPhone 上打开「设置 → 隐私与安全性 → 开发者模式」，按提示重启手机；重启后解锁，这里会自动打勾。"
S_TUNNEL = "和手机建立开发连接失败：解锁手机后再试；走无线的话换成数据线。"
S_DDI = "手机的开发者服务没准备好：解锁手机后再试；还不行就重启手机。"
PREPARE_COOLDOWN = 30   # 清单几秒问一次：配对框、更新映像这类有动静的步骤隔一会儿才重试


def _link(d):
    return d.get("connectionProperties", {})


def _mask(value):
    return f"{value[:4]}…{value[-2:]}" if len(value or "") > 8 else "…"


def _clean(text):
    """诊断里的 devicectl 原文：设备序列号、UUID 不进诊断包"""
    return re.sub(r"\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4,16}(?:-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})?\b", "…", str(text or ""))


def _with_raw(sentence, raw):
    """定句走翻译，原文另起一行照原样显示"""
    tail = _clean(raw).strip().splitlines()
    return sentence + ("\n" + tail[-1] if tail else "")


def device_hint(want):
    """上屏时怎么称呼玩家指定的那台：设备名照写；设备号只写末尾 6 位——整串会被诊断脱敏换成 [DEVICE_ID] 占位、玩家看不懂（11-11 真机验收测出）"""
    return f"编号末尾 {want[-6:]}" if re.fullmatch(r"[0-9A-Fa-f-]{20,}", want) else f"「{want}」"


def device_kind(d):
    """devicectl 设备 → 「iPad」或「iPhone」（文案里写实际的设备类型，不写死 iPhone）；Apple Vision Pro 按 iPad 兼容装，写 iPad"""
    from diag import is_vision
    h = d.get("hardwareProperties", {})
    return "iPad" if h.get("platform") == "iPadOS" or (h.get("productType") or "").startswith("iPad") or is_vision(d) else "iPhone"


def _pick_device(devices, want, install=False):
    """要装的那台：返回 (设备, 问题)。候选 = devicectl 列出的每台带硬件 UDID 的设备（diag.is_candidate）：
    不按 reality / 配对 / 开发者服务 / 连接方式这些元数据筛（11-13 维护者：「明明插着一台手机就是硬说识别不了」）——
    装得上装不上让 devicectl 装的那一步说了算，真失败报它的原文和下一步。
    给了 want（玩家选的那台）：列着就用它；没列着但只有一台候选就用那一台（完成页写的是实际装到的那台）；
    多台候选都对不上 → 列出看得到的让玩家选；一台都没有 → (None, "")，调用方按 USB 现状说。
    没给 want：只给装机前清单看（polling），有线优先挑出连着的那台供界面显示"""
    from diag import is_candidate
    phones = [d for d in devices if is_candidate(d)]
    if want:
        def named(d):
            return want in (d.get("identifier"), d.get("hardwareProperties", {}).get("udid"), (d.get("deviceProperties", {}).get("name") or "").strip())
        chosen = [d for d in phones if named(d)]
        if chosen:
            return chosen[0], ""
        if len(phones) == 1:
            return phones[0], ""
        if phones:
            seen = "、".join(f"{device_kind(d)}「{(d.get('deviceProperties', {}).get('name') or '').strip()}」" for d in phones)
            return None, S_PICK_LISTED.replace("{0}", device_hint(want)).replace("{1}", seen)
        return None, ""
    if install:   # 真装机没指定哪台：只有一台候选就用它；多台要玩家选；零台交调用方按 USB 现状说
        return (phones[0], "") if len(phones) == 1 else (None, S_DEVICE_REQUIRED if phones else "")
    live = [d for d in phones if _link(d).get("transportType") in ("wired", "localNetwork") or _link(d).get("tunnelState") == "connected"]
    wired = [d for d in live if _link(d).get("transportType") == "wired"]
    if len(wired) > 1:
        return None, S_MANY_WIRED
    if not wired and len(live) > 1:
        return None, S_MANY_NETWORK
    if wired or live:
        return (wired or live)[0], ""
    return None, ""


def _cooled(c, device, step, polling):
    """清单轮询时同一步隔 PREPARE_COOLDOWN 秒才再做一次；真正装机时总是做"""
    if not polling:
        return True
    path = c.private / "device-prepare.json"
    try:
        marks = json.loads(path.read_text())
    except (OSError, ValueError):
        marks = {}
    key = f"{device.get('identifier')}:{step}"
    if time.time() - marks.get(key, 0) < PREPARE_COOLDOWN:
        return False
    marks[key] = time.time()
    atomic(path, marks)
    return True


LINK_READS, LINK_RETRY_WAIT = 3, 3   # 真装机找设备：最多读 3 次、间隔 3 秒（单测置 0）


def device_link(c, want="", polling=False, install=False):
    """读设备并把它准备到能装（玩家不用打开 Xcode）：配对 → 开发者模式 → 拉起隧道和开发者服务 → 必要时更新开发者磁盘映像。
    返回 {device, stage, problem, trace}：stage 是没过的环节（connected / trusted / developerMode / prepare，全过为空）；polling = 装机前清单轮询。"""
    import target
    from diag import devicectl
    trace, limits = [], (30, 20, 120) if polling else (120, 60, 240)
    def note(step, rc, raw): trace.append(dict(step=step, rc=rc, raw=_clean(raw)[-1500:]))
    def say(text):
        if not polling: c.emit("progress", text)
    def read():
        devices, why, rc, raw = target.device_list(c, timeout=30 if polling else 60)
        note("list devices", rc, raw)
        return devices, why
    def step(name, *args, timeout):
        data = {}
        try:
            rc, data, raw = devicectl(c, *args, timeout=timeout)
        except subprocess.TimeoutExpired:
            rc, raw = "timeout", f"超过 {timeout} 秒"
        except OSError as e:
            rc, raw = "oserror", str(e)
        note(name, rc, raw)
        return rc, data, raw
    def out(device, stage, problem, devices=()):
        return dict(device=device, stage=stage, problem=problem, trace=trace, devices=list(devices))
    def again(dev):
        devices, why = read()
        fresh = next((d for d in devices if d.get("identifier") == dev.get("identifier")), None)
        return devices, why, fresh
    devices, why = read()
    dev, problem = (None, "") if why else _pick_device(devices, want, install)
    # 真装机时一次没读到 / 没看到要装的那台就再读：devicectl 列表会瞬时空、超时，或手机正从同网切到数据线（诊断#091bcf 同期玩家反馈
    #   「刚装上一款、下一次就说认不出」）。装机前清单轮询本来就每几秒再读，不在这里等
    for _ in range(LINK_READS - 1):
        if polling or not (why or problem or dev is None):
            break
        time.sleep(LINK_RETRY_WAIT)
        devices, why = read()
        dev, problem = (None, "") if why else _pick_device(devices, want, install)
    if why:
        return out(None, "connected", why)
    if problem:
        return out(None, "connected", problem, devices)
    if dev is None:
        usb = target.usb_idevices()
        note("usb", 0, "、".join(usb) or "没有 iPhone / iPad")  # 非玩家句
        return out(None, "connected", S_NOT_LISTED if usb else S_NO_USB, devices)
    # 下面每一环（配对 / 现查详情 / 开发者模式 / 开发者服务）真装机时只「去做」、不「拦」：做不成记进诊断轨迹，照样往下装，
    #   装的那一步失败时按 devicectl 原文归码给下一步（install.classify：未配对、开发者模式、锁屏…）。只有装机前清单轮询把它们当提示返回
    def hold(stage, problem):
        if polling:
            return out(dev, stage, problem, devices)
        note("继续装（不预先拦）", stage, problem)  # 非玩家句
        return None
    if _link(dev).get("pairingState") != "paired":
        raw = ""
        if _cooled(c, dev, "pair", polling):
            say("正在请手机信任这台 Mac")
            _, _, raw = step("manage pair", "manage", "pair", "--device", dev["identifier"], timeout=limits[0])
            devices, why, fresh = again(dev)
            dev = fresh or dev
        if _link(dev).get("pairingState") != "paired" and (r := hold("trusted", _with_raw(S_TRUST, raw))):
            return r
    def services_ready(d):
        return d.get("deviceProperties", {}).get("ddiServicesAvailable") is not False
    def refresh_details():
        nonlocal dev, devices
        rc, data, raw = step("device info details", "device", "info", "details", "--device", dev["identifier"], timeout=limits[1])
        fresh = data.get("result") if isinstance(data, dict) else None
        props = fresh.get("deviceProperties") if isinstance(fresh, dict) else None
        if rc != 0 or not isinstance(props, dict) or fresh.get("identifier") != dev["identifier"] or props.get("developerModeStatus") not in ("enabled", "disabled"):
            if _link(dev).get("transportType") != "wired":   # 走 Wi-Fi 的那台现查不通 = 它现在不在线（人走开 / 锁屏睡眠），如实说是哪台，原文进诊断
                return S_OFFLINE.replace("{0}", device_kind(dev)).replace("{1}", (dev.get("deviceProperties", {}).get("name") or "").strip())
            return _with_raw(S_TUNNEL, raw)
        # list devices 可能仍留着旧值；以指定设备的详情为准，同时更新诊断快照。
        dev = fresh
        devices = [fresh if d.get("identifier") == dev["identifier"] else d for d in devices]
        return ""
    # 真装机（非清单轮询）对选中的那台一律现查一次详情：设备列表里「同网」只说明配对过，不说明此刻连得上（11-11 玩家实报：
    #   没插线点续签，列表里那台标着同网，走到「读已装游戏」才报「找不到设备」）；现查不通就在开工前拦下
    if not polling or dev.get("deviceProperties", {}).get("developerModeStatus") not in ("enabled", None) or _link(dev).get("tunnelState") != "connected" or not services_ready(dev):
        say("正在和设备建立开发连接")
        problem = refresh_details()
        if problem and (r := hold("prepare", problem)):
            return r
    if dev.get("deviceProperties", {}).get("developerModeStatus") == "disabled" and (r := hold("developerMode", S_DEV_MODE)):
        return r
    if not services_ready(dev):
        raw = ""
        if _cooled(c, dev, "ddi", polling):
            say("正在准备手机的开发者服务")
            _, _, raw = step("manage ddis update", "manage", "ddis", "update", timeout=limits[2])
            problem = refresh_details()
            if problem and (r := hold("prepare", problem)):
                return r
            if dev.get("deviceProperties", {}).get("developerModeStatus") == "disabled" and (r := hold("developerMode", S_DEV_MODE)):
                return r
        if not services_ready(dev) and (r := hold("prepare", _with_raw(S_DDI, raw))):
            return r
    return out(dev, "", "", devices)


def _write_link_report(c, link):
    """失败时写进诊断包（日志目录 core.logs_root）：每步退出码和原文、每台设备的连接状态；序列号打码，不写设备名"""
    lines = ["# 设备连接诊断"]  # 非玩家句
    lines += [f"{t['step']}: rc={t['rc']} {t['raw']}" for t in link["trace"]]
    for d in link["devices"]:
        h, p, cp = d.get("hardwareProperties", {}), d.get("deviceProperties", {}), _link(d)
        lines.append(f"device {_mask(h.get('udid') or d.get('identifier') or '')} {h.get('productType')} reality={h.get('reality')} transport={cp.get('transportType')} pairing={cp.get('pairingState')} "
                     f"tunnel={cp.get('tunnelState')} boot={p.get('bootState')} developerMode={p.get('developerModeStatus')} ddi={p.get('ddiServicesAvailable')}")
    problem = link["problem"]
    for d in link["devices"]:   # 结论里列着「看得到的设备」时带了名字：报告不写设备名（同上）
        name = (d.get("deviceProperties", {}).get("name") or "").strip()
        if name:
            problem = problem.replace(f"「{name}」", "「<设备名>」")  # 非玩家句
    lines.append(f"结论: stage={link['stage']} {problem}")  # 非玩家句
    target = c.logs / f"{c.task}-device.log"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")


def phone(c):
    import target
    want = (c.r.get("device") or "").strip()   # 没给：只连着一台就用它（device_link → _pick_device install=True），多台才要玩家选
    link = device_link(c, want, install=True)
    if link["problem"]:
        _write_link_report(c, link)
        code = "device_required" if link["problem"] == S_DEVICE_REQUIRED else "device_not_connected" if link["stage"] == "connected" else "device_not_ready"
        raise Failure(link["problem"], code=code)
    return target._row(link["device"], c.env()["DEVELOPER_DIR"], with_link=True)


def udid_of(c, ident):
    from diag import devicectl
    rc, data, _ = devicectl(c, "list", "devices", timeout=60)
    for d in data.get("result", {}).get("devices", []) if rc == 0 else []:
        if d.get("identifier") == ident:
            return d.get("hardwareProperties", {}).get("udid") or ""
    return ""


def usb_mbps(udid, env=None):
    """这台 iPhone 现在的 USB 连接速率（Mb/s）：system_profiler 里设备序列号 = 去掉横杠的硬件 UDID；没插线 / 走无线 / 读不到 = 0"""
    if not udid:
        return 0
    try:
        p = subprocess.run(["system_profiler", "SPUSBHostDataType", "-json"], capture_output=True, text=True, env=env, timeout=60)
        data = json.loads(p.stdout or "{}")
    except Exception:
        return 0
    want = udid.replace("-", "").upper()
    def walk(o):
        if isinstance(o, dict):
            if str(o.get("USBDeviceKeySerialNumber", "")).replace("-", "").upper() == want:
                m = re.match(r"([\d.]+)\s*([MG])b/s", str(o.get("USBDeviceKeyLinkSpeed", "")))
                return int(float(m.group(1)) * (1000 if m.group(2) == "G" else 1)) if m else 0
            o = list(o.values())
        for v in o if isinstance(o, list) else ():
            r = walk(v)
            if r:
                return r
        return 0
    return walk(data)


# 申请描述文件 / 签名失败时 xcodebuild 的报错 → 玩家能照做的错误码
SIGN_ERRORS = (
    (r"等钥匙串授权超时", "keychain_prompt"),   # pack/ensure_profile.sh：第一次签名的钥匙串框没人点（11-08 Spare-host 实测）  # 非玩家句
    (r"Xcode 看不到这台 iPhone，账号上也还没登记它|has no devices from which to generate", "device_not_visible"),   # pack/ensure_profile.sh 兜底后核出描述文件不含这台 / 账号一台设备都没有（11-16）  # 非玩家句
    (r"needs to be unlocked|device is locked|Unlock the device", "device_locked"),
    # 电脑连不上手机的开发者通道（CoreDevice 4016：要的「服务已加载 / 可信远程连接 / 电源断言」一项都给不了，CurrentlyAssertableStates 为空）：
    #   开发者模式开着、也配过对，多见于手机走 Wi-Fi 连着时（#1578 D-43MF：transport=localNetwork、tunnel=disconnected、ddi=False，连续 3 次）。不是没解锁
    (r"CoreDeviceError error 4016|usage assertion requirements", "device_tunnel_unavailable"),   # 10-26 测试设备2 锁屏：xcodebuild 等不到目的设备超时
    # 免费 Apple ID 有两个不同的上限，原文措辞能分开，办法也不同：
    #   ① 包名（App ID）额度：签名前向 Apple 注册包名时被拒，Apple 错误 9120，Xcode 原文
    #      「Your maximum App ID limit has been reached. You may create up to 10 App IDs every 7 days.」——
    #      说的是「App ID」，按 7 天窗口计，删 App 不退额度 → free_appid_limit；
    #   ② 设备同时装 3 个自签 App：签名已过、装机（devicectl / 系统安装服务）时设备拒收，原文
    #      「This device has reached the maximum number of installed apps using a free developer profile」（iOS 27）、
    #      「The maximum number of apps for free development profiles has been reached」（较早 iOS / Xcode）——
    #      说的是「apps」装在这台设备上，删一个就能装 → free_app_limit。
    # 两条正则互不相交：①只认「App ID(s)」字样或带 error / code 前缀的 9120，②的「apps」后面不跟「ID」；
    # ①仍排在前面，万一一行里两种字样都有，按删 App 无效的那件事报，免得玩家白删。
    (r"App ID limit|\b10 App IDs\b|maximum number of App IDs|(?:error|code)\D{0,20}\b9120\b", "free_appid_limit"),
    (r"maximum number of (?:installed )?apps\b|(?:maximum|limit)[^\n]{0,100}free development profiles", "free_app_limit"),
    (r"Personal development teams[^\n]{0,160}do not support", "free_team_capability"),   # #897 免费个人团队申请了只有付费团队才有的权限（如扩展虚拟寻址）
    (r"(?:error|code)\D{0,20}\b9401\b|App ID with Identifier[^\n]{0,120}is not available", "app_id_taken"),   # 包名已被占用（Apple 9401）
    # 手机上已有同包名、别的签名装的 App（玩家自签装过 S2Arm）：系统安装服务拒绝覆盖（installation_proxy MismatchedApplicationIdentifierEntitlement）
    (r"MismatchedApplicationIdentifierEntitlement|application-identifier entitlement string[^\n]{0,240}does not match", "app_other_signer"),
    (r"Developer\s*Mode\s*(?:is\s*)?(?:disabled|not enabled|required)|DeveloperModeDisabled", "developer_mode_required"),
    (r"pairing (?:record|relationship)[^\n]{0,80}(?:invalid|missing|expired)|not paired|pair (?:the )?device again|InvalidHostID", "device_pairing_required"),
    (r"provisioning profile[^\n]{0,100}(?:has expired|is expired|expired on)|expired provisioning profile", "provisioning_expired"),
    (r"No Account for Team|No accounts|not logged in|Your session has expired", "xcode_account_missing"),
    (r"No signing certificate|No certificate for team|Signing certificate is invalid|doesn't include signing certificate", "signing_certificate_missing"),
)


TUNNEL_RETRY_S = 10   # 开发者通道没连上（4016）时再装一次之前等的秒数


def classify(text, default="signing_failed"):
    for pat, code in SIGN_ERRORS:
        if re.search(pat, text, re.I):
            return code
    return default


def ios_pack(c, gid):
    """这款的资源包（image/ios/.pack）；还没出 iPhone 版 / 没有资源包就不装、不报成功（审计 18:00 P1-58：原先照装、启动器写死「资源包也拷好了」，打开游戏才进缺资源页）"""
    if not layout.has_ios_kit(c, gid):
        raise Failure("这款游戏还没出 iPhone 包：先点「打 iPhone 包」。", code="job_failed")
    pack = layout.ios_pack(c, gid)
    if not (pack / "manifest.json").is_file():
        raise Failure(f"没找到这款的资源包（{pack}），没有装。请重新「打 iPhone 包」后再装。", code="pack_missing")
    return pack


def pack_id(pack):
    """资源包号：清单的 index_sha256；清单没写就按推送实际用的 index.tsv 现算（原先缺这个字段就整单不装，预检清单 #16：推送本来不读它）"""
    pid = read(Path(pack) / "manifest.json", {}).get("index_sha256") or ""
    if not pid:
        try:
            pid = hashlib.sha256((Path(pack) / "index.tsv").read_bytes()).hexdigest()
        except OSError:
            pid = ""
    return pid


PUSHING = "正在传输资源"   # 推送阶段的进度消息（定文案，两端前端按它本地化；数字走 done / total / unit）


def push_progress(c, done, total, step, steps):
    """推资源包的作业计数（Mac 解析 push_pack 的机器读行、Windows 直接回调，同一出口）：done / total 字节，unit = B；前端据此画百分比、速度、剩余时间。
    没有要推的（total = 0）不发计数，免得画出 0/0；传完（done = total）也只是「传完」，之后的机上解包另有「机上解包 k/n」计数，最终成功仍以解包确认为准"""
    if total > 0:
        c.emit(message=PUSHING, done=done, total=total, unit="B")


def push_stage(c, name, done=None, total=None):
    """推资源包之前的准备子阶段（Mac 解析 push_pack 的 S2A_PUSH_STAGE 行、Windows 直接回调，同一出口）：名字原样上屏，有字节就带计数"""
    if total:
        c.emit(message=name, done=done or 0, total=total, unit="B")
    else:
        c.emit("message", name)


def img_pack(pack):
    """单一 IMG 形态的资源包（manifest v3 format=img，pack/img.py convert）：传完并核过 game.img 长度即装好，不再起 App 等机上解包——
    机上要从 game.img 解出的几个小件由玩家打开 S2Arm 时自己完成（11-11 维护者：启动器到「完整传到手机并校验通过」就结束）"""
    try:
        return json.loads((Path(pack) / "manifest.json").read_text(encoding="utf-8")).get("format") == "img"
    except (OSError, ValueError, AttributeError):
        return False


def push_pack(c, ident, bid, pack, udid=None, docs=None, ctx=None, pack_root=None, tv=False):
    """资源包推进 App：调 Mac / Windows 共用的 tools/push_pack.py run（同 dev_run、Windows 一键装机、s2armctl push-pack，
    #206 / PR #470、11-06 两端合一）——按 sha256 只推机上没有 / 变了的文件（免费账号 7 天到期重装、改一点重出一版时只推几个文件），
    残留推送包按完整性处理、推前抽查机上实况，一批 ≤1 GB 流式写进 <资源包根>/.incoming/；传完并核过即装好，不起 App——机上解开由 App 第一次打开时自己做。
    多款容器：docs = 本款设备侧文档根 Documents/games/<游戏号>、pack_root = 本款设备侧资源包根（pick_pack_root 选的那处）、
    ctx = 本款启动上下文（有它即按多款容器拉起到游戏库）。tv = 推进 Apple TV（#1696）：不带 UDID（电视没有数据线，数据线流式通道每块都要先失败、
    等 3 秒重试才退到 devicectl），推包工具按电视说提示（S2A_PUSH_TV，不叫玩家插数据线）。
    返回（推的文件数, 字节数）"""
    import diag
    got = {}
    def line(text):
        m = re.match(r"S2A_PUSH_PROGRESS (\d+) (\d+) (\d+) (\d+)$", text.strip())   # tools/push_pack.py --progress-lines：每推完一批的已传 / 本次待传总字节（#266）
        if m:
            push_progress(c, *map(int, m.groups()))
            return
        m = re.match(r"S2A_PUSH_STAGE (.+?)\t(\d*)\t(\d*)$", text.strip("\r\n"))   # 推之前的准备子阶段（核对手机上的文件 / 检查手机空间 / 准备资源（拼镜像）按字节）
        if m:
            push_stage(c, m.group(1), int(m.group(2)) if m.group(2) else None, int(m.group(3)) if m.group(3) else None)
            return
        m = re.search(r"推 (\d+) 个文件 (\d+) MB", text)  # 非玩家句
        if m: got.update(files=int(m.group(1)), mb=int(m.group(2)))
        m = re.search(r"机上解包 (\d+)/(\d+)", text)   # tools/push_pack.py apply 每解完一包打一行：启动器显示「机上解包 13/17」  # 非玩家句
        if m: c.emit(message=f"机上解包 {m.group(1)}/{m.group(2)}", done=int(m.group(1)), total=int(m.group(2)), unit="包")  # 非玩家句
    env = dict(c.env(), S2A_DEVICECTL=str(diag.COREDEVICE / "Resources/bin/devicectl"))
    if c.r.get("acceptLowSpace") is True: env["S2A_PUSH_ACCEPT_LOW_SPACE"] = "1"   # 玩家在「手机空间看起来不够」那页点了「仍然继续」（device_space_low）
    if getattr(c, "trust_name", ""): env["S2A_TRUST_NAME"] = c.trust_name   # 推完拉不起 App、手机说没信任时，提示里写要点哪一项（tools/push_pack.py untrusted_why）
    if tv: env["S2A_PUSH_TV"] = "1"
    elif udid: env["S2A_PUSH_UDID"] = udid   # 有 UDID + App 内置 s2a-devpush（S2A_PREBUILT_TOOLS/devpush）→ 每批流式写手机，否则退回 devicectl（11-06）
    log = c.logs / (c.task + "-push" + (f"-{docs.rsplit('/', 1)[-1]}" if docs else "") + ".log")
    extra = (["--docs", docs] if docs else []) + (["--pack-root", pack_root] if pack_root else []) + (["--launch-ctx", ctx] if ctx else [])
    try:
        # 传完并核过就算装好，一律不由电脑起 App 解开（11-13 维护者：「把 App 装上以后直接把包传过去放在位置上不就行了吗，玩家自己在手机上再去点信任」）：
        #   解开由 App 第一次打开时自己做（多款容器的游戏库、单一 IMG、下游单款 App 的壳同样）。原先不带上下文的单款 App（下游壳）推完要拉起 App 解包，
        #   免费账号第一次装还没信任开发者时拉不起，作业打了红却报完成（假绿）；完成页本来就写「第一次打开前信任你的 Apple ID」（install.done.trust）
        c.run([sys.executable, "-B", c.engine / "tools/push_pack.py", "run", ident, bid, pack, "--via", "devicectl", "--progress-lines", *extra], log, env=env, on_line=line)
    except Failure as e:
        if e.code != "tool_exit":
            raise
        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        m = APPLY_FAILED.search(text)   # tools/push_pack.py apply：App 自己写的解开失败原因（如 #569 目录写不进），原样上屏，不说「推不进」「没有开始」
        if m:
            raise Failure(f"资源包已传到 iPhone，但手机上解开失败：{m.group(1)}", code="job_failed") from e
        m = re.search(r"资源包推进去了，但(机上解包 \d+ 秒没有新进展[^：\n]*|起 App 后 \d+ 秒机上解包没有开始)", text)   # tools/push_pack.py apply：按进度判停住（11-07）  # 非玩家句
        if m:   # 旧口径的「停住」（推包工具现在报成下面的「还差最后一步」）：同样按传输成功 + 提醒（装机原则 11-11）
            c.diagnostic(f"资源包已传完，但{m.group(1)}：请打开这款游戏，保持在前台等它导入完，会自动进入游戏。", code="manual_unpack_required", severity="warning")
            return got.get("files", 0), got.get("mb", 0) << 20
        m = re.search(r"资源包已传完，还差最后一步：[^\n]*", text)   # tools/push_pack.py NeedsPlayer：电脑拉不起 App，传输本身成功（#192）  # 非玩家句
        if m:
            c.diagnostic(m.group(0), code="manual_unpack_required", severity="warning")
            return got.get("files", 0), got.get("mb", 0) << 20
        m = re.search(r"^S2A_PUSH_CODE (device_space_low|ios_install_no_space|ios_install_dir_unwritable)\t([^\n]+)", text, re.M)   # tools/push_pack.py：估算空间不够（玩家可确认继续）/ 真写满了 / 机上目录 App 写不进（删除重装）
        if m:
            raise Failure(m.group(2), code=m.group(1)) from e
        m = re.search(r"iPhone 写不进资源包：[^\n]*", text)   # tools/push_pack.py probe：推之前写探针失败、手机确实锁着（#333）  # 非玩家句
        if m:
            raise Failure(m.group(0), code="device_locked") from e
        m = re.search(r"iPhone 写不进资源包（[^\n]*", text)   # probe：写不进但手机没锁 / 判不了 = 连接问题（带原文），不冒充锁屏（11-13）  # 非玩家句
        if m:
            raise Failure(m.group(0), code="resource_transfer_failed") from e
        from errors import tool_reason
        raise Failure("资源包推不进 iPhone：" + tool_reason(log), code="resource_transfer_failed") from e  # 非玩家句
    return got.get("files", 0), got.get("mb", 0) << 20


def push_cheats(c, device, bid, gid, docs, mkdir=None):
    """金手指（11-12）：这款文件夹 cheats/ 里的 txt（出包时从公开库取的 + 玩家导入的）拷到机上本款文档根 cheats/，
    游戏内金手指菜单（ios/vpad_cheats.m）与服务层 Cheats.cs 读那里；改代码的条目出包时已编进包。开关 enabled.txt / 状态 status.txt 不推（不盖玩家在手机上的选择）。
    拷不进只记黄、不挡装机"""
    import cheats
    d = cheats.cheats_dir(c, gid.upper(), create=False)
    files = [f for f in layout.entries(d, "*.txt") if f.name not in cheats.SKIP]   # ._<名>.txt（exFAT 附属文件）不推进手机金手指目录
    if files and mkdir:   # devicectl 会代建缺的目录，iOS 26.x 上代建的归 root、App 写不进开关文件（a48655）：先以 App 身份（s2a-devpush）建好；建不成照旧拷
        mkdir(f"{docs}/cheats")
    bad = 0
    for f in files:
        r = subprocess.run(["xcrun", "devicectl", "device", "copy", "to", "--device", device, "--domain-type", "appDataContainer",
                            "--domain-identifier", bid, "--source", str(f), "--destination", f"{docs}/cheats/{f.name}"],
                           capture_output=True, text=True, timeout=120)
        bad += r.returncode != 0
    if files:
        print(f"[装机] 金手指文件 {len(files) - bad}/{len(files)} 份拷进手机{'（有拷不进的，游戏内菜单里会缺几份）' if bad else ''}", file=sys.stderr, flush=True)  # 非玩家句


def push_mods(c, ph, gid, had):
    """资源替换类 mod（11-15）：这款游戏文件夹 mods/<名字>/romfs 推进机上本款文档根 mods/（服务层 RomFsMods.cs 起局叠上）。
    had = 上次推过的 {名字: 内容记号}（设备记录 games.<款>.mods）：记号变了的整份重推、电脑上删了的机上也删；玩家在手机上自己放的别的 mod 不碰。
    推不进只记黄、不挡装机。→ 这次推完后机上的 {名字: 记号}（推失败的那份按没推算，下次再推）"""
    import mods
    home = mods.mods_dir(c, gid, create=False)
    local = {d.name: mods.fingerprint(d) for d in layout.entries(home) if d.is_dir() and not d.name.startswith(".")}
    if not local and not had:
        return {}
    now, base = dict(had), f"{game_docs(gid)}/{mods.DIR}"
    try:
        with tempfile.TemporaryDirectory(dir=c.private) as w:
            cl = ph.client(w)
            for n in [n for n in had if local.get(n) != had[n]]:
                ok, out = cl.rm(f"{base}/{n}")
                if not ok:
                    raise RuntimeError(f"删手机上的旧 mod「{n}」：{out.splitlines()[-1] if out else '失败'}")  # 非玩家句
                now.pop(n)
            for n, fp in local.items():
                if had.get(n) == fp:
                    continue
                files = [f for f in layout.entries(home / n, recursive=True) if f.is_file() and not f.is_symlink()]   # ._<名> 不推进 romfs 叠加层
                c.emit("message", f"正在把 mod「{n}」推进 iPhone（{len(files)} 个文件）")
                for f in files:
                    cl.put(str(f), f"{base}/{n}/{f.relative_to(home / n).as_posix()}")
                now[n] = fp
    except Exception as e:   # 推不进：已推好的记下，其余下次装机再推
        c.diagnostic(f"mod 没全推进 iPhone，这次游戏里会缺几份，下次「推到 iPhone」会补推：{str(e)[:200]}", code="resource_transfer_failed", severity="warning")  # 非玩家句
    return now


APPLY_FAILED = re.compile(r"资源包推进去了，但手机上解开失败：([^\n]+)")   # tools/push_pack.py apply 读到解包日志「解包失败：…」时报的那句  # 非玩家句


def pull_seen(c, ident, bid, gid, tag, docs="Documents"):
    """装之前把这台设备记下的见过的键（直连线 Documents/wire/<程序号大写>/pso_user.tsv）拉回游戏库 games/<程序号>/seen/<设备>.tsv，
    下次打包按它重算两段式热键（hotset.py）；第一次装 / 没开过直连 / 拉不到都不算错。设备上是追加写：拉回的是已有那份的续写就覆盖，
    否则（换包后机上重新记了）另存一份，多份由 hotset.rank 合并去重"""
    from diag import devicectl
    dest = layout.path(c, gid, "seen", re.sub(r"[^0-9A-Za-z-]", "", tag) + ".tsv")
    with tempfile.TemporaryDirectory(dir=c.private) as d:
        rc, _, _ = devicectl(c, "device", "copy", "from", "--device", ident, "--domain-type", "appDataContainer", "--domain-identifier", bid,
                             "--source", f"{docs}/wire/{gid.upper()}/pso_user.tsv", "--destination", f"{d}/s", timeout=120)
        got = Path(d, "s")
        if rc or not got.is_file():
            return False
        new = got.read_bytes()
        if dest.is_file():
            old = dest.read_bytes()
            if old.startswith(new):
                return False
            if not new.startswith(old):
                dest = dest.with_name(f"{dest.stem}-{time.strftime('%Y%m%d%H%M%S')}.tsv")
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(got, dest)
        return True


def app_running(c, ident, bid):
    """手机上这个包名的 App（S2Arm 容器）现在是不是在跑：进程可执行文件落在它的安装目录下 = 在跑。→ True / False（没装也算不在跑）/ None（查不清）。
    自动推送前查（#8be819，phone_kept_pull）：在玩就不推，推送装 App 会把这局杀掉"""
    from diag import devicectl
    rc, apps, _ = devicectl(c, "device", "info", "apps", "--device", ident, "--bundle-id", bid, timeout=60)
    if rc:
        return None
    urls = [a["url"].rstrip("/") + "/" for a in (apps.get("result") or {}).get("apps") or [] if isinstance(a, dict) and a.get("bundleIdentifier") == bid and a.get("url")]
    if not urls:
        return False
    rc, procs, _ = devicectl(c, "device", "info", "processes", "--device", ident, timeout=60)
    if rc:
        return None
    exe = [p.get("executable") or "" for p in (procs.get("result") or {}).get("runningProcesses") or [] if isinstance(p, dict)]
    # 只认主程序本身：<安装目录>/<CFBundleExecutable>。安装目录下还有系统常驻拉起的扩展进程（主屏小组件 PlugIns/S2ArmWidget.appex，11-16 进壳），
    #   和玩家在不在玩无关——原先「落在安装目录下」就算在跑，phone_kept_pull 一直回 running=true，自动补件 / 提示更新从不发起
    #   （11-16 真机实测：App 已退出，只剩小组件进程）。按主程序全路径比，以后再加扩展（Live Activity 等）也不会算进来
    main = main_executable(c)
    return any(e == u + main for e in exe for u in urls)


def main_executable(c):
    """装到手机上的容器 App 的主程序文件名：游戏库根 S2Arm.ipa（layout.phone_ipa，装机时组装的那份）里 Info.plist 的 CFBundleExecutable；
    读不到（还没装过 / 文件坏）→ 壳的缺省名 S2Arm"""
    import zipfile
    try:
        with zipfile.ZipFile(layout.phone_ipa(c)) as z:
            name = next(n for n in z.namelist() if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", n))
            exe = plistlib.loads(z.read(name)).get("CFBundleExecutable")
            if isinstance(exe, str) and exe and "/" not in exe:
                return exe
    except (OSError, StopIteration, ValueError, KeyError, zipfile.BadZipFile, plistlib.InvalidFileException):
        pass
    return "S2Arm"


def pull_kept_nros(c, ident, bid, gid, docs="Documents"):
    """装之前把这台设备上宿主留下的 NRO（runtime/src/loader.c s2a_nro_keep：包里没有改写映像、游戏解开的整份，Documents/wire/<号>/nro/<SHA-256>.nro）
    拉回游戏库 games/<号>/nro_kept/，下次准备这款时改写进包（#8be819）。机上没有 / 列不出 / 拉不到都不算错；只收文件名 = 内容 SHA-256 的。→ 新收的个数
    Mac：devicectl 列 / 拉，收文件这段在 keep_nros；Windows 不收（WinPhone.pull_kept 回 0）"""
    from diag import devicectl
    rc, data, err = devicectl(c, "device", "info", "files", "--device", ident, "--domain-type", "appDataContainer", "--domain-identifier", bid,
                            "--subdirectory", f"{docs}/wire/{gid.upper()}/nro", timeout=120)
    if rc:
        if not dir_missing(err):   # 目录不在 = 这款没留下 NRO（常态）；别的列不出要看得见，不然真问题被吞成「收 0 个」
            note(c, f"收回 {gid} 留下的 NRO：列不出手机上的目录（{(err or '').strip().splitlines()[-1:] or rc}），这次没收")  # 非玩家句
        return 0
    names = {p.rsplit("/", 1)[-1] for p in (f.get("relativePath") or f.get("path") for f in ((data.get("result") or {}).get("files") or [])
                                            if isinstance(f, dict) and not (f.get("resources") or {}).get("isDirectory")) if p}
    def fetch(name, dst):
        rc, _, _ = devicectl(c, "device", "copy", "from", "--device", ident, "--domain-type", "appDataContainer", "--domain-identifier", bid,
                             "--source", f"{docs}/wire/{gid.upper()}/nro/{name}", "--destination", dst, timeout=300)
        return rc == 0
    return keep_nros(c, gid, names, fetch)


def keep_nros(c, gid, names, fetch):
    """机上列出的名字里，文件名 = 内容 SHA-256 的 NRO 逐个拉到临时文件（fetch(名字, 本地路径) → 成功?）、核过摘要收进 games/<号>/nro_kept/；
    已收过的不再拉。→ 新收的个数"""
    import hashlib
    new = 0
    for name in sorted(set(names)):
        if not re.fullmatch(r"[0-9a-f]{64}\.nro", name):
            continue
        dest = layout.nro_kept(c, gid, create=True) / name
        if dest.is_file():
            continue
        with tempfile.TemporaryDirectory(dir=c.private) as d:
            got = Path(d, "n")
            if not fetch(name, str(got)) or not got.is_file():
                note(c, f"收回 {gid} 留下的 NRO {name}：拉不回来，这次没收")  # 非玩家句
                continue
            if hashlib.sha256(got.read_bytes()).hexdigest() != name[:64]:
                note(c, f"收回 {gid} 留下的 NRO {name}：内容摘要与文件名不符，丢掉")  # 非玩家句
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            part = dest.with_name(f"{dest.name}.{os.getpid()}.partial")   # 临时文件 + 改名：补件（rewrite_refresh.add_kept_nros）同时在读也读不到半个
            shutil.copyfile(got, part); os.replace(part, dest); new += 1
    if new:
        c.emit("message", f"这款游戏有 {new} 个代码模块要在运行中才能取到，已经从 iPhone 收回，这次装机一并补上")
    return new


SAVE_REL = "S2A/ryujinx/bis/user/save"   # 文档根下的存档目录（Ryujinx 数据根 = <文档根>/S2A/ryujinx）
SAVE_DIR = "Documents/" + SAVE_REL


def save_dir_absent(c, ident, bid, save_dir):
    """存档目录列不出时，判它是不是根本不在：从它的父目录往上逐级列到 Documents，第一个列得出的父目录里没有它（也没有它下面的东西）→ True；
    父目录里看得到它、或一个父目录都列不出（锁屏 / 隧道断）→ False（调用方停装，宁可不装也不冒丢档的险）"""
    from diag import devicectl
    parts = save_dir.split("/")
    for i in range(len(parts) - 1, 0, -1):
        parent, rest = "/".join(parts[:i]), "/".join(parts[i:])
        rc, data, _ = devicectl(c, "device", "info", "files", "--device", ident, "--domain-type", "appDataContainer", "--domain-identifier", bid,
                                "--subdirectory", parent, timeout=120)
        if rc:
            continue
        seen = [f.get("relativePath", "") for f in ((data.get("result") or {}).get("files") or [])]
        return not any(r == rest or r.startswith(rest + "/") or rest.startswith(r + "/") and r == rest.split("/")[0] for r in seen)
    return False

def note(c, text):
    """装机原则（11-11：玩家主动点了装机就别替他拒——认得出的风险让他确认，认不出的直接装）：拿不准、照样继续的判断只记进本任务的日志 <任务号>-decisions.log，界面不拦"""
    line = time.strftime("%H:%M:%S ") + text
    try:
        c.logs.mkdir(parents=True, exist_ok=True)
        with open(c.logs / f"{c.task}-decisions.log", "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except (OSError, AttributeError, TypeError):
        pass
    print("[装机] " + text, file=sys.stderr, flush=True)  # 非玩家句


def backup_failed(c, why):
    """覆盖装前存档备份不了（装机原则「认得出的风险让用户确认」）：没带 acceptNoBackup → 停下报 save_backup_failed，界面问「仍然继续 / 先不」；
    玩家确认后带 acceptNoBackup 重发 → 记一笔「未备份」照装（返回 None = 没有备份目录）"""
    if c.r.get("acceptNoBackup") is True:
        note(c, "覆盖装前没能备份手机上的存档，玩家已确认继续：" + why)  # 非玩家句
        return None
    raise Failure("这次装机前没能备份手机上的存档（" + why + "）。存档没备份，万一覆盖出问题可能丢档；确认仍然继续再点一次。",  # 非玩家句
                  code="save_backup_failed", internal=why)


# 手机锁着（devicectl：com.apple.dt.RemotePairingError 1016「…has not been unlocked recently」等）：读不了 App 数据容器，解锁后就能读。
#   维护者 11-17（玩家判断不了的我们替他判）：不先问「仍然继续 / 先不」，提示解锁、每几秒自动重试，等到了照常备份；等满 2 分钟还锁着才走原来的确认
LOCKED = re.compile(r"RemotePairingError.{0,40}\b1016\b|not been unlocked|device is locked|passcode.{0,20}locked", re.I | re.S)
UNLOCK_WAIT_S, UNLOCK_POLL_S = 120, 5   # 等满 2 分钟还锁着才退回确认框


def wait_unlocked(c, call):
    """call() → (rc, data, err)。锁屏才等：rc 非 0 且错误是锁屏 → 提示解锁、每 UNLOCK_POLL_S 秒重试，直到不再是锁屏错误或等满 UNLOCK_WAIT_S；返回最后一次结果"""
    rc, data, err = call()
    if not rc or not LOCKED.search(err or ""):
        return rc, data, err
    c.emit("message", "iPhone 锁着，读不了存档：请解锁手机，解锁后会自动继续")
    end = time.monotonic() + UNLOCK_WAIT_S
    while rc and LOCKED.search(err or "") and time.monotonic() < end:
        c.check()   # 玩家取消照常能停
        time.sleep(UNLOCK_POLL_S)
        rc, data, err = call()
    note(c, "存档备份等手机解锁：" + ("已解锁，继续" if not (rc and LOCKED.search(err or "")) else f"等了 {UNLOCK_WAIT_S} 秒仍锁着"))  # 非玩家句
    return rc, data, err


# 存档备份失败时先分清两种玩家能看懂、能处理的情况，不笼统报「确认不了 / 读不到」（维护者 11-17：玩家更新手机上的 S2Arm 时卡在备份）：
#   ① 这台 Mac 磁盘满（devicectl 结果写不下、拉回的存档写不下）→ 说清「Mac 空间不够」、腾出多少再试，仍可确认后照装；
#   ② 玩家开了云存档、这款在 iCloud 云存档文件夹里有快照 → 覆盖装 App 不碰 iCloud 里那份，如实说一句、照装（不是删检查：手机上的本地存档照常备份，只有备份不成时才看云端）。
#   云存档文件夹由 Mac 启动器随请求带（cloudSavesFolder，玩家在环境页选的同一个；SaveSync CloudSaveFolder：<文件夹>/S2Arm Saves/<程序号大写>/head-<设备>.json）
BACKUP_FREE_GB = 1   # 磁盘满时请玩家至少腾出的空间：存档本身一般几 MB 到几十 MB，余量给 devicectl 结果文件与装机临时文件


def cloud_save_copy(c, gid):
    """这款在云存档文件夹里有快照 → 最新那份 head 的修改时刻（time.struct_time）；没开云存档 / 没有 / 读不了 → None。
    iCloud 还没下载到这台 Mac 的 head 是占位文件 .head-<设备>.json.icloud，同样算有"""
    folder = c.r.get("cloudSavesFolder")
    if not folder or not gid:
        return None
    d = Path(folder) / "S2Arm Saves" / gid.upper()
    try:
        heads = [p for p in d.iterdir() if (p.name.startswith("head-") and p.name.endswith(".json"))
                 or (p.name.startswith(".head-") and p.name.endswith(".json.icloud"))]
        return time.localtime(max(p.stat().st_mtime for p in heads)) if heads else None
    except (OSError, ValueError):
        return None


def mac_free_mb(c):
    try:
        return shutil.disk_usage(c.private).free >> 20
    except OSError:
        return -1


def save_backup_stuck(c, gid, why, nospace=False):
    """备份做不成时的出口：云端有这款的快照 → 记一笔照装（返回 None）；磁盘满 → 按「Mac 空间不够」问玩家；其余照旧 backup_failed"""
    at = cloud_save_copy(c, gid)
    if at:
        c.emit("message", "这款存档在 iCloud 云存档里有一份（{0}），覆盖装不会碰它：这次不另备份，照装".format(time.strftime("%Y-%m-%d %H:%M", at)))
        note(c, f"覆盖装前没能备份手机上的存档（{why}），云存档里有这款的快照，照装")  # 非玩家句
        return None
    if nospace:
        why = "这台 Mac 空间不够，存档备份放不下（只剩 {0} MB）：腾出 {1} GB 再试，或者确认后继续".format(mac_free_mb(c), BACKUP_FREE_GB)
    return backup_failed(c, why)


def backup_saves(c, ident, bid, gid, docs="Documents"):
    """覆盖装前把机上这款的整个存档目录（bis/user/save）逐文件拉回游戏库 games/<程序号>/save_backup/<时刻>/（10-27 运维线：
    斯普拉遁3 体感包覆盖装后进了选形象页，原档没有备份、无从比对）。第一次装 / 机上没有存档 = 不备份；拉失败报红停装，宁可不装也不冒丢档的险"""
    from diag import DEVICECTL_NOSPACE, devicectl
    save_dir = f"{docs}/{SAVE_REL}"
    rc, data, err = wait_unlocked(c, lambda: devicectl(c, "device", "info", "files", "--device", ident, "--domain-type", "appDataContainer",
                                                       "--domain-identifier", bid, "--subdirectory", save_dir, timeout=120))
    # 文件清单的路径字段（#908：玩家 Xcode 16.0 的 devicectl 有条目不带 relativePath，原先 f["relativePath"] 抛 KeyError、整个装机作业崩成
    #   「本地任务失败」，每次都装不上）：relativePath / path 都认；都没有的条目读不懂 → 走「备份不了存档、确认后照装」，条目字段名记进日志，不再崩
    entries = [f for f in ((data.get("result") or {}).get("files") or []) if isinstance(f, dict)]
    unknown = [sorted(f) for f in entries if not (f.get("relativePath") or f.get("path")) and not (f.get("resources") or {}).get("isDirectory")]   # 目录条目读不出路径不碍事
    files = [p for p in (f.get("relativePath") or f.get("path") for f in entries if not (f.get("resources") or {}).get("isDirectory"))
             if p and not p.rsplit("/", 1)[-1].startswith(".")]
    if not rc and unknown:
        print(f"[s2a] 存档文件清单有 {len(unknown)} 条读不出路径，字段：{unknown[0]}", file=sys.stderr)
        return save_backup_stuck(c, gid, f"读不懂 iPhone 返回的存档文件清单（{len(unknown)} 条没有路径字段，多半是 Xcode 版本不同）")
    if rc == DEVICECTL_NOSPACE:
        return save_backup_stuck(c, gid, err, nospace=True)
    if rc:
        # 列不出：App 不在机上（第一次装）才算没有存档；App 在却列不出要分清两种——devicectl 对「目录不存在」和「锁屏 / 隧道断」报同一个错
        #   （11-07 满屏线 Duo 实测：容器第一次装半途失败 / 从没进过游戏，机上 Documents/games 是空目录，原先每次都拒装、玩家永久卡死）。
        #   往上逐级列父目录，第一个列得出的父目录定：里面看不到存档目录 = 没有存档，照装；看得到却列不出 / 父目录一个都列不出 = 停装
        arc, apps, aerr = wait_unlocked(c, lambda: devicectl(c, "device", "info", "apps", "--device", ident, "--bundle-id", bid, timeout=60))
        if arc:   # 连「装没装过」都问不出（锁屏 / 超时 / 断开）：不能当成第一次装，否则跳过备份直接覆盖（#333）
            return save_backup_stuck(c, gid, "确认不了 iPhone 上有没有装过这款：" + (aerr.splitlines()[-1] if aerr else f"devicectl 退出 {arc}"), nospace=arc == DEVICECTL_NOSPACE)  # 非玩家句
        if any(a.get("bundleIdentifier") == bid for a in (apps.get("result") or {}).get("apps") or []):
            if not save_dir_absent(c, ident, bid, save_dir):
                return save_backup_stuck(c, gid, "读不到 iPhone 上这款的存档目录：" + (err.splitlines()[-1] if err else f"devicectl 退出 {rc}"))  # 非玩家句
        return None
    if not files:
        return None
    dest = layout.path(c, gid, "save_backup", time.strftime("%Y%m%d-%H%M%S"))
    for rel in files:
        try:
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            if e.errno != errno.ENOSPC:
                raise
            return save_backup_stuck(c, gid, str(e), nospace=True)
        rc, _, err = devicectl(c, "device", "copy", "from", "--device", ident, "--domain-type", "appDataContainer", "--domain-identifier", bid,
                               "--source", f"{save_dir}/{rel}", "--destination", str(dest / rel), timeout=120)
        if rc or not (dest / rel).is_file():
            return save_backup_stuck(c, gid, f"拉存档失败（{rel}）：" + (err.splitlines()[-1] if err else f"devicectl 退出 {rc}"),  # 非玩家句
                                     nospace=rc == DEVICECTL_NOSPACE)
    return dest


def phone_ipa_path(c):
    """「显示 IPA」用：装机时组装的那份 IPA（整个游戏库共用一份，在默认位置根 S2Arm.ipa）；还没装过、没有这个文件 → 空串（界面不给按钮）。
    原先给的是这款的 image/ios 目录，「显示 IPA」打开的是游戏目录"""
    ipa = layout.phone_ipa(c)
    return str(ipa) if ipa.is_file() else ""


def phone_img_path(c, gid):
    """给玩家拷到手机用的这款 .img：游戏库里资源包那份 game.img（两件套的大件，导出 .s2arm 时同盘硬链接的就是它；着色器库在 .s2arm 的 pack/ 里）。
    还没出 iPhone 版 → 空串（界面不给按钮）"""
    img = layout.ios_pack(c, gid) / "game.img" if gid else None
    return str(img) if img and img.is_file() else ""


def phone_shader_cache(c, gid):
    """导出可选附带的着色器缓存（加速首次进局，不带也能玩）：资源包里的 shaders/ 目录；没有 → 空串"""
    d = layout.ios_pack(c, gid) / "shaders" if gid else None
    return str(d) if d and d.is_dir() else ""


APPLE_DEVICES_URL = "https://developer.apple.com/account/resources/devices/list"   # 开发者网站的设备列表（登记设备）


def devcert_items(c, item, udid):
    """开发者证书模式的清单项（v0.2.4）：替掉 Apple ID / Impactor 两项。问题只提示、不挡（blocking 只给「没出包」）；
    到期 / 权限这类提醒随结果 devcert.warnings 给启动器画黄字。返回体检结果（读不出 → None）"""
    try:
        got = devcert.summary(c, udid=udid)
    except Failure as e:
        item("devcert", False, "开发者证书可用", str(e), action="devCertSettings")
        return None
    # 按钮落点（11-19：这两项原先不带 action，真实清单上证书项没按钮；落点契约测试用的 id 又是后端产生不了的，一直绿着）：
    #   证书本身的问题 → 设置页签名身份（导入 / 换证书）；设备不在描述文件里 → 苹果开发者网站的设备页（登记设备、重新生成描述文件）
    item("devcert", not got["blocking"], "开发者证书可用", got["blocking"][0]["text"] if got["blocking"] else "", action="devCertSettings")
    if udid and not got["allDevices"]:
        item("devcertDevice", got["deviceListed"], "这台 iPhone 在描述文件的设备列表里",
             "不在列表里：签出来装不上这台。请在开发者网站登记这台设备并重新生成描述文件，再在出包页重新导入。",
             url=APPLE_DEVICES_URL)
    return got


# 导出自签包不再是一种「装法」（11-15 维护者：让玩家先去设置里切模式本身就是多出来的一道）：游戏库「推到 iPhone」与「导出自签包」两个按钮并排，
#   导不导出只看这次请求带的 mode = export（wants_export）。原先存在私有目录 install.json 的装法设置作废：installMode 作业（两端启动时读签名身份）顺手删掉它，迁移一次
SIGN_CHOICE = "sign_mode.json"   # 玩家选的签名方式里「自己用别的工具签」（私有目录）；Apple ID / 自己的证书沿用开发者证书的「在用」开关（devcert state.json active）
SIGN_MODES = ("appleId", "devCert", "external")


def chosen_sign_mode(c):
    """玩家在设置里选的签名方式（11-17 维护者：登录过 Apple ID 的玩家也可能这次想自签，权利交给用户）：
    external = 自己用 AltStore 等工具签；devCert = 用自己的开发者证书；其余 appleId"""
    if (read(c.private / SIGN_CHOICE, {}) or {}).get("mode") == "external":
        return "external"
    return "devCert" if devcert.active(c) else "appleId"


def export_sign_mode(c):
    """这次导出按什么签：请求带的 sign（appleId / devCert / none，界面「这次换成…」就是带另一个 sign 再导一次）优先，只管这一次、不改设置；
    没带按设置里选的。none = 不签（同 external）"""
    want = {"none": "external"}.get(c.r.get("sign"), c.r.get("sign"))
    return want if want in SIGN_MODES else chosen_sign_mode(c)


EXPORT_UNSIGNED = "没读到登录的 Apple ID，这份游戏包未签名，导入后由手机上的 S2Arm 签名。"
EXPORT_UNSIGNED_NO_ID = "没读到登录的 Apple ID，这份游戏包未签名；手机上的 S2Arm 还没拿到签名身份，导入后签不了。用 AltStore 或 SideStore 装的 S2Arm：在它们的设置里导出签名证书，到手机上 S2Arm 齿轮 →「签名证书」→「导入 .p12…」导入。用启动器装的：在启动器里登录 Apple ID（或选用自己的证书），连上手机推一次游戏，把签名身份交给它。"


def any_phone_identity(c):
    """这台电脑交过签名身份的手机有没有（设备记录 identity，deliver_identity 写）：导出未签名的 .s2arm 时按它说导入后能不能签"""
    return any((rec or {}).get("identity") for _, rec in layout.devices(c))


def export_signer(c, mode):
    """导出 .s2arm 时签程序包要的 → (团队号, 包名, 签名器, 账号名)，同推到 iPhone 的签法（phone_signing）但不要手机：
    包名就是装 App 的那个 container_bundle_id(团队)；证书用户用证书的团队与描述文件的包名。做不到（没登录、证书缺件）照实报"""
    import program_pack
    if mode == "devCert":
        devcert.stored(c)   # 证书缺件 → devcert_missing
        t = devcert.team(c) or dict(id=devcert.state(c).get("teamId"), name=devcert.state(c).get("teamName") or "")
        return t["id"], devcert.state(c)["bundleId"], program_pack.devcert_signer(c), t.get("name") or t["id"]
    # Apple ID：Windows 与 Mac 都是装 App / 推游戏用的那个 plumesign 账号与签法（phone_signing）；证书用户这次换成 Apple ID 签同样走它
    if os.name != "nt":
        plume_home(c)
    W = win_install_mod(c)
    team = W.impactor.selected_team() if hasattr(W.impactor, "selected_team") else None
    if not team:
        if os.name != "nt":
            raise Failure(APPLE_SIGN_IN, code="apple_not_signed_in")
        raise Failure("读不到 Impactor 里登录的 Apple ID：在 Impactor 里登录后再导出，或者这次改成「自己用别的工具签」。", code="apple_not_signed_in")   # Windows 的下一步（WINDOWS_RECOVERY）：在 Impactor 里添加账号
    import ipa as packer
    exe = W.plumesign_path((W.impactor.bundled(c.r.get("resources")) or {}).get("plumesign"))
    win_cert_gate(c, W, exe)
    signer = win_selfsign_signer(c, W, exe, team)
    if signer is None:   # 同 phone_signing：没编 s2a-selfsign / 本机还没有这个团队的私钥，照旧 plumesign 签
        shell = Path(packer.container_shell(c, packer.current_shell_id(c))) / "root"
        signer = program_pack.win_signer(c, exe, shell)
    return team, container_bundle_id(team) if os.name == "nt" else container_bundle_id(team, c), signer, team


def sign_mode_set(c):
    """作业 signModeSet（设置里「签名方式」，请求 mode = appleId / devCert / external）：选了就记下，之后导出 .s2arm 照这个签。
    devCert 要先导入过证书（没有 → devcert_missing）；appleId / devCert 顺带切开发者证书的「在用」开关（同 devCert use）。→ {signMode}"""
    mode = c.r.get("mode")
    if mode not in SIGN_MODES:
        raise Failure("签名方式只能是 Apple ID、自己的证书或自己用别的工具签。", code="job_failed")  # 非玩家句（只有启动器传错参数才会到）
    if mode == "devCert":
        devcert.stored(c)
    if mode == "external":
        atomic(c.private / SIGN_CHOICE, dict(mode="external"))
    else:
        (c.private / SIGN_CHOICE).unlink(missing_ok=True)
        if devcert.state(c).get("certSha1") and devcert.active(c) != (mode == "devCert"):
            atomic(devcert.folder(c) / "state.json", dict(devcert.state(c), active=mode == "devCert"))
    return dict(signMode=chosen_sign_mode(c))


def install_mode_job(c):
    """作业 installMode：只读签名身份 → {signMode, exportAvailable}；旧版留下的装法设置 install.json 删掉（迁移一次）"""
    try:   # 删不掉（Windows 上被杀软 / 别的进程占着）不碍事：wants_export 已经不看它，下次启动再删；这个作业是启动时读签名身份的，不能因此失败
        (c.private / "install.json").unlink(missing_ok=True)
    except OSError:
        pass
    return dict(exportAvailable=True, signMode="devcert" if devcert.active(c) else "appleId", signChoice=chosen_sign_mode(c))


# 0.5.x「导出，自己签名安装」留在游戏库根的旧文件夹名（0.6.0 起不再产生）：只用于空间页把老玩家盘上还在的旧导出算进公共件（sizes）
EXPORT_DIRS = {"zh": "S2Arm 自签导出", "en": "S2Arm Self-Sign Export"}  # 非玩家句
_BAD_NAME = re.compile(r'[\\/:*?"<>|\x00-\x1f]')


def ui_lang(c):
    """玩家看得到的文件名 / 说明用哪种语言：中文界面（zh-Hans / zh-Hant、没带）用中文，其余用英文"""
    lang = c.r.get("uiLanguage") or "zh-Hans"   # 没带（旧客户端）照旧中文
    return "zh" if lang.startswith("zh") else "en"   # 中文界面（简 / 繁）用中文定名，其余语言用英文（11-14 加新语言：日 / 西 / 法界面不给中文文件夹）


def wants_export(c):
    """这次按导出走：请求带 mode = export（游戏库「导出自签包」、完成页「这次改为导出自签」、S 级优化升级页「自签导出」）；没带 = 一键装"""
    return c.r.get("mode") == "export"


S2ARM_SUFFIX = ".s2arm"
S2ARM_FORMAT = "s2arm"   # .s2arm 第一个条目 s2arm.json 的 format 字段（手机导入按它认文件）
# 游戏库根的固定文件夹（11-17 维护者定，单独一个、不放进 0.5.x 的「S2Arm 自签导出」：那里几 GB 的整包 0.6.0 起该删，放一起玩家分不清）。
#   只按第一次建的时候的界面语言选名字；库根已有其中一个就一直用它，换界面语言也不建第二个
S2ARM_DIRS = {"zh": "S2Arm 游戏包", "en": "S2Arm Game Packs"}  # 非玩家句


S2ARM_READMES = {"zh": "怎么导入.txt", "en": "How to import.txt"}  # 非玩家句
# 「S2Arm 游戏包」文件夹自己的说明（11-17 维护者定稿，只这三件事；「放这里导入」英文同 iOS 端 img.importFolder「Import Here」）
S2ARM_README_TEXT = {
    "zh": "点开 .s2arm 文件，在 S2Arm 里导入这款游戏。第一次导入这款时，还要把游戏库里的 game.img 放进 iPhone 上 S2Arm 的「放这里导入」文件夹"  # 非玩家句（说明文件按界面语言直接写，英文见下一项）
          "（启动器里「显示资源文件」能找到它）。.s2arm 删了，在启动器里再导出一次就有。\n",
    "en": "Open the .s2arm file to import the game into S2Arm. The first time you import a game, also put its game.img from the library into the "
          "“Import Here” folder of S2Arm on your iPhone (“Show resource file” in the launcher finds it). If you delete a .s2arm file, just export it again from the launcher.\n",
}


# 文件夹里导出过通用 S2Arm.ipa（export_shell_ipa）时，说明最前面加这一句；没导出就不出现（11-17 维护者定稿原话）
S2ARM_README_IPA = {
    "zh": "S2Arm.ipa：用你自己的签名工具（AltStore、SideStore 等）签好装到手机。只装一次，S2Arm 升级时再装一次。\n",  # 非玩家句（说明文件按界面语言直接写，英文见下一项）
    "en": "S2Arm.ipa: sign it with your own tool (AltStore, SideStore, etc.) and install it on your iPhone. You only need to do this once, and again when S2Arm updates.\n",
}


def write_s2arm_readme(folder, lang):
    """「S2Arm 游戏包」文件夹的说明：导出 .s2arm、导出通用 S2Arm.ipa 都按这一处写（谁后写都一样）；文件夹里有 S2Arm.ipa 才带 ipa 那句。→ 说明的路径"""
    readme = folder / S2ARM_READMES[lang]
    text = (S2ARM_README_IPA[lang] if (folder / "S2Arm.ipa").is_file() else "") + S2ARM_README_TEXT[lang]
    readme.write_text(text, encoding="utf-8-sig" if os.name == "nt" else "utf-8")
    return readme


def s2arm_dir(c):
    for name in S2ARM_DIRS.values():
        if (c.library / name).is_dir():
            return c.library / name
    return c.library / S2ARM_DIRS[ui_lang(c)]


def s2arm_name(c, gid):
    """<游戏名>.s2arm（同 _img_name 的取名）；游戏库里有别款同名时带上 [游戏号尾 4 位]，免得两款互相覆盖"""
    games = c.catalog()["games"]
    name = next((g.get("name") for g in games if g["id"].upper() == gid.upper()), "") or ""
    base = _BAD_NAME.sub(" ", name).strip().strip(".") or gid
    clash = sum(1 for g in games if (_BAD_NAME.sub(" ", g.get("name") or "").strip().strip(".") or g["id"]).lower() == base.lower()) > 1
    return (f"{base} [{gid[-4:]}]" if clash else base) + S2ARM_SUFFIX


def link_resource(src, dst):
    """两件套的资源本体（《文件与程序管理体系》B / L 项：资源本体不再写新文件）：游戏库里的 game.img 硬链接进「S2Arm 游戏包」，
    文件名同这款的 .s2arm（<名>.img，手机按文件头认游戏）——同一块盘上一个文件起两个名字，不占新空间、秒过，和 .s2arm 放在一起拿。
    已是同一个文件 → 不动；同名的是别的文件（上次链的那份 game.img 后来换过）→ 换成现在这份。建不了硬链接（exFAT / FAT 等）→ None，
    调用方照旧给游戏库里原文件的位置（界面「显示资源文件」）。→ 链接路径 | None"""
    try:
        if dst.exists() and os.path.samefile(src, dst):
            return dst
    except OSError:
        pass
    tmp = dst.with_name(f".{dst.name}.{os.getpid()}.link")
    try:
        tmp.unlink(missing_ok=True)
        os.link(src, tmp)
        os.replace(tmp, dst)
    except OSError as e:
        tmp.unlink(missing_ok=True)
        print(f"[s2a] 资源文件没能硬链接进游戏包文件夹（{type(e).__name__} {e.errno}），给游戏库里的原位置", file=sys.stderr, flush=True)  # 非玩家句
        return None
    return dst


def export_s2arm(c, gid):
    """0.6.0 通用壳（开关后面）两件套导出里的小游戏包 .s2arm（《文件与程序管理体系》B 项、⑦ ①，11-17 维护者定）：
    tar（只用 ustar、只有普通文件条目；路径放不进 ustar 就报错）整体压一个带内容校验的 zstd 帧；
    第一个条目 s2arm.json（格式、程序号、名字版本、约定号、改写器指纹、程序 sha、资源包号、bytes = 其余条目内容字节合计、signer），
    program/（同推送的程序包：dylib、Games/<号>/、catalogue.json，未签名，留给手机签），pack/（资源包目录里除 game.img 以外的派生件）。
    资源本体 game.img 不拷：同盘硬链接进同一文件夹（link_resource），建不了硬链接就给出它在游戏库里的原位置（界面「显示资源文件」）。放游戏库根的固定文件夹（s2arm_dir），
    不加锁不加权限，玩家删了下次导出重新生成；只换这一款自己的 .s2arm，同文件夹里别的东西不动。这一款的改写、出程序包都在这里前台按需做"""
    import hashlib, io as _io, tarfile
    import program_pack
    import ipa as packer
    from rewrite_refresh import refresh_ios
    gid = gid.upper()
    c.begin("准备这款的程序（按需更新）", 1, 3)
    pack = ios_pack(c, gid)
    with packer.program_locks(c, [gid]):
        refresh_ios(c, [gid])
        pkg = Path(program_pack.build(c, gid))
        meta = json.loads((pkg / program_pack.META).read_text(encoding="utf-8"))
        image = pkg / program_pack.image_name(gid)
        source_sha = program_pack.catalogue(gid, meta, image, None)["image_sha256"]
        mode = export_sign_mode(c)
        signed_with = None
        if mode != "external":   # 玩家选了用 Apple ID / 自己的证书签：导出前就签好，手机导入看清单 team 非空不再签（11-17 维护者定）
            try:
                team, bundle, signer, who = export_signer(c, mode)
            except Failure as e:
                # Apple ID 没登录不拦导出（11-19 维护者：导出 / 自签不加拦截）：照样导出未签名的（同「自己用别的工具签」），导入后由手机上的 S2Arm 用电脑交过去的身份签
                if mode != "appleId" or e.code != "apple_not_signed_in":
                    raise
                c.diagnostic(EXPORT_UNSIGNED if any_phone_identity(c) else EXPORT_UNSIGNED_NO_ID, code="apple_not_signed_in", severity="warning", internal=str(e))   # 下一步同没登录：想在电脑上签就去登录
                team = None
            if team:
                pkg = Path(program_pack.sign(c, gid, team, bundle, signer, pkg=pkg))
                image = pkg / program_pack.image_name(gid)
                signed_with = dict(mode=mode, name=who, team=team)
        cat = json.loads((pkg / program_pack.CATALOGUE).read_text(encoding="utf-8")) if signed_with else program_pack.catalogue(gid, meta, image, None)
        c.begin("打包 .s2arm", 2, 3)
        files = [("program/" + p.relative_to(pkg).as_posix(), p) for p in sorted(pkg.rglob("*"))
                 if p.is_file() and p.name not in (program_pack.META, program_pack.KEY, program_pack.CATALOGUE)]
        derived = [("pack/" + p.relative_to(pack).as_posix(), p) for p in sorted(Path(pack).rglob("*"))
                   if p.is_file() and p.relative_to(pack).as_posix() != "game.img"]
        manifest = read(Path(pack) / "manifest.json", {})
        cat_bytes = json.dumps(cat, ensure_ascii=False, indent=1).encode("utf-8")
        total = len(cat_bytes) + sum(p.stat().st_size for _, p in files + derived)   # 解压后其余条目内容字节合计（手机导入前看空间、显示进度）
        head = dict(format=S2ARM_FORMAT, schema=1, title_id=gid, name=cat["name"], display_version=cat["display_version"],
                    host_abi=cat["host_abi"], chain=cat["chain"], rewriter_id=cat["rewriter_id"], image=cat["image"],
                    image_sha256=cat["image_sha256"], source_sha256=source_sha, pack=(manifest or {}).get("index_sha256", ""),
                    bytes=total, signer=dict(mode=signed_with["mode"], team=signed_with["team"]) if signed_with else None)
        buf = _io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
            def add(name, data=None, path=None):
                info = tarfile.TarInfo(name)
                info.mode, info.mtime = 0o644, 0
                if path is not None:
                    info.size = path.stat().st_size
                    with open(path, "rb") as f:
                        t.addfile(info, f)
                else:
                    info.size = len(data)
                    t.addfile(info, _io.BytesIO(data))
            try:
                add("s2arm.json", json.dumps(head, ensure_ascii=False, indent=1).encode("utf-8"))
                add("program/catalogue.json", cat_bytes)
                for name, p in files + derived:
                    add(name, path=p)
            except ValueError as e:   # ustar 放不下的路径（name 100 + prefix 155）：不改用 pax / GNU 长名（手机只认 ustar）
                raise Failure(f"{gid} 有文件路径太长，打不进 .s2arm：{e}", code="job_failed") from e  # 非玩家句
    c.begin("写进「S2Arm 游戏包」文件夹", 3, 3)
    import programs
    data = programs._compress(buf.getvalue(), checksum=True)   # 带内容校验（与 iOS 导入侧对定）；核数只在 programs 里取（tools/pool_size_lint.py 白名单）
    folder = s2arm_dir(c)
    folder.mkdir(parents=True, exist_ok=True)   # 玩家删了文件夹也照样重建；不设特殊权限
    out = folder / s2arm_name(c, gid)
    tmp = folder / f".{out.name}.{os.getpid()}.partial"
    tmp.write_bytes(data)
    os.replace(tmp, out)
    lang = ui_lang(c)   # 这份文件夹的说明（玩家删了下次导出照样写回）
    write_s2arm_readme(folder, lang)
    game_img = Path(pack) / "game.img"
    linked = link_resource(game_img, out.with_suffix(".img")) if game_img.is_file() else None
    return dict(gameId=gid, s2armPath=str(out), s2armBytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                resourcePath=str(linked or game_img) if game_img.is_file() else "", resourceLinked=bool(linked), exported=True,
                signedWith=signed_with or dict(mode="none", name="", team=None))   # 完成页：「已用 Apple ID {name} 签好」/「未签名，导入后由手机签」


def _img_name(name, gid, used):
    """game.img 在导出文件夹里的名字：<游戏名>.img（玩家在「文件」App 里认得出）；名字不能做文件名 / 重名时带上游戏号。
    手机上按文件头认游戏（pack_apply.c s2a_img_scan 认 S2AIMG01 魔数与头里的游戏号），文件叫什么都行"""
    base = _BAD_NAME.sub(" ", name or "").strip().strip(".") or gid
    out = base + ".img"
    if out.lower() in used:
        out = f"{base} {gid}.img"
    used.add(out.lower())
    return out


EXPORT_STAMPS = "export-stamps.json"   # 私有目录：上次导出文件夹里每个 .img 的来源戳（不往玩家的导出文件夹里放杂文件）


UPDATE_SUFFIX = {"zh": " 着色器更新包.img", "en": " shader update.img"}  # 非玩家句


IMPORT_DIRS = ("放这里导入", "Import Here")   # 手机上 S2Arm 扫的导入文件夹（runtime/src/pack_apply.c s2a_img_import_dirs，同序）  # 非玩家句


def _img_head(path):
    """.img 开头：(游戏号, 头一页字节)；不是 S2AIMG01 → ("", b"")。一页 = pack/img.py PAGE（tools/push_pack.py IMG_PAGE）"""
    with open(path, "rb") as f:
        page = f.read(16384)
    return (page[16:32].decode("ascii", "replace").upper(), page) if page[:8] == b"S2AIMG01" else ("", b"")


EXPORT_SWAP_TRIES, EXPORT_SWAP_WAIT = 5, 2.0   # 换导出文件夹被占用时的退避：最多 5 次、每次等 2 秒（资源管理器刷新、杀毒扫完常在几秒内放手）


def export_shell_ipa(c):
    """「导出通用 S2Arm.ipa」：只有壳、不带任何一款游戏（ipa.assemble_shell，同「安装 S2Arm」装的那个），未签名，
    写成「S2Arm 游戏包」文件夹（s2arm_dir，和各款 .s2arm 同一处）里的 S2Arm.ipa，就地换掉上一份；同文件夹里别的东西不动。
    0.5.x 的「S2Arm 自签导出」文件夹不碰。不读游戏库、不出游戏的程序包，所以游戏多少都一样快。玩家关了主屏小组件就去掉扩展再交（同 export_phone）。
    结果字段同 export_phone（完成页照用），games / files 为空"""
    import ipa as packer
    c.begin("组 S2Arm（不带游戏）", 1, 2)
    work, tmp = Path(tempfile.mkdtemp(prefix=".export-", dir=c.library)), None
    try:
        ipa, _, sid = packer.assemble_shell(c, dest=work / "assemble")
        c.begin("写进「S2Arm 游戏包」文件夹", 2, 2)
        folder = s2arm_dir(c)
        folder.mkdir(parents=True, exist_ok=True)   # 玩家删了文件夹也照样重建
        out = folder / "S2Arm.ipa"
        tmp = folder / f".S2Arm.ipa.{os.getpid()}.partial"
        if not (c.r.get("widget") is False and ipa_strip_widget(ipa, tmp)):
            shutil.move(str(ipa), str(tmp))   # 临时目录和文件夹都在游戏库所在盘上：通常是改名
        os.replace(tmp, out)
    finally:
        shutil.rmtree(work, ignore_errors=True)
        if tmp is not None:
            tmp.unlink(missing_ok=True)   # 半路失败不留半个文件（换上去了就已不在）
    lang = ui_lang(c)
    readme = write_s2arm_readme(folder, lang)
    return dict(exported=True, outputPath=str(folder), ipa=str(out), readme=str(readme), shellId=sid, sgrade=patches_results(c),
                games=[], files=[], skipped=[], exportBytes=out.stat().st_size, fresh=[], reused=0, unwritten=[])


def readiness(c):
    """装机前清单（用户体验线 11-02）：启动器装机页每几秒问一次、AI 代劳也读它，逐项说「好了没有、没好要谁做什么」，只有 iPhone 版没做好才挡（blocking），其余只提示，真失败由 install 给原因。
    只读不改：不申请描述文件、不签名、不碰设备上的 App。每项 {id, ok, title, detail, who}；who = human（只能玩家本人做）/ app（启动器自己能做）。
    文案都是定句（启动器按原句查英文），账号名、设备名另放 account / device。
    设备一项一项判（连上 → 信任 → 开发者模式），前一项没好，后面的标 waiting、不给误导的「没打开」"""
    gid = c.r.get("gameId") or ""
    if os.name == "nt":
        return readiness_windows(c, gid)
    items = []
    ipa_path = ""
    def item(key, ok, title, detail, who="human", waiting=False, note="", action=None, url=None):
        wait = waiting and not ok
        # 等前一步的不给说明，免得 AI 拿去催玩家；好了的项也不带说明，只有 note（好了但值得知道的，如只靠网络连着）照带
        # action / url：没好的项的按钮落点（11-19 落点契约，唯一来源 launchers/shared/experience.v1.json install_landing；同 readiness_windows）；
        #   等前一步的也不带（那一步还轮不到玩家做，启动器不画按钮）
        items.append(dict(id=key, ok=bool(ok), waiting=wait, blocking=key == "package" and not ok, title=title, detail=note if ok else "" if wait else detail, who=who,
                          action=None if ok or wait else action, url=None if ok or wait else url))
    import downstream
    scheme = downstream.requested(c, next((g for g in c.catalog()["games"] if g["id"].upper() == gid.upper()), dict(id=gid)))
    if scheme:   # 下游深度方案出的是独立 App（install → downstream.install_ios）：iPhone 版看它自己的出包记录，不看容器件（原先一律报没做好，实际装得上）
        rec = downstream.last_record(c, gid.upper(), scheme)
        ipa_path = (rec or {}).get("ipa") or ""
        item("package", bool(rec), "这款游戏的 iPhone 版已经做好", "还没做好：先做 iPhone 版，做好后这里自动打勾。", who="app", action="openExport")
    else:
        try:
            ios_pack(c, gid)
            ipa_path = phone_ipa_path(c)
            item("package", True, "这款游戏的 iPhone 版已经做好", "")
        except Failure:
            item("package", False, "这款游戏的 iPhone 版已经做好", "还没做好：先做 iPhone 版，做好后这里自动打勾。", who="app", action="openExport")
    use_cert = devcert.active(c)
    team = None
    if not use_cert:   # Apple ID 用随包 plumesign 登录（mac_plume）：看它当前选中的账号，不看 Xcode
        acct = plume_account(c, win_install_mod(c)) if mac_plume(c) else None
        team = dict(id=acct["team"], name=acct["teamName"] or acct["account"]) if acct else None
        item("appleId", team, "已登录 Apple ID", "" if team else APPLE_SIGN_IN, action="appleLogin")   # S2Arm 自己的登录面板
    # 清单只看玩家选的那台、每次现查（11-11 玩家实报：没插线却亮「iPhone 已用数据线连上」——原先没选设备时作业自己挑了同网的 iPad 来打勾）
    want = (c.r.get("device") or "").strip()
    link = device_link(c, want, polling=True) if want else dict(device=None, stage="connected", problem=S_DEVICE_REQUIRED, trace=[], devices=[])
    one, stage = link["device"], link["stage"]
    cert = devcert_items(c, item, (one or {}).get("hardwareProperties", {}).get("udid") or "") if use_cert else None
    name = (one.get("deviceProperties", {}).get("name") or "").strip() if one else ""
    kind = device_kind(one) if one else "iPhone / iPad"
    how = "数据线" if one and _link(one).get("transportType") == "wired" else "同一 Wi-Fi"
    item("connected", one, "{0}「{1}」已连上（{2}）".replace("{0}", kind).replace("{1}", name).replace("{2}", how) if one else "选的设备已连上（数据线或同一 Wi-Fi）",
         link["problem"] if stage == "connected" else "", note=S_NETWORK if one and how != "数据线" else "", action="pickDevice")   # 只靠网络连着：能装，但提醒插线更稳（玩家反馈 11-11）
    paired = bool(one) and stage != "trusted"
    item("trusted", paired, "{0} 已信任这台 Mac".replace("{0}", kind), link["problem"] if stage == "trusted" else S_TRUST, waiting=not one, action="trustHelp")
    mode_pending = paired and stage == "prepare" and one.get("deviceProperties", {}).get("developerModeStatus") not in ("enabled", None)
    dev_mode = paired and stage != "developerMode" and not mode_pending
    item("developerMode", dev_mode, "{0} 已打开开发者模式".replace("{0}", kind), S_DEV_MODE, waiting=not paired or mode_pending, action="developerModeHelp")
    item("deviceReady", dev_mode and stage == "", "{0} 已准备好安装".replace("{0}", kind), link["problem"] if stage == "prepare" else "", who="app", waiting=not paired or stage == "developerMode")
    account = (cert or {}).get("teamName") or "" if use_cert else (team["name"] or team["id"]) if team else ""
    return dict(gameId=gid, items=items, ready=not any(i["blocking"] for i in items), device=name, deviceKind=kind.lower() if one else "", account=account, ipaPath=ipa_path, imgPath=phone_img_path(c, gid), shaderCachePath=phone_shader_cache(c, gid),
                signing="", signMode="devcert" if use_cert else "appleId", devcert=cert)


def linked_sdk(exe):
    """主程序链接时的 iOS SDK 版本（Mach-O LC_BUILD_VERSION 的 sdk 字段，xxxx.yy.zz 按半字节编码），读不到返回 None。
    11-07 Duo 实测：iOS 按主程序链接的 SDK 判 App 认不认识本机型——旧 SDK（26.5）出的壳在 iPhone Duo（iOS 27 新机型）上按旧 iPhone 尺寸
    （667×375 点）缩小居中、四周留黑；同一份代码用 27.1 SDK 链接就铺满 2034×1398。启动屏键（UILaunchStoryboardName / UILaunchScreen）有没有都一样"""
    import struct
    try:
        with open(exe, "rb") as f:
            head = f.read(4096 * 8)
    except OSError:
        return None
    if len(head) < 32 or struct.unpack_from("<I", head)[0] != 0xFEEDFACF:   # 只认 64 位单架构（iOS 壳只出 arm64）
        return None
    ncmds, off = struct.unpack_from("<I", head, 16)[0], 32
    for _ in range(ncmds):
        if off + 8 > len(head):
            return None
        cmd, size = struct.unpack_from("<II", head, off)
        if cmd == 0x32 and off + 16 <= len(head):   # LC_BUILD_VERSION：platform, minos, sdk
            v = struct.unpack_from("<I", head, off + 16)[0]
            return (v >> 16, (v >> 8) & 0xFF, v & 0xFF)
        if size < 8:
            return None
        off += size
    return None


def check_shell_sdk(c, app, dev):
    """iPhone 的系统大版本比壳链接的 SDK 新 → 黄字 shell_sdk_old（照常装）：这一代系统新出的机型（如 iPhone Duo）会把 App 按旧尺寸缩小显示"""
    try:
        exe = app / plistlib.loads((app / "Info.plist").read_bytes()).get("CFBundleExecutable", "S2Arm")
    except (OSError, plistlib.InvalidFileException):
        return
    sdk, os_major = linked_sdk(exe), str(dev.get("os") or "").split(".")[0]
    if sdk and os_major.isdigit() and int(os_major) > sdk[0]:
        c.diagnostic(f"这个包是用 iOS {sdk[0]}.{sdk[1]} 的 Xcode 出的，这台 iPhone 是 iOS {dev.get('os')}：iOS {os_major} 新出的机型（如 iPhone Duo）"
                     "上整个 App 会按旧 iPhone 尺寸缩小在屏幕中间", code="shell_sdk_old", severity="warning")


PROFILE_REJECTED = "0xe8008015"   # 「A valid provisioning profile for this executable was not found」（MIInstallerErrorDomain 13）


def set_aside_profiles(c, picked):
    """dev_sign 记下的那几份描述文件挪进 <产物根>/.stale_profiles/（同 ensure_profile.sh 挪缺设备的旧描述文件那个目录；不删，挪不动的跳过）→ 挪走的"""
    try:
        paths = {p.strip() for p in Path(picked).read_text(encoding="utf-8").splitlines() if p.strip()} if picked and Path(picked).is_file() else set()
    except OSError:
        paths = set()
    env = c.env()
    sd = Path(env.get("S2A_ROOTS") or Path(env.get("S2A_RUN_ROOT") or Path.home() / ".s2arm/run") / "s2arm/roots") / ".stale_profiles"
    moved = []
    for p in sorted(paths):
        try:
            sd.mkdir(parents=True, exist_ok=True)
            shutil.move(p, sd / Path(p).name)
            moved.append(p)
        except OSError as e:
            note(c, f"挪不开描述文件 {p}：{e}")  # 非玩家句
    return moved


def app_has_widget(app):
    return any((Path(app) / "PlugIns").glob("*.appex")) if (Path(app) / "PlugIns").is_dir() else False


def profile_trust_name(profile, env=None):
    """签好的 App 里 embedded.mobileprovision 的签名证书名（如「Apple Development: someone@example.com (ABCDE12345)」）：
    iPhone「设置 → 通用 → VPN 与设备管理 → 开发者 App」下列的就是它，玩家照着点「信任」。读不到（非 Mac、文件没有、解不开）回 ""，界面退回不带名字的说法"""
    try:
        p = subprocess.run(["security", "cms", "-D", "-i", str(profile)], capture_output=True, env=env, timeout=5)
        if p.returncode:
            return ""
        certs = plistlib.loads(p.stdout).get("DeveloperCertificates") or []
        if not certs:
            return ""
        x = subprocess.run(["/usr/bin/openssl", "x509", "-inform", "DER", "-noout", "-subject", "-nameopt", "utf8,sep_multiline"],
                           input=bytes(certs[0]), capture_output=True, env=env, timeout=5)
        m = re.search(r"^\s*CN=(.+)$", x.stdout.decode("utf-8", "replace"), re.M)
        return m.group(1).strip() if x.returncode == 0 and m else ""
    except (OSError, subprocess.TimeoutExpired, ValueError, TypeError, AttributeError):
        return ""


def sign_and_install(c, ipa, team, dev, udid, bid, games, steps=True):
    """steps=False：不报主进度（出包重叠早装在后台线程里调，进度条归出包）。解 ipa → 改包名 → 申请开发描述文件 → 用玩家 Apple ID 签名 →
    按各款文档根 Documents/games/<游戏号> 拉跑局知识与存档备份 → devicectl 覆盖装（不卸载，Documents 保留）"""
    ents = c.engine / "pack/app.entitlements"
    stage = Path(tempfile.mkdtemp(prefix="s2a-install-", dir=c.private))
    log = c.logs / (c.task + "-install.log")
    try:
        # ditto 解：保留可执行位与符号链接（zipfile 会丢，codesign / 安装都要）
        r = subprocess.run(["/usr/bin/ditto", "-x", "-k", str(ipa), str(stage)], capture_output=True, text=True)
        app = next((p for p in (stage / "Payload").glob("*.app") if p.is_dir()), None)
        if r.returncode or not app:
            raise Failure("解不开 iPhone 包：" + (r.stderr.strip()[-200:] or str(ipa)), code="job_failed")  # 非玩家句
        info = app / "Info.plist"
        pl = plistlib.loads(info.read_bytes()); pl["CFBundleIdentifier"] = bid
        shown = container_display_name(app)
        if shown:
            pl["CFBundleDisplayName"] = shown
        info.write_bytes(plistlib.dumps(pl))
        check_shell_sdk(c, app, dev)
        # 主屏小组件扩展（PlugIns/S2ArmWidget.appex，11-16）：扩展包名跟着主包名（<主包名>.widget）。玩家在设置里关了（widget: false）→ 去掉；
        #   玩家自己的证书只有主程序一份描述文件 → 去掉并提示；Apple ID 签名带上（sign_with_apple_id 申请两份描述文件，哪步不成就去掉重签，绝不因小组件装不上）
        widget = widget_prepare(c, app, bid, devcert=devcert.active(c))
        from diag import is_vision
        c.profile_picked = stage / "profile-picked.txt"   # dev_sign 记下这次用了哪份描述文件（手机拒了就挪开它重申）
        if devcert.active(c):   # 玩家自己的开发者证书（v0.2.4）：rcodesign 直接读 p12 签，不申请描述文件、不要 Xcode 账号
            if steps: c.begin("用你的开发者证书签名", 4, 6)
            devcert.sign_app(c, app, log)
            devcert.export_ipa(c, stage / "Payload")
        else:
            sign_with_apple_id(c, app, team, bid, ents, udid, log, steps, vision=is_vision(dev), widget=widget)
        c.trust_name = profile_trust_name(app / "embedded.mobileprovision")   # 完成页「信任」那一步要点的名字（装机结果 trustName）
        if steps: c.begin("装到 iPhone", 5, 6)
        from diag import devicectl
        for g in games:
            pull_seen(c, dev["id"], bid, g, udid or dev["id"], docs=game_docs(g))
            backup_saves(c, dev["id"], bid, g, docs=game_docs(g))
        rc, _, err = devicectl(c, "device", "install", "app", "--device", dev["id"], str(app), timeout=1800)
        if rc and classify(err, default="") == "device_tunnel_unavailable":
            # 开发者通道这一下没建起来（Wi-Fi 连着时常见）：等 10 秒原样再装一次，两次都不行才报
            note(c, "iPhone 的开发者通道没连上（CoreDevice 4016）：10 秒后再装一次")  # 非玩家句
            time.sleep(TUNNEL_RETRY_S)
            rc, _, err = devicectl(c, "device", "install", "app", "--device", dev["id"], str(app), timeout=1800)
        if rc and PROFILE_REJECTED in err and not devcert.active(c):
            # 11-17 #1447 / #1448：手机说这次签名用的描述文件不能用（0xe8008015）——本机核得出的几项都过了、手机仍不认（证书在别处被换掉 / 吊销、
            #   描述文件被别的工具改过等）：把这次用过的描述文件挪开（不删），按 0.5.1 的做法向 Xcode 申请一份新的、重签、再装一次；仍不认照常报
            moved = set_aside_profiles(c, c.profile_picked)
            note(c, f"iPhone 不认这次用的开发描述文件（0xe8008015）：挪开 {len(moved)} 份、重新申请后再签再装一次")  # 非玩家句
            c.profile_force_request = True
            try:
                sign_with_apple_id(c, app, team, bid, ents, udid, log, steps, vision=is_vision(dev), widget=widget and app_has_widget(app))
            finally:
                c.profile_force_request = False
            rc, _, err = devicectl(c, "device", "install", "app", "--device", dev["id"], str(app), timeout=1800)
        if rc:
            # 识别 devicectl 的实际原因，不把额度、配对和开发者模式失败统称签名。
            code = classify(err, default="device_install_failed")
            reason = err.strip()[-2000:] or f"devicectl 退出 {rc}（未提供错误详情）"
            raise Failure("装不上 iPhone：" + reason, code=code)  # 非玩家句
    finally:
        shutil.rmtree(stage, ignore_errors=True)


WIDGET_QUOTA = "这个免费 Apple ID 7 天内注册的包名已满 10 个，小组件要多注册一个（App 照常装上，额度恢复后再装一次就带上）"


def ipa_strip_widget(src, dst):
    """另存一份去掉小组件扩展（Payload/*.app/PlugIns/）的 ipa → 原包里有没有扩展。没有就不写 dst"""
    import zipfile
    with zipfile.ZipFile(src) as z:
        names = z.infolist()
        drop = [i for i in names if re.match(r"Payload/[^/]+\.app/PlugIns/", i.filename)]
        if not drop:
            return False
        with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as o:
            for i in names:
                if i not in drop:
                    o.writestr(i, z.read(i), compress_type=i.compress_type)
    return True


def widget_mod(c):
    """pack/widget_ext.py（出壳 / 组装 / 装机同一份包名与去扩展规则）：按请求给的引擎树（c.engine，同本文件取 tools / pack/orch）导入。
    原先按本文件位置往上两级找：源码树里是仓库根，对；Mac App 里作业层在 Contents/Resources/Jobs、引擎在 Resources/Engine，
    往上两级是 Contents、没有 pack，装到 iPhone 一律 ModuleNotFoundError（0.5.0 正式候选 667ccdb92，11-16 维护者）。
    引擎树里没有这份时（单测给的空引擎）才退回本文件所在的源码树"""
    root = Path(c.engine)
    if not (root / "pack" / "widget_ext.py").is_file():
        root = Path(__file__).resolve().parents[2]
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    from pack import widget_ext
    return widget_ext


def widget_group(team_id):
    """小组件与主程序共用的 App Group：全苹果唯一，按团队号区分（壳里写的 group.s2arm.widget 是给侧载工具改名用的占位；运行时按前缀认）"""
    return f"group.s2arm.widget.{team_id}"


def widget_prepare(c, app, bid, devcert=False):
    """装机前处理小组件扩展 → 还带不带（True = 交给签名带上）。没有扩展（单款包 / 旧壳）→ False"""
    if not (Path(app) / "PlugIns").is_dir():
        return False
    W = widget_mod(c)
    if not W.appexes(app):
        return False
    W.retarget(app, bid)
    if c.r.get("widget") is False:   # 玩家在设置里关了「装到 iPhone 时带主屏小组件」：不提示
        W.strip(app)
        return False
    if devcert:
        widget_skip(c, app, "用你自己的开发者证书签名只有主程序一份描述文件，签不了小组件")
        return False
    return True


def widget_skip(c, app, why):
    """去掉小组件扩展、照常装 App，黄字说明这次为什么没带（玩家决定要不要换签名方式 / 等额度）"""
    widget_mod(c).strip(app)
    c.diagnostic(f"这次没装小组件：{why}", code="widget_skipped", severity="warning")


def _sign_or_request(c, sign, bid, team, ents, udid, log, env, steps):
    """先用本机已有的开发描述文件签（sign = dev_sign.py 命令；退出码 3 = 没有合适的：没有、过期或一天内到期、不含这台手机、权限不够），
    3 才申请（ensure_profile.sh：xcodebuild 拿这台手机当目标、登记设备，要 Xcode 看得到它）再签一次。
    11-16 诊断 D-44DP：原先每次装机都先申请，Xcode 看不到这台手机（Unable to find a destination）时本机明明有能用的描述文件，主程序也签不成；
    ensure_profile.sh 文件头写的本意就是「签名报退出码 3 时才调」。dev_sign 带 S2A_PROFILE_NEED_UDIDS = 这台，描述文件不含它也算没有"""
    #   11-17 #1447 / #1448（0.5.2 装上被手机拒 0xe8008015）：不知道这台手机的 UDID 时本机描述文件含不含它核不了 → 不用本机的，先申请（0.5.1 的做法）；
    #   c.profile_force_request（sign_and_install 手机拒了那份之后重来）同样先申请；c.profile_picked：dev_sign 把用了哪份描述文件记进这个文件
    picked = getattr(c, "profile_picked", None)
    senv = dict(env or c.env(), **({"S2A_PROFILE_NEED_UDIDS": udid} if udid else {}), **({"S2A_DEV_SIGN_PICKED": str(picked)} if picked else {}))
    if udid and not getattr(c, "profile_force_request", False):
        if steps: c.begin("用你的 Apple ID 签名", 4, 6, keychain=True)   # codesign 用钥匙串里的签名身份：可能弹授权（开发者证书模式读 p12，不弹）
        c.last_rc = 0
        c.run(sign, log, senv, ok_codes=(0, 3))
        if c.last_rc != 3:
            return
    if steps: c.begin("申请开发描述文件（第一次要几十秒）", 3, 6, keychain=True)   # 用 Xcode 账号申请：可能弹钥匙串授权
    c.run(["bash", c.engine / "pack/ensure_profile.sh", bid, team["id"], ents] + ([udid] if udid else []), log, env)
    if steps: c.begin("用你的 Apple ID 签名", 4, 6, keychain=True)
    c.run(sign, log, senv)


def sign_with_apple_id(c, app, team, bid, ents, udid, log, steps, vision=False, widget=False):
    """Apple ID 模式：按玩家 Xcode 账号申请开发描述文件（ensure_profile.sh）再用钥匙串身份签（dev_sign.py）。
    vision：目标是 Apple Vision Pro（按 iPad 兼容装）——申请描述文件的目标写 id=<UDID>，让 Xcode 自己认平台与「Designed for iPad」变体、登记这台设备
    （原样写 platform=iOS 会找不到这台；理论支持、未真机验证）。
    widget：带主屏小组件扩展——主程序与扩展各申请一份描述文件（扩展包名 <主包名>.widget，两边都带共享组 group.s2arm.widget.<团队号>），
    先签扩展再签主程序；其中任何一步不成（免费账号包名额度 9120、个人团队不给共享组等）→ 去掉扩展、按原来的权限重签，黄字说原因"""
    if widget:
        try:
            return sign_with_widget(c, app, team, bid, ents, udid, log, steps, vision)
        except Failure as e:
            if e.code in ("keychain_prompt",):   # 钥匙串框没人点：去掉扩展也一样要签，照原样报
                raise
            widget_skip(c, app, WIDGET_QUOTA if e.code == "free_appid_limit" else str(e).removeprefix("签名没成：") or e.code)  # 非玩家句（去前缀用）
    try:
        env = dict(c.env(), S2A_PROFILE_DEST=f"id={udid}") if vision and udid else None
        _sign_or_request(c, [sys.executable, c.engine / "pack/dev_sign.py", app, bid, team["id"], ents], bid, team, ents, udid, log, env, steps)
    except Failure as e:
        if e.code != "tool_exit":
            raise
        from errors import recovery, tool_reason
        text = log.read_text(errors="replace")[-8000:] if log.is_file() else ""
        code = classify(text)
        raise Failure("签名没成：" + tool_reason(log) + "；" + recovery(code, packaged=bool(c.r.get("engineBundle"))), code=code) from e  # 非玩家句


def sign_with_widget(c, app, team, bid, ents, udid, log, steps, vision):
    """sign_with_apple_id 带小组件的那一支：权限文件临时写（主程序 = 原权限 + 共享组；扩展 = 只有共享组），扩展先签（dev_sign --extension 不核内存上限）"""
    W = widget_mod(c)
    group = widget_group(team["id"])
    wbid = W.bundle_id(bid)
    with tempfile.TemporaryDirectory(dir=c.private) as t:
        main_ents = Path(t) / "app.entitlements"
        pl = plistlib.loads(Path(ents).read_bytes()); pl["com.apple.security.application-groups"] = [group]
        main_ents.write_bytes(plistlib.dumps(pl))
        ext_ents = Path(t) / "widget.entitlements"
        ext_ents.write_bytes(plistlib.dumps({"com.apple.security.application-groups": [group]}))
        try:
            env = dict(c.env(), S2A_PROFILE_DEST=f"id={udid}") if vision and udid else None
            for x in W.appexes(app):   # 扩展先签（主程序的封印要包住签好的扩展）
                _sign_or_request(c, [sys.executable, c.engine / "pack/dev_sign.py", x, wbid, team["id"], ext_ents, "--extension"], wbid, team, ext_ents, udid, log, env, steps)
            _sign_or_request(c, [sys.executable, c.engine / "pack/dev_sign.py", app, bid, team["id"], main_ents], bid, team, main_ents, udid, log, env, steps)
        except Failure as e:
            if e.code != "tool_exit":
                raise
            from errors import tool_reason
            text = log.read_text(errors="replace")[-8000:] if log.is_file() else ""
            raise Failure("签名没成：" + tool_reason(log), code=classify(text)) from e  # 非玩家句


def win_install_mod(c):   # Windows 签名装机 + 推包（pack/orch/install_win.py：玩家自装的 Impactor 同版 plumesign + s2a-devpush）
    sys.path.insert(0, str(c.engine / "pack/orch"))
    import install_win
    return install_win


def win_udids(W):
    """设备服务里的手机：有 USB 连着的只算 USB 的，同网配对的不算（同 target.cable_first）"""
    devs = W.mux_devices()
    return sorted({u for u, how, _ in devs if u and how == "USB"} or {u for u, _, _ in devs if u})


def win_udid(c, W):
    """要装的那台：请求给的 device（UDID，玩家选的那台）在设备服务里（USB 或同网都算）就用它；没给、或给的那台这次没列出，
    设备服务里只有一台就用那一台（11-13 维护者：插着一台手机就别说识别不了）；多台对不上才让玩家选。
    设备服务列表会瞬时空（插拔 / 同网切数据线），最多读 3 次、间隔 3 秒再判"""
    want = (c.r.get("device") or "").strip()
    udids = []
    for k in range(LINK_READS):
        try:
            links = W.mux_devices()
        except OSError as e:
            raise Failure(f"连不上本机苹果设备服务（{e}）：从 Microsoft Store 装「Apple Devices」后重试。", code="device_pairing_required") from e
        udids = list(dict.fromkeys(u for u, _, _ in links if u))
        if (want and want in udids) or (not want and udids):
            break
        if k + 1 < LINK_READS:
            time.sleep(LINK_RETRY_WAIT)
    if want and want in udids:
        return want
    if len(udids) == 1:
        return udids[0]
    if not udids:
        raise Failure("这台电脑的设备服务里没看到 iPhone / iPad：插上数据线并解锁手机，在手机上点「信任」，再试一次。", code="device_not_connected")
    raise Failure(S_DEVICE_REQUIRED if not want else
                  f"要装的那台设备（编号末尾 {want[-6:]}）这次没列出来，这台电脑现在连着 {len(udids)} 台：在装机页选其中一台再装。",
                  code="device_required" if not want else "device_not_connected")


def readiness_windows(c, gid):
    """装机前清单的 Windows 版：做好 iPhone 版 → Impactor（实测版本 + plumesign）→ 手机连着（设备服务里有且只有一台）；同 Mac 版的项格式"""
    W = win_install_mod(c)
    items = []
    ipa_path = ""
    def item(key, ok, title, detail, who="human", waiting=False, url=None, action=None):
        wait = waiting and not ok
        # 只有没出包才真挡；其余只提示，直接去签名，真失败再给原因
        items.append(dict(id=key, ok=bool(ok), waiting=wait, blocking=key == "package" and not ok, title=title, detail="" if ok or wait else detail, who=who,
                          url=None if ok else url,   # 玩家自装件的官方下载页（同环境检查 winenv 的 url 字段）：启动器渲染成「打开官方下载页」，不从说明文字里抠链接
                          action=None if ok else action))   # 启动器本地动作（openImpactor：打开随包的 Impactor）；只认枚举，不传路径
    try:
        ios_pack(c, gid)
        ipa_path = phone_ipa_path(c)
        item("package", True, "这款游戏的 iPhone 版已经做好", "")
    except Failure:
        item("package", False, "这款游戏的 iPhone 版已经做好", "还没做好：先做 iPhone 版，做好后这里自动打勾。", who="app", action="openExport")   # 按钮落点：这款的出包页（11-19 契约，同 Mac）
    resources = c.r.get("resources")
    use_cert = devcert.active(c)
    st = W.impactor.status(resources=resources)
    if use_cert:
        pass   # 证书模式不用 Impactor 账号（plumesign 只装不签，随包就有）；证书两项排在设备之后（要设备 UDID）
    elif st.get("bundled"):   # 发版启动器随包（11-07）：零跳转；第一次要在 Impactor 里登录 Apple ID（密码只在 Impactor 里输、只发给 Apple，我们不代输）
        item("impactor", st["verdict"] == "ok", "签名工具 Impactor 已就绪（随 S2Arm 附带的第三方开源工具）",
             recovery("impactor_missing", packaged=True))
        many = len(W.impactor.unselected_accounts()) > 1   # 存了多个 Apple ID 却没选中（D-ZM37）；只存一个的装机时替玩家选上，照常打勾
        item("impactor_login", st["verdict"] != "ok" or (W.impactor.logged_in() and not many), "已在 Impactor 里登录 Apple ID（只需一次）",
             "Impactor 里存着几个 Apple ID，但没有选中要用的那个：点「打开 Impactor」，在「设置」里选中它点「移除账号」，再「添加账号」重新登录（添加的就是选中的），回来这里自动打勾。"
             if many else "点「打开 Impactor」，在「设置 → 添加账号」登录你的 Apple ID（免费的就行），登录好回来这里自动打勾。",
             action="openImpactor")
    else:
        item("impactor", st["verdict"] == "ok" and bool(W.plumesign_path()), "Impactor 已装好并登录过 Apple ID",
             recovery(st["code"] or "impactor_missing", packaged=False), url=st["download"])
    # 清单只看要装的那台、每次现查：请求带了 device（续签 = 当初那台）就只认它（USB 或同网都算）；没带就是唯一连着的那台（前端拿它的号去装）
    want = (c.r.get("device") or "").strip()
    try:
        links = W.mux_devices()
        udids = ([want] if want in {u for u, _, _ in links if u} else []) if want else win_udids(W)
    except OSError:
        links, udids = [], []
    how = "数据线" if udids and any(u == udids[0] and t == "USB" for u, t, _ in links) else "同一 Wi-Fi"
    item("connected", len(udids) == 1, "{0}已连上（{1}）".replace("{0}", "设备").replace("{1}", how) if len(udids) == 1 else "选的设备已连上（数据线或同一 Wi-Fi）",
         (f"要装的那台设备（编号末尾 {want[-6:]}）现在不在线：插上数据线并解锁，或让它和这台电脑连同一个 Wi-Fi。" if want else
          "连着不止一台 iPhone / iPad：只留要装的那一台。" if len(udids) > 1 else "用数据线把 iPhone / iPad 连到这台电脑，或让它和这台电脑连同一个 Wi-Fi；解锁并点「信任」。"))
    cert = devcert_items(c, item, udids[0] if len(udids) == 1 else "") if use_cert else None
    kind = ""
    if len(udids) == 1 and hasattr(W, "device_kind"):   # #487：连着的是 iPad 时入口写「装到 iPad」
        try:
            kind = W.device_kind(udids[0])
        except (OSError, ValueError):
            kind = ""
    return dict(gameId=gid, items=items, ready=not any(i["blocking"] for i in items), device=udids[0] if len(udids) == 1 else "", deviceKind=kind,
                account=(cert or {}).get("teamName") or "", ipaPath=ipa_path, imgPath=phone_img_path(c, gid), shaderCachePath=phone_shader_cache(c, gid), signMode="devcert" if use_cert else "appleId", devcert=cert)


def devpush_client(c, udid, bid, work, device=None):
    """s2a-devpush（house_arrest + AFC）：两端都随发版带，devicectl 没有的「删容器里的目录」「整目录拉回」走它。
    device = Mac 上这台的 CoreDevice 标识：写文件时 s2a-devpush 找不到设备（usbmuxd 那一刻列不出、devicectl 连着，#1756）就整份改走 devicectl"""
    sys.path.insert(0, str(c.engine / "tools"))
    import push_pack as P
    prebuilt = c.engine / "Binaries/Prepare/devpush" / P.EXE
    exe = P.devpush_path(str(prebuilt) if prebuilt.is_file() else None, c.env())
    if not exe:
        raise Failure("找不到 s2a-devpush：请重新安装完整的 S2Arm。", code="tool_missing")
    cl = P.Devpush(exe, udid, bid, work, c.env())
    if device:
        cl.fallback = lambda src, rel: P.Devicectl(device, bid, work, env=c.env()).put(src, rel)
    return cl


class MacPhone:
    """Mac：Xcode 账号签名 + devicectl 装 + push_pack 推（出包重叠早装传入开工前认好的账号与设备）"""

    def __init__(self, c, team=None, dev=None, udid=None, bid=None):
        self.c = c
        self.team = team or devcert.team(c)   # PlumeMacPhone 传 plumesign 账号的团队；直接构造 MacPhone 的只有证书用户（mac_phone）
        self.dev = dev or phone(c)
        self.udid = udid if udid is not None else udid_of(c, self.dev["id"])
        # 证书模式：描述文件定的包名（和 Apple ID 模式是两个 App）；ExternalPhone 直接给手机上装着的那个
        self.bid = bid or (devcert.state(c)["bundleId"] if devcert.active(c) else container_bundle_id(self.team["id"], c))
        self.key = self.udid or self.dev["id"]   # devices/<设备>.json 的键
        # deviceKind / deviceName：完成页、诊断写「已装到 iPad「名字」」，不写死 iPhone
        self.info = dict(device=self.dev["name"], deviceName=self.dev["name"],
                         deviceKind="iPad" if (self.dev.get("productType") or "").startswith(("iPad", "RealityDevice")) else "iPhone",   # Apple Vision Pro 按 iPad 兼容装
                         team=self.team["name"], free=self.team["free"])
        self.pending = None   # 资源传完但没拉起 App 解开时的那句（put_game 带进结果 unpackPending）

    def app_running(self):
        return app_running(self.c, self.dev["id"], self.bid) if self.bid else None

    def pull_kept(self, games):
        """这几款在手机上留下的 NRO 都收回游戏库（pull_kept_nros，#8be819）→ 新收的个数；收不到不挡装机"""
        n = 0
        for g in games:
            try:
                n += pull_kept_nros(self.c, self.dev["id"], self.bid, g, docs=game_docs(g))
            except (OSError, Failure) as e:
                print(f"[s2a] 收回 {g} 留下的 NRO 失败：{e}", file=sys.stderr)
        return n

    def backup(self, g):
        return backup_saves(self.c, self.dev["id"], self.bid, g, docs=game_docs(g))

    def app_missing(self, bid, why):
        """s2a-devpush 进不了容器时，这台手机确实没装这个包名吗？Mac 上没装时 s2a-devpush 只回「device socket io failed」（真机实测），
        和真断线分不开，所以问 devicectl 的应用列表（同 backup_saves）；连它也问不出 → 停装"""
        from diag import devicectl
        rc, apps, err = devicectl(self.c, "device", "info", "apps", "--device", self.dev["id"], "--bundle-id", bid, timeout=60)
        if rc:
            note(self.c, "问不出 iPhone 上有没有装过 S2Arm（" + (err.splitlines()[-1] if err else f"devicectl 退出 {rc}") + "），按装着、读不到继续")  # 非玩家句
            return False
        return not any(a.get("bundleIdentifier") == bid for a in (apps.get("result") or {}).get("apps") or [])

    def install(self, ipa, games, steps=True):
        try:
            sign_and_install(self.c, ipa, self.team, self.dev, self.udid, self.bid, games, steps=steps)
        except Failure as e:
            # 包名被别处占用（玩家先自签装过 S2Arm）：自动换一个包名再签一次、记下，之后都用它；换过的也被占才照报 app_id_taken
            if e.code != "app_id_taken" or devcert.active(self.c) or self.bid.endswith(ALT_BUNDLE_SUFFIX):
                raise
            old, self.bid = self.bid, self.bid + ALT_BUNDLE_SUFFIX
            sign_and_install(self.c, ipa, self.team, self.dev, self.udid, self.bid, games, steps=steps)
            bundle_switched(self.c, self.team["id"], self.bid)
            self.c.diagnostic(f"包名 {old} 已被别的签名占用，这次自动改用 {self.bid} 装好了", code="bundle_id_switched", severity="warning")
        if getattr(self.c, "trust_name", ""):
            self.info["trustName"] = self.c.trust_name   # 装机结果带上（**ph.info）：完成页写「点「<证书名>」→ 信任」

    def before_push(self, packs):
        mbps = usb_mbps(self.udid, self.c.env())
        if 0 < mbps < 5000:   # 10-26 实测：两台测试设备经集线器只有 480 Mb/s，资源包约 31 MB/秒；USB 3 直插约 10 倍
            gb = sum(f.stat().st_size for p in packs.values() for f in p.rglob("*") if f.is_file()) / 2**30
            if not packs or gb <= 0:   # 这次一个资源包都不推（只换壳更新、续签）：不说「第一次全拷约 N 分钟」（11-11 维护者截图：0.0 GB 也照报，误导）
                return
            self.c.diagnostic(f"这台 iPhone 现在按 USB 2.0（{mbps} Mb/s）连着：资源包 {gb:.1f} GB 第一次全拷约 {max(1, round(gb * 1024 / 31 / 60))} 分钟，"
                              "换 USB 3 能快约 10 倍（以后重装只推变了的文件）", code="usb_slow", severity="warning")

    def push(self, g, pack):
        n0 = len(self.c.job["diagnostics"])
        r = push_pack(self.c, self.dev["id"], self.bid, pack, udid=self.udid, docs=game_docs(g), ctx=game_ctx(g), pack_root=pick_pack_root(self.c, self, g))
        with tempfile.TemporaryDirectory(dir=self.c.private) as w:
            try: dp = self.client(w) if self.udid else None
            except Failure: dp = None
            push_cheats(self.c, self.dev["id"], self.bid, g, game_docs(g), mkdir=dp.mkdir if dp else None)
        hit = [d for d in self.c.job["diagnostics"][n0:] if d.get("code") == "manual_unpack_required"]   # push_pack 按「需要玩家操作」收尾时记的那条
        if hit: self.pending = hit[-1].get("summary") or ""
        return r

    def client(self, work):
        return devpush_client(self.c, self.udid, self.bid, work, device=self.dev["id"])   # #1756：usbmuxd 看不到这台时整份改走 devicectl

    def purge(self, g):
        with tempfile.TemporaryDirectory(dir=self.c.private) as w:
            return purge_game(devpush_client(self.c, self.udid, self.bid, w), g)


CERT_REVOKE_ASK = "这台电脑上没有这个 Apple ID 能用的开发证书，要新建一张。账号证书满了的话，苹果会撤掉列表里的第一张「{0}」（在 {1} 上创建，{2} 生效）。在 AltStore / SideStore 上创建的那张就是它们在用的证书，撤掉后 AltStore / SideStore 和它们装的 App 会打不开。"
CERT_REVOKE_ASK_NO_MACHINE = "这台电脑上没有这个 Apple ID 能用的开发证书，要新建一张。账号证书满了的话，苹果会撤掉列表里的第一张「{0}」（{1} 生效）。在 AltStore / SideStore 上创建的那张就是它们在用的证书，撤掉后 AltStore / SideStore 和它们装的 App 会打不开。"   # 苹果列表里这张没记机器名


def win_cert_gate(c, W, exe):
    """Windows Apple ID 签名前（11-17 维护者定）：plumesign 这次要新建开发证书、而账号里有别人的开发证书时，先问玩家——满了它会不问就撤列表第一张
    （install_win.cert_check）。玩家点继续（请求带 acceptCertRevoke）才放行；本机私钥的证书还在 / 列表里没有别人的 / 列不出来都不问"""
    if c.r.get("acceptCertRevoke") is True or devcert.active(c) or not exe:
        return
    team = W.impactor.selected_team() if hasattr(W.impactor, "selected_team") else None
    if not team:
        return
    hit = W.cert_check(exe, team, log=c.logs / (c.task + "-certs.log"))
    if hit:
        name, created = hit["name"] or "?", hit["created"] or "?"
        text = CERT_REVOKE_ASK.format(name, hit["machine"], created) if hit["machine"] else CERT_REVOKE_ASK_NO_MACHINE.format(name, created)
        raise Failure(text, code="cert_revoke_confirm")


def win_install_failure(e):
    """Windows plumesign 签名装机失败（install_win.Fail）→ 作业失败。有对应处理建议的码（free_appid_limit = Apple 9120 等）：上屏说人话的原因与办法，
    plumesign 英文原文只进诊断（Windows 实机：原先整句英文「Developer API error 9120 …」直接上屏，玩家只看到签名失败）；没有专门建议的照旧上屏原文。
    容器装机（WinPhone.install）与下游独立 App 装机（downstream.install_ios_win）同用"""
    code = e.err or "device_install_failed"
    from errors import RECOVERY
    if code in RECOVERY and code not in ("device_install_failed", "job_failed"):
        # 连不上签名服务器：plumesign 那句是系统的本地化原话（如「不知道这样的主机。 (os error 11001)」），一并上屏，玩家 / 帮他的人能照着查网络
        said = f"（Impactor 原话：{str(e).split('：', 1)[-1].strip()}）" if code == "signing_server_unreachable" else ""
        return Failure("签名装机没成：" + RECOVERY[code] + said, code=code, internal=str(e))  # 非玩家句
    return Failure(str(e), code=code)


def win_say(c, m):
    """install_win.install_app 的进度回调（容器与下游单款 App 共用）：进度行照常发出；签好的包丢了「提高内存上限」（install_win.iml_line 以「【IML 丢了】」开头）
    不当进度显示，记一条黄字 iml_missing（办法见 errors.RECOVERY），原句进 internal——不挡装机"""
    if not m.startswith("【IML 丢了】"):  # 非玩家句
        c.emit("message", m)
    else:
        c.diagnostic("这次签好的 S2Arm 没带「提高内存上限」权限：装上后能玩，但大游戏可能内存不够、开局中止。", code="iml_missing", severity="warning", internal=m)


class WinPhone:
    """Windows：Impactor 同版 plumesign 签名装机（包名由它按账号定、装完才知道，记在这台的设备记录 devices/<设备>.json 的 bundleId，供下次覆盖前备份）+ s2a-devpush 备份 / 推 / 删"""

    def __init__(self, c):
        self.c, self.W = c, win_install_mod(c)
        self.udid = win_udid(c, self.W)
        self.key = self.udid
        self.bid = devcert.state(c)["bundleId"] if devcert.active(c) else layout.load_device(c, self.key).get("bundleId") or None
        self.devpush = c.engine / "Binaries/Prepare/devpush/s2a-devpush.exe"
        # Windows 设备服务给不出设备名，类型按 USB 产品号认（同装机前清单 deviceKind）；认不出写空，前端写「iPhone / iPad」
        kind = ""
        if hasattr(self.W, "device_kind"):
            try:
                kind = {"ipad": "iPad", "iphone": "iPhone"}.get(self.W.device_kind(self.udid), "")
            except Exception:
                kind = ""
        self.info = dict(device=self.udid, deviceName="", deviceKind=kind)
        self.pending = None   # 资源传完但没拉起 App 解开时的那句（put_game 带进结果 unpackPending）

    def pull_kept(self, games):   # 留下的 NRO（#8be819）：Windows 不收（0.5.0 维护者定：从手机收回只保留 Mac 已发版的那条，Windows 端这次不上）
        return 0

    def app_running(self):   # 查不了进程（Windows 没有 devicectl）：调用方改用 playing（游玩时间心跳）
        return None

    def playing(self, games):
        """按游玩时间心跳判手机上是不是正在玩（playing_by_heartbeat）：True 在玩 / False 没在玩 / None 查不清"""
        if not self.bid:
            return None
        import tempfile
        with tempfile.TemporaryDirectory(prefix="s2a-hb-") as work:
            try:
                client = self.client(work)
            except Failure:
                return None
            return playing_by_heartbeat(client, games, work)

    def client(self, work):
        return devpush_client(self.c, self.udid, self.bid, work)

    def app_missing(self, bid, why):
        """Windows 上没有 devicectl：手机明确答「没有这个 App」时 s2a-devpush 的原文含 ApplicationLookupFailed（Windows 实机核过）；
        进不了容器时 s2a-devpush 再用 installation_proxy 按包名查一次，确实没装就打结构化标记 S2A_APP_MISSING（11-11 实机实测：
        删了 S2Arm 再装，原文只有「device socket io failed」，原先被当成锁屏挡住）。两样都没有 = 判不了，交给调用方按装机原则继续"""
        return "ApplicationLookupFailed" in why or "S2A_APP_MISSING" in why

    def backup(self, g):
        """覆盖装前备份本款存档（同 Mac backup_saves 口径：拉失败就停装）。按这次要装的包名查（这台没记录也可能有 Mac 装的同包名 App 和存档）；
        只有手机明确答「没有这个 App」（ApplicationLookupFailed）才算没装过、不备份。别的取不到（锁屏 / 断开 / 配对问题）一律停装，
        不当成没装过——当成没装会跳过备份直接覆盖（#333）"""
        bid = self.bid or self.bundle_for_signing()
        if not bid:
            # 这台没记过包名、又读不到 Impactor 当前账号的团队号：定不了包名就查不了手机上有没有这款的存档。原先直接放行、交给 plumesign 自己定包名
            #   装上去，等于不备份就覆盖（审计 P2-253）。真实 accounts.json 每个账号都带十位 team_id（Windows 实机核过），读不到 = 存档坏了或没选账号，停装
            return backup_failed(self.c, "读不到 Impactor 当前账号的团队号，定不了要装的包名，查不了手机上这款的存档")
        dest = layout.path(self.c, g, "save_backup", time.strftime("%Y%m%d-%H%M%S"))
        with tempfile.TemporaryDirectory(dir=self.c.private) as w:
            ok, out = devpush_client(self.c, self.udid, bid, w).get_tree(f"{game_docs(g)}/{SAVE_REL}", dest)
        if not ok:
            if "ApplicationLookupFailed" in out or "S2A_APP_MISSING" in out:   # 机上确实没装这个包名（Windows 实机核过的原文 / s2a-devpush 查 installation_proxy 的标记）
                return None
            return backup_failed(self.c, "拉存档失败：" + out[-200:])  # 非玩家句
        return dest if dest.is_dir() else None

    def install(self, ipa, games, steps=True):
        b = self.W.impactor.bundled(self.c.r.get("resources")) or {}   # 发版启动器随包的 plumesign（11-07）；源码树没有 → install_win 照旧找
        if not devcert.active(self.c):
            # 只存了一个 Apple ID 却没选中（D-ZM37）：先选上，备份要的团队号 / 包名和 plumesign 签名都认选中的那个
            picked = self.W.impactor.select_only_account(self.W.plumesign_path(b.get("plumesign")))
            if picked: self.c.emit("message", "Impactor 里存着一个 Apple ID 但没选中，已替你选上")
        for g in games:
            self.backup(g)
        if devcert.active(self.c):
            return self.install_devcert(ipa, steps)
        win_cert_gate(self.c, self.W, self.W.plumesign_path(b.get("plumesign")))   # 满了 plumesign 会撤别人的证书：先问（11-17）
        if steps: self.c.begin("签名并装到 iPhone（Impactor 官方命令行 plumesign）", 3, 6)
        log = self.c.logs / (self.c.task + "-install.log")
        self.c.last_log = log
        # 主屏小组件扩展（11-16）：plumesign 自己给扩展注册包名与共享组。玩家关了 → 去掉再交；带着签不成（免费账号包名额度 9120 等）→ 去掉重来一次，黄字说原因
        with tempfile.TemporaryDirectory(dir=self.c.private) as t:
            plain = Path(t) / "S2Arm.ipa"
            had = ipa_strip_widget(ipa, plain)
            if had and self.c.r.get("widget") is False:
                ipa, had = plain, False
            ns = argparse.Namespace(ipa=str(ipa), udid=self.udid, name=None, plumesign=b.get("plumesign"), log=str(log), bundle=self.bundle_for_signing())
            try:
                self.bid = self.W.install_app(ns, say=self.say)
            except self.W.Fail as e:
                if not had:
                    raise win_install_failure(e) from e
                first = win_install_failure(e)
                print(f"[装机] 带小组件签名装机没成（{e.err or ''}：{e}），去掉小组件重来", file=sys.stderr, flush=True)  # 非玩家句
                ns.ipa = str(plain)
                try:
                    self.bid = self.W.install_app(ns, say=self.say)
                except self.W.Fail as e2:
                    raise win_install_failure(e2) from e2
                why = WIDGET_QUOTA if e.err == "free_appid_limit" else str(first)
                self.c.diagnostic(f"这次没装小组件：{why}", code="widget_skipped", severity="warning")
            got = getattr(ns, "signed_expiry", None)
            if got:   # 签好的包里的真实期限（#1772）：免费 / 付费照它判，不再按账号猜（plumesign 判不了付费，PlumeMacPhone 原先一律记免费、付费账号显示 7 天）
                self.info.update(expires=got["expires"], free=got["days"] <= 7)
        rec = layout.load_device(self.c, self.key)
        layout.save_device(self.c, self.key, dict(rec, bundleId=self.bid))

    def install_devcert(self, ipa, steps):
        """玩家自己的开发者证书（v0.2.4）：解 ipa → rcodesign 签（同 Mac devcert.sign_app）→ 另存 S2Arm.signed.ipa → plumesign 只装不签（device --install）"""
        c = self.c
        log = c.logs / (c.task + "-install.log")
        c.last_log = log
        stage = Path(tempfile.mkdtemp(prefix="s2a-install-", dir=c.private))
        try:
            import zipfile
            with zipfile.ZipFile(ipa) as z:
                z.extractall(stage)
            app = next((p for p in (stage / "Payload").glob("*.app") if p.is_dir()), None)
            if not app:
                raise Failure("解不开 iPhone 包：" + str(ipa), code="job_failed")  # 非玩家句
            widget_prepare(c, app, devcert.state(c)["bundleId"], devcert=True)   # 自己的证书只有主程序一份描述文件：去掉小组件扩展（玩家关了不提示）
            if steps: c.begin("用你的开发者证书签名", 3, 6)
            bid, _ = devcert.sign_app(c, app, log)
            devcert.export_ipa(c, stage / "Payload")
            if steps: c.begin("装到 iPhone", 4, 6)
            b = self.W.impactor.bundled(c.r.get("resources")) or {}
            try:
                self.W.install_signed(self.W.plumesign_path(b.get("plumesign")), app, self.udid, log)
            except self.W.Fail as e:
                raise Failure(str(e), code=e.err or "device_install_failed") from e
        finally:
            shutil.rmtree(stage, ignore_errors=True)
        self.bid = bid
        rec = layout.load_device(c, self.key)
        layout.save_device(c, self.key, dict(rec, bundleId=bid))

    def say(self, m):
        win_say(self.c, m)

    def bundle_for_signing(self):
        """交给 plumesign 的包名：和 Mac 同一个（container_bundle_id(团队号)，团队号取 Impactor 当前账号）。这台记录过只差大小写的包名
        （早先 Windows 让 plumesign 自己定的 io.s2arm.games.<大写团队号>）就沿用它：换成小写会在手机上多出一个 S2Arm、存档留在旧的里。
        读不到团队号 → None（照旧由 plumesign 定）"""
        if devcert.active(self.c):
            return devcert.state(self.c)["bundleId"]
        team = self.W.impactor.selected_team() if hasattr(self.W.impactor, "selected_team") else None
        if not team:
            return None
        want = container_bundle_id(team)
        return self.bid if self.bid and self.bid.lower() == want.lower() else want

    def before_push(self, packs):
        pass

    def push(self, g, pack):
        try:
            return self.W.push_pack(self.udid, self.bid, str(pack), str(self.devpush) if self.devpush.is_file() else None, accept_low_space=self.c.r.get("acceptLowSpace") is True,
                                    say=lambda m: self.c.emit("message", m), docs=game_docs(g), ctx=game_ctx(g), apply_after=not img_pack(pack) and not game_ctx(g),   # 容器传完即装好（同 Mac push_pack）
                                    pack_root=pick_pack_root(self.c, self, g),
                                    progress=lambda d, t, k, n: push_progress(self.c, d, t, k, n),
                                    stage=lambda name, d=None, t=None: push_stage(self.c, name, d, t))
        except self.W.Fail as e:
            if e.err == "manual_unpack_required":
                self.c.diagnostic(str(e), code=e.err, severity="warning")
                self.pending = str(e)
                return e.transferred
            raise Failure(str(e), code=e.err or "resource_transfer_failed") from e

    def purge(self, g):
        with tempfile.TemporaryDirectory(dir=self.c.private) as w:
            return purge_game(self.client(w), g)


def sign_mode(c):
    """这次用什么签的 App（设备记录 signed.mode，总控台写签名方式）：自己的开发者证书 → devCert；Apple ID（Xcode / Impactor）→ appleId"""
    return "devCert" if devcert.active(c) else "appleId"


def mac_plume(c):
    """0.6.0 通用壳 Mac 的 Apple ID（11-17 方案 A）：改用随包 plumesign 登录、签名、自管私钥——系统 codesign 不给程序库 dylib 带权限，
    免费团队签的程序库手机不认；plumesign 的私钥在自己手里，程序库就能用 s2a-selfsign 按免费团队认的签法签，私钥也能交给手机自签。
    Mac、没在用自己的开发者证书 → True，并把 plumesign 数据目录（账号存档 + keys/<团队>/key.pem）定在私有目录 plumesign/"""
    if os.name == "nt" or devcert.active(c):
        return False
    plume_home(c)
    return True


def plume_home(c):
    """Mac 的 plumesign 数据目录（账号存档 + keys/<团队>/key.pem）定在私有目录 plumesign/：作业进程内有效（impactor.data_base / plume_env 都认它）"""
    home = Path(c.private) / "plumesign"
    home.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.environ[win_install_mod(c).impactor.PLUME_HOME_ENV] = str(home)


def plumesign_exe(c, W):
    """随包 plumesign（启动器 Resources/ThirdParty/Impactor）> S2A_PLUMESIGN / PATH（源码树开发）；都没有 → None"""
    return W.plumesign_path((W.impactor.bundled(c.r.get("resources")) or {}).get("plumesign"))


APPLE_SIGN_IN = "还没用 Apple ID 登录：在 S2Arm 里用你的 Apple ID 登录一次（邮箱、密码、手机上的验证码），再装到 iPhone。"


def plume_account(c, W):
    """plumesign 账号存档里选中的账号 → {account, team, teamName}；没登录 / 没选中 → None。teamName：plumesign 命令行给不出团队名，
    用账号存档里的名字（first_name；个人团队的团队名就是本人姓名）"""
    try:
        with open(Path(W.impactor.data_base()) / "PlumeImpactor" / "accounts.json", encoding="utf-8") as f:
            store = json.load(f)
        acct = store.get("selected_account")
        got = (store.get("accounts") or {}).get(acct or "") or {}
    except (OSError, ValueError, AttributeError):
        return None
    team = W.impactor.selected_team()
    return dict(account=acct, team=team, teamName=got.get("first_name") or "") if acct and team else None


class PlumeMacPhone(MacPhone):
    """Mac 通用壳 Apple ID（mac_plume）：账号是随包 plumesign 自己登录的那个（作业 appleLogin），签名装机同 Windows
    （plumesign sign --register-and-install，install_win.install_app），包名同 Mac 容器包名；备份 / 推 / 删 / 查进程照旧 Mac（devicectl + s2a-devpush）"""

    def __init__(self, c, dev=None, udid=None):
        self.W = win_install_mod(c)
        acct = plume_account(c, self.W)
        if not acct:
            raise Failure(APPLE_SIGN_IN, code="apple_not_signed_in")
        # free：plumesign 登录时判不了付费 / 免费，先按免费记；装好后按签好的包里的真实期限改（WinPhone.install，#1772）
        super().__init__(c, team=dict(id=acct["team"], name=acct["teamName"] or acct["account"], free=True), dev=dev, udid=udid)

    install = WinPhone.install                       # plumesign 签名装机（含 9401 旧写法、设备已登记、小组件去掉重来）
    say = WinPhone.say                               # install 里交给 plumesign 的进度回调（没借它：Mac 免费账号装机一调就 AttributeError）
    bundle_for_signing = WinPhone.bundle_for_signing

    def install_devcert(self, ipa, steps):   # 证书签名用户不走这个类（mac_plume 已排除）
        raise AssertionError("PlumeMacPhone 不处理证书签名")  # 非玩家句


class ExternalPhone(MacPhone):
    """Mac：别的工具（AltStore / SideStore / ESign …）装的 S2Arm（11-19 走查结构 4）：不要 Apple ID、不签名、不装 App，包名就是手机上装着的那个。
    只推文件进它的容器：程序包是未签名的 .s2arm（进「放这里导入」，手机用导入的证书签），资源包照旧按 sha256 推；备份 / 推 / 查进程照旧 Mac（devicectl + s2a-devpush）"""
    external = True

    def __init__(self, c, bid, dev=None, udid=None):
        super().__init__(c, team=dict(id=None, name="", free=True), dev=dev, udid=udid, bid=bid)

    def install(self, *a, **k):   # 这类手机不由启动器装 App（会新建证书、顶掉玩家 AltStore 的证书）
        raise AssertionError("别的工具装的 S2Arm 不由启动器装")  # 非玩家句


def mac_phone(c, team=None, dev=None, udid=None):
    return PlumeMacPhone(c, dev, udid) if mac_plume(c) else MacPhone(c, team, dev, udid)


def phone_for(c):
    return WinPhone(c) if os.name == "nt" else mac_phone(c)


def push_phone(c):
    """推游戏（推到 iPhone / 更新这款 / 更新所选）用的手机对象（11-19 走查结构 4）：手机上装的是别的工具的 S2Arm（external_s2arm）→ 推进那个包名
    （Mac：ExternalPhone，不要 Apple ID；Windows：WinPhone 换成那个包名），程序包不签、由手机用导入的证书签，不进签名 / 新建证书那条路（phone_signing）；
    否则同 phone_for。不读电脑的签名方式（sign_mode）：电脑选什么，手机上照样可能是 AltStore 装的"""
    if os.name == "nt":
        ph = WinPhone(c)
        who, bid = s2arm_owner(c, ph.udid, layout.load_device(c, ph.key))
        if who == "choose":
            raise Failure(PHONE_APP_CHOICE, code="phone_app_choice_required")
        if who == "external":
            ph.bid, ph.external = bid, True
        return ph
    dev = phone(c)
    udid = udid_of(c, dev["id"])
    who, bid = s2arm_owner(c, udid, layout.load_device(c, udid or dev["id"]))
    if who == "choose":   # 装着不止一个、玩家还没选：不替玩家挑，总控台列出来让他选（phoneChooseApp）
        raise Failure(PHONE_APP_CHOICE, code="phone_app_choice_required")
    return ExternalPhone(c, bid, dev, udid) if who == "external" else mac_phone(c, dev=dev, udid=udid)


GID_RE = re.compile(r"[0-9A-F]{16}")
# 机上目录不存在：s2a-devpush ls 的结构化错误码 S2A_AFC_ERR 8（AFC ObjectNotFound）；旧工具 / devicectl 只有原文，兜底认
#   「No such file」与「Object not found / ObjectNotFound」（带不带空格、大小写都认）。玩家实机（Windows 0.2.7、iOS 27.0.1）原文是
#   「afc error: Object not found」，原先只认 ObjectNotFound，首次装机（App 在、还没有 Documents/games）被当成读不出设备挡住（v0.2.8 热修）
AFC_NOT_FOUND = 8


def dir_missing(out):
    m = re.search(r"S2A_AFC_ERR (\d+)", out or "")
    if m:
        return int(m.group(1)) == AFC_NOT_FOUND
    return bool(re.search(r"no such file|object\s*not\s*found", out or "", re.I))


PREEMPT_OPERATIONS = ("installUpdatePrepare",)   # 前台装机让它们先停的后台自动作业：同 main.AUTO_HEAVY（单测锁住两处一致；main 是入口脚本，这里不 import 它）


@contextlib.contextmanager
def packer_phone_lock(c):
    """装机类作业（装到 iPhone / 更新 / 装回上一版 / 从手机删 / 推导出包）一次一个（ipa.phone_lock）。拿锁前先取消正在跑的后台自动作业
    （core.preempt_jobs，11-17 #1450：玩家点的优先，不在降到最低优先级的后台改写后面排队）"""
    import ipa as packer
    from core import preempt_jobs
    preempt_jobs(c, PREEMPT_OPERATIONS, say=lambda t: trace(c, t))
    trace(c, "拿手机锁…")
    with packer.phone_lock(c):
        trace(c, "拿到手机锁")
        yield


def trace(c, text):
    """装机过程记进本任务日志（core.trace，进诊断包）：进锁前后、各款改写、组容器的起止（11-17 #1450：原先只看得到停在第几步）"""
    from core import trace as _trace
    _trace(c, text)


def phone_bid(ph):
    """这次要装的包名：这台记录过的；Windows 第一次装还没记录 → 按 Impactor 当前账号算（同 WinPhone.bundle_for_signing）"""
    return ph.bid or (ph.bundle_for_signing() if hasattr(ph, "bundle_for_signing") else None)


def phone_games(c, ph):
    """这台手机上的 S2Arm 里有哪些游戏——真相在手机上，不在某一台电脑的设备记录里（同一台手机 Mac 装几款、Windows 再装一款时，
    各自的记录只有自己装的）。列这个包名的资源包新旧两处游戏目录（PACK_GAMES、Documents/games）：只认目录名是 16 位游戏号、
    pack_listing 选出的那处资源包已解开（.applied）的，别的跳过。
    进不了容器时问平台这个包名装没装（ph.app_missing）：确实没装 → None（和「装着、一款都没有」的空集分开：没装时本机记录的各款也不在手机上了，
    container_others 据此不再带）。装着却列不出、或连装没装都判不了（锁屏 / 断开 / 工具原文认不出）：照装机原则（11-11「认不出的直接装」）
    按「这处没有别的游戏」继续，只记进日志——最坏是这台电脑不知道的别的款这次没带进容器，它们的资源包和存档还在手机上，回原电脑再装就回来"""
    bid = phone_bid(ph)
    if not bid or not ph.udid:
        return set()
    with tempfile.TemporaryDirectory(dir=c.private) as w:
        try:
            cl = devpush_client(c, ph.udid, bid, w)
        except Failure as e:   # 工具不在（预检清单 #29）：同「列不出」——按没有别的游戏继续装，不拦整单
            note(c, f"列不出{_who(ph)}上装了哪些游戏，按没有别的游戏继续装（{e}）")
            return set()
        names = set()
        for root in (DOCS_GAMES, PACK_GAMES, PACK_GAMES_ALT):
            rc, out = cl.call("ls", root)
            if rc:
                if dir_missing(out):   # 这处还没建（App 在、还没装过任何一款 / 只有新或旧一处）：这处当 0 款，装机照常（推资源包时自会建目录）
                    continue
                if ph.app_missing(bid, out):
                    return None
                note(c, f"读不出{_who(ph)}上 {root} 里装了哪些游戏，按这处没有别的游戏继续装（s2a-devpush ls：{out[-300:].strip()}）")
                continue
            names |= {n.strip().rstrip("/").upper() for n in out.splitlines()}
        return {g for g in names if GID_RE.fullmatch(g) and ".applied" in (pack_listing(cl, g, ph)[1] or [])}


PHONE_LEDGER = "Documents/.s2a-games.json"   # 手机 App 写的游戏账本（11-16 维护者「手机端得有个记录在根目录下」）：{"version": 1, "games": {游戏号: {"state": "present" | "removed", "at", "program"}}}


def _who(ph):
    info = getattr(ph, "info", None) or {}
    return f"{info.get('deviceKind') or 'iPhone / iPad'}「{info.get('deviceName')}」" if info.get("deviceName") else (info.get("deviceKind") or "设备")


def pack_listing(cl, gid, ph=None):
    """本款资源包在手机上的哪一处 → (资源包根, 根下列表或 None)：新位置在 → 新位置；新位置不在、旧位置在 → 旧位置（App 下次启动整个同盘挪到新位置，
    含推进 .incoming 的包；增量仍按旧位置的 index.applied.tsv 算，不因新位置是空的整份重推几 GB）；两处都不在（第一次装这款）→ (新位置, None)。
    列不出（锁屏 / 断开等，不是「目录不在」）：照装机原则按「两处都不在」推到新位置继续，只记进日志（最多整份重推一次）。
    老位置真有一份旧的也不会两份长期并存：App 下次打开游戏库时 s2a_pack_migrate_one 见新旧都在、新的已应用，就删掉老的（runtime/src/pack_apply.c）"""
    rc, out = cl.call("ls", PACK_GAMES_ALT)   # 手机上有备用位置（新壳搬过）：宿主只认它，这款在不在都推它
    if not rc:
        root = game_pack(gid, PACK_GAMES_ALT)
        rc, out = cl.call("ls", root)
        return root, (None if rc else out.splitlines())
    for root in (game_pack(gid), game_pack_old(gid)):
        rc, out = cl.call("ls", root)
        if not rc:
            return root, out.splitlines()
        if not dir_missing(out):
            if getattr(ph, "c", None) is not None:
                note(ph.c, f"读不出{_who(ph)}上 {gid} 的资源放在哪（{root}），按第一次装推到新位置（s2a-devpush ls：{out[-300:].strip()}）")
            return game_pack(gid), None
    return game_pack(gid), None


def pick_pack_root(c, ph, gid):
    """推这款资源包推到哪（推送、列机上文件、判已应用都用这一处，push_pack.py --pack-root）：规则见 pack_listing"""
    with tempfile.TemporaryDirectory(dir=c.private) as w:
        return pack_listing(devpush_client(c, ph.udid, phone_bid(ph), w), gid, ph)[0]


def purge_game(cl, gid):
    """删手机上这款的数据：文档根（存档 / 金手指 / 解包日志，连同旧位置的资源包）+ 新位置本款目录（资源包）。→ (都删掉?, 输出)"""
    ok1, out1 = cl.rm(game_docs(gid))
    ok2, out2 = cl.rm(f"{PACK_GAMES}/{gid.upper()}")
    ok3, out3 = cl.rm(f"{PACK_GAMES_ALT}/{gid.upper()}")   # 备用位置（不在算删掉）
    return ok1 and ok2 and ok3, "\n".join(o for o in (out1, out2, out3) if o)


def phone_game_name(c, ph, gid, names):
    """确认框里的名字：本机游戏库有就用库里的，否则读手机上这款资源包自带的 control.nacp，再不行用游戏号"""
    if names.get(gid):
        return names[gid]
    try:
        import library
        with tempfile.TemporaryDirectory(dir=c.private) as w:
            dst = Path(w) / "control.nacp"
            cl = devpush_client(c, ph.udid, phone_bid(ph), w)
            if any(cl.get(f"{root}/ctl/control.nacp", dst) for root in (game_pack(gid, PACK_GAMES_ALT), game_pack(gid), game_pack_old(gid))):
                dump = library.engine_tool(c, "ns2_dump")
                n = dst.read_bytes()
                return dump.nacp_title(n, 14) or dump.nacp_title(n, 0) or gid
    except Exception:   # noqa: BLE001 —— 名字读不到就用游戏号
        pass
    return gid


IMPORT_DIRS = ("放这里导入", "Import Here")   # 非玩家句（App 的文件夹名）；手机 App 的导入文件夹（runtime/src/pack_apply.c s2a_img_import_dirs 认的两个名字，App 按界面语言只建一个）


def look_of(custom):
    """启动器里改的游戏名 / 封面（ipa.container_custom 的一项）→ 指纹（名字 + 封面内容摘要；没改 = 原名原图同样有指纹，变了才重推）"""
    custom = custom or {}
    cover = Path(custom["cover"]) if custom.get("cover") else None
    h = hashlib.sha256((custom.get("name") or "").encode("utf-8"))
    if cover is not None and cover.is_file():
        h.update(hashlib.sha256(cover.read_bytes()).digest())
    return h.hexdigest()[:16]


def import_inbox(cl, docs="Documents"):
    """推 .s2arm 放哪：文档根下 App 已建的那个导入文件夹（按界面语言只建一个，写进另一个名字会在「文件」里多出一个文件夹）；
    列不出 / 都还没建 → Import Here（App 两个名字都扫）"""
    names = cl.ls(docs) if hasattr(cl, "ls") else None
    have = next((n for n in IMPORT_DIRS if names and n in [x.strip() for x in names]), None)
    return f"{docs}/{have or IMPORT_DIRS[1]}"


def push_program(cl, gid, pkg, inbox, custom=None, cat=None):
    """签好的程序包（program_pack.sign 的目录）推进手机（11-19 维护者定：电脑推送与玩家分享同一个 .s2arm、同一条安装路径）：
    打成一个 .s2arm（同电脑导出两件套的格式：zstd 一帧带内容校验、里面 ustar，第一个条目 s2arm.json；只放程序 program/，资源包照旧由 push_pack 按 sha256 推），
    一次写进手机 App 的导入文件夹 inbox。App 用导入同一条代码解开、校验、写进自己的版本目录 program/v-<sha16>-s2arm/、最后改名换上清单
    （runtime/src/pack_apply.c s2a_s2arm_import）：App 开着时盯着导入文件夹、静下来约 1 秒就导；没开就下次打开 / 回前台时导；还没传完的判「还在拷」留着下次看。
    清单带着这台的团队号（程序已在电脑上签好），手机导入后不再签。原先逐个文件推（大游戏 1544 个文件、每个两个工具进程，约一小时）、电脑与 App 各有一套写入规则。
    续推：机上导入文件夹里已有同一份（长度与开头一页都对，App 还没导走）就不重推；没推完的整份重推（一个文件，按数据线带宽走）。
    custom = 启动器里改的游戏名 / 封面：名字写进清单，封面换掉程序包里的 Games/<号>/icon.jpg。
    → 设备记录里这款的 program 记录 {sha, team, host_abi, rewriter_id, dir, look}"""
    import programs
    import program_pack
    import tarfile
    gid, pkg = gid.upper(), Path(pkg)
    # cat：未签名包（别的工具装的 S2Arm，push_programs team 为空）没有 catalogue.json 文件，由调用方现算传进来；团队号为空 = 手机导入后用导入的证书签
    cat = cat or json.loads((pkg / program_pack.CATALOGUE).read_text(encoding="utf-8"))
    vdir = f"v-{cat['image_sha256'][:16]}-s2arm"   # 手机导入写的版本目录（pack_apply.c s2a_s2arm_import）
    shown = dict(cat)
    files = [("program/" + p.relative_to(pkg).as_posix(), p) for p in sorted(pkg.rglob("*"))
             if p.is_file() and p.name not in (program_pack.META, program_pack.KEY, program_pack.CATALOGUE)]
    if custom and custom.get("name"):
        shown["name"] = custom["name"]
    if custom and custom.get("cover") and Path(custom["cover"]).is_file():
        icon = f"program/Games/{gid}/icon.jpg"
        files = [x for x in files if x[0] != icon] + [(icon, Path(custom["cover"]))]
        shown["icon"] = f"Games/{gid}/icon.jpg"
    cat_bytes = json.dumps(shown, ensure_ascii=False, indent=1).encode("utf-8")
    head = dict(format=S2ARM_FORMAT, schema=1, title_id=gid, name=shown.get("name", ""), display_version=shown.get("display_version", ""),
                host_abi=shown["host_abi"], chain=shown.get("chain", ""), rewriter_id=shown.get("rewriter_id", ""), image=shown["image"],
                image_sha256=shown["image_sha256"], bytes=len(cat_bytes) + sum(p.stat().st_size for _, p in files),
                signer=dict(mode="push", team=shown.get("team")))
    dest = f"{inbox}/{gid}.s2arm"
    with tempfile.TemporaryDirectory(prefix=".s2arm-push-", dir=pkg.parent) as w:
        out = Path(w) / f"{gid}.s2arm"
        try:
            with programs.zstd_writer(out, checksum=True) as z, tarfile.open(fileobj=z, mode="w|", format=tarfile.USTAR_FORMAT) as t:
                def add(name, data=None, path=None):
                    info = tarfile.TarInfo(name)
                    info.mode, info.mtime = 0o644, 0
                    if path is None:
                        info.size = len(data)
                        t.addfile(info, io.BytesIO(data))
                        return
                    info.size = path.stat().st_size
                    with open(path, "rb") as f:
                        t.addfile(info, f)
                add("s2arm.json", json.dumps(head, ensure_ascii=False, indent=1).encode("utf-8"))
                add("program/catalogue.json", cat_bytes)
                for name, path in files:
                    add(name, path=path)
        except ValueError as e:   # ustar 放不下的路径（手机只认 ustar）
            raise Failure(f"{gid} 有文件路径太长，打不进 .s2arm：{e}", code="job_failed") from e  # 非玩家句
        size = out.stat().st_size
        got = cl.head(dest, 4096) if hasattr(cl, "head") else None
        with open(out, "rb") as f:
            first = f.read(4096)
        if not (got and got[0] == size and got[1] == first[:len(got[1])]):
            try:
                cl.put(out, dest)
            except Exception as e:   # push_pack.Fail：已是给玩家的话
                if isinstance(e, Failure):
                    raise
                raise Failure(str(e), code=getattr(e, "err", None) or "resource_transfer_failed") from e
    return dict(sha=cat["image_sha256"], team=cat.get("team"), host_abi=cat["host_abi"], rewriter_id=cat.get("rewriter_id", ""), dir=vdir, look=look_of(custom))


def push_programs(c, cl, games, rec, team, bundle, signer, docs="Documents"):
    """通用壳装机：每款按设备记录判要不要推（program_pack.need），要推的出包、按这台的团队签、打成 .s2arm 推进 App 的导入文件夹（push_program）。
    rec = 设备记录；docs = 手机上 App 的文档根（导入文件夹在它下面；电视是 Library/Caches/S2Arm）。→ {游戏号: program 记录}（没推的照旧记录原样带回）；推到一半失败照抛，已推好的由调用方记下"""
    import program_pack
    import ipa as packer
    looks = packer.container_custom(c)
    out = {}
    inbox = None
    for g in games:
        old = (rec.get("games", {}).get(g) or {}).get("program")
        cat = None
        if team:
            pkg = program_pack.sign(c, g, team, bundle, signer)
        else:   # 别的工具装的 S2Arm（push_phone）：不签——build 出的程序库自带 ad-hoc 签名（手机签名要它，别去掉），清单团队号为空，手机导入后用导入的证书签
            pkg = program_pack.build(c, g)
            meta = json.loads((Path(pkg) / program_pack.META).read_text(encoding="utf-8"))
            cat = program_pack.catalogue(g, meta, Path(pkg) / program_pack.image_name(g), None)
        custom = next((v for k, v in looks.items() if k.upper() == g.upper()), None)
        want = dict(cat or json.loads((Path(pkg) / "catalogue.json").read_text(encoding="utf-8")), look=look_of(custom))
        why = program_pack.need(c, g, old, team, want=want)
        if not why:
            out[g] = old
            continue
        note(c, f"{g} 的程序推进手机（{'、'.join(sorted(why))}）")  # 非玩家句
        inbox = inbox or import_inbox(cl, docs)
        out[g] = push_program(cl, g, pkg, inbox, custom=custom, cat=cat)
    return out


PROGRAM_SENT = "已送到手机，打开 S2Arm 时自动装好。"   # 推完程序包、手机还没导入（App 没开 / 等不到）时完成页的那句（结果 unpackPending）
# 别的工具装的 S2Arm 推完核签名身份（导入不签名、起局时才签：没身份或团队对不上，推完别报「装好了」）。文案维护者 11-20 定（译文进 Localizable.json 与 l10n 四语）
EXTERNAL_NO_IDENTITY = "已送到手机，但手机上的 S2Arm 还没有签名证书，这款还开不了：在手机上打开 S2Arm → 齿轮 →「签名证书」，SideStore 装的点「从 SideStore 获取」；AltStore 装的先在 AltStore 设置里导出签名证书，再点「导入 .p12…」。"
EXTERNAL_IDENTITY_TEAM = "已送到手机，但手机上的 S2Arm 导入的签名证书（团队 {0}）不是给这个 S2Arm 签名的那张（团队 {1}），这款还开不了：在手机上打开 S2Arm → 齿轮 →「签名证书」，SideStore 装的点「从 SideStore 获取」；AltStore 装的先在 AltStore 设置里重新导出签名证书，再点「导入 .p12…」。"


def external_identity_gap(c, ph):
    """别的工具装的 S2Arm 推完：手机上的签名身份够不够起这款 → (code, 那句话) | None（够）。
    没导入（current.json 不在）→ phone_identity_missing；导入了、但团队与手机上这个包的团队（installed_s2arm 读得到时）对不上 → phone_identity_team_mismatch"""
    meta = phone_identity_meta(c, ph.udid, ph.bid)
    if not meta:
        return "phone_identity_missing", EXTERNAL_NO_IDENTITY
    team = next((a["team"] for a in installed_s2arm(c, ph) or [] if a["bundleId"].lower() == ph.bid.lower()), None)
    got = meta.get("team") or ""
    if team and got and got != team:
        return "phone_identity_team_mismatch", EXTERNAL_IDENTITY_TEAM.format(got, team)
    return None
PROGRAM_WAIT = "等 iPhone 上的 S2Arm 装好这款的程序"
IMPORT_WAIT_S = (300, 20)   # App 开着：等它导入的上限（大游戏 1.6 GB 程序包在手机上解开、校验）；查不清开没开（Windows 没有 devicectl）：只看这么久


def program_imported(cl, gid, inbox, want, work):
    """手机上这款的程序包换上了没有：导入文件夹里的 <号>.s2arm 已被 App 导走（导好才删，失败原样留着），且清单 dir 是这次推的那版"""
    left = cl.ls(inbox)
    if left is None or f"{gid}.s2arm" in [x.strip() for x in left]:
        return False
    dst = Path(work) / "catalogue.json"
    if not cl.get(f"{PACK_GAMES}/{gid}/program/catalogue.json", dst):
        return False
    try:
        return json.loads(dst.read_text(encoding="utf-8")).get("dir") == want
    except ValueError:
        return False


def await_import(c, ph, gid, want):
    """推完程序包后照实报（11-19 维护者定）：App 开着就等它用导入那条代码解开、清单换上再报完成；App 没开、查不清又没等到、等到上限还没换上
    → PROGRAM_SENT（作业结果 unpackPending，两端完成页显示这句、不说「已推到 iPhone」）。→ None（已装好）/ 那句话"""
    running = ph.app_running()
    if running is False:
        return PROGRAM_SENT
    with tempfile.TemporaryDirectory(dir=c.private) as w:
        try:
            cl = ph.client(w)
        except Failure:
            return PROGRAM_SENT
        inbox = import_inbox(cl)
        end = time.monotonic() + IMPORT_WAIT_S[0 if running else 1]
        while not program_imported(cl, gid, inbox, want, w):
            if time.monotonic() >= end:
                return PROGRAM_SENT
            c.check()
            time.sleep(2)
    return None


SHELL_INSTALL_FIRST = "手机上还没装 S2Arm：先点「安装 S2Arm」，装好后再把这款推到 iPhone。"
SHELL_UPDATE_FIRST = "手机上的 S2Arm 不是这一版：先点「更新 S2Arm」，再把这款推到 iPhone。"


def win_selfsign_signer(c, W, exe, team):
    """Windows Apple ID：plumesign 存的私钥文件（keys/<团队>/key.pem）+ 苹果列表里它对应的那张证书 → s2a-selfsign 签程序库（免费团队也认）。
    证书按私钥指纹缓存在私有目录（plume_cert.json），私钥没换就不再联网列证书。缺工具 / 缺私钥 / 列不出证书 → None"""
    import program_pack
    tool = program_pack.selfsign_path(c)
    key = W.plume_key_path(team) if hasattr(W, "plume_key_path") else None
    if not tool or not key or not Path(key).is_file():
        return None
    fp = W.key_fingerprint(team)
    cache = c.private / "plume_cert.json"
    got = (read(cache, {}) or {}).get(team) or {}
    if got.get("key_fp") == fp and got.get("cert"):
        cert = base64.b64decode(got["cert"])
    else:
        ident = W.plume_identity(exe, team, log=c.logs / (c.task + "-identity.log")) if exe else None
        if not ident:
            trace(c, f"列不出团队 {team} 私钥对应的证书，这次程序照旧用 plumesign 签")  # 非玩家句
            return None
        cert = ident["cert"]
        atomic(cache, dict(read(cache, {}) or {}, **{team: dict(key_fp=fp, cert=base64.b64encode(cert).decode("ascii"))}))
    return program_pack.selfsign_signer(c, tool, key, cert)


def phone_signing(c, ph):
    """通用壳：这台手机上程序包按什么签 → (团队号, 包名, 签名器)。同装机签 App 的那一处：开发者证书 → 证书的团队与描述文件的包名；
    Windows Apple ID / Mac 通用壳 Apple ID（PlumeMacPhone）→ plumesign 当前账号的团队、容器包名；开关关着的 Mac Apple ID → 钥匙串账号的团队、容器包名"""
    import program_pack
    if devcert.active(c):
        return devcert.team(c)["id"], devcert.state(c)["bundleId"], program_pack.devcert_signer(c)
    if os.name == "nt" or isinstance(ph, PlumeMacPhone):   # Mac 通用壳 Apple ID 同 Windows：plumesign 的账号与私钥（mac_plume）
        team = ph.W.impactor.selected_team() if hasattr(ph.W.impactor, "selected_team") else None
        if not team:
            if os.name != "nt":
                raise Failure(APPLE_SIGN_IN, code="apple_not_signed_in")
            raise Failure("读不到 Impactor 里登录的 Apple ID：在 Impactor 里登录后再推到 iPhone。", code="apple_not_signed_in")   # 原先报 Mac 的 xcode_account_missing，下一步叫 Windows 玩家开 Xcode
        import ipa as packer
        exe = ph.W.plumesign_path((ph.W.impactor.bundled(c.r.get("resources")) or {}).get("plumesign"))
        win_cert_gate(c, ph.W, exe)
        signer = win_selfsign_signer(c, ph.W, exe, team)
        if signer is None:   # 源码树没编 s2a-selfsign / 本机还没有这个团队的私钥：照旧 plumesign 签（付费团队能用，免费团队要手机重签）
            shell = Path(packer.container_shell(c, packer.current_shell_id(c))) / "root"
            signer = program_pack.win_signer(c, exe, shell)
        return team, ph.bundle_for_signing() or ph.bid, signer
    raise AssertionError("没有证书时手机对象只会是 WinPhone / PlumeMacPhone")  # 非玩家句


PHONE_SAVE_MARK = ".s2a-save-backup.json"   # 通用壳存档快照的记号（设备、内容摘要、时刻）：只有带它的备份参与「同内容不新建、每台每款留 10 份」
PHONE_SAVE_KEEP = 10


def _save_digest(d):
    h = hashlib.sha256()
    for p in sorted(x for x in Path(d).rglob("*") if x.is_file() and x.name != PHONE_SAVE_MARK):
        h.update(p.relative_to(d).as_posix().encode("utf-8") + b"\0" + hashlib.sha256(p.read_bytes()).digest())
    return h.hexdigest()


def _marked(d):
    return read(Path(d) / PHONE_SAVE_MARK, None) if (Path(d) / PHONE_SAVE_MARK).is_file() else None


def snapshot_phone_saves(c, ph, games):
    """0.6.0 通用壳（11-17 维护者定，《文件与程序管理体系》）：推游戏、装壳都不再覆盖手机上的存档，所以在它们开头顺手把这几款的手机存档拉回电脑
    （ph.backup，同覆盖装前那份 games/<号>/save_backup/<时刻>/），和这台手机这款的上一份比内容：一样就删掉刚拉的、不新建；变了留下并记上记号，
    这台这款带记号的只留最近 PHONE_SAVE_KEEP 份。没带记号的旧备份（覆盖装前拉的）一份不删。拉不回（锁屏 / 断开 / 没装过）只记一行，不挡推送。
    → {游戏号: 这份存档在电脑上的备份目录名（新建的或内容相同的上一份）}；开关关着 → {}"""
    out = {}
    for g in [str(x).upper() for x in games]:
        try:
            dest = ph.backup(g)
        except Failure as e:   # 包括 save_backup_failed：这里不覆盖存档，备份不了只记一行
            trace(c, f"{g} 的手机存档这次没拉回电脑（不挡推送）：{str(e)[:200]}")  # 非玩家句
            continue
        if not dest or not Path(dest).is_dir():
            continue
        dest = Path(dest)
        digest = _save_digest(dest)
        mine = sorted((d for d in dest.parent.iterdir() if d.is_dir() and d != dest and (_marked(d) or {}).get("device") == ph.key), key=lambda d: d.name)
        if mine and (_marked(mine[-1]) or {}).get("digest") == digest:
            shutil.rmtree(dest, ignore_errors=True)
            out[g] = mine[-1].name
            continue
        files = [p for p in dest.rglob("*") if p.is_file()]
        atomic(dest / PHONE_SAVE_MARK, dict(device=ph.key, digest=digest, at=time.time(), files=len(files), bytes=sum(p.stat().st_size for p in files)))
        for old in (mine + [dest])[:-PHONE_SAVE_KEEP]:
            shutil.rmtree(old, ignore_errors=True)
        out[g] = dest.name
    return out


def phone_saves(c):
    """手机存档在电脑上的备份（作业 phoneSaves，只读）：每款 games/<号>/save_backup/ 下的每一份，新的在前。带记号的给设备、时刻、
    文件数、字节数；覆盖装前拉的旧备份没有记号，给 legacy: True（设备不知道）。请求带 gameId 只列这一款"""
    want = (c.r.get("gameId") or "").upper()
    games = []
    for g in c.catalog()["games"]:
        gid = g["id"].upper()
        if want and gid != want:
            continue
        root = layout.path(c, gid, "save_backup", create=False)
        rows = []
        for d in sorted((x for x in root.iterdir() if x.is_dir()), key=lambda x: x.name, reverse=True) if root.is_dir() else []:
            m = _marked(d)
            if m:
                rows.append(dict(id=d.name, device=m.get("device"), at=m.get("at"), files=m.get("files"), bytes=m.get("bytes"), legacy=False))
            else:
                files = [p for p in d.rglob("*") if p.is_file()]
                rows.append(dict(id=d.name, device=None, at=d.stat().st_mtime, files=len(files), bytes=sum(p.stat().st_size for p in files), legacy=True))
        games.append(dict(gameId=gid, backups=rows))
    return dict(games=games)


def restore_phone_save(c):
    """把电脑上的一份存档备份还原到手机（作业 phoneSaveRestore：gameId、device、backup = phoneSaves 给的 id）。
    先拉一份手机现在的存档（snapshot_phone_saves，内容没变就不多存），再把备份里的文件写回手机这款的存档目录、删掉手机上备份里没有的文件。
    手机上 S2Arm 明确在运行就停下（边玩边换存档会写坏）；判不清照常还原。通用壳开关后面"""
    gid = (c.r.get("gameId") or "").upper()
    name = str(c.r.get("backup") or "")
    root = layout.path(c, gid, "save_backup", create=False)
    src = root / name
    if not name or "/" in name or "\\" in name or name.startswith(".") or not src.is_dir():
        raise Failure("找不到这份存档备份：请在列表里重新选一份。", code="save_invalid")
    c.begin("把存档还原到 iPhone", 1, 2)
    ph = phone_for(c)
    try:
        running = ph.playing([gid]) if hasattr(ph, "playing") else ph.app_running()
    except Exception:   # noqa: BLE001 —— 判不清照常还原
        running = None
    if running is True:
        raise Failure("先在 iPhone 上退出 S2Arm，再还原存档。", code="phone_app_running")
    now = snapshot_phone_saves(c, ph, [gid]).get(gid)   # 还原前手机上的存档也留一份（内容和已有的一样就不多存）
    have = {p.relative_to(root / now).as_posix() for p in (root / now).rglob("*") if p.is_file() and p.name != PHONE_SAVE_MARK} if now else set()
    files = sorted(p for p in src.rglob("*") if p.is_file() and p.name != PHONE_SAVE_MARK)
    keep = {p.relative_to(src).as_posix() for p in files}
    base = f"{game_docs(gid)}/{SAVE_REL}"
    c.begin("写回存档文件", 2, 2)
    with tempfile.TemporaryDirectory(dir=c.private) as w:
        cl = ph.client(w)
        for p in files:
            cl.put(p, f"{base}/{p.relative_to(src).as_posix()}")
        for rel in sorted(have - keep):
            cl.rm(f"{base}/{rel}")
    return dict(gameId=gid, restored=name, files=len(files), removed=len(have - keep), safetyBackup=now or "")


IDENTITY_INCOMING = "Library/Application Support/S2Arm/identity/incoming"   # App 启动时导入钥匙串、导入成功就删（手机侧，0.6.0）


# App 一打开就导入钥匙串、立刻删这三份（11-17 维护者：私钥是玩家自己的，进他自己账号的加密备份也无妨，不另放 Caches）。
#   已交没交按手机上的现状判：App 导入后写的 current.json / 还没导入的 incoming 清单里是同一张证书就不重交
IDENTITY_INCOMING = "Library/Application Support/S2Arm/identity/incoming"
IDENTITY_CURRENT = "Library/Application Support/S2Arm/identity/current.json"   # App 导入成功后写（团队号、证书 sha1，不含私钥）


def _identity_on_phone(c, ph, sha1):
    """手机上这张证书已导入（current.json）或还等着导入（incoming/identity.json）→ True；都没有（从没交过 / App 删了重装）→ False"""
    if not sha1:
        return False
    with tempfile.TemporaryDirectory(dir=c.private) as w:
        cl = ph.client(w)
        for rel in (IDENTITY_CURRENT, f"{IDENTITY_INCOMING}/identity.json"):
            got = Path(w) / "probe.json"
            if cl.get(rel, got):
                try:
                    if json.loads(got.read_text(encoding="utf-8")).get("cert_sha1") == sha1:
                        return True
                except ValueError:
                    pass
    return False


IDENTITY_NOT_HERE = "这台电脑上没有这个 Apple ID 能交给手机的签名证书，启动器不会为此新建（账号证书满了时，新建会让苹果撤掉一张，可能正是 AltStore 或 SideStore 在用的那张）。用 AltStore 或 SideStore 装的 S2Arm：在 AltStore 或 SideStore 的设置里导出签名证书，到手机上 S2Arm 的「签名证书」里导入就行。确实想让启动器新建证书，就点「安装 S2Arm」，它会先告诉你可能撤掉哪张证书，再由你决定。"


def _identity_source(c, ph):
    """这次交给 App 的身份从哪来 → (团队号, 私钥指纹, 取身份的函数, source)；交不了 → None。
    证书签名用户（devcert.active，两端都行）：导入时存的 p12；Windows Apple ID 与 Mac 通用壳 Apple ID（PlumeMacPhone）：plumesign 的 keys/<团队>/key.pem。
    开关关着的 Mac Apple ID 借 Xcode 钥匙串，交不了"""
    if devcert.active(c):
        team = devcert.team(c)["id"]
        fp = hashlib.sha256((devcert.folder(c) / "cert.p12").read_bytes()).hexdigest()[:16]   # 同 devcert.identity 的 key_fp
        return team, fp, lambda: devcert.identity(c), "devCert"
    if not hasattr(ph, "W"):   # 带 W 的只有 plumesign 签的手机对象（WinPhone、PlumeMacPhone）
        return None
    team = ph.W.impactor.selected_team()
    fp = ph.W.key_fingerprint(team) if team else None
    if not fp:
        return None
    def plume():
        exe = ph.W.plumesign_path((ph.W.impactor.bundled(c.r.get("resources")) or {}).get("plumesign"))
        return ph.W.plume_identity(exe, team, log=c.logs / (c.task + "-identity.log")) if exe else None
    return team, fp, plume, "appleId"


S2ARM_BID_PREFIX = "io.s2arm.games"   # 启动器装的包名前缀；AltStore / SideStore 等会在后面加一段（常见 .<团队号>），前缀不变
IDENTITY_TEAM_MISMATCH = ("手机上的 S2Arm 是用另一个 Apple ID 或证书签的（团队 {0}），和这台电脑上的签名身份（团队 {1}）不是同一个团队，交过去也签不了游戏："
                          "请在启动器里登录装 S2Arm 用的那个 Apple ID，或选用同一团队的证书。")


def installed_s2arm(c, ph):
    """手机上装着的 S2Arm（启动器装的、AltStore / SideStore / ESign 等别的工具装的都认）→ [{bundleId, team}]（team 读不到为 None）；
    按包名前缀 io.s2arm.games 或主程序名（main_executable）认。列不出（工具旧 / 断开）→ None"""
    return installed_s2arm_on(c, ph.udid)


def installed_s2arm_on(c, udid):
    """同 installed_s2arm，按 UDID 查（总控台逐台现查、推送前还没有手机对象时用）"""
    if not udid:
        return None
    with tempfile.TemporaryDirectory(dir=c.private) as w:
        rows = devpush_client(c, udid, "-", w).apps()
    if rows is None:
        return None
    main = main_executable(c)
    return [dict(bundleId=b, team=t or None) for b, e, t in rows if b.lower().startswith(S2ARM_BID_PREFIX) or e == main]


def s2arm_owner(c, udid, rec):
    """这台手机上用哪个 S2Arm（11-19 走查结构 4、5；总控台与推送共用这一个判断）→ (谁, 包名)：
    ('launcher', 包名)：用的是这台电脑的启动器装的（设备记录 rec 有壳号、记下的包名还装着）；('external', 包名)：用的是别的工具（AltStore / SideStore / ESign …）装的；
    ('choose', None)：装着两个及以上、玩家还没选（或选过的那个已经不在）——让玩家在总控台选（phoneChooseApp），不替玩家挑（11-20 维护者：玩家自选优先，
    原先按「已导入签名身份 → 包名排序」挑，同机还装着别的电脑的启动器装的那份时挑错）；('none', None)：一个都没装；(None, None)：列不出已装 App（工具旧 / 断开）。
    只有一个时直接用它。不看电脑的签名方式（sign_mode 管电脑签不签，与手机上是谁装的正交）"""
    apps = installed_s2arm_on(c, udid)
    if apps is None:
        return None, None
    if not apps:
        return "none", None
    own = (rec.get("bundleId") or "").lower() if rec.get("shellId") else ""
    if len(apps) == 1:
        bid = apps[0]["bundleId"]
    else:
        chosen = (rec.get("chosenBundleId") or "").lower()
        bid = next((a["bundleId"] for a in apps if chosen and a["bundleId"].lower() == chosen), None)
        if not bid:
            return "choose", None
    return ("launcher" if own and bid.lower() == own else "external"), bid


def s2arm_candidates(c, udid, rec):
    """总控台让玩家选的候选（s2arm_owner 判 choose 时列）：手机上装着的每个 S2Arm → [{bundleId, team, byThisLauncher, identityTeam}]，按包名排序；
    byThisLauncher = 这台电脑的启动器装的（设备记录的壳号与包名），identityTeam = 这个 S2Arm 导入的签名证书的团队（没导入为 None）。列不出 → []"""
    apps = installed_s2arm_on(c, udid) or []
    own = (rec.get("bundleId") or "").lower() if rec.get("shellId") else ""
    out = []
    for a in sorted(apps, key=lambda a: a["bundleId"]):
        meta = phone_identity_meta(c, udid, a["bundleId"])
        out.append(dict(bundleId=a["bundleId"], team=a["team"], byThisLauncher=bool(own) and a["bundleId"].lower() == own,
                        identityTeam=(meta or {}).get("team") or None))
    return out


PHONE_APP_CHOICE = "这台 iPhone 上装着不止一个 S2Arm：先在 iPhone 总控台选好用哪一个，再推游戏。"
PHONE_APP_GONE = "选的这个 S2Arm 不在这台 iPhone 上了：在总控台重新选一次。"


def phone_choose_app(c):
    """作业 phoneChooseApp（总控台「选用哪个 S2Arm」，11-20 维护者：玩家自选优先）：请求带 device（UDID）与 bundleId；核实它现在装在这台手机上，
    记进设备记录 chosenBundleId，之后推送 / 总控台都用它，直到它不在了再问。→ {device, bundleId}"""
    udid = (c.r.get("device") or "").strip()
    want = (c.r.get("bundleId") or "").strip()
    if not udid or not want:
        raise Failure("没说要选哪台 iPhone 上的哪一个 S2Arm。", code="job_failed")  # 非玩家句（前端按清单发，不会缺）
    apps = installed_s2arm_on(c, udid)
    if apps is None:
        raise Failure("读不出这台 iPhone 上装了哪些 App：确认 iPhone 连着、已解锁，再试一次。", code="device_not_ready")
    hit = next((a["bundleId"] for a in apps if a["bundleId"].lower() == want.lower()), None)
    if not hit:
        raise Failure(PHONE_APP_GONE, code="phone_app_gone")
    rec = layout.load_device(c, udid)
    layout.save_device(c, udid, dict(rec, chosenBundleId=hit))
    return dict(device=udid, bundleId=hit)


def phone_identity_meta(c, udid, bid):
    """手机上这个 S2Arm 导入过的签名身份（App 导入证书后写 IDENTITY_CURRENT：cert_sha1 / team，不含私钥）→ dict；没导入 / 读不到 → None"""
    if not udid or not bid:
        return None
    with tempfile.TemporaryDirectory(dir=c.private) as w:
        dst = Path(w) / "current.json"
        try:
            if not devpush_client(c, udid, bid, w).get(IDENTITY_CURRENT, dst):
                return None
            got = json.loads(dst.read_text(encoding="utf-8"))
        except (OSError, ValueError, Failure):
            return None
    return got if isinstance(got, dict) else None


def external_s2arm(c, udid, rec):
    """别的工具装的那个 S2Arm 的包名（s2arm_owner 判 external）；启动器装的 / 一个都没装 / 列不出 → None（照旧走启动器那条路）"""
    who, bid = s2arm_owner(c, udid, rec)
    return bid if who == "external" else None


def identity_target(c, ph, team):
    """交签名身份交给手机上的哪个 S2Arm（11-19 维护者：不管 App 是谁装的，团队号对上就交）→ 包名。
    启动器自己的包名装着且团队对得上（或读不到团队）优先；否则挑团队相同的那个；都不是这个团队 → identity_team_mismatch（照实说是哪个团队）。
    列不出已装 App（工具旧 / 断开）→ 照旧用启动器的包名；一个都没装 → shell_install_required"""
    apps = installed_s2arm(c, ph)
    if apps is None:
        return ph.bid
    if not apps:
        raise Failure(SHELL_INSTALL_FIRST, code="shell_install_required")
    ok = [a for a in apps if a["team"] in (team, None)]
    mine = [a for a in ok if ph.bid and a["bundleId"].lower() == ph.bid.lower()]
    known = [a for a in ok if a["team"] == team]
    pick = (mine or known or ok)
    if not pick:
        raise Failure(IDENTITY_TEAM_MISMATCH.format("、".join(sorted({a["team"] for a in apps})), team), code="identity_team_mismatch")
    return pick[0]["bundleId"]


def deliver_identity(c, ph, strict=False, force=False):
    """0.6.0 通用壳：把手机自签要的私钥 + 叶证书交给 App（cmdr 方案「私钥怎么交给 App」，11-17 真机验过 DER 两份能配成签名身份）。
    只走本机 USB（s2a-devpush），写进 App 数据容器 IDENTITY_INCOMING：key.der（PKCS#1）、cert.der、最后改名就位的 identity.json（不含私钥）。
    通用壳开关关着（0.6.0 前缺省）一个字节都不写。身份来源见 _identity_source（证书签名用户 / Windows Apple ID）。
    设备记录 identity.key_fp 与本机私钥指纹相同、且手机上已导入或还等着导入同一张证书 = 交过了，不联网、不重交（force 照样重交）。
    推一款 / 装壳顺带交（strict 假）：交不出 / 推不进不挡，记一行，照旧电脑签 → 交了的证书 sha1 | None。
    单独的「把证书交给 App」作业（strict 真）：交不了就报真实原因 → {delivered, certSha1, team, source}"""
    try:
        src = _identity_source(c, ph)
        if not src:
            if strict and hasattr(ph, "W"):   # Apple ID 方式、这台电脑上没有可交的证书：不为此新建（会顶掉玩家 AltStore / SideStore 的证书，11-19 实测），引导手机上导入证书
                raise Failure(IDENTITY_NOT_HERE, code="identity_unavailable")
            if strict:
                raise Failure("现在没在用你自己的开发者证书签名，没有要交给 iPhone 的证书：先在设置里导入并选用你的证书。", code="devcert_missing")
            return None
        team, fp, get, source = src
        done = layout.load_device(c, ph.key).get("identity") or {}
        if not force and done.get("team") == team and done.get("key_fp") == fp and _identity_on_phone(c, ph, done.get("cert_sha1")):
            return dict(delivered=False, certSha1=done["cert_sha1"], team=team, source=source) if strict else None   # 同一把私钥、手机上已导入或还等着导入：不重交
        ident = get()
        if not ident:
            if strict:
                raise Failure("苹果的证书列表里找不到这台电脑签名用的那张证书，没能交给 iPhone 上的 S2Arm：先在电脑上「安装 S2Arm」一次。", code="job_failed")
            trace(c, "私钥没交给 App：苹果列表里没有本机私钥对应的证书（或列不出），照旧在电脑签")  # 非玩家句
            return None
        sha1 = hashlib.sha1(ident["cert"]).hexdigest()
        meta = dict(schema=1, team=team, cert_sha1=sha1, created=ident["created"], source=source)
        with tempfile.TemporaryDirectory(dir=c.private) as w:
            d = Path(w) / "identity"
            d.mkdir()
            key = d / "key.der"
            key.touch(mode=0o600)
            key.write_bytes(ident["key"])
            (d / "cert.der").write_bytes(ident["cert"])
            (d / "identity.json").write_text(json.dumps(meta), encoding="utf-8")
            cl = ph.client(w)
            cl.put(key, f"{IDENTITY_INCOMING}/key.der")
            cl.put(d / "cert.der", f"{IDENTITY_INCOMING}/cert.der")
            cl.put(d / "identity.json", f"{IDENTITY_INCOMING}/identity.json.pc.tmp")
            ok, out = cl.mv(f"{IDENTITY_INCOMING}/identity.json.pc.tmp", f"{IDENTITY_INCOMING}/identity.json")
            if not ok:
                raise OSError(f"identity.json 改名就位失败：{out[-200:]}")  # 非玩家句（只进日志；单独作业时由下面包成 job_failed 的定句上屏）
        rec = layout.load_device(c, ph.key)
        layout.save_device(c, ph.key, dict(rec, identity=dict(team=team, key_fp=fp, cert_sha1=sha1, at=time.time())))
        trace(c, f"私钥与证书已交给 App（证书 sha1 {sha1[:12]}）")  # 非玩家句
        return dict(delivered=True, certSha1=sha1, team=team, source=source) if strict else sha1
    except Failure:
        if strict:
            raise
        return None
    except Exception as e:   # 推送工具的失败类型在 tools/push_pack.py，作业层不单独 import
        if strict:
            raise Failure("没能把证书写进 iPhone 上的 S2Arm：确认 iPhone 连着、已解锁，再试一次。", code="job_failed", internal=f"{type(e).__name__} {str(e)[:300]}") from e
        trace(c, f"私钥没交给 App（照旧在电脑签）：{type(e).__name__} {str(e)[:200]}")  # 非玩家句
        return None


def phone_identity(c):
    """作业 phoneIdentity（总控台「把证书交给 App」，证书签名用户）：连着的这台手机，把证书交给 App 保存，以后手机上自己签游戏。
    交过了（同一张证书、手机上已导入或还等着导入）不重交，delivered = False；请求带 force 照样重交。通用壳开关关着 → {enabled: False}"""
    with packer_phone_lock(c):   # 装机类作业一次一个（ipa.phone_lock）
        c.begin("把证书交给 iPhone 上的 S2Arm", 1, 1)
        ph = phone_for(c)
        src = _identity_source(c, ph)
        if src:   # 别的工具装的 S2Arm（包名被改过）也交：按手机上实际装着、团队对得上的那个（identity_target）
            ph.bid = identity_target(c, ph, src[0])
        return dict(deliver_identity(c, ph, strict=True, force=c.r.get("force") is True), device=ph.key, bundleId=ph.bid)


def push_game(c, gid, ph, steps=True):
    """「推到 iPhone」：只推这一款的程序包与资源包，绝不签名装壳（11-17 维护者定：装 App 与推游戏是两个独立作业）。
    手机上没装 S2Arm（这台没有装壳记录）或壳不是本版 → 报「先安装 / 更新 S2Arm」、不推。程序包按设备记录判要不要推（program_pack.need），
    资源包照 put_game 的判据（资源包号变了才推）。装上后这款的记录加 program"""
    import ipa as packer
    gid = gid.upper()
    pack = ios_pack(c, gid)
    rec = layout.load_device(c, ph.key)
    ext = getattr(ph, "external", False)   # 别的工具装的 S2Arm（push_phone）：不查启动器的壳号、不签名、不交身份，推进它自己的包名
    if not ext:
        if not rec.get("shellId") or not ph.bid:
            raise Failure(SHELL_INSTALL_FIRST, code="shell_install_required")
        if rec["shellId"] != packer.current_shell_id(c):
            raise Failure(SHELL_UPDATE_FIRST, code="shell_update_required")
    snapshot_phone_saves(c, ph, [gid])   # 0.6.0：推游戏不再覆盖存档，开头顺手把手机上这款的存档拉回电脑（内容变了才新建、每台每款留 10 份）
    team, bid, signer = (None, ph.bid, None) if ext else phone_signing(c, ph)
    if steps: c.begin("把这款的程序推进 iPhone", 2, 3)
    from rewrite_refresh import refresh_ios
    ph.pull_kept([gid])   # 机上留下的 NRO 先收回（#8be819），refresh_ios 补进程序件
    with packer.program_locks(c, [gid]):
        import sgrade_bf   # 着色器链变了：只重编这款的着色器库（游戏库里的 game.img / 借 Mac 版），不重出整款；没原料不挡推送（11-17 维护者定）
        if sgrade_bf.rechain_ios(c, gid) == "unavailable":
            trace(c, f"{gid} 着色器链更新了，这次没重编着色器库（原因见上一行；「建议更新」照旧提示）")  # 非玩家句
        refresh_ios(c, [gid])
        with tempfile.TemporaryDirectory(dir=c.private) as w:
            progs = push_programs(c, ph.client(w), [gid], rec, team, bid, signer)
    old = rec.get("games", {}).get(gid) or {}
    rec = layout.load_device(c, ph.key)
    layout.save_device(c, ph.key, dict(rec, games=dict(rec.get("games") or {}, **{gid: dict(old, program=progs[gid])}), at=time.time(),
                                       **({"externalBundleId": ph.bid} if ext else {})))   # 总控台不查手机时也认得出这台是别的工具装的
    if not ext:   # 别的工具装的：签名身份由玩家在总控台「把签名身份交给手机」单独交（phoneIdentity，按团队对得上的那个包名），推送不顺带
        deliver_identity(c, ph)   # 0.6.0：手机自签要的私钥交给 App（交过了 / 交不出都直接跳过，不挡推送）
    if steps: c.begin("把这款的资源推进 iPhone（只推变了的文件；第一次大游戏要几分钟）", 3, 3)
    files = size = 0
    pushed, mods = old.get("pack", ""), old.get("mods") or {}
    try:
        # 这一款照推，不按电脑上的记录跳过（同 put_game「这款照推」）：push_pack 按 sha256 核手机上的实况，只推缺的 / 变了的。
        #   记录说推过、手机上却没有（玩家在手机上删过这款、删 App 重装清了数据、别处装过同包名）时，原先推 0 个文件、起局卡「资源包不完整」（11-18 真机查出）
        ph.before_push({gid: pack})
        files, size = ph.push(gid, pack)
        pushed = pack_id(pack)
        mods = push_mods(c, ph, gid, mods)
    finally:
        rec = layout.load_device(c, ph.key)
        layout.save_device(c, ph.key, dict(rec, games=dict(rec.get("games") or {}, **{gid: dict(program=progs[gid], pack=pushed, **({"mods": mods} if mods else {}))}),
                                           at=time.time()))
    pull_shader_counts(c, ph, [gid])
    if progs[gid] != old.get("program") and not ph.pending:   # 这次推了程序包（没推的原样带回）：App 导入换上了才算装好
        if steps: c.begin(PROGRAM_WAIT, 3, 3)
        ph.pending = await_import(c, ph, gid, progs[gid]["dir"])
    gap = external_identity_gap(c, ph) if ext else None   # 导入不签名、起局才签：没身份就别报「装好了」
    if gap:
        ph.pending = gap[1]
    extra = dict(unpackPending=ph.pending) if ph.pending else {}
    return dict(bundleId=ph.bid, games=[gid], packFiles=files, packBytes=size, **ph.info, **extra, openToFinish=True,
                **({"installedBy": "external"} if ext else {}), **({"phoneIdentity": gap[0]} if gap else {}))


PUSH_SELECTED = "push_selected.json"   # 「更新所选」这一轮的进度（私有目录）：{device, games, done, failed}，续推按它接着来
# 换哪一款都一样做不下去的：手机上没装 / 壳不是本版、手机断开或没准备好——整轮停下报这个原因；别的失败只记这一款、接着推下一款
PUSH_SELECTED_STOP = ("shell_install_required", "shell_update_required", "device_not_connected", "device_not_ready", "device_required")


def push_selected(c):
    """作业 pushSelected（总控台「更新所选」，0.6.0 通用壳）：所选几款按顺序各推一次（同单款「推到 iPhone」：push_game，只做这一款、要改写就前台改写）。
    一款失败记下原因、接着推下一款；换哪款都做不下去（PUSH_SELECTED_STOP）才整轮停。玩家点停 = 取消作业，推完的留着。
    每推完一款记一次进度：请求带 resume: true 就从上一轮没推成的（没推到的 + 失败的）接着推；带 gameIds 就开新一轮。
    → {games: [{gameId, ok, code?, message?}], done, failed, remaining}；开关关着 → {enabled: False}"""
    from core import Cancelled
    state_path = c.private / PUSH_SELECTED
    with packer_phone_lock(c):
        c.begin("找签名账号与 iPhone", 1, 1)
        ph = push_phone(c)   # 别的工具装的 S2Arm 推进它自己的包名（结构 4）
        if c.r.get("resume") is True:
            old = read(state_path, {}) or {}
            games = [g for g in old.get("games") or [] if g not in (old.get("done") or [])] if old.get("device") == ph.key else []
        else:
            games = list(dict.fromkeys(str(g).upper() for g in c.r.get("gameIds") or []))
        state = dict(device=ph.key, games=games, done=[], failed={})
        atomic(state_path, state)
        rows = []
        for i, g in enumerate(games, 1):
            c.check()
            c.begin("按顺序把所选游戏推到 iPhone", i, len(games), key="pushSelected")
            c.emit("progress", "", done=i - 1, total=len(games), current=g)   # 前端按 current（游戏号）标正在推的那一行（gameId 是作业自带的键）
            try:
                push_game(c, g, ph, steps=False)
                state["done"].append(g); state["failed"].pop(g, None)
                rows.append(dict(gameId=g, ok=True))
            except Cancelled:
                raise
            except Failure as e:
                if e.code in PUSH_SELECTED_STOP:
                    raise
                state["failed"][g] = dict(code=e.code, message=str(e))
                rows.append(dict(gameId=g, ok=False, code=e.code, message=str(e)))
            except Exception as e:   # 这一款的意外错误：记下、接着推下一款（日志里有完整原因）
                trace(c, f"{g} 推送出错：{type(e).__name__} {str(e)[:300]}")  # 非玩家句
                state["failed"][g] = dict(code="job_failed", message=str(e)[:300])
                rows.append(dict(gameId=g, ok=False, code="job_failed", message=str(e)[:300]))
            finally:
                atomic(state_path, state)
    # 有款没推成：整单按失败报（#1756：原先照报「完成」，两端界面都不看逐款结果，进度条跑完、手机上还是「需要导入」、总控台还是「更新」）。
    #   原因用第一款失败的原话与错误码（已是给玩家的话）；失败页「重试」再推一轮，推成的那几款程序包已在手机上，按记录跳过
    if state["failed"]:
        first = next(r for r in rows if not r["ok"])
        raise Failure(first["message"], code=first["code"])
    return dict(games=rows, done=len(state["done"]), failed=len(state["failed"]), remaining=len(games) - len(state["done"]))


def forget_other_app(c, ph):
    """刚装上的 S2Arm 和设备记录里的不是同一个 App（换了团队 / 签名方式 = 换包名 = 新沙盒）：记录里各款的程序包、资源包、交过的证书、
    量过的占用都在原来那个 App 里，这个 App 里没有 → 清掉，之后「推到 iPhone」按没推过整款推（同 0.5.x _put 的 moved）。
    原来那个 App 和它的游戏留在手机上不动；包名没变什么都不做"""
    rec = layout.load_device(c, ph.key)
    if rec.get("bundleId") and ph.bid and rec["bundleId"] != ph.bid:
        note(c, f"手机上装的是另一个 App（{rec['bundleId']} → {ph.bid}）：各款按没推过算")  # 非玩家句
        layout.save_device(c, ph.key, {k: v for k, v in rec.items() if k not in ("games", "identity", "usage")})


def install_shell(c, ph, steps=True):
    """「安装 / 续签 / 更新 S2Arm」：只组只有壳的容器、签名、装壳，绝不推任何一款的程序包与资源包（11-17 维护者定）。
    玩家点了就照做：每次都签名安装（手机上已是同一个 App 也重签——续签、补回「提高内存上限」等权限都靠这一下）。装上后记壳号、包名、签名时刻；各款记录原样留着（程序包在手机上不受装壳影响）"""
    import ipa as packer
    rec = layout.load_device(c, ph.key)
    before = last_signer(rec)   # 装之前取（换包名时 forget_other_app 会清记录）
    snapshot_phone_saves(c, ph, list(rec.get("games") or {}))   # 0.6.0：装壳前把手机上各款的存档拉回电脑（同 push_game）
    if steps: c.begin("组 S2Arm（不带游戏）", 2, 3)
    ipa, _, sid = packer.assemble_shell(c)
    if steps: c.begin("签名并装到 iPhone", 3, 3)
    ph.install(ipa, [], steps=False)
    forget_other_app(c, ph)
    rec = layout.load_device(c, ph.key)
    signed = dict(signed=dict(at=time.time(), free=bool(ph.info["free"]), device=ph.info["device"], mode=sign_mode(c),
                              **({"expires": ph.info["expires"]} if ph.info.get("expires") else {}))) if "free" in ph.info else {}
    # 换了签名方式后第一次装（Mac 从 Xcode 钥匙串换到 plumesign、证书 ↔ Apple ID）：结果带 signerSwitched，完成页写「这一次要重新装一下」，只这一次
    now = signer_kind(c, ph)
    layout.save_device(c, ph.key, dict(rec, shellId=sid, bundleId=ph.bid or rec.get("bundleId", ""), at=time.time(), signer=now, **signed))
    packer.commit_phone_ipa(c, ipa, sid)
    deliver_identity(c, ph)   # 证书签名用户：装 App 时同时把证书交给 App，以后手机上自己签游戏（交过了跳过、交不出不挡）
    return dict(updated=True, outputPath=str(layout.phone_ipa(c)), bundleId=ph.bid, games=[], toShell=sid,
                **(dict(signerSwitched=True) if before and before != now else {}), **legacy_result(c, ph), **ph.info)


def legacy_games(c, rec):
    """0.5.x 整包装上的老款（这台的设备记录：资源推过、没有程序包——同总控台 suggested / missing，host_update.phone_overview）：
    装上 0.6.0 通用 App 后程序不在手机上了，要电脑推一次程序包才能玩（资源不重推）。→ (这台电脑能更新的 [{gameId, name}], 要回别的电脑更新的 [同])：
    能更新 = 这台电脑的游戏库里有这款的 iPhone 程序件与资源包清单（push_game 开头 ios_pack 的条件）"""
    todo = sorted(str(g).upper() for g, v in (rec.get("games") or {}).items() if isinstance(v, dict) and v.get("pack") and not v.get("program"))
    if not todo:   # 绝大多数装 App 都没有老款：不读游戏库目录
        return [], []
    names = {str(g.get("id", "")).upper(): g.get("name") or g.get("id") for g in c.catalog()["games"]}
    here, elsewhere = [], []
    for g in todo:
        ok = g in names and layout.has_ios_kit(c, g) and (layout.ios_pack(c, g) / "manifest.json").is_file()
        (here if ok else elsewhere).append(dict(gameId=g, name=names.get(g) or g))
    return here, elsewhere


def legacy_result(c, ph):
    """装完 App 的结果里带上老款（界面在同一页接着列出、默认全勾，点「更新这 N 款」走 pushSelected，设备给 legacyDevice = 这台的设备记录键；空的不带）"""
    here, elsewhere = legacy_games(c, layout.load_device(c, ph.key))
    return dict(**({"legacy": here, "legacyDevice": ph.key} if here else {}), **({"legacyElsewhere": elsewhere} if elsewhere else {}))


def last_signer(rec):
    """设备记录里上次装 App 的签名方式；记 signer 之前装的（0.5.x）按当时只有的签法推：证书模式 → devCert，Windows → plumesign，Mac → xcode；没装过 → None"""
    if rec.get("signer"):
        return rec["signer"]
    if not rec.get("bundleId"):
        return None
    return "devCert" if (rec.get("signed") or {}).get("mode") == "devCert" else "plumesign" if os.name == "nt" else "xcode"


def signer_kind(c, ph):
    """这次 App 用什么签的（设备记录 signer，判「换了签名方式」）：devCert / plumesign（Windows、Mac 通用壳 Apple ID）/ xcode（Mac 钥匙串）"""
    return "devCert" if devcert.active(c) else "plumesign" if hasattr(ph, "W") else "xcode"


def shell_outdated(c, rec, sid, games=None):
    """这台手机（设备记录 rec）上的 S2Arm 壳比本版旧时 → {from, to, games, stale}；不旧 / 没装过 → None。
    stale = 新壳认不得、要按新版重新出包的款（ipa.usable_kit 为 None：HOST_ABI 或直连链号变了）；游戏库里已删的款不列（组装时本来就不带）。
    games 缺省取记录里的各款（游戏库页检测用，不连手机）；update_phone 传手机上实际有的"""
    import ipa as packer
    games = sorted(games if games is not None else (rec.get("games") or {}))
    if not games or not rec.get("shellId") or rec.get("shellId") == sid:
        return None
    names = {g["id"].upper(): g.get("name") or g["id"] for g in c.catalog()["games"]}
    games = [g for g in games if g in names]
    stale = [g for g in games if packer.usable_kit(c, g, sid, rec.get("failedShells", [])) is None]
    return dict(fromShell=rec["shellId"], toShell=sid, games=[dict(gameId=g, name=names[g]) for g in games],
                stale=[dict(gameId=g, name=names[g]) for g in stale]) if games else None


def phone_updates(c):
    """游戏库页右上角「更新 iPhone 上的 S2Arm」：各台手机的设备记录里，壳比本版旧的 → [{device, name, fromShell, toShell, games, stale}]。
    只读本机记录（不连手机、不跑出包），catalog 每次带上；没有装过 iPhone 的玩家不算壳号"""
    recs = [(k, r) for k, r in layout.devices(c) if isinstance(r, dict) and r.get("games") and r.get("shellId")]
    if not recs:
        return []
    import ipa as packer
    sid = packer.current_shell_id(c)
    out = []
    import rewrite_refresh
    for key, rec in recs:
        st = shell_outdated(c, rec, sid)
        # 运行时留下、已收回游戏库还没补进 iPhone 件的 NRO（#8be819）：壳是本版也要更新（只推程序），按钮亮、启动器自动发起（kept 列这几款）
        kept = [g for g in rec.get("games") or [] if isinstance(g, str) and rewrite_refresh.kept_todo(c, g, layout.ios_kit(c, g))]
        if st or kept:
            if not st:
                names = {g["id"].upper(): g.get("name") or g["id"] for g in c.catalog()["games"]}
                st = dict(fromShell=rec["shellId"], toShell=sid, games=[dict(gameId=g, name=names.get(g, g)) for g in sorted(rec.get("games") or {})], stale=[])
            out.append(dict(device=key, name=(rec.get("signed") or {}).get("device") or "", **st, kept=[dict(gameId=g, name=g) for g in kept]))
    return out


PLAYING_WINDOW_S = 45   # 游玩时间心跳：游戏在前台时每 10 秒写一次 playtime.json（runtime/src/playtime.c，平台壳 10 秒定时器）；最近 45 秒内写过 = 正在玩


def playing_by_heartbeat(client, games, work, now=None):
    """手机上各款的游玩时间心跳（<容器>/Documents/games/<程序号>/playtime.json 的 lastPlayed，手机写入时的时刻）：
    任一款在 PLAYING_WINDOW_S 秒内写过 → True（正在玩，装 App 会杀掉这局）；都没有 / 文件不在（没玩过）→ False；
    一个都读不了（锁屏、断开）→ None（查不清）。Windows 没有 devicectl 查进程，靠它判（11-16 定的判据）"""
    import time as _t
    now = _t.time() if now is None else now
    got_any, stale = False, True
    for g in games:
        dst = Path(work) / f"playtime-{g}.json"
        try:
            ok = client.get(f"{game_docs(g)}/playtime.json", str(dst), 20)
        except Exception as e:   # 进不了容器（锁屏 / 断开 / 没装）：这一款查不清
            print(f"[s2a] 读 {g} 的游玩心跳失败：{e}", file=sys.stderr)
            continue
        got_any = True
        if not ok:
            continue   # 文件不在：这款没玩过，不算在玩
        try:
            last = int(json.loads(dst.read_text(encoding="utf-8")).get("lastPlayed") or 0)
        except (OSError, ValueError, TypeError):
            continue
        if now - last < PLAYING_WINDOW_S:
            return True
    return False if got_any or not games else None


def phone_kept_pull(c):
    """「手机插着、启动器开着」时启动器隔一阵调一次（phoneKeptPull 作业）：连着的那台手机上，宿主留下的 NRO（#8be819）拉回游戏库。
    → {device, new: 新收的个数, kept: 还没补推进 iPhone 的款, pushed: 这一轮补推了的款, shellUpdate: 壳比本版旧, stale: 新壳认不得、更新后要再推的款,
       running: 手机上 S2Arm 正在跑或查不清（有要做的才查）}。收回的 NRO 在这里直接补推进那几款的程序包（不装壳；正在玩就等下一轮）；
    shellUpdate 且 running 为假时启动器自动发起「更新 S2Arm」（只装壳）。没连手机 / 没装过 = new 0、不报错"""
    try:
        ph = phone_for(c)
    except Failure as e:
        if e.code == "device_required" and not (c.r.get("device") or "").strip():
            return _kept_pull_many(c)
        return _kept_pull_none()
    return _kept_pull_on(c, ph)


def _usage_games(cl):
    """手机上有哪几款（资源根两处 + 文档根下的游戏号目录，以手机为准，不只看这台电脑推过的）"""
    found = set()
    for root in (PACK_GAMES, PACK_GAMES_ALT, DOCS_GAMES):
        rc, out = cl.call("ls", root)
        if not rc:
            found.update(n.upper() for n in out.splitlines() if GID_RE.fullmatch(n.strip().upper()))
    return sorted(found)


def _usage_measure(cl, games):
    """→ {游戏号: {program, resources, saves, snapshots, files}}（字节）；量不了（工具旧 / 断开 / 锁屏）→ None。
    程序 = 两处资源根下 <号>/program/；快照 = 两处资源根下 <号>/snapshot/（《文件与程序管理体系》H 项定的落点，没有给 0）；
    存档 = 文档根 <号>/S2A/…/save；资源 = 这款其余全部（资源包、旧位置的资源、金手指、日志等）"""
    roots = {}
    for g in games:
        roots[g] = dict(total=[f"{PACK_GAMES}/{g}", f"{PACK_GAMES_ALT}/{g}", game_docs(g)],
                        program=[f"{PACK_GAMES}/{g}/program", f"{PACK_GAMES_ALT}/{g}/program"], saves=[f"{game_docs(g)}/{SAVE_REL}"],
                        snapshots=[f"{PACK_GAMES}/{g}/snapshot", f"{PACK_GAMES_ALT}/{g}/snapshot"])
    got = cl.du(*[r for v in roots.values() for rs in v.values() for r in rs]) if games else {}
    if got is None:
        return None
    def size(rs, i=0):
        return sum((got.get(r) or (0, 0))[i] for r in rs)
    out = {}
    for g, v in roots.items():
        total, prog, saves, snaps = size(v["total"]), size(v["program"]), size(v["saves"]), size(v["snapshots"])
        out[g] = dict(program=prog, resources=total - prog - saves - snaps, saves=saves, snapshots=snaps, files=size(v["total"], 1))
    return out


def phone_usage(c):
    """作业 phoneUsage（总控台「空间」手机那一列，0.6.0 通用壳）：连着的这台手机上每款占多少——程序 / 资源 / 存档（字节），量完记进设备记录 usage（带时刻）。
    没连 / 量不了 → 给各台上次量的（live 假）。只读：不推、不删、不拉起 App。
    → {enabled, phones: [{device, at, live, games: [{gameId, program, resources, saves, snapshots, files}]}]}（旧记录没量快照的给 None）；开关关着 → {enabled: False, phones: []}"""
    live = None
    try:
        ph = phone_for(c)
    except Failure as e:
        trace(c, f"没量手机（给上次的）：{e.code}")  # 非玩家句
        ph = None
    if ph is not None and phone_bid(ph):
        with tempfile.TemporaryDirectory(dir=c.private) as w:
            cl = devpush_client(c, ph.udid, phone_bid(ph), w)
            got = _usage_measure(cl, _usage_games(cl))
        if got is not None:
            rec = layout.load_device(c, ph.key)
            layout.save_device(c, ph.key, dict(rec, usage=dict(at=time.time(), games=got)))
            live = ph.key
    phones = []
    for key, rec in layout.devices(c):
        u = rec.get("usage") or {}
        if not u:
            continue
        games = [dict(dict(snapshots=None), **v, gameId=g) for g, v in sorted((u.get("games") or {}).items())]
        phones.append(dict(device=key, at=u.get("at"), live=key == live, games=games))
    return dict(phones=phones)


def _kept_pull_none():   # 没连手机 / 都问不到：每次一份新的（列表不共用）
    return dict(device="", new=0, kept=[], shellUpdate=False, stale=[], running=False)


def _kept_pull_many(c):
    """多台手机同时连着、请求没说哪台（phone_for 报 device_required）：按游戏库里有设备记录的逐台问
    （11-16 候选验收：Mac 插着两部 iPhone，启动器的 2 分钟循环每次都拿到 device=""，自动更新永远不触发）。
    带上设备号建手机对象，认到的不是这台就跳过（这台没连着时选机会退到唯一连着的那台，那不是这条记录）；
    有事要做（收回了 NRO / 有没补进的款 / 壳要更新）且没在玩的那台先返回，其次是有事但在玩的；都没事返回第一台问到的；一台都问不到 → 空结果。
    启动器每轮只装返回的那台，下一轮再问时它已经是新版，就轮到下一台。请求里的 device 问完还原"""
    had, old = "device" in c.r, c.r.get("device")
    busy = idle = None
    try:
        for key, _ in layout.devices(c):
            c.r["device"] = key
            try:
                ph = phone_for(c)
            except Failure:
                continue
            if ph.key != key:
                continue
            r = _kept_pull_on(c, ph)
            if r["new"] or r["kept"] or r["shellUpdate"]:
                if not r["running"]:
                    return r
                busy = busy or r
            idle = idle or r
    finally:
        if had:
            c.r["device"] = old
        else:
            c.r.pop("device", None)
    return busy or idle or _kept_pull_none()


def pull_shader_counts(c, ph, games):
    """各款手机上宿主写的着色器现场翻译计数（Documents/games/<号>/shader_fallback.json，几十字节）拉回这台的设备记录 shaderFallback，
    给「建议重新出包」角标判（host_update.shader_hint）。读不到（没有 / 断开）照旧，不挡推送"""
    import host_update
    got = {}
    try:
        with tempfile.TemporaryDirectory(dir=c.private) as w:
            cl = ph.client(w)
            for g in [str(x).upper() for x in games]:
                dst = Path(w) / f"{g}.json"
                if cl.get(f"{game_docs(g)}/{host_update.SHADER_FALLBACK}", dst):
                    try:
                        d = json.loads(dst.read_text(encoding="utf-8"))
                        got[g] = dict(count=int(d.get("count") or 0), at=d.get("at"), app=d.get("app"))
                    except (ValueError, TypeError, AttributeError):
                        pass
    except Exception as e:   # noqa: BLE001 —— 只是给角标的读数，读不到不挡推送
        trace(c, f"着色器计数没拉回：{type(e).__name__} {str(e)[:200]}")  # 非玩家句
        return {}
    if got:
        rec = layout.load_device(c, ph.key)
        layout.save_device(c, ph.key, dict(rec, shaderFallback=dict(rec.get("shaderFallback") or {}, **got)))
    return got


def _kept_pull_on(c, ph):
    rec = layout.load_device(c, ph.key)
    games = [g for g in rec.get("games") or [] if isinstance(g, str)]
    # 只和真冲突互斥（维护者「只按真冲突互斥」）：同一台手机上的装机 / 推包 / 续签（ipa.phone_lock）。手机在用就这一轮不收、下一轮再来；
    #   别的作业（出包、升级首启的程序件迁移……）照跑——原先前端「有任何作业在跑就整轮跳过」，升级首启迁移几分钟，拉回拖到第 360 秒（11-16 候选验收）。
    #   同一款程序件的迁移 / 现建由 programs-migrate 锁串行（ios_kit 里），收件写 nro_kept 是临时文件 + 改名（keep_nros），读的一方不会读到半个
    with c.idle("phone") as free:
        if not free:
            return _kept_pull_none()
        new = ph.pull_kept(games)
        pull_shader_counts(c, ph, games)   # 顺手拉回着色器现场翻译计数（给「建议重新出包」角标）
    import rewrite_refresh
    kept = [g for g in games if rewrite_refresh.kept_todo(c, g, layout.ios_kit(c, g))]   # 没补进的款（手动按钮兜底同 phone_updates）
    # 这台手机上的 S2Arm 壳比本版旧（启动器升级了）：shellUpdate = 可以不问玩家直接更新（没有要按新版重新出包的款）；
    #   有要重出的款 → shellUpdate 为假、stale 列出来，留给游戏库顶栏「更新 iPhone」按钮问（11-16 启动器升级顺带更新 iPhone App）
    import ipa as packer
    st = shell_outdated(c, rec, packer.current_shell_id(c))
    stale = [s["gameId"] for s in (st or {}).get("stale", [])]
    shell_update = bool(st)   # 装壳不碰各款：壳旧了就能直接更新；新壳认不得的款更新后在总控台标「建议更新」、玩家推一次就好
    running = False
    if kept or shell_update:
        running = ph.app_running()
        if running is None and hasattr(ph, "playing"):   # 查不了进程（Windows）：按游玩时间心跳判
            running = ph.playing(games)
        running = running is not False
    pushed = []
    if kept and not running:   # 收回的 NRO 补进这几款的程序包：只推这几款（push_game，不装壳）；正在玩就等下一轮
        for g in kept:
            try:
                push_game(c, g, ph, steps=False)
                pushed.append(g)
            except Failure as e:
                trace(c, f"{g} 收回的 NRO 没补推进 iPhone（下一轮再试）：{e.code} {str(e)[:160]}")  # 非玩家句
        kept = [g for g in kept if g not in pushed]
    return dict(device=ph.key, new=new, kept=kept, pushed=pushed, shellUpdate=shell_update, stale=stale, running=running)


def online_devices(c):
    """现在连着的设备号（Windows 设备服务里 USB 或同网的都算）；Mac / 读不到 → 空集（不影响更新本身，只少一句说明）"""
    if os.name != "nt":
        return set()
    try:
        return {u for u, _, _ in win_install_mod(c).mux_devices() if u}
    except OSError:
        return set()


def other_device_current(c, want):
    """#900：「更新手机上的 S2Arm」要找的那台（壳旧了的设备记录）不在线，而现在连着的另一台已经是本版壳 → 说清是两台、连着的这台不用更新；
    否则 None（照原来的「不在线」说）。玩家从游戏库推送时连着的那台已顺带换上新壳，更新按钮指的是游戏库里另一台旧设备的记录"""
    import ipa as packer
    if not want:
        return None
    online = online_devices(c)
    sid = packer.current_shell_id(c)
    fresh = [k for k, r in layout.devices(c) if k != want and k in online and isinstance(r, dict) and r.get("shellId") == sid]
    if not fresh:
        return None
    return (f"要更新的是另一台设备（编号末尾 {want[-6:]}），它现在不在线；现在连着的这台（编号末尾 {fresh[0][-6:]}）已经是新版 S2Arm，不用更新。"
            f"那台以后要用时连上它再点更新；不再用了的话，删掉游戏库文件夹 devices 里名字以 {want[-6:]} 结尾的那个文件，这个更新提醒就不再出现。")


def update_prepare(c):
    """「更新 iPhone 上的 S2Arm」的电脑侧准备（新版启动器首次启动时后台调）。原先在这里按各台手机记录逐款 refresh_ios——改写器指纹单独变了本来就跳过，
    做的全是 S 级补丁与可读瓦片版着色器（_bf）；11-17 维护者定 S 级优化不再自动运行（「改成不自动运行，玩家手动点击 S 级优化再升级」），
    所以不做任何事：玩家对一款点 S 级优化（sgrade.upgrade_game）或装机 / 推送时前台做。→ {prepared: [], updated: []}"""
    return dict(prepared=[], updated=[], manual=True)


def update_phone(c):
    """「更新 iPhone 上的 S2Arm」：用本版壳 + 手机上各款已有的程序模块组新容器 → 签名 → 只装 App；资源包（IMG）、直连库只推变了的（通常一个都不推），
    手机上的游戏库、存档不动。新壳认不得的款（要按新版重新出包）：没带 acceptStale 就停下报 container_stale 列出它们，玩家确认后不带它们装
    （它们的资源包和存档还在手机上，重新出包后再装就回来）"""
    with packer_phone_lock(c):   # 装机类作业一次一个（ipa.phone_lock）
        return _update_phone(c)


def _update_phone(c):
    if wants_export(c):   # 「导出新版 S2Arm」：只出不带游戏的通用 S2Arm.ipa（游戏走 .s2arm 两件套）
        return dict(export_shell_ipa(c), updated=True)
    c.begin("找签名账号与 iPhone", 1, 6)
    try:
        ph = phone_for(c)
    except Failure as e:
        hint = e.code == "device_not_connected" and other_device_current(c, c.r.get("device") or "")
        if hint:
            raise Failure(hint, code="device_not_connected") from e
        raise
    return install_shell(c, ph)   # 「安装 / 续签 / 更新 S2Arm」只装壳，不碰各款


def install(c):
    """默认「推到 iPhone」：这款进容器（一个 App 装下整个游戏库，免费账号也只占 1 个 App 额度）。加款 = 同一个包名覆盖安装，Documents 保留"""
    with packer_phone_lock(c):   # 装机类作业一次一个（ipa.phone_lock）
        return _install(c)


def _install(c):
    gid = c.r.get("gameId") or ""
    import downstream
    scheme = downstream.requested(c, dict(id=gid))   # 下游深度方案出的是独立 App：自己的包名，不进容器（pack/jobs/downstream.py）
    if scheme:
        if wants_export(c):   # 导出自签包只组 S2Arm 容器；独立 App 的资源靠推送进去，玩家自签装上也拿不到资源——说清楚，不悄悄改成连手机装机
            raise Failure(f"这款用的是 {scheme['name']} 方案（独立 App），现在只能「推到 iPhone」，还不能导出自签包。", code="downstream_export_unsupported")
        return downstream.install_ios(c, next((g for g in c.catalog()["games"] if g["id"].upper() == gid.upper()), dict(id=gid)), scheme)
    ios_pack(c, gid)
    if wants_export(c):   # 一款导出成小游戏包 .s2arm + 资源本体原位（两件套）
        return export_s2arm(c, gid)
    c.begin("找签名账号与 iPhone", 1, 6)
    return push_game(c, gid, push_phone(c))   # 「推到 iPhone」只推这一款，不装壳；别的工具装的 S2Arm 推进它自己的包名（结构 4）


def restore_previous(c):
    """装回换壳前的那一版手机 S2Arm（默认位置根 S2Arm.previous.ipa）：新运行层在手机上不好用时的回滚。
    装上后现行与上一版对调（再点一次 = 换回来）；被换下的壳号记进这台的 failedShells，之后给这台装机不再把旧款只换宿主带进它（要按新版重出）。
    手机上的资源包和存档不动；换宿主之后才新装的游戏不在上一版里，App 里暂时看不到（数据还在）"""
    with packer_phone_lock(c):   # 装机类作业一次一个（ipa.phone_lock）
        return _restore_previous(c)


def _restore_previous(c):
    import ipa as packer
    current, previous = layout.phone_ipa(c), layout.phone_ipa(c, previous=True)
    games, old_sid = packer.ipa_games(previous)
    _, dropped = packer.ipa_games(current)
    if not games:
        raise Failure("没有可以装回的上一版手机 S2Arm。", code="job_failed")
    c.begin("找签名账号与 iPhone", 1, 3)
    ph = phone_for(c)
    c.begin("装回上一版 S2Arm", 2, 3)
    ph.install(previous, games, steps=False)
    c.begin("记下手机上现在的版本", 3, 3)
    tmp = current.with_name(current.name + ".swap")
    if current.is_file():
        os.replace(current, tmp)
    os.replace(previous, current)
    if tmp.is_file():
        os.replace(tmp, previous)
    rec = layout.load_device(c, ph.key)
    failed = sorted((set(rec.get("failedShells", [])) | ({dropped} if dropped else set())) - {old_sid})   # 装回的这一版不算「不好用」：连点两次 = 换回来，那一版照常能只换宿主
    layout.save_device(c, ph.key, dict(rec, games={g: rec["games"].get(g) or {} for g in games}, shellId=old_sid, failedShells=failed, bundleId=ph.bid or rec.get("bundleId", "")))
    return dict(games=games, bundleId=ph.bid, restoredShell=old_sid, **ph.info)


def remove_from_phone(c):
    """从手机上的 S2Arm 里删一款：备份本款存档 → 删机上本款的数据（文档根 + 资源包 / 程序包目录，purge_game）→ 设备记录去掉这款。
    App 本身不动、不重装（通用壳：各款程序包在 App 外的 program/，删一款不用重签重装）。手机上 S2Arm 明确在运行就停下（边玩边删会写坏）；判不清照常删"""
    with packer_phone_lock(c):   # 装机类作业一次一个（ipa.phone_lock）
        return _remove_from_phone(c)


def _remove_from_phone(c):
    gid = (c.r.get("gameId") or "").upper()
    c.begin("找 iPhone", 1, 3)
    ph = phone_for(c)
    rec = layout.load_device(c, ph.key)
    # 包名：这台记过的；没记过（Windows 第一次在这台电脑上操作）按当前账号算（预检清单 #36：原先没记就不让删）
    ph.bid = phone_bid(ph)
    if not ph.bid:
        raise Failure("定不出手机上 S2Arm 的包名（这台电脑没装过、也读不到当前签名账号）：先在这台电脑上「安装 S2Arm」一次再删。", code="job_failed")
    # 真相在手机上（预检清单 #35）：玩家在手机上删了整个 S2Arm（on_phone 为 None）或手机上本来就没有这款 → 不用删，只把设备记录对齐
    on_phone = phone_games(c, ph)
    if on_phone is None or gid not in on_phone:
        note(c, f"手机上的 S2Arm 里本来就没有 {gid}，不用删" if on_phone is not None else f"手机上没装这个 S2Arm，{gid} 不在手机上，不用删")
        left = {} if on_phone is None else {g: v for g, v in (rec.get("games") or {}).items() if g != gid}
        layout.save_device(c, ph.key, dict(rec, games=left))
        return dict(games=sorted(on_phone or []), bundleId=ph.bid, removed=gid, saveBackup="", dataRemoved=True, alreadyAbsent=True, **ph.info)
    try:
        running = ph.playing([gid]) if hasattr(ph, "playing") else ph.app_running()
    except Exception:   # noqa: BLE001 —— 判不清照常删
        running = None
    if running is True:
        raise Failure("先在 iPhone 上退出 S2Arm，再删这款。", code="phone_app_running")
    c.begin("备份这款的存档", 2, 3)
    backup = ph.backup(gid)
    c.begin("删除手机上这款", 3, 3)
    ok, out = ph.purge(gid)
    if not ok:
        raise Failure("没能删掉手机上这款的数据：确认 iPhone 连着、已解锁，再试一次。", code="container_purge_failed", internal=out[-300:])
    rec = layout.load_device(c, ph.key)
    left = {g: v for g, v in (rec.get("games") or {}).items() if g != gid}
    layout.save_device(c, ph.key, dict(rec, games=left))
    return dict(games=sorted(left), bundleId=ph.bid, removed=gid, saveBackup=str(backup) if backup else "", dataRemoved=True, **ph.info)

