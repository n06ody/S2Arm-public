"""Package S2Arm's own engine, reader and preparation source closure.

Only repository inputs enter the snapshot; ignored outputs and user data never do.
A fresh single-commit Git repository is built over the snapshot only to compute the
HEAD-based pipeline keys; they are written into engine.json, checked against the
readers, and the repository is removed before shipping (seal_snapshot). At runtime
the snapshot is cloned to the app's private writable cache.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pack/jobs"))
from core import Context, atomic, digest, tree_hash
from build import compile_host, nospace
import toolchain

INPUTS = ["runtime", "ios", "graphics", "translator", "services", "compat", "pack", "tools", "Makefile",
          "platform/build.mk", "launchers/macos/build.mk", "macos", "tests/visual/galrec",
          # 打 iPhone 包（make ios）要编 platform/apple/plat_apple.c（10-23 Spare-host 前端打 iPhone 包撞「no such file」）
          "platform/apple", "shell", "launchers/shared/SaveSync", "launchers/macos/frontend", "launchers/macos/adapter", "platform/plat.h", "platform/plat_asm.h",
          # 不收 android/（11-16 安装包瘦身，约 11 MB）：安卓导出未对玩家开放（启动器回「即将上线」），开放时再加回
          "LICENSE", "README.md",
          # 随包 Python 的钉版依赖清单：Windows 环境检查（pack/jobs/winenv.py）按它核随包 Python 的库版本（11-07 发版产物端到端：快照缺它、整份清单报错误码合同无法核实）
          "launchers/macos/requirements-runtime.txt",
          # 游戏设置定义表（pack/jobs/community.py 按它读写 launch.json、转宿主开关；11-09 启动设置）
          "launchers/shared/game_settings.v1.json",
          # iPhone 宿主包的图标（pack/ipa_tail.py、pack/jobs/ipa.py）：引擎快照里没有它，容器就退回用游戏图标（11-08 维护者）
          "launchers/macos/Branding/AppIcon-iOS.png",
          # 根 .gitignore（11-02 beta.3 走查）：出包会在快照副本里编出 bin/ obj/，没有忽略规则时 pack/ipa.sh「构建相关目录有未提交改动」
          #   把这些编译产物当成改动、拒绝出包——玩家导出 iPhone 版必死在最后一步
          ".gitignore",
          # 启动器公告的随包兜底（pack/jobs/updater.py inbox：官方下载页上的同一份取不到时用它）
          "docs/site/notice.json",
          # 新功能大窗的随包内容（updater.py feature_spotlight：文字在 feature.json，图在 feature/）
          "docs/site/feature.json", "docs/site/feature"]


ANDROID_ENGINE_ENV = "S2A_ANDROID_ENGINE_DIR"


def inputs():
    """快照收哪些仓库路径：INPUTS；发版给了安卓预编引擎（S2A_ANDROID_ENGINE_DIR，android/tools/engine_template.py 产出）时再收 android/
    （安卓出包要 android/tools、verify-apk.py 等）。安卓出包在开关后（pack/jobs/android_export.py，缺省关）；不给就和以前一样不收。"""
    return INPUTS + (["android"] if os.environ.get(ANDROID_ENGINE_ENV) else [])


def bundle_android_engine(destination):
    """预编引擎 APK 随引擎发：<引擎>/Binaries/Android/engine（android_export.engine_dir 认这个位置）。先核 engine.json 与 APK 身份一致。"""
    source = os.environ.get(ANDROID_ENGINE_ENV)
    if not source:
        return None
    sys.path.insert(0, str(ROOT / "android/tools"))
    import apk_assemble
    doc, _ = apk_assemble.load_engine(source)
    if not (Path(source) / "identity.json").is_file():
        raise SystemExit(f"【红】安卓预编引擎 {source} 缺 identity.json")
    target = Path(destination) / "Binaries/Android/engine"
    target.parent.mkdir(parents=True, exist_ok=True)
    for name in ("engine.apk", "engine.json", "identity.json"):
        target.mkdir(exist_ok=True)
        shutil.copy2(Path(source) / name, target / name)
    return doc


def include_source(relative):
    """发版引擎快照不收测试树（11-16 安装包瘦身：出包流程不跑测试，各处 tests/ 约 18 MB）。
    例外：根目录 tests/visual/galrec 是出包要读的（INPUTS 单独列进来），按顶层 tests 之后的目录判。"""
    parts = Path(relative).parts
    directories = parts[1:-1] if parts[:1] == ("tests",) else parts[:-1]
    return not any(p in ("test", "tests", "androidTest", "testFixtures") or p.endswith("-tests")
                   for p in directories)


def snapshot_files(root=ROOT):
    """引擎快照收的文件（仓库相对路径，排好序）：INPUTS 下 git 跟踪 / 未忽略的文件，去掉 include_source 排除的测试树。bundle 与单测共用"""
    names = subprocess.check_output(["git", "-C", str(root), "ls-files", "-co", "--exclude-standard", "-z", "--", *inputs()]).split(b"\0")
    return [Path(os.fsdecode(n)) for n in sorted(set(names) - {b""}) if include_source(Path(os.fsdecode(n)))]


def source_fingerprint(root):
    """Apply the same tree hash as the other jobs to the shipped inputs."""
    root = Path(root)
    paths = []
    for folder in inputs():
        top = root / folder
        if top.is_dir():
            paths.extend(str(p.relative_to(root)) for p in sorted(top.rglob("*"))
                         if p.is_file() and include_source(p.relative_to(root)))
        elif include_source(Path(folder)):
            paths.append(folder)
    return tree_hash(root, paths)


MACHO_MAGIC = (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def bundle_tzdata(c, destination, source_root=ROOT):
    """Freeze verified IANA sources and their checked output into the App engine."""
    env = c.env()
    if os.environ.get("S2A_TZDATA_CACHE"):
        env["S2A_TZDATA_CACHE"] = os.environ["S2A_TZDATA_CACHE"]
    c.run(["bash", source_root / "services/systemdata/make_tzdata.sh", "--bundle-to", destination],
          c.logs / "tzdata-bundle.log", env=env)


# 随 App 分发的预编静态库（11-03 维护者：编解码库等与游戏无关，不在玩家机器上编）：发版机按 services/<库>/build.sh 照常构建（整机缓存命中秒过），
#   静态库 + 头文件 + 许可原文 + build.txt（构建键、版本、源码与库的 sha256）放进 Binaries/Prepare/libs/<库>/<平台>/；玩家出包时 build.sh 见 S2A_PREBUILT_TOOLS
#   就核键与 sha256、直接回这份库（services/prebuilt_lib.sh），不构建。opus、C# 库（nxaot）还在玩家机器上现编，另作一单。
# 追加一个库：在 prebuilt_libs 里加一项（库名 = services/<库名>/build.sh；静态库文件名），它的 build.sh 照 vpx / dav1d 开头那三行接 services/prebuilt_lib.sh
# 两个解码库都按本机探测放行（缺省 auto）：dav1d（AV1 软解兜底）按 dav1d_wanted（tools/av1hw.sh：VideoToolbox 建得出 AV1 会话 = 有硬解）、
#   vpx（VP8 / VP9 软解）按 vpx_wanted（tools/vp9hw.sh：注册 supplemental 后建得出 VP9 会话 = 可走系统）——探测过时整条不放：
#   本机构建不链真身（services/build.mk 同一判据，链的是全失败空垫片 services/vpx/stub.c），出包也不放预编库。出的 App 会装到
#   没有 AV1 硬解 / 不可走系统 VP9 的机器上时（发版机：M1 / M2、A16 及更早）必设 S2A_DAV1D=1 S2A_VPX=1 再出，
#   否则那些机器上的内建宿主链接要真身、App 里却没有
PREBUILT_LIBS = []


def host_arch():
    machine = platform.machine()
    if machine not in ("arm64", "x86_64"):
        raise RuntimeError(f"Mac 启动器只支持 arm64 / x86_64，本机是 {machine}")
    return machine


def host_rid():
    return "osx-arm64" if host_arch() == "arm64" else "osx-x64"


def host_slice():
    return "arm64" if host_arch() == "arm64" else "x86_64"


def prebuilt_rids():
    """iOS 壳始终 arm64；Mac 侧预编库跟出包机架构走（Intel 只出包、不起游戏）。"""
    return ("ios-arm64", host_rid())


def _probe_wanted(script, extra_env):
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    try:
        p = subprocess.run(["bash", str(ROOT / script), "--want"], capture_output=True, text=True, env=env)
    except OSError:
        return True   # 探测失败保守按要算（= 原默认行为）
    lines = p.stdout.split()
    return lines[-1] != "0" if lines else True


def dav1d_wanted(extra_env=None):
    """dav1d 软解兜底（AV1）要不要进本机构建 / 随包：S2A_DAV1D=1 / 0 直说，缺省 1；显式 auto = 探测本机
    AV1 硬解（tools/av1hw.sh --want：VideoToolbox 建得出 AV1 会话 = 有硬解，软解整条不要）。"""
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    v = env.get("S2A_DAV1D", "1")   # 缺省链：随包的宿主要跑在玩家机上，探测构建机判不了（auto 只显式设）
    if v in ("0", "1"):
        return v == "1"
    return _probe_wanted("tools/av1hw.sh", extra_env)


def vpx_wanted(extra_env=None):
    """真 libvpx（VP8 / VP9 软解）要不要进本机构建 / 随包：S2A_VPX=1 / 0 直说，缺省 1；显式 auto = 探测本机可走
    系统 VP9（tools/vp9hw.sh --want：注册 supplemental 后建得出 VP9 会话，VP9 走 VideoToolbox、VP8 如实报红）。"""
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    v = env.get("S2A_VPX", "1")   # 同上
    if v in ("0", "1"):
        return v == "1"
    return _probe_wanted("tools/vp9hw.sh", extra_env)


def prebuilt_libs(want_dav1d, want_vpx):
    """随包预编库清单：dav1d / vpx 各按本机探测 / S2A_DAV1D / S2A_VPX（见 dav1d_wanted / vpx_wanted）"""
    libs = []
    if want_vpx:
        libs.append(("vpx", "libs2a_vpx.a"))
    if want_dav1d:
        libs.append(("dav1d", "libs2a_dav1d.a"))
    return libs


def bundle_prebuilt_libs(tools, source_root=ROOT, env=None, developer_dir=None):
    import re
    env = dict(env if env is not None else os.environ)
    env.pop("S2A_PREBUILT_TOOLS", None)
    extra = {"DEVELOPER_DIR": developer_dir} if developer_dir else None
    for name, lib in prebuilt_libs(dav1d_wanted(extra), vpx_wanted(extra)):
        script = source_root / "services" / name / "build.sh"
        pin = re.search(r"^VER=([0-9.]+); SHA=([0-9a-f]{64})$", script.read_text(), re.M)
        if not pin:
            raise RuntimeError("%s 的版本 / sha256 钉行认不出" % script)
        for rid in prebuilt_rids():
            key = subprocess.check_output(["bash", str(script), rid, "--key"], env=env, text=True).strip()
            built = Path(subprocess.check_output(["bash", str(script), rid], env=env, text=True).strip().splitlines()[-1])
            out = tools / "libs" / name / rid
            out.mkdir(parents=True)
            shutil.copy2(built, out / lib)
            shutil.copytree(built.parent.parent / "include", out / "include")
            for notice in ("COPYING", "LICENSE", "PATENTS"):
                if (built.parent.parent / notice).is_file():
                    shutil.copy2(built.parent.parent / notice, out / notice)
            (out / "build.txt").write_text("key=%s\nversion=%s\nsource_sha256=%s\nlib_sha256=%s\n" % (key, pin.group(1), pin.group(2), digest(out / lib)))


def bundle_dotnet_env(c):
    """Give NativeAOT publishes no-space cache paths.

    The launcher's private tool cache inherits the repository path, which may
    contain spaces.  NuGet's package folder is expanded into the final clang
    command by Microsoft.NETCore.Native.targets, so its path must be stable and
    shell-safe before restore/publish starts.
    """
    env = c.env()
    for key in ("NUGET_PACKAGES", "DOTNET_CLI_HOME"):
        raw = env.get(key)
        if not raw or " " not in raw:
            continue
        path = Path(raw)
        try:
            path.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            raise RuntimeError(f"无法准备 .NET 缓存目录：{path}") from error
        env[key] = str(nospace(path))
    return env


def strip_debug_maps(root, developer_dir, keep_dir=None):
    """发布进 App 的二进制去掉调试映射与本地符号（strip -S -x）：调试映射记着出包机 .o 与源码目录的绝对路径；本地符号玩家机器上没有代码读
    （11-15 瘦身：宿主 121.5 → 48.7 MB，导出符号一个不少，dlsym 照常）。keep_dir 给了就先把未去符号的原件按相对路径存过去（发版存档，离线还原崩溃报告）。
    strip 会让签名失效，所以随后由里到外重签（保留原 entitlements / 标识）。玩家机器上现编的宿主不走这里，照常可调试。"""
    env = dict(os.environ, DEVELOPER_DIR=developer_dir)
    def is_macho(p):
        with p.open("rb") as f:
            return f.read(4) in MACHO_MAGIC
    machos = [p for p in sorted(root.rglob("*")) if p.is_file() and not p.is_symlink() and is_macho(p)]
    keep = ["--preserve-metadata=identifier,entitlements,flags,runtime"]
    for p in machos:
        if keep_dir:
            dst = Path(keep_dir) / p.relative_to(root)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
        subprocess.run(["xcrun", "strip", "-S", "-x", str(p)], env=env, check=True, capture_output=True)
        subprocess.run(["codesign", "--force", "--sign", "-", *keep, str(p)], env=env, check=True, capture_output=True)
    bundles = sorted((p for p in root.rglob("*") if p.is_dir() and p.suffix in (".app", ".framework")), key=lambda p: -len(p.parts))
    for b in bundles:
        subprocess.run(["codesign", "--force", "--sign", "-", *keep, str(b)], env=env, check=True, capture_output=True)
    return len(machos)


def bundle(destination, developer_dir):
    c = Context(dict(schema=1, taskId="bundle-engine", operation="prepareEngine", engineDir=str(ROOT),
                     privateRoot=str(ROOT / "build/macos/engine-build"), developerDir=developer_dir))
    fingerprint = source_fingerprint(ROOT)
    # 发布进 App 的 .NET 程序集不带出包机路径：MSBuild 把环境变量当属性读，PathMap 让编译器把仓库根（含 build/ 下的 obj）写成 /_/（C 侧同理见各 -ffile-prefix-map）
    os.environ["PathMap"] = "%s=/_/" % ROOT
    toolchain.prepare(c)
    # Intel Mac 只做出包壳：s2arm-host 是 AArch64 原生执行体（entry.S + 改写后的 ARM64 代码），x86_64 编不出也跑不了游戏。
    # 出包用的工具另在下面按本机 RID 预编；不塞一个跑不动的宿主进 App，免得玩家误以为能在 Mac 起局。
    if host_arch() == "arm64":
        host = compile_host(c, fingerprint, c.env(), c.logs)
    else:
        host = None
        c.emit("message", "Intel Mac：本 App 带出包工具，不带在 Mac 上跑游戏的宿主；给 iPhone 出包不受影响。")
    make_root = nospace(ROOT)
    prepare = c.private / "tools/prepare"
    # 每次从空目录编：dotnet publish -o 不清目标目录，上一次（别的编法）留下的产物会混进来（v0.4.4 第三次候选：上一版编出的 dsh_batch.dSYM
    #   残留，去符号那步拿它当二进制处理报错）。这几样工具每次本来就全量重编，清掉不损失增量
    shutil.rmtree(prepare, ignore_errors=True)
    prepare.mkdir(parents=True, exist_ok=True)
    c.run(["make", "-C", make_root / "graphics/metal", "ROOT=" + str(make_root),
           str(make_root / "build/gal/gal_xc"),
           "DEVELOPER_DIR=" + developer_dir], c.logs / "gal-tools.log")
    c.run(["make", "-C", make_root / "graphics/dsx", "ROOT=" + str(make_root),
           str(make_root / "build/dsx/libdsx.dylib"),
           "DEVELOPER_DIR=" + developer_dir], c.logs / "dsx.log")
    # 出包用的 C# 工具随发版预编为 NativeAOT 单文件（romfs_index 的 PublishAot 写在工程里：服务层闭包含 netstandard2.0 生成器，-p: 全局属性会波及，同 services/aot/S2A.Aot.csproj）：
    #   玩家机每游戏构建直接执行，App 不再携带 .NET 运行时（bundle_runtime.py 也不再收）
    dotnet_env = bundle_dotnet_env(c)
    c.run(
        [
            "dotnet",
            "publish",
            ROOT / "graphics/tools/shbake/S2A.ShBake.csproj",
            "-c",
            "Release",
            "-r",
            host_rid(),
            "-p:PublishAot=true",
            "-p:UseSharedCompilation=false",
            "--nologo",
            "-o",
            prepare / "shbake",
        ],
        c.logs / "shbake.log",
        env=dotnet_env,
    )
    c.run(
        [
            "dotnet",
            "publish",
            ROOT / "services/romfs_index/romfs_index.csproj",
            "-c",
            "Release",
            "-r",
            host_rid(),
            "--nologo",
            "-o",
            prepare / "romfs",
        ],
        c.logs / "romfs.log",
        env=dotnet_env,
    )
    # 直连库的 C# 批量进程（translator/dsh_batch.py）随发版编好（NativeAOT 单文件）：玩家首次出包不再现编（11-06 维护者「玩家体验至上」，Spare-host 约 25 秒 CPU），也不再要 .NET 运行时
    c.run(
        [
            "dotnet",
            "publish",
            ROOT / "graphics/tools/direct/dsh/batch/dsh_batch.csproj",
            "-c",
            "Release",
            "-r",
            host_rid(),
            "-p:PublishAot=true",
            "-p:UseSharedCompilation=false",
            "--nologo",
            "-o",
            prepare / "dsh_batch",
        ],
        c.logs / "dsh_batch.log",
        env=dotnet_env,
    )
    # 推资源包流式写手机（pack/devpush，Rust，只依赖 idevice MIT；11-06 维护者：玩家 Mac / Windows 两端都要）：tools/push_pack.py 见 S2A_PREBUILT_TOOLS/devpush/s2a-devpush 就用，
    #   不在就退回 devicectl copy。--locked：按仓库里的 Cargo.lock 取依赖，发版间不漂
    cargo = shutil.which("cargo") or str(Path.home() / ".cargo/bin/cargo")
    c.run([cargo, "build", "--release", "--locked", "--manifest-path", ROOT / "pack/devpush/Cargo.toml", "--target-dir", ROOT / "build/devpush"],
          c.logs / "devpush.log")
    # 电脑侧签程序库（pack/selfsign，Rust，只依赖 ring；0.6.0 通用壳：免费团队认的 dylib 签名系统 codesign 签不出，program_pack 用它签）
    c.run([cargo, "build", "--release", "--locked", "--manifest-path", ROOT / "pack/selfsign/Cargo.toml", "--target-dir", ROOT / "build/selfsign"],
          c.logs / "selfsign.log")
    c.run(["clang", "-ffile-prefix-map=%s=." % ROOT, "-fobjc-arc", "-O1", ROOT / "graphics/tools/shbake/gpu_arch.m", "-o", prepare / "gpu_arch",
           "-framework", "Metal", "-framework", "Foundation"], c.logs / "gpu-arch.log")
    destination.mkdir(parents=True)
    for relative in snapshot_files():
        source = ROOT / relative
        if source.is_symlink():
            raise RuntimeError("Engine source symlinks must be resolved explicitly: " + str(relative))
        if not source.is_file():
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    # Include license notices from every source subtree in the repository closure.
    env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1",
               GIT_AUTHOR_NAME="S2Arm", GIT_AUTHOR_EMAIL="bundle@localhost",
               GIT_COMMITTER_NAME="S2Arm", GIT_COMMITTER_EMAIL="bundle@localhost")
    for args in (["init", "-q", "--template="], ["config", "gc.auto", "0"],
                 ["config", "maintenance.auto", "false"], ["add", "--force", "--all"],
                 ["-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", "commit", "-qm", "Bundled S2Arm engine inputs"]):
        subprocess.run(["git", "-C", str(destination), *args], env=env, check=True)
    # Finish compaction synchronously before hashing/signing. Background Git
    # maintenance would otherwise invalidate the signed resource seal.
    subprocess.run(["git", "-C", str(destination), "-c", "gc.autoDetach=false", "gc", "--prune=now"], env=env, check=True)
    if host:
        shutil.copytree(host, destination / "Binaries/S2ArmGame.app")
    # 读取器：NativeAOT 单文件（同 toolchain.prepare 的配方），整个 Reader 目录只有这一个可执行文件
    (destination / "Binaries/Reader").mkdir(parents=True)
    shutil.copy2(toolchain.reader(c), destination / "Binaries/Reader/nx_unpack")
    bundle_android_engine(destination)   # 给了 S2A_ANDROID_ENGINE_DIR 才带（安卓出包在开关后）
    tools = destination / "Binaries/Prepare"
    # 预编工具各收自己的单文件（AOT publish 会另出符号文件 / 中间件，不随包）：shbake 的 gpu_arch（clang 编的原生件）同目录
    (tools / "shbake").mkdir(parents=True)
    shutil.copy2(prepare / "shbake/S2A.ShBake", tools / "shbake/S2A.ShBake")
    bundle_tzdata(c, tools / "Tzdata")
    shutil.copy2(prepare / "gpu_arch", tools / "shbake/gpu_arch")
    (tools / "romfs").mkdir(parents=True)
    shutil.copy2(prepare / "romfs/romfs_index", tools / "romfs/romfs_index")
    bundle_prebuilt_libs(tools, env=c.env(), developer_dir=developer_dir)
    (tools / "dsh_batch").mkdir(parents=True)
    shutil.copy2(prepare / "dsh_batch/dsh_batch", tools / "dsh_batch/dsh_batch")
    # 摘要按 App 里这份源码算（dsh_batch.py 同一函数）：玩家那边摘要相同才用内置件，不同（App 装坏了）报重装、不现编
    digest_out = subprocess.run([sys.executable, "-I", str(destination / "translator/dsh_batch.py"), "--src-digest"],
                                env=c.env(), check=True, capture_output=True, text=True).stdout.strip()
    (tools / "dsh_batch/.src_digest").write_text(digest_out + "\n", encoding="utf-8")
    # 同一份 App 内源码的翻译器指纹（dmsh_chain.py --xfp 按 DshXfpInput 现算）：玩家机没有 .NET（SDK 更没有），展不开清单，direct_lib.sh 出库后核库印章取这份当「本树」值
    #   （dmsh_chain.tree_xfp ← dsh_batch.prebuilt_xfp，摘要相同才认）。与内置件自报的编译期 Xfp 不同就是发版件没按这份源码编，当场停
    xfp_out = subprocess.run(
        [sys.executable, "-I", str(destination / "translator/dmsh_chain.py"), "--xfp"],
        env=c.env(),
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    xfp_bin = subprocess.run(
        [str(tools / "dsh_batch/dsh_batch"), "--xfp"],
        env=c.env(),
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if xfp_out != xfp_bin:
        raise RuntimeError(f"Bundled dsh_batch reports translator fingerprint {xfp_bin}, the bundled sources give {xfp_out}")
    (tools / "dsh_batch/.xfp").write_text(xfp_out + "\n", encoding="utf-8")
    (tools / "devpush").mkdir()
    shutil.copy2(ROOT / "build/devpush/release/s2a-devpush", tools / "devpush/s2a-devpush")
    (tools / "selfsign").mkdir()
    shutil.copy2(ROOT / "build/selfsign/release/s2a-selfsign", tools / "selfsign/s2a-selfsign")
    # 开发者证书签名工具 rcodesign（apple-codesign 官方发布件，版本 / SHA-256 钉在 pack/jobs/devcert.py，不符拒打；v0.2.4）
    import devcert
    devcert.fetch_rcodesign(tools / "rcodesign", "mac")
    # 官方只发 universal（arm64 + x86_64，51.8 MB）；只留当前出包机那一片（各片签名各自独立，抽出来仍有效）。
    # Intel 出包机要用 x86_64 那片，不能像 Apple Silicon 那样只留 arm64。
    exe = tools / "rcodesign/rcodesign"
    archs = subprocess.check_output(["lipo", "-archs", str(exe)], text=True).split()
    if host_slice() not in archs:
        raise RuntimeError(f"rcodesign 没有 {host_slice()} 分片（实际 {archs}）：这台 Mac 上签不了")
    if len(archs) > 1:
        thin = str(exe) + "." + host_slice()
        subprocess.run(["lipo", str(exe), "-thin", host_slice(), "-output", thin], check=True)
        os.replace(thin, exe)
    (tools / "gal").mkdir()
    shutil.copy2(ROOT / "build/gal/gal_xc", tools / "gal/gal_xc")
    (tools / "dsx").mkdir()   # 直连翻译库（pack/stage_direct.sh 认 dsx/libdsx.dylib，11-08 从 libnxgal 拆出）
    shutil.copy2(ROOT / "build/dsx/libdsx.dylib", tools / "dsx/libdsx.dylib")
    sym = os.environ.get("S2A_SYMBOLS_OUT")   # 发版存档未去符号原件（private-tools release/build_mac.sh 给 <产物目录>/symbols）
    strip_debug_maps(destination / "Binaries", developer_dir, Path(sym) / "Engine/Binaries" if sym else None)
    if source_fingerprint(ROOT) != fingerprint:
        raise RuntimeError("Engine sources changed during packaging; retry with stable inputs")
    precompile(destination, destination.parent / RUNTIME_PYTHON)   # 先于清单：.pyc 和源码一起进 engine.json、一起签
    return seal_snapshot(destination, fingerprint)


# 快照不带 .git（瘦身约 28 MB）：出包各阶段原先在快照库里用 git 取的值，打包时（快照库已 commit + gc、文件都放齐、签名之前）用 git 算好写进 engine.json，
#   读方有字段就用（pack/stage_keyfiles.sh engine_snap、pack/orch/roots.py snapshot、tools/stage_deps.py _listed）。不能签名后按文件重算：
#   签名改写了快照里的 Mach-O（translator/dmsh_xlate_bin/dmsh_xlate 在直连库阶段的输入表里），按文件算出的对象号与提交里的不同，所有玩家的直连库都会重编
# 只在打包时参与算键、玩家机上没人执行的文件：键算完、和 .git 一起从包里拿掉。
#   translator/dmsh_xlate_bin/dmsh_xlate：翻译器的 .NET 启动壳（要系统装 .NET 才起得来）。直连库阶段的输入表登记了它，所以照算键；
#   但出包作业里 dmsh_chain 只读同目录的 .source_sha 与几个 dll 的哈希，翻译走 NativeAOT 的 dsh_batch，没有地方执行它
#   （去 .NET 静态扫描点名它）。从仓库删会改直连库的键、所有款重编，拿出包更便宜
BUILD_ONLY = ("translator/dmsh_xlate_bin/dmsh_xlate",)
SNAP_KEYS = ("commit", "stageFilesKeys", "scanTree", "romfsIndexOids", "listed")
SNAP_STAGES = ("extract", "direct", "direct_seed", "direct_cpso")   # 玩家出包路径上调 stage_files_key 的阶段
ROMFS_WRITER = ("services/S2A.Services/RomFsIndex.cs", "services/romfs_index", "services/romfs_index.sh", "compat/tree/Directory.Packages.props")


def _bash():
    sys.path.insert(0, str(ROOT)); from pack.orch.bash import find_bash   # Windows 打包机不认 WSL 存根（同出包编排层）
    exe = find_bash()
    if not exe:
        raise RuntimeError("打包自检要 bash（算阶段键）")
    return exe


def _git_out(destination, *args):
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(Path(destination).resolve().parent), GIT_OPTIONAL_LOCKS="0")
    env.pop("DEVELOPER_DIR", None)
    return subprocess.run(["git", "-C", str(destination), *args], env=env, check=True, capture_output=True).stdout


def snapshot_fields(destination):
    """在带 .git 的快照上用 git 算出 engine.json 的快照字段（旧读法的原值）"""
    destination = Path(destination)
    keys = {}
    for st in SNAP_STAGES:
        keys[st] = subprocess.run([_bash(), "-c", 'source "$1/pack/stage_keyfiles.sh" && stage_files_key "$2" "$1"', "_", str(destination), st],
                                  check=True, capture_output=True, text=True).stdout.strip()
    listed = _git_out(destination, "ls-files", "-co", "--exclude-standard", "-z").decode("utf-8", "surrogateescape").split("\0")
    return dict(commit=_git_out(destination, "rev-parse", "HEAD").decode().strip(),
                stageFilesKeys=keys,
                scanTree=_git_out(destination, "rev-parse", "HEAD:graphics/widen/scan").decode().strip(),
                romfsIndexOids=_git_out(destination, "rev-parse", *("HEAD:" + p for p in ROMFS_WRITER)).decode().split(),
                listed=sorted({f for f in listed if f}))


PROBE_PY = """import json, sys
sys.dont_write_bytecode = True
e = sys.argv[1]; sys.path.insert(0, e); sys.path.insert(0, e + '/tools')
from pack.orch import roots, stages
import stage_deps
files, _ = stage_deps._listed(e)
print(json.dumps(dict(py_head=roots.head(e), py_scan=stages.scan_tree(), py_romfs=stages.romfs_writer_oids(),
                      listed=sorted(files) if files is not None else None)))
"""


def reader_values(destination):
    """出包各读方在这份快照上实际读到的值（bash 与 Python 两套读方各跑一遍）：带 .git、engine.json 没有快照字段时走 git，删 .git 后走 engine.json"""
    destination = str(Path(destination))
    out = {}
    for name, call in (("sh_commit9", 'src_commit9 "$1"'), ("sh_scan", 'scan_tree "$1"'), ("sh_romfs", 'romfs_writer_oids "$1"'),
                       *((f"sh_key_{st}", f'stage_files_key {st} "$1"') for st in SNAP_STAGES),
                       ("sh_root_src", 'source "$1/pack/roots.sh" && _root_head')):
        p = subprocess.run([_bash(), "-c", 'source "$1/pack/stage_keyfiles.sh" && ' + call, "_", destination], capture_output=True, text=True,
                           env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
        out[name] = [p.returncode, p.stdout]
    p = subprocess.run([sys.executable, "-B", "-I", "-c", PROBE_PY, destination], capture_output=True, text=True, check=True)
    out.update(json.loads(p.stdout))
    return out


def _rmtree_writable(path):
    """删整棵目录；删不动的先加写权限再删一次（Windows 上 git 的对象文件、commit-graph 是只读的，rmtree 报 WinError 5 拒绝访问）"""
    def retry(fn, p, _):
        os.chmod(p, stat.S_IWRITE | stat.S_IREAD)
        fn(p)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=retry)
    else:
        shutil.rmtree(path, onerror=retry)


def seal_snapshot(destination, fingerprint):
    """快照库已 commit + gc、引擎目录里的文件都放齐之后调：git 算快照字段写进 engine.json → 删 .git → 各读方新旧两种读法逐项比对，不一致就停、不出包"""
    destination = Path(destination)
    write_manifest(destination, fingerprint, snap={})   # engine.json / prepare.json 先落盘：玩家副本里 git ls-files 会列出它们，listed 要算进去
    before = reader_values(destination)                 # 旧读法（git）
    fields = snapshot_fields(destination)
    _rmtree_writable(destination / ".git")
    for rel in BUILD_ONLY:   # 键已按它们算进 engine.json：从包里拿掉不改任何键
        (destination / rel).unlink(missing_ok=True)
    write_manifest(destination, fingerprint, snap=fields)
    after = reader_values(destination)                  # 新读法（engine.json）
    failed = [k for k, v in before.items() if k.startswith("sh_") and v[0]]
    diff = sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))
    if failed or diff:
        raise RuntimeError(f"引擎快照自检不过：git 读法报错 {failed}；删 .git 后读到的值与 git 算的不同 {diff}")
    leftovers = sorted(str(p.relative_to(destination)) for p in destination.rglob("*.dSYM"))
    if leftovers:   # 调试符号包不随包（去符号那步会拿它当二进制处理；也不该给玩家）：多半是构建目录的残留
        raise RuntimeError(f"引擎目录里有调试符号包（构建残留），不出包：{leftovers[:5]}")
    return write_manifest(destination, fingerprint)     # 再写一遍（沿用字段）：顺带核自检没往引擎目录里多写文件


# 签名封存的 App 包运行时一个字节都不能多（多了 codesign --verify 报「a sealed resource is missing or invalid」、Gatekeeper 拒开）。
#   只靠各调用点带 -B 守不住：-I 忽略 PYTHONDONTWRITEBYTECODE、子进程不继承 -B、encodings 等在 site 之前导入（sitecustomize 管不到）。
#   出包时就把包里所有 .py 用内置 Python 编成 checked-hash 的 .pyc 一起签进去：导入时按源码哈希核对、对得上直接用，没有可写的；
#   再在内置 Python 的 site-packages 放 sitecustomize 关掉写字节码，兜住没预编到的（如运行时现生成的模块）
RUNTIME_PYTHON = "Runtime/python/bin/python3"
SITECUSTOMIZE = "# S2Arm：App 包签名封存，运行时不写 .pyc（launchers/macos/bundle_engine.py seal_runtime）\nimport sys\nsys.dont_write_bytecode = True\n"


def precompile(root, python):
    """root 下所有 .py 用 python（必须是包里那份内置 Python：.pyc 带解释器版本号）编成 checked-hash .pyc；有编不过的就停（随包源码编不过本身是错）"""
    python = Path(python)
    if not python.is_file():
        raise RuntimeError(f"预编 .pyc 要用 App 自带的 Python，没找到：{python}")
    # -B：解释器起来时自己会导入 encodings / sitecustomize，不加 -B 就先把它们写成按时间戳失效的 .pyc，compileall 见已有就不重写；
    #   装机改了源码时间后第一次起包里的 Python 就重写这 4 份（sitecustomize 关写字节码在它们之后才生效），签名封存坏（v0.4.4 候选验收⑦）。
    #   -f：已有的一律按 checked-hash 重写。compileall 自己显式写 .pyc，不受 -B 影响
    subprocess.run([str(python), "-I", "-B", "-m", "compileall", "-q", "-f", "-j", "0", "--invalidation-mode", "checked-hash", str(root)],
                   stdout=subprocess.DEVNULL, check=True)


def seal_runtime(resources):
    """内置 Python 放 sitecustomize（关写字节码）并预编标准库与第三方包；Jobs / GUIJobs 这些随包的 Python 源码一并预编"""
    resources = Path(resources)
    lib = next((resources / "Runtime/python/lib").glob("python3.*"))
    (lib / "site-packages/sitecustomize.py").write_text(SITECUSTOMIZE)
    dirs = (lib, *(resources / name for name in ("Jobs", "GUIJobs") if (resources / name).is_dir()))
    for d in dirs:
        precompile(d, resources / RUNTIME_PYTHON)
    # 自检：签名封存的包里 .pyc 必须全是 checked-hash（标志 3）——按时间戳失效的那种，装机改了源码时间后第一次起包里的 Python 就会改写它、
    #   签名封存坏（v0.4.4 候选验收⑦：4 份启动模块）。发现就停、不出包，出包时当场红，不等到候选验收
    stale = [str(p.relative_to(resources)) for d in dirs for p in d.rglob("*.pyc")
             if int.from_bytes(p.read_bytes()[4:8], "little") != 3]
    if stale:
        raise RuntimeError(f"内置 Python 有 {len(stale)} 份 .pyc 不是 checked-hash（装机后会被改写、签名封存坏）：{stale[:5]}")


def write_manifest(destination, fingerprint, snap=None):
    """按引擎目录里文件的最终字节写 prepare.json 与 engine.json（逐文件大小 + sha256，启动时 embedded.artifact 校验）。
    打包时（seal_snapshot）调；发版签名（sign_release.sh）重签了里面的二进制、字节变了，签完整包之前再调一次（不重算会报「准备产物缺失或大小不符」）。
    snap = 快照字段（SNAP_KEYS）；不给 = 沿用已有 engine.json 里的，这时文件集合不许变（listed 是按打包时的文件集合算的）"""
    destination = Path(destination)
    old = {}
    if snap is None:
        old = json.loads((destination / "engine.json").read_text(encoding="utf-8"))
        snap = {k: old[k] for k in SNAP_KEYS if k in old}
    tools = destination / "Binaries/Prepare"
    tool_files = {str(p.relative_to(tools)): [p.stat().st_size, digest(p)] for p in tools.rglob("*") if p.is_file() and p.name != "prepare.json"}
    atomic(tools / "prepare.json", dict(schema=1, source=fingerprint, files=tool_files))
    # Binaries are hashed too. engine.json itself is the signed bundle's manifest.
    files = {str(p.relative_to(destination)): [p.stat().st_size, digest(p)]
             for p in sorted(destination.rglob("*")) if p.is_file() and p != destination / "engine.json"}
    if old.get("listed") and set(files) != set(old.get("files", {})):
        raise RuntimeError("引擎目录在封存快照之后多了 / 少了文件，engine.json 的 listed 已过期：%s" % sorted(set(files) ^ set(old.get("files", {})))[:5])
    identity = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    # 壳编号（pack/shell/manifest.py current_id）：在开发树按壳输入表算好写进来，快照里的出包直接读（快照按目录挑文件、剔测试目录，重算会对不上发布的壳）
    sys.path.insert(0, str(ROOT)); from pack.shell import manifest as shell_manifest
    # 发版号（三端同号）：在开发树按最近 tag 算好写进来，快照里组装 iPhone 容器直接读（core.release_version）；
    #   显示版本（v0.3.0 / v0.2.6-dev+ba0ef07）同理在开发树算好写进来——快照是全新单提交 Git 库，在快照里现算会得到快照自己的提交号（core.display_version）
    from core import display_version, release_version
    atomic(destination / "engine.json", dict(schema=1, id=identity, source=fingerprint, version=release_version(ROOT), displayVersion=display_version(ROOT),
                                           memoryRequirementsVersion=2, shellId=shell_manifest.current_id(), shellIdTvos=shell_manifest.current_id("tvos"), **snap, files=files))
    return identity
