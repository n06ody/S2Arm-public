# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""S 级优化升级补「可读瓦片版」着色器（_bf，#1152）：老包的直连库出自旧翻译器，缺 _bf，片上读取走不了、每帧整张快照（010074F013262000 Mac ④ 70800 次 / GPU 10.37 → 7.70 ms）。

可读瓦片版代数 BF_GEN：翻译器每扩一次 _bf 覆盖面（新包会多出旧包没有的 _bf）就 +1。出包时记进 game.json ios.bfGen / Mac 运行版本 manifest.json bfGen；
低于本引擎的 = 要补。补法 = 按出包同一套阶段（pack/stage_direct_profile.sh → stage_extract.sh → stage_direct.sh → stage_direct_seed.sh → stage_direct_cpso.sh）整份重出直连库，进临时目录：
  翻译缓存键含翻译器指纹，必全未命中、要重翻；Metal 编译缓存按 MSL 内容寻址，没变的着色器多数命中。
  Mac：Mac 运行版本本身就是一份产物根（romfs / exefs / extract / 改写产物都在），克隆成临时产物根重出，新库整份换进运行版本。
  iPhone：从游戏库取材建临时产物根——game.img 标准 RomFS 段（原样 RomFS）+ 追加内容条目 + original-exefs（没有就借同款 Mac 运行版本的 exefs）；
    没有 game.img 就整份借同款 Mac 运行版本；都没有 → 「需要原件」（返回 None，调用方点名，不静默）。重出后 overlay(旧库, 新库) 出覆盖层 <库目录>/bf/
    （宿主 graphics/nvn/lib_pack.inc lp_pick 同键同种类先取覆盖层），加进资源包清单；「更新 iPhone」按机上清单只推变了的文件（010074F013262000 约 160 MB），主容器不动不推。"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import layout
from core import Failure

BF_GEN = 1   # 1：#1152（d34f14298）片元自算 NDC→uv、_bf 输出上限 8、容样本掩码成员、认单行块 return
WIN = os.name == "nt"   # Windows 重出走 Windows 出包的直连库那一步（_rebuild_win），Mac 走 bash 阶段脚本链（STAGES）
STAGES = (("direct_profile", ["--require-consumer"]), ("extract", []), ("direct", []), ("direct_seed", []), ("direct_cpso", []))


def stale(gen):
    try:
        return int(gen or 0) < BF_GEN
    except (TypeError, ValueError):
        return True


def lib_dir(direct):
    """shaders/direct 下的库目录 b-<链>（只认一份；没有 / 多份 → None）"""
    ds = [d for d in Path(direct).glob("b-*") if d.is_dir() and not d.name.endswith("-vk") and (d / "lib.idx").is_file()]
    return ds[0] if len(ds) == 1 else None


def _clone(c, src, dst):
    r = subprocess.run([sys.executable, "-X", "utf8", str(Path(c.engine) / "tools/clonedir.py"), str(src), str(dst)], capture_output=True, text=True)
    if r.returncode:
        raise Failure("克隆产物根失败：" + (r.stderr.strip().splitlines() or ["无输出"])[-1][:200])  # 非玩家句


def rebuild(c, gid, src_root, plat, work):
    """src_root（产物根形态的目录）克隆进临时产物根，按出包同一套阶段重出直连库 → 新的 shaders/direct 目录（在 work 里）。
    plat：macos / ios（S2A_PACK_PLAT：Mac 版只出 Mac 库、iPhone 只出 iOS 库）。Windows：_rebuild_win（这台电脑没装 Metal 开发工具 → None）"""
    if WIN:
        return _rebuild_win(c, gid, src_root, work)
    work = Path(work); roots = work / "roots"; roots.mkdir(parents=True)
    tag = f"{gid}@sgbf"
    _clone(c, src_root, roots / tag)
    for stale_out in ("shaders/direct", ".stage_direct", ".stage_direct_seed", ".stage_direct_cpso"):   # 旧库与它的阶段键不带进来：键对得上会被当成「已是本树」跳过
        p = roots / tag / stale_out
        shutil.rmtree(p) if p.is_dir() else p.unlink(missing_ok=True)
    os.symlink(tag, roots / f"{gid}@现行")  # 非玩家句
    env = c.env()
    env.update(S2A_ROOTS=str(roots), S2A_ROOTS_SHARED=str(work / "never-shared"), S2A_ROOTS_PRIV=str(work / "roots-private"), S2A_PACK_PLAT=plat)
    logs = Path(c.logs) / c.task; logs.mkdir(parents=True, exist_ok=True)
    for st, extra in STAGES:
        c.run(["bash", Path(c.engine) / f"pack/stage_{st}.sh", gid, *extra], logs / f"sgbf_{st}.log", env)
    cur = Path(os.path.realpath(roots / f"{gid}@现行"))  # 非玩家句
    new = cur / "shaders/direct"
    if not (new / "keys.tsv").is_file() or lib_dir(new) is None:
        raise Failure("重出的直连着色器库不完整")  # 非玩家句
    return new


def _win_profile(c, root):
    """Windows 重出前的配置档，同 pack/stage_direct_profile.sh（Mac 每次重出也重跑这一段）：Vulkan / OpenGL 款不用 NVN 配置档；
    零配置推导的款写进产物根 direct-profile/<键>/ 与 direct_profile.json（direct() 按它取 DM_GAMES_DIR），手写档的款不写；最后 --selected 核一遍"""
    eng, root = Path(c.engine), Path(root)
    def py(*args):
        r = subprocess.run([sys.executable, "-X", "utf8", *map(str, args)], cwd=eng, env=c.env(), capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode:
            raise Failure(f"着色器配置档没准备好（{Path(str(args[0])).name} 退出 {r.returncode}）：" + ((r.stderr or r.stdout).strip().splitlines() or ["无输出"])[-1][:200])  # 非玩家句
        return r.stdout
    api = py(eng / "graphics/nvn/tools/gfx_api.py", root / "exefs").split("\t", 1)[0].strip()
    if api in ("vulkan", "gl"):
        return
    meta = json.loads(py(eng / "pack/direct_profile_stage.py", root, "--key"))
    if meta.get("source") == "generated":
        d = json.loads(py(eng / "pack/direct_profile_stage.py", root, "--out", root / "direct-profile" / meta["key"]))
        d["profile"] = Path(d["profile"]).resolve().relative_to(root.resolve()).as_posix()
        d["DM_GAMES_DIR"] = Path(d["profile"]).parent.as_posix()
        (root / "direct_profile.json").write_text(json.dumps(d, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")
    py(eng / "pack/direct_profile_stage.py", root, "--selected")


WIN_DIRECT = """import os, sys
from pathlib import Path
from pack.orch import oneclick_win as ow
root, work, tid = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
out = ow.direct(ow.Run(work), root, "ios", os.cpu_count() or 4, tid)
(work / "direct_result.txt").write_text(str(out) if out else "", encoding="utf-8")
"""


def _rebuild_win(c, gid, src_root, work):
    """Windows 重出直连库：同 Windows 出 iOS 包的直连库那一步（oneclick_win.direct：Git Bash 跑 graphics/tools/direct/direct_lib.sh，没有现成抽取就当场抽，
    Metal for Windows 编）。照出包作业那样另起进程、用作业层的环境（c.env：随包预编件 S2A_PREBUILT_TOOLS 等；作业进程自己的环境里没有）＋ iOS 平台。
    输入是 ios_inputs 建的产物根形态目录（临时目录，就地补配置档）。直连缓存用这款出包时那份（library.direct_cache_dir：Metal 编译结果按内容寻址，
    没变的着色器多数命中）。→ 新的 shaders/direct 目录；这台电脑没装 Metal 开发工具 → None（同 Windows 出包：不带着色器库）"""
    import library
    root, work = Path(src_root), Path(work) / "win"
    for stale_out in ("shaders/direct", "extract"):   # 旧库 / 旧抽取不带进来：按本引擎重抽重翻
        p = root / stale_out
        shutil.rmtree(p) if p.is_dir() else p.unlink(missing_ok=True)
    _win_profile(c, root)
    work.mkdir(parents=True, exist_ok=True)
    env = c.env()
    env.update(S2A_PACK_PLAT="ios", S2A_TARGET="ios", S2A_PY=sys.executable, S2A_DIRECT_CACHE=str(library.direct_cache_dir(c, gid)))
    logs = Path(c.logs) / c.task; logs.mkdir(parents=True, exist_ok=True)
    c.run([sys.executable, "-X", "utf8", "-c", WIN_DIRECT, str(root), str(work), gid.upper()], logs / "sgbf_win_direct.log", env, cwd=c.engine)
    got = (work / "direct_result.txt").read_text(encoding="utf-8").strip() if (work / "direct_result.txt").is_file() else None
    if got is None:
        raise Failure("重出直连着色器库的进程没交回结果")  # 非玩家句
    if not got:
        return None
    out = Path(got)
    if not (out / "keys.tsv").is_file() or lib_dir(out) is None:
        raise Failure("重出的直连着色器库不完整")  # 非玩家句
    return out


# ---- Mac ----

def upgrade_mac(c, gid, runtime, hold=None):
    """Mac 运行版本补 _bf：整份重出直连库换进运行版本（运行版本自带全部输入，不用原件）→ True = 换了。代数已是本引擎的 / 这次放弃 → False。
    hold（同 rewrite_refresh.refresh_mac）：重出只读运行版本、在锁外做；换库与改清单前进 hold()，它 yield False（玩家正在开始游戏）就整款放弃、
      不记代数，下次再补。不给 hold = 调用方已持锁"""
    import tempfile
    from core import read
    runtime = Path(runtime)
    manifest = read(runtime / "manifest.json", {})
    if not isinstance(manifest.get("files"), dict) or not stale(manifest.get("bfGen")):
        return False
    if not (runtime / "shaders/direct/keys.tsv").is_file() or not _has_romfs(runtime):
        return False   # 不是直连包 / 没有 RomFS：没有可补的
    c.emit("message", "正在给这款重出着色器库（补上片上读取用的着色器，资源不动；首次要几分钟到几十分钟）")
    # 运行版本的 RomFS 是 game.img（0.6.0 一款一份资源，pack/jobs/onerom.py）：重出要产物根形态的 romfs.std，同 iPhone 那条（ios_inputs）从 RomFS 段拷出一份，补完删掉
    std = runtime / "romfs.std"
    made = not _std_or_dir(runtime) and (runtime / "game.img").is_file()
    if made:
        h = _img(c).header(str(runtime / "game.img"))
        with open(runtime / "game.img", "rb") as f:
            _copy_range(f, h["romfs"][0], h["romfs"][1], std)
    try:
        with tempfile.TemporaryDirectory(dir=c.private) as tmp:
            return _upgrade_mac(c, gid, runtime, hold, Path(tmp))
    finally:
        if made:
            std.unlink(missing_ok=True)


def _upgrade_mac(c, gid, runtime, hold, tmp):
    """upgrade_mac 的重出与换库（tmp：重出用的临时目录）"""
    import contextlib
    from core import atomic, read
    new = rebuild(c, gid, runtime, "macos", tmp)
    stage = runtime / "shaders/direct.sgbf"; old = runtime / "shaders/direct.sgbf-old"
    for p in (stage, old):
        shutil.rmtree(p, ignore_errors=True)
    shutil.copytree(new, stage, copy_function=_clone_file)
    with (hold() if hold else contextlib.nullcontext(True)) as free:
        if not free:
            shutil.rmtree(stage, ignore_errors=True)
            return False
        manifest = read(runtime / "manifest.json", {})   # 锁里再读一次：重出期间别的作业可能改过清单（refresh_mac 补程序）
        os.replace(runtime / "shaders/direct", old)
        try:
            os.replace(stage, runtime / "shaders/direct")
        except OSError:
            os.replace(old, runtime / "shaders/direct")   # 换不上：旧库原样回位
            raise
        files = {k: v for k, v in manifest["files"].items() if not k.startswith("shaders/direct/")}
        for rel, size in _lib_files(runtime):
            files[rel] = [size, ""]   # 同 build.snapshot：只记大小
        atomic(runtime / "manifest.json", dict(manifest, files=files, bfGen=BF_GEN))
    shutil.rmtree(old, ignore_errors=True)
    return True


def _lib_files(runtime):
    """运行版本里直连库目录的文件 → [(相对运行版本的路径, 字节)]（单列一个函数，只看库目录）"""
    base = Path(runtime)
    return [(p.relative_to(base).as_posix(), p.stat().st_size) for p in sorted((base / "shaders/direct").rglob("*")) if p.is_file()]


def _clone_file(src, dst):
    """同卷 APFS 克隆（不占新空间），不行就普通拷贝"""
    if sys.platform == "darwin":
        import ctypes
        libc = ctypes.CDLL("libc.dylib", use_errno=True)
        if libc.clonefile(os.fsencode(src), os.fsencode(dst), 0) == 0:
            return dst
    return shutil.copy2(src, dst)


# ---- iPhone ----

def ios_inputs(c, gid, work):
    """iPhone 款的重出原料 → (产物根形态目录, 来处 game.img / mac) 或 (None, 'none')。见文件头的取材顺序"""
    import sgrade
    img = _img(c)
    pack, kit = layout.ios_pack(c, gid), layout.ios_kit(c, gid)
    mac = layout.image_mac(c, gid, create=False)
    exefs = sgrade.saved_program(c, gid) or ((mac / "exefs") if (mac / "exefs/main").is_file() and (mac / "exefs/main.npdm").is_file() else None)
    gimg = pack / "game.img"
    if gimg.is_file() and exefs is not None:
        root = Path(work) / "src"
        h = img.header(str(gimg))
        root.mkdir(parents=True)
        with open(gimg, "rb") as f:
            # 标准 RomFS 段原样拷成产物根的镜像 romfs.std（段内偏移相对段首，拷出来就是独立的标准 RomFS），不再展开成几十万个散文件
            if h["romfs"][1]:
                _copy_range(f, h["romfs"][0], h["romfs"][1], root / "romfs.std")
            else:
                (root / "romfs").mkdir()
            for rel, off, size in img.entries(str(gimg)):   # 追加内容等其余条目（aoc/… 与 aoc.tsv）原样放回产物根
                if rel.startswith("aoc/") or rel == "aoc.tsv":
                    _copy_range(f, off, size, root / rel)
        shutil.copytree(exefs, root / "exefs")
        for d in ("ctl", "systemdata"):
            if (pack / d).is_dir():
                shutil.copytree(pack / d, root / d)
        for p in kit.iterdir():   # 配置推导的输入与改写产物（运行时 GLSLC 款抽取要读 main.fixups.tsv）
            if p.is_file() and (p.name.startswith("direct") or p.suffix in (".tsv", ".bin")):
                shutil.copyfile(p, root / p.name)
        if (kit / "direct-profile").is_dir():
            shutil.copytree(kit / "direct-profile", root / "direct-profile")
        return root, "game.img"
    if _has_romfs(mac) and (mac / "exefs/main").is_file():
        return mac, "mac"
    return None, "none"


def _copy_range(f, off, size, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    f.seek(off)
    with open(dst, "wb") as g:
        left = size
        while left:
            b = f.read(min(left, 1 << 22))
            if not b:
                raise Failure(f"game.img 截短：{dst.name} 少 {left} 字节")  # 非玩家句
            g.write(b); left -= len(b)


def _img(c):
    """pack/img.py（不在作业层目录里；作业层别处起子进程调它，这里要读它的格式函数 header / entries / std_files）"""
    import importlib.util
    spec = importlib.util.spec_from_file_location("s2a_pack_img", Path(c.engine) / "pack/img.py")
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


def _std_or_dir(root):
    """产物根形态的 RomFS：散文件目录 romfs/，或镜像 romfs.std（标准 RomFS，与 romfs/ 同级）"""
    return (Path(root) / "romfs").is_dir() or (Path(root) / "romfs.std").is_file()


def _has_romfs(root):
    """目录有 RomFS：产物根形态（_std_or_dir），或 Mac 运行版本的 game.img（0.6.0 一款一份资源，用时拷出 RomFS 段）"""
    return _std_or_dir(root) or (Path(root) / "game.img").is_file()


def overlay(c, old_lib, new_lib, out):
    """dsh_batch --libpack overlay：只收旧库缺的 _bf 与配套反射 → True = 出了覆盖层（out 目录）；没有要补的 → False"""
    r = subprocess.run([sys.executable, "-X", "utf8", str(Path(c.engine) / "translator/dsh_batch.py"), "--libpack", "overlay", str(old_lib), str(new_lib), str(out)],
                       cwd=c.engine, env=c.env(), capture_output=True, text=True)
    if r.returncode:
        raise Failure("生成着色器覆盖层失败：" + ((r.stderr or r.stdout).strip().splitlines() or ["无输出"])[-1][:200])  # 非玩家句
    return Path(out).is_dir()


def add_to_pack(pack, prefix, rels):
    """资源包清单（pack/pack.py 格式：index.tsv 路径 / 字节 / SHA-256，manifest.json index_sha256）里 prefix 下的旧行换成 rels 这些文件，重写清单 → 新 index_sha256"""
    pack = Path(pack)
    rows = [l.split("\t") for l in (pack / "index.tsv").read_text(encoding="utf-8").splitlines() if l]
    keep = [r for r in rows if not r[0].startswith(prefix)]
    for rel in rels:
        p = pack / rel; h = hashlib.sha256()
        with open(p, "rb") as f:
            for b in iter(lambda: f.read(1 << 22), b""):
                h.update(b)
        keep.append([rel, str(p.stat().st_size), h.hexdigest()])
    ix = "".join("\t".join(r) + "\n" for r in keep)
    (pack / "index.tsv.tmp").write_text(ix, encoding="utf-8", newline="\n"); os.replace(pack / "index.tsv.tmp", pack / "index.tsv")
    m = json.loads((pack / "manifest.json").read_text(encoding="utf-8"))
    m.update(files=len(keep), total=sum(int(r[1]) for r in keep), index_sha256=hashlib.sha256(ix.encode()).hexdigest())
    (pack / "manifest.json.tmp").write_text(json.dumps(m, indent=1), encoding="utf-8"); os.replace(pack / "manifest.json.tmp", pack / "manifest.json")
    return m["index_sha256"]


def upgrade_ios(c, gid):
    """iPhone 款补 _bf → 'skip'（代数已是本引擎的）| 'unavailable'（取不到原料：要原件）| 'none'（重出后没有要补的）| 'updated'（覆盖层已进资源包）"""
    import tempfile
    side = layout.game_info(c, gid).get("ios") or {}
    if not stale(side.get("bfGen")):
        return "skip"
    pack = layout.ios_pack(c, gid)
    old = lib_dir(pack / "shaders/direct")
    if old is None:
        return "skip"   # 不是直连包
    with tempfile.TemporaryDirectory(dir=c.private) as tmp:
        src, where = ios_inputs(c, gid, Path(tmp))
        if src is None:
            return "unavailable"
        c.emit("message", f"{gid}：正在重出着色器库补上片上读取用的着色器（原料取自 {where}；首次要几分钟到几十分钟，资源只推新增的）")  # 非玩家句
        new = rebuild(c, gid, src, "ios", Path(tmp) / "w")
        if new is None:   # Windows 没装 Metal 开发工具：这次不补、不记代数，装好后下次再补（调用方接住 Failure 照常装机）
            raise Failure("这台电脑没装 Metal 开发工具，片上读取用的着色器这次没补上")  # 非玩家句
        nl = lib_dir(new)
        if nl is None:
            raise Failure(f"重出着色器库没产出库目录（资源包里是 {old.name}）")  # 非玩家句
        if nl.name != old.name:   # 维护者 11-17：校验不过不再整个拒绝——只影响这款，黄字说清后照常补
            c.diagnostic(f"重出的库链版本 {nl.name} 与资源包 {old.name} 不同，照常补上；这款游戏里画面出问题时重新出包", code="job_failed", severity="warning")  # 非玩家句
        out = Path(tmp) / "bf"
        got = overlay(c, old, nl, out)
        dst = old / "bf"
        shutil.rmtree(dst, ignore_errors=True)
        rels = []
        if got:
            shutil.copytree(out, dst)
            rels = [p.relative_to(pack).as_posix() for p in sorted(dst.rglob("*")) if p.is_file()]
        pid = add_to_pack(pack, dst.relative_to(pack).as_posix() + "/", rels)
    layout.update_info(c, gid, ios={**side, "bfGen": BF_GEN, "pack": pid})
    return "updated" if rels else "none"


def rechain_ios(c, gid):
    """0.6.0 通用壳（11-17 维护者定）：着色器链（直连库链号，game.json ios.dmshChain）和本引擎不同、或库内容代数（ios.libGen）旧了 → 只重编这款的着色器库，不重出整款。
    原料同 upgrade_ios（ios_inputs：游戏库里的 game.img；没有就借同款 Mac 运行版本；都没有才要原件），按出包同一套阶段重出（rebuild），
    新库（b-<新链>）整份换掉资源包里的 shaders/direct，改资源包清单（add_to_pack），记新链号与本代 _bf（整份重出已含）。
    资源包号随之变，推送按机上清单只推变了的文件。由玩家对这一款「推到 iPhone」时前台调（install.push_game），不进后台。
    → 'skip'（链号已是本引擎的 / 读不到本引擎链号 / 不是直连包）| 'unavailable'（没原料：调用方记一行，不挡推送）| 'updated'"""
    import host_update
    import tempfile
    want = host_update.engine_compat(c)
    side = layout.game_info(c, gid).get("ios") or {}
    # 库内容代数（host_update.LIB_GEN）旧了也重编：链号没变、库里却缺了设备要用的内容（0.5.x 出的库不带客体输入，同链号）
    if not want or (side.get("dmshChain", host_update.LEGACY_CHAIN) == want["dmshChain"] and not host_update.lib_stale(side.get("libGen"))):
        return "skip"
    pack = Path(layout.ios_pack(c, gid))
    direct = pack / "shaders/direct"
    if lib_dir(direct) is None:
        return "skip"   # 不是直连包
    with tempfile.TemporaryDirectory(dir=c.private) as tmp:
        src, where = ios_inputs(c, gid, Path(tmp))
        if src is None:
            c.emit("message", f"{gid}：着色器链更新了，但游戏库里没有 game.img、也没有 Mac 版可借：这次没重编着色器库")  # 非玩家句
            return "unavailable"
        c.emit("message", f"{gid}：着色器链更新了，正在用{where}重编这款的着色器库（不重出整款；首次要几分钟到几十分钟）")  # 非玩家句
        new = rebuild(c, gid, src, "ios", Path(tmp) / "w")
        if new is None:   # Windows 没装 Metal 开发工具（同 Windows 出包不带着色器库）：照推旧库，游戏里首次遇到的画面现编
            c.emit("message", f"{gid}：着色器链更新了，但这台电脑没装 Metal 开发工具：这次没重编着色器库（在「准备这台电脑」装好后再推一次就换上新库）")  # 非玩家句
            return "unavailable"
        if lib_dir(new) is None:
            raise Failure("重编的直连着色器库不完整")  # 非玩家句
        staged, old = pack / "shaders/.direct-new", pack / "shaders/.direct-old"
        for d in (staged, old):
            shutil.rmtree(d, ignore_errors=True)
        shutil.copytree(new, staged)
        os.replace(direct, old)
        os.replace(staged, direct)
        shutil.rmtree(old, ignore_errors=True)
        rels = [p.relative_to(pack).as_posix() for p in sorted(direct.rglob("*")) if p.is_file()]
        pid = add_to_pack(pack, "shaders/direct/", rels)
    layout.update_info(c, gid, ios={**side, "dmshChain": want["dmshChain"], "bfGen": BF_GEN, "libGen": host_update.LIB_GEN, "pack": pid})
    return "updated"


def due(c, gid):
    """iPhone 款要不要补 _bf（install.sgrade_pending 用）：代数低于本引擎、这一代没因缺原料试过、资源包是直连包"""
    side = layout.game_info(c, gid).get("ios") or {}
    if not stale(side.get("bfGen")) or side.get("bfMissing") == BF_GEN:
        return False
    try:
        return lib_dir(layout.ios_pack(c, gid) / "shaders/direct") is not None
    except OSError:
        return False
