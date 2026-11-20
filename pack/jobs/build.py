"""Mac preparation/launch adapter. All engine transformations stay in S2Arm."""
from __future__ import annotations
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
import community
import host_update
import layout
from core import Cancelled, Failure, atomic, clone, contained, digest, read, resolve_path, tree_hash
from library import input_identity, require_game, selected_packages, source_path, stage_sources, validate_input
import milestones as ms
import toolchain

LANGUAGES = {
    "ja": "Japanese", "en-US": "AmericanEnglish", "fr": "French", "de": "German",
    "it": "Italian", "es": "Spanish", "zh-CN": "Chinese", "ko": "Korean", "nl": "Dutch",
    "pt": "Portuguese", "ru": "Russian", "zh-TW": "Taiwanese", "en-GB": "BritishEnglish",
    "fr-CA": "CanadianFrench", "es-419": "LatinAmericanSpanish", "zh-Hans": "SimplifiedChinese",
    "zh-Hant": "TraditionalChinese", "pt-BR": "BrazilianPortuguese",
}


def nospace(path):
    """Return a stable no-space alias for make/NativeAOT paths.

    The repository and launcher cache can live under directories containing
    spaces.  Make expands several paths into shell fragments and NativeAOT
    passes library paths through a shell command, so those callers need an
    alias rather than quoted argv entries.  Reuse the repository helper so the
    alias rules stay identical for shell and Python entry points.
    """
    raw = os.fspath(path)
    if " " not in raw:
        return Path(raw)
    helper = Path(__file__).resolve().parents[2] / "tools/nospace.sh"
    try:
        alias = subprocess.check_output(
            ["bash", "-c", '. "$1"; s2a_nospace "$2"', "s2a-nospace", str(helper), raw],
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise Failure("无法为带空格的构建路径创建无空格别名。") from error
    if not alias or " " in alias:
        raise Failure("带空格的构建路径没有得到无空格别名。")
    return Path(alias)


def snapshot(c, root):
    """产物清单：每个文件的大小（v0.2.3 清理：原先逐个 sha256，几十 GB 的游戏资源要读一遍；校验一律只核大小，摘要没人用了。
    第二项留空串，旧清单的摘要照旧读得进来、不再比对）。软链由 clone_tree 拷的时候就拒了，这里不再重复"""
    result = {}
    for p in sorted(root.rglob("*")):
        if p.is_file() and not layout.os_junk(p.name):   # 访达 .DS_Store / ._ 等系统文件不进清单：出包期间会被写入 / 改动 / 删掉（#382）
            c.check()
            result[str(p.relative_to(root))] = [p.stat().st_size, ""]
    return result


ENGINE_SOURCES = ["runtime", "ios", "graphics", "translator", "services", "compat", "pack", "Makefile", "platform/build.mk", "macos",
                  "launchers/macos/build.mk", "tools/toolchain.env", "tools/toolchain.sh", "tools/addr_need.py"]


def source_fingerprint(c):
    """产物根的键里「引擎源码」这一项：发版应用的引擎副本是冻结的，engine.json 的 id（键里已有 engineBundleId）就代表它，不再整份哈希源码树
    （v0.2.3 清理：每次出包两遍 rglob + sha256 整个引擎源码）；源码树开发照旧哈希"""
    return None if c.r.get("engineBundle") else tree_hash(c.engine, ENGINE_SOURCES)


def validate_files(c, root, files, full=False):
    if not isinstance(files, dict) or not files:
        raise Failure("运行产物校验清单缺失。", code="runtime_invalid")
    # 判据同逐个 contained()：解析后在 root 里、本身是普通文件（不是软链）、大小对。所在目录只解析一次（11-07：运行包 6 万多个文件，
    #   逐个 realpath 要一百多万次 lstat，catalog 一次 9–15 秒）；文件本身 lstat 一次，不跟软链。
    root = resolve_path(root)
    dirs = {}
    for name, expected in files.items():
        c.check()
        if layout.os_junk(Path(name).name):   # 旧清单里记进去的系统文件（#382 之前 snapshot 没过滤）：不核，访达随时会改它
            continue
        p = root / name
        if p.parent not in dirs:
            dirs[p.parent] = contained(root, p.parent) if p.parent != root else root
        p = dirs[p.parent] / p.name
        try:
            st = os.lstat(p)
        except OSError:
            st = None
        if st is None or not stat.S_ISREG(st.st_mode) or st.st_size != expected[0]:
            raise Failure("准备产物缺失或大小不符，请重新准备：" + name, code="runtime_invalid")  # 非玩家句
        if full and expected[1] and digest(p) != expected[1]:   # full 只在调用方明确要求时核摘要（v0.2.3 清理：缺省只核大小，#304 被改的文件就是靠大小查出来的）
            raise Failure("准备产物校验失败，请重新准备：" + name, code="runtime_invalid")  # 非玩家句


QUICK_FILES = ("main", "main.npdm")   # 游戏库列表快判也核的几个文件（主程序与程序描述）：在、是普通文件、大小同清单


def heap_learned_now(env, gid, since):
    """这一局（since 之后）运行时记下了这款要的普通堆（runtime/src/svc_mem.c heap_need_note → <文档根>/wire/<程序号>/heap_need）"""
    docs = env.get("S2A_DOCS")
    if not docs:
        return False
    p = Path(docs) / "wire" / gid.upper() / "heap_need"
    try:
        return p.stat().st_mtime >= since - 1
    except OSError:
        return False


def has_runtime_romfs(root):
    """运行版本里的 RomFS 三种形态（约定同 graphics/nvn/tools/romfs_list.py）：romfs/ 真目录，或目录不在、只有标准 RomFS 镜像 romfs.std（普通文件，
    宿主 services/S2A.Services/RomFsIndex.cs OpenStd 整个交给游戏），或 game.img（0.6.0 一款一份资源：iPhone 资源包那份的硬链接，pack/jobs/onerom.py；
    宿主 OpenImg 先认它）。软链都不算：运行版本要独立于构建缓存"""
    romfs, std, img = root / "romfs", root / "romfs.std", root / "game.img"
    if romfs.is_symlink() or romfs.exists():
        return romfs.is_dir() and not romfs.is_symlink()
    return (std.is_file() and not std.is_symlink()) or (img.is_file() and not img.is_symlink())


def validate_runtime(c, runtime, full=False, files=True, doubts=None):
    """doubts：起局传一个列表（11-17 维护者：校验不过不再拒起局）：清单的 schema / engine / target 对不上、主程序 / 程序描述缺或大小不符都记进去、照常返回，
    由起局黄字说明、照常起；不传照旧抛错（游戏库判「就绪」用）。files=False：只看清单和入口（游戏库列表用），不逐文件读状态，只核主程序 / main.npdm 在且大小对（QUICK_FILES，审计 P2-212：
    原先清单没变、主程序被删也报就绪，点开始才报错）。起局也用它（v0.3.1：原先每次起局逐个核约 6 万个文件，又慢、任何一个不对就不让玩）；
    其余文件真坏了，游戏会非正常退出，launch 那时再完整核一遍、说清缺了什么（missing_after_crash）"""
    root = resolve_path(runtime)
    layout.require_in_location(c, root)
    manifest = read(root / "manifest.json", {})
    if manifest.get("schema") != 1 or manifest.get("engine") != "S2Arm" or manifest.get("target") != "macos-arm64":
        if doubts is None:
            raise Failure("当前游戏没有完整的 S2Arm Mac 运行版本。", code="runtime_invalid")
        got = f"schema={manifest.get('schema')}, engine={manifest.get('engine')}, target={manifest.get('target')}"
        doubts.append(f"运行版本清单对不上（{got}）")
    if not has_runtime_romfs(root):
        raise Failure("运行包缺少独立的 RomFS 游戏资源目录，请重新准备。", code="runtime_invalid")
    if files:
        validate_files(c, root, manifest.get("files"), full)
    elif isinstance(manifest.get("files"), dict):
        quick = {k: manifest["files"][k] for k in QUICK_FILES if k in manifest["files"]}
        if quick:
            try:
                validate_files(c, root, quick, full=False)
            except Failure as e:
                if doubts is None:
                    raise
                doubts.append(f"运行文件 {str(e).split('：', 1)[-1]} 缺失或大小不符")
    if manifest.get("memoryRequirementsVersion") == 2:
        table = root / "addr_need.tsv"
        if not table.is_file() or manifest.get("memoryRequirementsSha256") != digest(table):
            # v0.3.1（认不出的直接做）：宿主读这张表只认几行、程序号对不上就按 0（runtime/src/svc_mem.c alias_first_hint），
            #   表坏了最多是布局提示归 0，不该挡起局；只记一笔
            print("[s2a] 内存配置表（addr_need.tsv）和清单记的不一致，照常起局（布局提示可能不生效）", file=sys.stderr)
    return manifest


def runtime_manifest(runtime):
    """起局用：只读运行版本清单，不核任何文件（11-19）。读不到 → {}（随后按「与所选游戏不匹配」报）"""
    return read(resolve_path(runtime) / "manifest.json", {})


def missing_after_crash(c, runtime, manifest):
    """游戏非正常退出后补核运行文件（起局前只核主程序，v0.3.1）：有缺 / 坏的 → 返回第一个的说明（给玩家看的例子），都好 → None。核不了（清单缺等）也返回 None"""
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        return None
    try:
        validate_files(c, resolve_path(runtime), files, full=False)
    except Failure as e:
        return str(e).split("：", 1)[-1][:120] or str(e)[:120]
    return None


def docked_default(c):
    """没选过档位时的运行模式：只认共用设置定义表 game_settings.v1.json 的 docked 缺省（iPhone / Mac 同一处来源，11-17 起两端都是掌机）。
    请求明给布尔（作业测试 / 内部调用）照给的；玩家按款选过的不在这里，launch() 用 community.launch_env 覆盖"""
    v = c.r.get("docked")
    if isinstance(v, bool):
        return "1" if v else "0"
    return "1" if community._default(community.settings_table(c)["docked"], {}, "mac") == "docked" else "0"


def runtime_env(c, game, runtime, input_ok=True):
    # 游戏语言：请求带 gameLanguage（玩家在启动设置里逐款选）就设 S2A_SYSTEM_LANGUAGE；不带就跟 Mac 系统首选语言（宿主 services/S2A.Services/Host.cs PickLanguage
    #   → SystemLocale.FromPreferred；11-13 起不跟启动器界面语言：界面只有中英，日 / 韩语玩家的 Switch 存档名字会乱码）；游戏不支持的落英语，英语也没有用游戏自己的缺省
    language = c.r.get("gameLanguage")
    if language is not None and language not in LANGUAGES:
        raise Failure("游戏语言代码无效。")
    env = c.env()
    env["S2A_IPHONE_VA"] = "0"   # 玩家起局不用 iPhone 地址模拟；开发 / 普查保留原生缺省（#1530）。
    env.pop("S2A_VAMM_LAYOUT", None)
    env.pop("S2A_PATCH_DIR", None)   # 补丁只在出包时读（随包 patch.tsv 在游戏数据目录里，宿主自己找）；玩家导入的在 <S2A_DOCS>/S2A/patches
    save = contained(layout.root_of(c, game["id"]), layout.path(c, game["id"], "saves"))
    # 存档位置（11-09 维护者）：这款游戏文件夹里的 SaveData/，原生存档格式，放进去下次起局就读、退出时写回（services/S2A.Services/SaveFolder.cs）。
    #   不叫 Saves：同一文件夹里已有运行层内部的 saves/，APFS / NTFS 不分大小写会撞名；ASCII 名避开非 ASCII 路径的坑
    # 本地玩家（#623）：请求带 player {user: first | 32 位十六进制, name, folder} = 启动器顶栏选的玩家 → 服务层 S2A_PLAYER_USER / S2A_PLAYER_NAME
    #   （services/S2A.Services/LocalPlayer.cs），存档位置换成这款文件夹里这个玩家的那个（第一个玩家仍是 SaveData）；没带 = 旧行为
    from saves import requested_player   # 校验规则与导入社区存档共用一份（saves.py）
    player = requested_player(c.r)
    save_folder = contained(layout.root_of(c, game["id"]), layout.path(c, game["id"], player["folder"] if player else "SaveData"))
    cache = contained(layout.root_of(c, game["id"]), layout.path(c, game["id"], "shader-cache"))
    docs = contained(c.library, c.library / "temp" / game["id"])
    for p in (save, cache, docs):
        p.mkdir(parents=True, exist_ok=True)
    # S2A_SAVE_DIR is a test/import seed that clears saves at boot; launch() sets it
    # only for a save the player just imported, never for normal play. Persist the Ryujinx data root instead.
    # 游玩时间（runtime/src/playtime.c）写进这款的游戏文件夹：S2A_DOCS 在 temp/（总览里算可清的临时件），清了不能把累计时长一起清掉
    env.update(S2A_GAME_DATA=str(runtime), S2A_RYU_DATA=str(save), S2A_DOCS=str(docs), S2A_SAVE_FOLDER=str(save_folder),
               S2A_PLAYTIME=str(layout.path(c, game["id"], "playtime.json")),
               # 即时快照（0.6.0）落在这款的游戏文件夹里（同游玩时间，不放可清的 temp/），删这款时随文件夹一起删；宿主 runtime/src/snapshot.c s2a_snap_dir_for 读
               S2A_SNAPSHOT_DIR=str(layout.path(c, game["id"], "snapshot")),
               NXGAL_SHADER_CACHE_DIR=str(cache / "shaders"), NXGAL_PSO_DEV_DIR=str(cache / "pso"),
               S2A_MACRO_DUMP=str(cache / "macros"),
               S2A_DOCKED=docked_default(c),   # 缺省掌机（11-17 维护者定，同 iPhone）；玩家在这款设置里选过的由 launch() 里 community.launch_env 盖上
               S2A_AUTOPLAY="0", S2A_GAME_NAME=game["name"], S2A_GAME_ID=game["id"],
               S2A_INPUT_GUIDE_PREFS=str(c.private / "input-guides" / (game["id"] + ".json")),
               # 金手指（11-12）：这款游戏文件夹的 cheats/（玩家导入 + 启动器从公开库取的 txt、开关 enabled.txt），Mac 宿主 Cheats.cs 读它
               S2A_CHEATS_DIR=str(contained(layout.root_of(c, game["id"]), layout.path(c, game["id"], "cheats"))),
               # 资源替换类 mod（11-15）：这款游戏文件夹的 mods/（启动器「游戏设置 → Mod」导入），服务层 RomFsMods.cs 起局叠到 RomFS 上
               S2A_MODS_DIR=str(contained(layout.root_of(c, game["id"]), layout.path(c, game["id"], "mods"))))
    if player:
        env["S2A_PLAYER_USER"] = player["user"]
        if player.get("name"):
            env["S2A_PLAYER_NAME"] = player["name"]
    if language is not None:
        env["S2A_SYSTEM_LANGUAGE"] = LANGUAGES[language]
    if c.r.get("devHud"):
        env["S2A_HUD"] = "1"   # S2Arm 诊断浮层：front_mac.m 绘制，hud.c 格式化
        if c.r.get("buildId"):
            env["S2A_BUILD_ID"] = c.r["buildId"]   # 浮层「包」一行：启动器 Info.plist 的 S2ABuildID
    if c.r.get("metalHud"):
        env.setdefault("MTL_HUD_ENABLED", "1")
    controller_auto_connect = c.r.get("switch2ProAutoConnect")
    if controller_auto_connect is not None:
        if not isinstance(controller_auto_connect, bool):
            raise Failure("Switch 2 Pro 自动连接设置无效。")
        env["S2A_NS2_AUTOCONNECT"] = "1" if controller_auto_connect else "0"
    controller_relay = c.r.get("switch2ProRelaySocket")
    if controller_relay is not None:
        if not isinstance(controller_relay, str) or not controller_relay or "\0" in controller_relay:
            raise Failure("Switch 2 Pro 本机通道设置无效。")
        env["S2A_NS2_SOCKET"] = controller_relay
    xbox_relay = c.r.get("xboxGipRelaySocket")
    if xbox_relay is not None:
        if not isinstance(xbox_relay, str) or not xbox_relay or "\0" in xbox_relay:
            raise Failure("Xbox GIP 本机通道设置无效。")
        env["S2A_XBOX_GIP_SOCKET"] = xbox_relay
    # 启动器设置「启动游戏时自动全屏」（#575）：给全局缺省，宿主（macos/front_mac.m）只在这款没显式给 S2A_FULLSCREEN 时用它
    if c.r.get("launchFullscreen") is True:
        env["S2A_FULLSCREEN_DEFAULT"] = "1"
    # Freeze exactly what the pre-launch card showed. Never rewrite another run's snapshot.
    # input_ok = False：宿主不认键位映射（launch 判的）→ 这局不带映射、用默认键位（v0.3.1：认不出照常玩，不让玩家先去重新打包）
    config = c.r.get("launchInputConfig") if input_ok else None
    if config is None and input_ok:
        config = read(layout.path(c, game["id"], "input.json", create=False)) or read(c.private / "default-input.json")
    if config is not None:
        try:
            validate_input(config)
        except Failure as e:   # 映射不合规（旧版本 / 手改坏了）：用默认键位照常起局，日志记一笔，不挡（v0.3.1 装机原则：认不出的直接做）
            print(f"[起局] 键位映射不合规（{e}），这局用默认键位", file=sys.stderr, flush=True)
            config = None
    if config is not None:
        path = c.private / "runtime-input" / game["id"] / (c.task + ".json")
        atomic(path, config)
        env["S2A_INPUT_CONFIG"] = str(path)
    # 玩家 2–4 自己的键位（启动器按玩家保存的 input-pN.json，键位设置里选玩家）：有就照玩家 1 同一套校验后交给宿主
    #   （S2A_INPUT_CONFIG_P2..P4；front_mac.m 槽 i = 玩家 i+1，各手柄照自己玩家的映射采样）；没存的那位照玩家 1 的键位。
    #   input_ok = False（宿主不认键位映射）时不带，同玩家 1 的口径
    if input_ok:
        for player in (2, 3, 4):
            player_config = read(layout.path(c, game["id"], f"input-p{player}.json", create=False))
            if not player_config:
                continue
            try:
                validate_input(player_config)
            except Failure as e:   # 映射不合规（旧版本 / 手改坏了）：这位照玩家 1 的键位照常玩，不挡
                print(f"[起局] 玩家 {player} 的键位映射不合规（{e}），这位照玩家 1 的键位", file=sys.stderr, flush=True)
                continue
            path = c.private / "runtime-input" / game["id"] / f"{c.task}-player{player}.json"
            atomic(path, player_config)
            env[f"S2A_INPUT_CONFIG_P{player}"] = str(path)
    if c.r.get("inputGuideHandled") is True:
        env["S2A_INPUT_GUIDE_SHOWN"] = "1"
    if c.r.get("uiLanguage") in ("zh-Hans", "zh-Hant", "en", "ja", "es", "fr"):   # 宿主自己的界面文案（按键引导等）跟启动器界面语言；不再决定游戏语言（见上 gameLanguage）
        env["S2A_UI_LANGUAGE"] = c.r["uiLanguage"]
    return env


def collect_pipeline_seed(c, game_id, docs):
    """把 GUI Mac 局见过的管线并进下一次出包读取的不可变来源；失败不改变本局结果。"""
    game_id = game_id.upper()
    source = Path(docs) / "wire" / game_id / "pso_user.tsv"
    if not source.is_file() or source.is_symlink():
        return
    script = c.engine / "tools" / "pso_seed_keep.sh"
    message = ""
    try:
        result = subprocess.run(["/bin/bash", str(script), "--one", game_id, str(source)],
                                env=c.env(), capture_output=True, text=True, timeout=30)
        output = (result.stdout if result.returncode == 0 else result.stderr or result.stdout).strip()
        if result.returncode:
            message = f"管线种子来源：本局记录未能收纳（退出 {result.returncode}：{output[-300:]}）"  # 非玩家句
        elif output:
            message = output.splitlines()[-1]
    except (OSError, subprocess.TimeoutExpired) as error:
        message = f"管线种子来源：本局记录未能收纳（{error}）"  # 非玩家句
    if message:
        try:
            c.emit("message", message)
        except Cancelled:
            pass


def keep_runtime_nros(c, game_id, source):
    """宿主这局留下的 NRO（runtime/src/loader.c s2a_nro_keep：包里没有改写映像、游戏解开的整份 <SHA-256>.nro）收进游戏库 games/<号>/nro_kept/，
    下次准备这款时改写进包（#8be819）。只收文件名 = 内容 SHA-256 的；收到新的就告诉玩家重新准备一次。失败不改变本局结果。→ 新收的个数"""
    import hashlib
    new = 0
    try:
        files = sorted(p for p in Path(source).glob("*.nro") if p.is_file() and not p.is_symlink() and len(p.name) == 68)
    except OSError:
        return 0
    for f in files:
        try:
            data = f.read_bytes()
            if hashlib.sha256(data).hexdigest() != f.name[:64]:
                continue
            dest = layout.nro_kept(c, game_id, create=True) / f.name
            if dest.is_file() and dest.stat().st_size == len(data):
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            tmp = dest.with_name(dest.name + ".tmp"); tmp.write_bytes(data); os.replace(tmp, dest); new += 1
        except OSError as error:
            print(f"[s2a] 收运行时留下的 NRO {f.name[:8]}… 失败：{error}", file=sys.stderr)
    if new:
        try:
            c.emit("message", f"这款游戏有 {new} 个代码模块要在运行中才能取到，已经收下，正在自动补上")
        except Cancelled:
            pass
    return new


def launch(c):
    import sgrade
    # 执行锁读写化（11-19）：起局整局只拿执行锁的共享模式——别的游戏照样能起、能出包；清理 / 删除 / 搬家等全库动作（独占）照旧等这局结束。
    #   这一款由按款锁挡：同一款的出包（produce_locks）、S 级升级、再起一局都要等这局结束
    with c.lock("execution", shared=True), c.idle(sgrade.game_lock(c.r.get("gameId") or "")) as free:
        if not free:
            import filelock
            holder = filelock.read_holder(c.library / ("." + sgrade.game_lock(c.r.get("gameId") or "") + ".lock"))
            if holder.get("operation") in ("launch", "build", "ipa"):   # 拿着这款锁的是起局 / 出包：同原先排执行锁到上限后的说法
                from errors import library_lock_failure
                raise library_lock_failure(holder, "另一个任务正在使用游戏库，请稍后重试。", f"这款的按款锁被占：{holder}")  # 非玩家句
            # 这款正在升级 S 级优化（sgrade.upgrade_game 拿着这款的锁）：只挡这一款，升级做完自动解除
            raise Failure("这款游戏正在升级 S 级优化，升级完成后就能开始。", code="sgrade_upgrading")
        game = c.game()
        from saves import recover
        recover(c, game, override=c.r.get("overrideRecovery") is True)   # 回退失败：玩家单独确认「仍要启动」才放行（同 iPhone，审计 18:00 P0-3）
        runtime = layout.image_mac(c, game["id"], create=False)
        # 起局前不再自动补 S 级优化 / 按新改写器重改写（11-17 维护者：「改成不自动运行，玩家手动点击 S 级优化再升级」，sgrade.upgrade_game）
        from rewrite_refresh import kept_nros_mac
        kept_nros_mac(c, game["id"], runtime)   # 运行时留下、还没补进的 NRO（#8be819）：上次收局没补成的这里再补
        c.begin("验证运行文件")   # 阶段名是前端固定的机读名（PlaySessionView.stages），这一段现在只读清单、不核文件
        # 起局前不核任何运行文件（11-19 维护者：「能不能去掉检验……动了启动失败就行了」）：原先这里核主程序 / 程序描述在不在、大小对不对，
        #   再给内存配置表算一次摘要；坏了照样起（11-17 起不拒），这一步只剩花时间。真读到坏的，游戏非正常退出后 missing_after_crash 再核一遍、说清缺什么
        doubts = ["请求里带着已停用的旧内存布局设置（vammLayout）"] if "vammLayout" in c.r else []
        manifest = runtime_manifest(runtime)
        for doubt in doubts:
            print("[起局] 照常起局：" + doubt, file=sys.stderr, flush=True)  # 非玩家句
            c.diagnostic(f"{doubt}，先照常启动；起不来的话重新出一次 Mac 版。", code="runtime_invalid", severity="warning")
        doubt_note = ("\n" + f"起局前已知：{'；'.join(doubts)}") if doubts else ""
        if manifest.get("gameId") != game["id"]:
            raise Failure("运行资源与所选游戏不匹配。", code="runtime_invalid")
        if manifest.get("memoryRequirementsVersion") != 2:
            raise Failure("这款游戏需要重新打包，完成后即可启动。", code="runtime_invalid")
        host, trial = host_update.choose(c, game["id"], runtime, manifest)   # 公共宿主：兼容就换上本版的（这局是试玩），否则用原来的
        executable = host_update.host_exe(c, host)
        if not os.access(executable, os.X_OK):
            raise Failure("S2Arm Mac 宿主不可执行。", code="permission_denied")
        input_ok = True
        if c.r.get("launchInputConfig") is not None:
            supported, _ = toolchain.input_interface(c, executable)
            if not supported:   # 宿主认不得键位映射：这局用默认键位照常起（v0.3.1），重新打包后映射才生效
                print("[起局] 这款的宿主不认键位映射，这局用默认键位（重新打包后映射生效）", file=sys.stderr, flush=True)
                c.diagnostic("这款游戏的运行包认不得自定义键位，这次先用默认键位；重新打包这款后自定义键位就会生效。", code="runtime_invalid", severity="warning")
                input_ok = False
        env = runtime_env(c, game, runtime, input_ok=input_ok)
        from saves import claim_seed, finish_seed
        seed = claim_seed(c, game["id"])
        if seed:
            env["S2A_SAVE_DIR"] = str(seed)   # 玩家导入过存档、这是第一次启动：运行时建好存档再灌入
        import cheats   # 上次崩溃自动关掉的：启动器换了版本就重开一次（多半已修），要在算启动开关之前
        back = cheats.restore_after_update(c, game)
        if back:
            c.diagnostic("上次游戏崩溃时自动关掉的设置，新版 S2Arm 已重新打开：" + "、".join("帧率" if t == "fps" else "满屏" if t == "fs" else k.split("/", 1)[-1] for t, k in back) + "。", code="crash_restored", severity="warning")
        env.update(community.launch_env(c, game["id"], existing=env))   # 宽屏 / 60 帧宿主开关（社区资源面板，10-27）：关掉的才设，下次启动生效
        # 宿主菜单栏「切换游戏」（macos/front_mac.m）：收尾前在这里留标记，结果带 returnToLibrary，启动器回到游戏库。先删上一局残留
        marker = c.private / "runtime-return" / (c.task + ".flag")
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.unlink(missing_ok=True)
        env["S2A_RETURN_MARKER"] = str(marker)
        back = False
        try:
            c.begin("游戏运行中", key="game_running")
            log = c.logs / (c.task + "-game.log")
            started, wall = time.monotonic(), time.time()
            try:
                import onerom   # 一款一份资源（0.6.0）：游戏跑过加载期后在后台把 Mac 运行版本合并到 iPhone 那份 game.img（玩家不等，退出就停、下次接着）
                onerom.during(c, game["id"], lambda: c.run([executable, runtime], log, env, cwd=c.library / "temp" / game["id"]))
            except Failure as f:
                if trial:   # 新宿主试玩：TRIAL_OK_SECONDS 内异常退出 = 不认这款，下次退回原来的；玩过这么久才崩不怪宿主
                    host_update.settle(c, game["id"], host, ok=time.monotonic() - started >= host_update.TRIAL_OK_SECONDS)
                if f.code == "tool_exit":   # 开局就崩：记下（带本局日志），下次开始前若有上一份存档就问玩家要不要回去（save_previous）
                    import save_previous
                    save_previous.note_launch(c, game["id"], time.monotonic() - started < save_previous.EARLY_SECONDS)
                if f.code == "tool_exit" and heap_learned_now(env, game["id"], wall):   # 这一局普通堆不够、已记下要的堆（runtime svc_mem.c heap_need）：下一局按它给，不是崩溃、不关金手指
                    raise Failure("这款需要更多内存：已经记下要多少，再点一次开始就能进。", code="game_heap_learned", internal=str(f)) from None
                if f.code == "tool_exit":   # 游戏进程自己非 0 退出（多为崩溃，139 = 段错误）：给启动页一个说人话的码，原文与退出码随错误摘要带走
                    import cheats   # 开着金手指 / 补丁时退出：当场关掉、记下，下次开始时启动器通知关了哪几条（11-13 维护者改定：不问，cheats.note_crash）
                    cheats.note_crash(c, game, env.get("S2A_DOCS"))   # 带文档根：读运行时写的崩溃落点（宿主自己崩第一次不关）
                    broken = missing_after_crash(c, runtime, manifest)
                    if broken:   # 起局前没逐个核（v0.3.1）：退出后再核，运行文件缺 / 坏了就直说，别让玩家以为是游戏崩了
                        raise Failure(f"这款游戏的 Mac 运行文件缺了或坏了（例：{broken}），游戏读到它时退出了。重新出一次 Mac 版就能修好；存档不受影响。" + doubt_note,
                                      code="runtime_invalid", internal=str(f)) from None
                    if getattr(c, "last_rc", None) == 86:   # 游戏卡住后被停下（显卡挂死安全停 graphics/nvn/host_adapt.m s2a_gpu_safe_stop、gal_core.m 命令缓冲出错、卡住暂停时玩家选退出 macos/front_mac.m），不是玩完（#1257、#1378）
                        raise Failure("游戏卡住了，S2Arm 把这局安全停下了。" + doubt_note, code="game_gpu_stopped", internal=str(f)) from None
                    raise Failure(f"游戏运行中意外退出了（退出码 {getattr(c, 'last_rc', '?')}）。" + doubt_note, code="game_crashed", internal=str(f)) from None
                raise
            if trial:
                host_update.settle(c, game["id"], host, ok=True)
            import save_previous
            save_previous.note_launch(c, game["id"], False)   # 好好收局：清掉上次「开局就崩」的记号
            import cheats   # 好好收局：记下这时开着的，下次崩了只关比这多开的（cheats.narrow）
            cheats.note_good(c, game)
            back = marker.exists()
        finally:
            marker.unlink(missing_ok=True)
            host_update.forget_registrations(app=Path(executable).parents[2])   # 系统起宿主时把它登记成 App、启动台多出「S2ArmGame」：每局结束注销（#1126）；不是真 App 包不碰
            if seed:
                finish_seed(c, game["id"], log)
            collect_pipeline_seed(c, game["id"], env["S2A_DOCS"])
            if keep_runtime_nros(c, game["id"], Path(env["S2A_DOCS"]) / "wire" / game["id"].upper() / "nro"):
                kept_nros_mac(c, game["id"], runtime)   # 收局就补上（#8be819），玩家再点开始就能玩，不用判断要重出什么
            import cheats   # 停帧救场：局内画面停住时帧率补丁已被运行时关掉、留了标记 → 这款「帧率」写关，下次开始时通知（正常退出 / 崩溃都要读）
            cheats.note_stall(c, game, env["S2A_DOCS"])
            cheats.note_widen(c, game, env["S2A_DOCS"])   # 我方通用满屏自检退回：满屏写关、下次开始时通知
            # Context.run waits for the host, including failure/cancellation.
            # Do not accumulate per-run snapshots after the process exits.
            if env.get("S2A_INPUT_CONFIG"):
                path = contained(c.private / "runtime-input", Path(env["S2A_INPUT_CONFIG"]))
                try:
                    path.unlink(missing_ok=True)
                except OSError:
                    c.emit("message", "本局映射快照清理失败，请检查应用数据目录权限。")
        return {"gameId": game["id"], "returnToLibrary": True} if back else {"gameId": game["id"]}


VAGUARD = "libs2a_vaguard.dylib"


def host_cache_ok(app, stamp):
    """缓存的 Mac 宿主能不能复用：可执行文件摘要对、地址窗口护栏库在且摘要对、两者签名都验得过（10-24 #100 作者：删掉副本里的护栏库仍判命中，
    返回缺库的 App，macOS 27 上起局即占不到窗口）。旧格式的 ready.json 没记护栏库摘要 → 不复用、重新构建"""
    executable = app / "Contents/MacOS/s2arm-host"
    guard = executable.parent / VAGUARD
    if not executable.is_file() or stamp.get("sha256") != digest(executable):
        return False
    if not guard.is_file() or not stamp.get("guard_sha256") or stamp.get("guard_sha256") != digest(guard):
        return False
    for target in (guard, app):
        if subprocess.run(["codesign", "--verify", "--strict", str(target)], capture_output=True).returncode != 0:
            return False
    return True


def compile_host(c, fingerprint, env, log_root):
    from embedded import artifact
    bundled = artifact(c, "S2ArmGame.app")
    if bundled:
        if read(c.engine / "engine.json", {}).get("memoryRequirementsVersion") != 2:
            raise Failure("请更新 S2Arm，再重新打包这款游戏。", code="runtime_invalid")
        c.emit("cache", "已校验内置 S2Arm Mac 宿主", cacheHit=True)
        ms.reuse(c, "host")
        return bundled
    host_root = c.private / "tools/mac-host" / fingerprint
    app = host_root / "S2ArmGame.app"
    executable = app / "Contents/MacOS/s2arm-host"
    stamp = read(host_root / "ready.json", {})
    if host_cache_ok(app, stamp):
        c.emit("cache", "已校验并复用 S2Arm Mac 宿主（含地址窗口护栏库）", cacheHit=True)
        ms.reuse(c, "host")
        return app
    # Create the real output directory before aliasing it.  A symlink to a
    # not-yet-existing directory makes make's mkdir -p fail with ENOENT.
    (host_root / "build").mkdir(parents=True, exist_ok=True)
    # Cache roots also reach the linker through services/*.sh.  Keep their
    # real directories present, then expose stable no-space aliases so paths
    # under this repository's spaced parent are not split by make/shell.
    cache_keys = ("S2A_RUN_ROOT", "S2A_CS_CACHE", "S2A_VPX_CACHE",
                  "S2A_OPUS_CACHE", "S2A_DAV1D_CACHE")
    for key in cache_keys:
        raw = env.get(key)
        if not raw:
            if key == "S2A_DAV1D_CACHE" and env.get("S2A_RUN_ROOT"):
                raw = str(Path(env["S2A_RUN_ROOT"]) / "s2arm/cache/dav1d")
            else:
                continue
        path = Path(raw)
        try:
            path.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            raise Failure(f"无法准备构建缓存目录：{path}") from error
        env[key] = str(nospace(path))
    engine = nospace(c.engine)
    build = nospace(host_root / "build")
    args = ["make", "-C", engine, "host", "S2A_CS=1", "S2A_MAC_FRONTEND=1",
            "ROOT=" + str(engine), "BUILD=" + str(build),
            "DEVELOPER_DIR=" + env["DEVELOPER_DIR"]]
    # c.env() 会清掉 S2A_*；构建开关显式带给 make（缺省 auto = 引擎快照里的 make 按本机探测，tools/av1hw.sh / tools/vp9hw.sh）
    for k in ("S2A_DAV1D", "S2A_VPX"):
        v = os.environ.get(k)
        if v in ("0", "1"):
            args.append(k + "=" + v)
    c.run(args, log_root / "host.log", env)
    binary = host_root / "build/s2arm-host"
    if not binary.is_file():
        raise Failure("S2Arm Mac 宿主未生成。")
    executable.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, executable)
    # 地址窗口护栏（宿主弱链接 @executable_path/libs2a_vaguard.dylib，macOS 27 上没有它窗口会被 malloc 占走、起不了局；issue #100 D01）
    guard = host_root / "build" / VAGUARD
    if not guard.is_file():
        raise Failure("S2Arm Mac 宿主缺地址窗口护栏库 libs2a_vaguard.dylib。")
    shutil.copy2(guard, executable.parent / guard.name)
    c.run(["codesign", "--force", "--sign", "-", executable.parent / guard.name], log_root / "sign.log", env)
    # 不进系统「游戏模式」：游戏模式下 GPU 忙着也被压在最低频（上游参考实现（MIT）cb59a672 / frame-rate.md：一个约 100 行、不含宿主代码的全屏 Metal 程序
    #   游戏模式下约 160 秒后 618 → 338 MHz、9.5 → 18 ms 一帧；关掉 924 MHz、约 7 ms；一款大型开放世界游戏 10.5 → 19.7 ms 落到 30 帧）。LSSupportsGameMode false 是苹果给的关法，
    #   保留游戏类别。系统记得认过游戏的包名，同一包名改成 false 仍开着游戏模式：包名换成 org.s2arm.macos.host，老玩家机器也从头算（第一次起会再问一次蓝牙权限）；
    #   手柄重连记忆的存储名 org.s2arm.macos.game（NS2LauncherController 的 UserDefaults 套件）与包名无关，不改
    plist = dict(CFBundleIdentifier="org.s2arm.macos.host", CFBundleName="S2Arm Game",
                 CFBundleExecutable="s2arm-host", CFBundlePackageType="APPL", LSMinimumSystemVersion="15.0",
                 NSBluetoothAlwaysUsageDescription="连接 Switch 2 Pro 手柄，接收按键与体感并发送游戏震动。",  # 非玩家句
                 NSHighResolutionCapable=True, LSSupportsGameMode=False,
                 LSApplicationCategoryType="public.app-category.games")
    (app / "Contents/Info.plist").write_bytes(plistlib.dumps(plist))
    c.run(["codesign", "--force", "--sign", "-", app], log_root / "sign.log", env)
    atomic(host_root / "ready.json", dict(schema=1, sha256=digest(executable), guard_sha256=digest(executable.parent / VAGUARD)))
    return app


def romfs_list_path():
    # romfs_list.py 两个落点：出包布局里随 Jobs 一起带（build_app.py 拷进 Resources/Jobs，同 redact.py 的规矩，
    # 出包后 Jobs 的 parents[2] 是 Contents、没有 gfx，11-07 首跑即 input_missing）；源码树里在仓库根 graphics/nvn/tools。
    here = Path(__file__).resolve()
    for candidate in (here.parent / "romfs_list.py", here.parents[2] / "graphics/nvn/tools/romfs_list.py"):
        if candidate.is_file():
            return candidate
    raise Failure("RomFS 清单实现缺失：romfs_list.py 不在 Jobs 目录，也不在仓库 graphics/nvn/tools。")


def clone_tree(c, source, destination, *, materialize_romfs=False):
    """→ True = 每个文件都是真克隆（和源同属一个克隆族，不另占盘）；有一个退成普通复制就 False"""
    # 统一盘点也用于普通目录克隆；不修改模块搜索路径，避免和作业模块重名。
    spec = importlib.util.spec_from_file_location("_s2arm_clone_romfs_list", romfs_list_path())
    romfs_list = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(romfs_list)
    tree = romfs_list.scan(source)
    error = next(iter(tree.errors.values()), None)
    if error is None:
        error = next((e.lstat_error for e in tree.entries if e.lstat_error is not None), None)
    if error is not None:
        raise Failure("准备产物目录读取失败：" + str(error)) from error  # 非玩家句
    entries = sorted(Path(e.path) for e in tree.entries)
    del tree
    romfs = None
    for p in entries:
        if not p.is_symlink():
            continue
        if materialize_romfs and p == source / "romfs":
            # 仅展开 roots.sh 定义的本款共享存储链接；发布包不依赖构建缓存。
            game = source.name.split("@", 1)[0]
            link = os.readlink(p)
            prefix = f"../.romfs_store/{game}/"
            identity = link.removeprefix(prefix)
            if not link.startswith(prefix) or not identity or identity.startswith(".") or "/" in identity:
                raise Failure("RomFS 链接不属于本款共享存储。")
            expected = resolve_path(source.parent) / ".romfs_store" / game / identity
            if not expected.is_dir() or resolve_path(expected) != expected.absolute():
                raise Failure("RomFS 共享存储缺失或包含符号链接。")
            romfs = expected
        else:
            raise Failure("输入目录包含符号链接。")
    # 镜像形态（romfs/ 不在、只有 romfs.std）：镜像是一个普通文件，下面按普通文件克隆进运行版本（APFS 同卷一次克隆，不再逐个克隆几万个文件）
    if materialize_romfs and not ((source / "romfs").is_dir() or has_runtime_romfs(source)):
        raise Failure("准备产物缺少 RomFS 游戏资源目录。")
    # 空目录也要建：拷出的目录与源一模一样（RomFS 里的空目录、宿主包结构；10-23 走查起因是当年快照 .git 的空 refs 目录）
    for d in (p for p in entries if p.is_dir() and not p.is_symlink()):
        (destination / d.relative_to(source)).mkdir(parents=True, exist_ok=True)
    files = [p for p in entries if p.is_file()]
    total = sum(p.stat().st_size for p in files)
    done, cloned = 0, True
    for p in files:
        c.check()
        if p.is_symlink():
            raise Failure("输入目录包含符号链接。")
        dst = destination / p.relative_to(source)
        cloned = clone(p, dst, lambda n, t: c.progress(done + n, total)) and cloned
        shutil.copymode(p, dst)
        done += p.stat().st_size
    if romfs is not None:
        (destination / "romfs").mkdir(parents=True, exist_ok=True)
        cloned = clone_tree(c, romfs, destination / "romfs") and cloned
    return cloned




def shared_dir(c, game):
    """一款游戏各平台共用的准备目录 games/<英文名>/build/：解包（extract/ + extract-checkpoint.json）只此一份，Mac / iPhone 都从这里取（11-02 维护者：多平台导出平台无关阶段只做一次）"""
    return contained(layout.root_of(c, game["id"]), layout.build_dir(c, game["id"]))


def extract_valid(c, game):
    """共用解包目录能不能直接复用：快照里的所选包身份与现在相同、文件逐个对得上（只看大小与类型，不读内容）"""
    base = shared_dir(c, game)
    # 快照连同所选包的身份一起记（原先解包目录在按 inputIdentity 分的 <键>/ 下；挪到共用目录后换了所选包 / DLC 不能还认旧解包）
    checkpoint = read(base / "extract-checkpoint.json", {})
    try:
        if checkpoint.get("inputIdentity") != input_identity(game):
            raise Failure("所选包变了")
        validate_files(c, base / "extract", checkpoint.get("files"))
        return True
    except (Failure, OSError, ValueError, AttributeError, KeyError):
        return False


def reader_program(c):
    """读取器（nx_unpack）的可直接执行的程序，给要自己调它的脚本（ns2_dump 解 ZBIC 压缩的 NSO：S2A_NX_UNPACK）：
    Windows 发版预编件 dll 旁边就有 nx_unpack.exe；Mac 的读取器本身就是自包含可执行文件（发版件 NativeAOT 单文件 / 源码树 prepare 照同一配方编）"""
    exe = Path(toolchain.reader(c))
    if os.name == "nt":
        return exe.with_suffix(".exe")
    return exe


def warn_offline_empty(c, root):
    """直连库只能运行时现编（runtime_only_lib.sh 写 shaders/direct/runtime_only.txt，首行是原因）：出包照常算成功，但完成页要说（#470：
    抽取红、离线 0 支，完成页却是纯绿的「出包完成」、diagnostics 为空）。Mac 包（build.prepare）与 iPhone 包（ipa.build）共用"""
    p = Path(root) / "shaders/direct/runtime_only.txt"
    if not p.is_file():
        return
    why = (p.read_text(encoding="utf-8", errors="replace").strip().splitlines() or [""])[0][:300]
    c.diagnostic("这款的着色器离线预编 0 支，游戏能玩，但首次遇到的画面要现编、开头会卡顿" + (f"（{why}）" if why else ""), code="direct_offline_empty", severity="warning")  # 非玩家句


DEGRADE_HEAD = "# 阶段\t类别\t处数\t玩家提示\t说明"  # 非玩家句


def degrade_rows(text):
    """降级表（各出包阶段降级时写在产物根的 degrade.<阶段>.tsv，开发丙线 v0.3.1 约定）→ (行, 格式不对的行号)。
    每行 阶段 / 类别 / 处数 / 玩家提示 / 说明 五列，以 # 开头的是表头；格式不对的跳过、不报错（拿不准不停）"""
    rows, bad = [], []
    for i, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) != 5 or not f[2].strip().isdigit():
            bad.append(i)
            continue
        rows.append(dict(stage=f[0].strip(), kind=f[1].strip(), count=int(f[2]), hint=f[3].strip(), note=f[4].strip()))
    return rows, bad


def collect_degrade(c, root):
    """出包成功收尾：读产物根的 degrade.*.tsv → 完成卡上逐条显示的玩家提示（去重、保序）。
    同时把整表存进本任务日志目录（<任务>-degrade.log）：产物根出包后会清，诊断包只收日志目录（同 save_rewrite_digest），「降级汇总」也从这里算。
    Mac 包（prepare）与 iPhone 包（ipa.build）共用"""
    if not root:
        return []
    rows = []
    for p in sorted(p for p in Path(root).glob("degrade.*.tsv") if not layout.os_junk(p.name)):
        got, bad = degrade_rows(p.read_text(encoding="utf-8", errors="replace"))
        rows += got
        if bad:
            c.emit("message", "降级表 %s 有 %d 行格式不对，已跳过（第 %s 行）" % (p.name, len(bad), "、".join(map(str, bad[:10]))))
    if not rows:
        return []
    try:
        log = c.logs / f"{c.task}-degrade.log"
        log.write_text(DEGRADE_HEAD + "\n" + "".join("\t".join([r["stage"], r["kind"], str(r["count"]), r["hint"], r["note"]]) + "\n" for r in rows), encoding="utf-8")
    except OSError as e:
        c.emit("message", f"降级表没存进日志目录（{e}），诊断包里会少这一份；出包结果不受影响")
    return list(dict.fromkeys(r["hint"] for r in rows if r["hint"]))


def extract_nup(c, game, packages, extraction, env, logs):
    """Switch 2 NUP 容器（导入时 library.nup_inspect 记 format=nup）→ 共用解包目录：pack/ns2_nup.py --to-extract（展开 + pack/ns2_dump.py 整理，
    与一键出包 pack/oneclick/oneclick.sh / pack/orch/oneclick_win.py 同两步、同目录布局，共享解包阶段 pack/stages/unpack.sh 也走它）。
    产物与 nx_unpack 解包同形（exefs/ romfs/ ctl/ aoc/ unpack.tsv），往后的 NPDM / NACP / 程序位数 / 追加内容检查与快照同 Switch 1 原件。不要 prod.keys。
    一个 NUP 自带本体 + 更新 + DLC，整个容器一起展开：同一款放过几个 NUP（如新旧两版）时用选中的最新更新所在的那个（没有更新用本体所在的），
    别的 NUP 里选中的 DLC 这次不带，发黄字 aoc_missing（同仅授权 DLC 的口径，不静默）"""
    nups = [p for p in packages if p.get("format") == "nup"]
    main = next((p for p in nups if p["type"] == "update"), None) or next(p for p in nups if p["type"] == "base")
    source = source_path(c, main)
    left = [p["titleId"] for p in packages if p["type"] == "dlc" and source_path(c, p) != source]
    if left:
        c.diagnostic("追加内容 %d 份在另一个 NUP 里，包里不含（%s）：同一款只留一个包含全部内容的 NUP" % (len(left), "、".join(left[:5])),
                     code="aoc_missing", severity="warning")
    # --name：没有控制分区时 ns2_dump 拿它当标题，用游戏库里的名字（导入时同样取自文件名）；S2A_NX_UNPACK：ns2_dump 解 ZBIC 压缩的 NSO 用
    c.run(
        [
            sys.executable,
            "-X",
            "utf8",
            c.engine / "pack/ns2_nup.py",
            "--to-extract",
            source,
            extraction,
            "--name",
            game["name"],
        ],
        logs / "ns2_nup.log",
        dict(env, S2A_NX_UNPACK=str(reader_program(c))),
    )


EXTRACTED_FORMATS = ("dump", "extract", "loose")


def _dir_link(source, link):
    """目录链接：link → source。Windows 上建符号链接要开发者模式或管理员（普通玩家 WinError 1314「客户端没有所需的特权」，
    v0.4.0 已解包散文件出包在解包一步就失败，诊断#1fad25）：先建目录联接（本机磁盘不要特权），联接不支持的位置（网络盘等）再试符号链接，
    都不行说清楚。Mac / Linux 照旧符号链接"""
    if os.name != "nt":
        os.symlink(source, link)
        return
    from pack.orch.winlong import create_junction
    try:
        create_junction(str(source), str(link))
        return
    except OSError as e:
        junction_error = e
    try:
        os.symlink(source, link, target_is_directory=True)
    except OSError as e:
        raise Failure(f"没法把已解包的游戏文件夹接进出包目录：Windows 建不了目录联接（{junction_error.strerror or junction_error}），"
                      f"也建不了符号链接（{e.strerror or e}）。游戏文件夹在网络盘上时请先拷到本机磁盘，或在 Windows 设置里打开「开发者模式」后重试。",
                      code="permission_denied") from e


def extract_folder(c, game, packages, extraction, env, logs):
    """已解包的游戏文件夹（导入时 library.extract_inspect 记 format）→ 共用解包目录，不要 prod.keys：
    extract 本来就是解包产物，克隆过来；dump（Switch 2 导出）交 pack/ns2_dump.py 整理（同 NUP）；loose（exefs + romfs 平铺）软链成 base/ 再走同一条路。"""
    main = next(p for p in packages if p.get("format") in EXTRACTED_FORMATS)
    source, fmt = source_path(c, main), main["format"]
    if fmt == "extract":
        c.run([sys.executable, c.engine / "tools/clonedir.py", source, extraction], logs / "extract.log", env)
        return
    with tempfile.TemporaryDirectory(prefix="loose-", dir=c.private) as stage:
        if fmt == "loose":
            _dir_link(source, Path(stage) / "base")
        c.run([sys.executable, "-X", "utf8", c.engine / "pack/ns2_dump.py", stage if fmt == "loose" else source, extraction, "--name", game["name"]],
              logs / "ns2_dump.log", dict(env, S2A_NX_UNPACK=str(reader_program(c))))


def ensure_extract(c, game, packages, env, logs, valid=None):
    """共用解包目录：校验快照对得上就复用（打复用行），否则按所选包重解、核程序位数与 DLC、写快照。→ 解包目录。
    valid：调用方刚判过的 extract_valid（Mac 打包发计划前判，免得算两遍）"""
    base = shared_dir(c, game)
    base.mkdir(parents=True, exist_ok=True)
    extraction = base / "extract"
    identity = input_identity(game)
    if valid is None:
        valid = extract_valid(c, game)
    ms.reuse(c, "unpack", valid)   # 打 iPhone 包在这一步开始后才判：当场改标、重发计划
    ms.enter(c, "unpack")
    if not valid:
        if extraction.exists():
            from library import _rmtree
            _rmtree(extraction)   # 走统一删除：只读 / Windows 封存 / 删的同时有程序往里写系统文件（#369 外置盘 Errno 66）都在这一处处理
        paths = list(dict.fromkeys(source_path(c, p) for p in packages))
        if any(p.get("format") == "nup" for p in packages):
            extract_nup(c, game, packages, extraction, env, logs)
        elif any(p.get("format") in EXTRACTED_FORMATS for p in packages):
            extract_folder(c, game, packages, extraction, env, logs)
        else:
            try:
                c.run(
                    [
                        toolchain.reader(c),
                        c.keys,
                        extraction,
                        "--aoc-index",
                        "--title-id=" + game["id"],
                        *paths,
                    ],
                    logs / "extract.log",
                    env,
                )
            except Failure as error:
                # #1411：打 iPhone 包（ipa）在自己里面解包，读取器失败原先落进通用 tool_exit（「工具跑失败」），原因被藏；
                #   Mac 准备（build）在 core.run 里已按 build_tool_failure 归好类、码不是 tool_exit，不走这里。两种作业同一判法：按解包阶段归类
                if error.code != "tool_exit":
                    raise
                from errors import cause_facts, classify_failure, player_words, tool_reason
                code, why = classify_failure("unpack", logs / "extract.log")
                said, advice = player_words(code, logs / "extract.log")
                raise Failure(said, code=code, advice=advice, internal="解包失败：" + tool_reason(logs / "extract.log") + "\n" + why,  # 非玩家句
                              facts=cause_facts(code, logs / "extract.log")) from error
        c.emit(
            "message", "解包已完成，正在校验合并结果并写入缓存"
        )  # 解包读满 100% 后还要校验、写缓存：说明在做什么，进度不停在「解包 100%」（社区贡献者补丁 #8）
        if (
            not (extraction / "exefs/main.npdm").is_file()
            or not (extraction / "ctl/control.nacp").is_file()
        ):
            raise Failure("解包结果缺少 NPDM 或 NACP。")
        # 程序位数只走 nx_unpack --npdm-isa（全仓唯一判法，pack/nx_unpack/Inspect.cs NpdmIsa）：读不了 / unknown 退 3，同样拒
        isa_log = logs / "npdm_isa.log"
        c.run(
            [toolchain.reader(c), "--npdm-isa", extraction / "exefs/main.npdm"],
            isa_log,
            env,
            ok_codes=(0, 3),
        )
        isa = (isa_log.read_text(encoding="utf-8", errors="replace").strip().splitlines() or [""])[-1].strip()
        if c.last_rc != 0 or isa != "aarch64":
            raise Failure("合并后的游戏程序不是有效的 AArch64 NPDM（nx_unpack --npdm-isa：" + (isa or "无输出") + "）。")  # 非玩家句
        # 所选 DLC 有一行文件数 -1（nx_unpack：数据不在给的文件里 = 只有授权，或数据分区解不开）：本体照常出、不挡，
        #   但不能静默变成仅授权——同 oneclick / pack/stages/aoc.sh 与出包完成的口径发黄字 aoc_missing，完成页画「完成（有警告）」
        #   （审计 P1-89：原先整单失败、本体也不出，如暗黑2 卡带里 100 KB 的密钥代号 DLC；命令行 oneclick 不拦本体，两边口径不一）
        rows = (extraction / "aoc.tsv").read_text().splitlines() if (extraction / "aoc.tsv").is_file() else []
        lic = [row.split("\t")[1] for row in rows[1:] if row.strip() and int(row.split("\t")[3]) < 0]
        if lic:
            c.diagnostic("追加内容 %d 份只有授权或数据解不开，包里不含（%s）" % (len(lic), "、".join(lic[:5]) + (" 等" if len(lic) > 5 else "")), code="aoc_missing", severity="warning")
        atomic(base / "extract-checkpoint.json", {"inputIdentity": identity, "files": snapshot(c, extraction)})
    else:
        c.emit("cache", "已验证并复用解包产物", cacheHit=True)
    return extraction


def oneclick_kuan(c, gid):
    """一键出包给这款起的款名（pack/oneclick/oneclick.sh 同口径）：titles.tsv 里有用登记名，没有用 t<程序号小写>"""
    t = c.engine / "pack/oneclick/titles.tsv"
    for line in (t.read_text(encoding="utf-8").splitlines() if t.is_file() else []):
        f = line.split("\t")
        if len(f) >= 2 and not line.startswith("#") and f[0].upper() == gid.upper():
            return f[1].strip()
    return "t" + gid.lower()


def sibling_roots(c, game):
    """同一款在别的平台已有的现行产物根（新的在前）：iPhone games/<英文名>/build/ios/roots/*@现行、Mac games/<英文名>/build/<键>/roots/<程序号>@现行"""
    found = []
    for link in [*layout.ios_work(c, game["id"], create=False).glob("roots/*@现行"), *(p for p in layout.build_dir(c, game["id"], create=False).glob("*/roots/*@现行") if p.parent.parent.name != "ios")]:  # 非玩家句
        try:
            target = Path(os.readlink(link)) if _path_is_link(link) else link
            cur = contained(c.library, resolve_path(target if target.is_absolute() else link.parent / target))
        except (Failure, OSError):
            continue
        if cur.is_dir():
            found.append((cur.stat().st_mtime, cur))
    return [p for _, p in sorted(found, reverse=True)]


def _path_is_link(path):
    return path.is_symlink() or getattr(os.path, "isjunction", lambda _: False)(path)


def seed_from_sibling(c, game, roots, kuan):
    """本平台还没有现行根、别的平台有：克隆它的现行根起步（不从解包目录重新播种）。平台无关阶段的键按内容算，与本树一致就跳过、不一致照常重做
    （兄弟根是旧提交出的、键不同 → 重做）。克隆走 tools/clonedir.py：同一 APFS 卷 clonefile，跨卷 / exFAT 等退回普通拷贝（目标不存在才改名发布），--report 打出走了哪条路。
    → 用了兄弟根 True；没有可用的兄弟根 False（调用方照常从解包目录播种）"""
    mine = resolve_path(roots) if roots.exists() else roots
    for cur in sibling_roots(c, game):
        if cur.parent == mine:
            continue
        roots.mkdir(parents=True, exist_ok=True)
        dst = roots / (kuan + "@sib-" + cur.name.split("@", 1)[-1])
        if dst.exists():
            shutil.rmtree(dst)
        log = c.logs / (c.task + "-sibling.log")
        log.parent.mkdir(parents=True, exist_ok=True)
        try:
            c.run([sys.executable, c.engine / "tools/clonedir.py", "--report", cur, dst], log, c.env())
        except Failure:
            # 克隆失败（盘满、权限、源被删等）：不用兄弟根，回到从共用解包目录播种（解包不重做），黄字说清走了哪条路
            shutil.rmtree(dst, ignore_errors=True)
            why = (log.read_text(encoding="utf-8", errors="replace").strip().splitlines() or ["原因未记"])[0]  # 非玩家句
            c.diagnostic(f"兄弟平台的产物根 {cur.name} 克隆不过来（{why}），改从解包目录准备，平台无关阶段这次要重做", code="sibling_clone_failed", severity="warning")
            return False
        route = (log.read_text(encoding="utf-8", errors="replace").strip().splitlines() or [""])[-1]
        link = roots / (kuan + "@现行")  # 非玩家句
        tmp = roots / ("." + kuan + "@现行.new")  # 非玩家句
        tmp.unlink(missing_ok=True); tmp.symlink_to(dst.name); tmp.replace(link)
        src = (cur / ".root_src").read_text().strip()[:9] if (cur / ".root_src").is_file() else "未知"  # 非玩家句
        c.emit("cache", f"复用兄弟平台的产物根 {cur.name}（提交 {src}；{route}）：平台无关阶段按键核对，键相同跳过、不同照常重做", cacheHit=True)
        return True
    return False


# 运行时不用、但会留在产物根顶层的项：pack/ipa_tail.py 的打包工作区 pkg/（打 iPhone 包时资源包与 ipa 都先落在这里）、组装临时 App build/、
# 阶段日志与来源标记。image/mac 是 prepare() 从产物根克隆出来的，这些项属于「出包过程」而不属于「运行版本」；
# 原先只清了顶层的 *.log 与三个标记文件，pkg/ 与 build/ 跟着克隆进了运行版本，此后没有任何一步回收（ipa_tail 只在下一次运行开头清 build/）。
RUNTIME_EXTRAS = ("seed.txt", ".root_src", ".root_base", "pkg", "build")


def prune_runtime_extras(root):
    """删掉运行版本里不是运行依赖的顶层项（RUNTIME_EXTRAS 与顶层 *.log）。只动顶层、不跟软链。

    运行依赖全在 romfs/、exefs/、ctl/、systemdata/、shaders/ 里：validate_runtime() 只核 romfs/ 与清单、
    check_pack_inputs() 只核 exefs/main.npdm、ctl/control.nacp、systemdata/fonts、shaders/direct，都不看 pkg/ 与 build/。
    清单 snapshot() 在本函数之后量，删掉的项因此也不会再记进 manifest.json（否则起局后的 missing_after_crash 会按清单照样报缺失）。"""
    for log in root.glob("*.log"):
        if not log.is_dir():
            log.unlink(missing_ok=True)
    for name in RUNTIME_EXTRAS:
        extra = root / name
        if extra.is_symlink() or extra.is_file():
            extra.unlink(missing_ok=True)
        elif extra.is_dir():
            from library import _rmtree
            _rmtree(extra)   # 走统一删除：pkg/pack/romfs 克隆自产物根，可能带只读目录；删不掉要报出来，不能悄悄留半棵进运行版本


@contextlib.contextmanager
def produce_locks(c):
    """出包（Mac prepare、iPhone ipa.build）拿的锁（执行锁读写化，11-19）：执行锁共享（和起局并行，挡全库独占动作）+ 出包锁独占（两次出包照旧
    一个接一个：共用编译缓存、缓存修剪 cache_retention 要独占）+ 这一款的按款锁（这款在玩 / 在升级 S 级优化时等它结束）"""
    import sgrade
    with c.lock("execution", shared=True), c.lock("produce"), c.lock(sgrade.game_lock(c.r.get("gameId") or "")):
        yield


def prepare(c):
    with produce_locks(c):
        c.begin("检查游戏输入")
        c.require_source()
        game = c.game()
        if not __import__("re").fullmatch(r"[0-9A-F]{16}", game["id"]):
            raise Failure("无效的游戏标识。")
        require_game(c, game)
        from library import apply_location, check_pack_space
        apply_location(c, game)   # 出包页选的存放位置（游戏库结构 v3）：在别处就先搬过去
        check_pack_space(c, game)
        packages = selected_packages(game)
        size_gb = sum(p.get("size", 0) for p in {p["sha256"]: p for p in packages}.values()) / 1e9
        # 先定这次的产物根（键 = 引擎源码、工具版本、所选包、字体、引擎号），再发计划：哪几步会复用缓存要在计划里先标出来（11-07）
        fingerprint = source_fingerprint(c)
        fonts = c.fonts_dir()
        if fonts is None:
            raise Failure("应用自带的共享字体缺失，请重新安装完整应用。")
        env = c.env()
        from embedded import artifact
        if c.r.get("engineBundle"):
            artifact(c, "Prepare")
        versions = {}
        for name, command in (
            ("sdk", ["xcrun", "--sdk", "macosx", "--show-sdk-version"]),
            ("metal", ["xcrun", "--sdk", "macosx", "metal", "--version"]),
        ):
            ok, version = toolchain.probe(c, command)
            if not ok:
                code = {"sdk": "xcode_unavailable", "metal": "metal_unavailable"}[name]
                raise Failure("构建工具不可用：" + name, code=code)  # 非玩家句
            versions[name] = version
        identity = dict(schema=1, target="macos-arm64", source=fingerprint, tools=versions,
                        inputIdentity=input_identity(game), fonts=tree_hash(fonts, ["."]),
                        engineBundleId=read(c.engine / "engine.json", {}).get("id"))
        key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:24]
        root = contained(layout.root_of(c, game["id"]), layout.build_dir(c, game["id"]) / key)
        root.mkdir(parents=True, exist_ok=True)
        # 会复用缓存的步：共用解包快照对得上 → 解包；现行镜像就是从这一份产物根（同样的代码、工具、输入）出的（game.json imageKey）→ 改写与准备资源、
        #   直连库（各阶段按键跳过）。直连库若其实要重出（跑局知识等变了，DirectWatcher 看到直连临时版本），当场改回冷步
        unpack_ok = extract_valid(c, game)
        warm = layout.game_info(c, game["id"]).get("imageKey") == key and (root / "roots" / (game["id"] + "@现行")).exists()  # 非玩家句
        ms.plan(c, size_gb, "build_dual", reused=(["unpack"] if unpack_ok else []) + (["refresh", "analyze", "direct"] if warm else []))
        logs = c.logs / c.task
        logs.mkdir(parents=True, exist_ok=True)
        env.update(S2A_ROOTS=str(root / "roots"), S2A_ROOTS_SHARED=str(root / "never-shared"),
                   S2A_ROOTS_PRIV=str(root / "roots-private"), S2A_FONTS=str(fonts))
        extraction = ensure_extract(c, game, packages, env, logs, valid=unpack_ok)
        import patches   # 金手指 / 社区补丁（11-12）：解包后按真构建号查库，交改写阶段（S2A_CHEATS_DIRS / S2A_CPATCH_DIRS）
        patches.prepare(c, game, extraction, env)
        env["S2A_NRO_KEPT_DIR"] = str(layout.nro_kept(c, game["id"]))   # 运行时留下的 NRO（#8be819），没有 = 改写阶段当无
        # The memory table is collected from the repository table and the recent run logs only; the previously
        # installed table is not fed back (its values would never age out once the host stops needing them)
        if not (root / "roots" / (game["id"] + "@现行")).exists() and not seed_from_sibling(c, game, root / "roots", game["id"]):  # 非玩家句
            c.run(["bash", c.engine / "pack/seed_root.sh", game["id"], "import-" + key, extraction], logs / "seed.log", env)
        # 出包同一条流水线（10-26 维护者 / 维护者「iOS 和 macOS 不要这儿新那旧的」）：和一键出包 --target mac 一样调 pack/refresh.sh——改写 ‖ 系统数据 ‖ 宽屏布局 ‖
        #   追加内容 → 直连配置推导 → RomFS 索引 → 抽取 → 直连库 → 直连出成则通用 lazy（C.3：不翻译 / 编译 / 预建管线，首遇安全网兜底）。
        #   原先这里自己逐段调、只出通用全量：侍魂晓预建 13187 条管线 38 分钟、「备用快速版本」是通用函数体动态库，直连库一个都不出。
        #   10-30 起直连单轨（维护者）：直连做不成 refresh.sh 报红「直连单轨停在 <哪一步>」退 1，任务按它报红（tool_reason 取日志里的红行），不退成通用；
        #   11-07 兼容模式（只出通用）整条拆掉。tests/pack_entry_test.sh 防回退
        ms.enter(c, "refresh")
        env.update(S2A_PACK_PLAT="macos", S2A_WIDEN_AUTO_RUN="0", S2A_WIDEN_PKG="0")   # 宽屏自动出表按 iPhone 屏幕比例补画面、要占 Mac 局槽跑闭环；Mac 窗口按 16:9（同 --target mac）
        # refresh.sh 链上的追加内容 / 原件核对要原始包、密钥和 nx_unpack（同一键出包：pack/oneclick/oneclick.sh 设 S2A_GAME_SRC / S2A_KEYS）：
        #   原始包 = 这款选中的原件软链进临时目录（stage_sources；不设就去扫 ~/Downloads：玩家机上弹「访问下载文件夹」授权框，10-26 侍魂晓日志实测扫了）；
        #   nx_unpack = 应用自带的读取工具（toolchain.reader，自包含单文件：发版件 NativeAOT 预编，源码树 prepare 照同一配方编），直接可执行
        src = root / "src"; shutil.rmtree(src, ignore_errors=True)
        stage_sources(c, packages, src)
        env.update(
            S2A_GAME_SRC=str(src), S2A_KEYS=str(c.keys), S2A_NX_UNPACK=str(toolchain.reader(c))
        )
        try:
            with ms.DirectWatcher(c, root / "roots", game["id"]):
                c.run(["bash", c.engine / "pack/refresh.sh", game["id"]], logs / "refresh.log", env)
        finally:
            shutil.rmtree(src, ignore_errors=True)
        current = resolve_path(root / "roots" / (game["id"] + "@现行"))  # 非玩家句
        contained(root, current)
        if not (current / "shaders/direct/keys.tsv").is_file():   # 只有直连：refresh.sh 退 0 却没有直连库就是出包出错（11-07 拆通用模式）
            raise Failure("直连着色器库不完整。")
        warn_offline_empty(c, current)
        notes = collect_degrade(c, current)   # 降级继续的玩家提示：随结果交给完成卡（v0.3.1）
        ms.enter(c, "host")
        host = compile_host(c, fingerprint, env, logs)
        ms.enter(c, "install")
        # 镜像只留一份（游戏库结构双端统一）：新镜像先出到 image/mac.partial，校验过再换进来；出成功前旧镜像不动。宿主进 hosts/mac 多款共用
        image = contained(layout.root_of(c, game["id"]), layout.image_mac(c, game["id"]))
        temporary = image.with_name("mac.partial")
        if temporary.exists():
            shutil.rmtree(temporary)
        temporary.parent.mkdir(parents=True, exist_ok=True)
        temporary.mkdir()
        try:
            all_cloned = clone_tree(c, current, temporary, materialize_romfs=True)
            patches.record(c, game["id"], "mac", current)   # 补丁打入结果（打上 / 没打上，完成页列，11-14）：产物根里编译器的记录
            # Transient stage logs, the packaging workspace and source paths are not runtime dependencies.
            prune_runtime_extras(temporary)
            if not c.r.get("engineBundle") and fingerprint != source_fingerprint(c):   # 只有源码树开发会在出包中途改源码；发版引擎冻结
                raise Failure("构建期间 S2Arm 源码发生变化，请重试；旧运行版本已保留。")
            manifest = dict(identity, memoryRequirementsVersion=2,
                            memoryRequirementsSha256=digest(current / "addr_need.tsv"),
                            engine="S2Arm", gameId=game["id"], gameName=game["name"], compatibility="unverified",
                            files=snapshot(c, temporary), **(host_update.engine_compat(c) or {}),   # 只换宿主的判据（host_update.py）
                            rewriterId=__import__("rewrite_refresh").rewriter_id(c),
                            bfGen=__import__("sgrade_bf").BF_GEN, libGen=host_update.LIB_GEN, builtBy=host_update.built_by(c))   # 直连库的可读瓦片版代数（S 级升级按它判要不要补 _bf，pack/jobs/sgrade_bf.py）   # 改写产物出自哪版改写器：改写器修了，起局前只核记号不同的（rewrite_refresh.refresh_mac）
            atomic(temporary / "manifest.json", manifest)   # v0.2.3 清理：不再紧接着全量复验——清单就是同一进程刚从这些文件量出来的
            sha = host_update.install_host(c, host)
            c.check()
            old = image.with_name("mac.old")
            if old.exists():
                shutil.rmtree(old)
            if image.exists():
                os.replace(image, old)
            os.replace(temporary, image)
            layout.update_info(c, game["id"], hostMac=sha, hostFailed=[], imageKey=key)
            import onerom   # 一款一份资源（0.6.0）：这款已有 iPhone 资源包、RomFS 是同一份 → 新 Mac 运行版本直接链到它、删掉刚克隆进来的 romfs.std
            onerom.note(c, game["id"], onerom.dedupe(c, game["id"]))
            # 出包实测账（library.record_pack_usage）：在清旧镜像之前量，新旧同在盘上才是这一单真占到的峰值
            from library import record_pack_usage
            record_pack_usage(c, game)
            # 新运行版本已换上：以下只是省盘 / 记账 / 刷新显示，出错只提示、不把这次出包判失败（v0.2.3 清理，#343）
            if old.exists():
                host_update.tidy(c, "删除上一版运行文件", shutil.rmtree, old)
            host_update.tidy(c, "清理不再用的旧运行层", host_update.prune, c)
        finally:
            if temporary.exists():
                shutil.rmtree(temporary)
        host_update.tidy(c, "记录出包里程碑用时", ms.record, c, size_gb)
        from library import _part_bytes, _rm
        # 解包和中间目录：运行版本整棵是克隆出来的（APFS 同卷，和产物根、解包同属一个克隆族）就保留——几乎不另占盘，下次出另一端的包不用重解（#329，
        #   同一款的数据在盘上只占一份）；有文件退成了普通复制（跨卷、非 APFS、Windows）运行版本已是独立的一份，照旧删掉解包省一份
        unpacked = layout.build_dir(c, game["id"], create=False)
        if unpacked.exists() and not all_cloned:
            host_update.tidy(c, "删除解包目录", _rm, c, layout.root_of(c, game["id"]), unpacked)
        import sizes
        host_update.tidy(c, "刷新游戏占用大小", sizes.refresh, c, game["id"])   # 每款各部分大小写回 game.json（游戏库只读不量）
        return dict(gameId=game["id"], outputPath=str(image), outputBytes=_part_bytes([image]), target="macos-arm64", compatibility="unverified",
                    boost=patches.boost(c, game, image, layout.game_info(c, game["id"]).get("sgradeResult", {}).get("mac")), **({"notes": notes} if notes else {}))
