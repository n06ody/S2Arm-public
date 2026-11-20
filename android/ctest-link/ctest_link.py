#!/usr/bin/env python3
"""安卓 Linux CTest 链接符号核对（合入前层，秒级，不要 colima）。

起因：外部 PR #1003 给 runtime/src/svc_mem.c 加了 s2a_android_guest_writers_quiesce / _begin_serial 两个外部符号，
安卓 Linux CTest 的 guest_page_test 链接目标里没有定义也没有替身 → ld undefined reference，41 项 CTest 全 Not Run；
10-28 同类（svc_mem.c 新外部符号漏补桩）。完整 CTest（android/linux_ctest.sh）要 colima 里的 aarch64 Linux、
几分钟，只在合入后层跑，合入前拦不住。

做法：不自己解析 CMake / C 源码，让真东西判——
  1. 用 CMake 按 CTest 的真实配置求值 android/CMakeLists.txt：交叉工具链声明 CMAKE_SYSTEM_NAME=Linux、aarch64
     （走 if(ANDROID) 的 else 分支 + Linux 专属目标，与 colima 里的 CTest 同一组目标、同一组源 / 替身 / 宏 / 链接参数）；
  2. 用 NDK 29 的 clang + ld.lld（与 core-link/link_symbols.py 同一套 NDK 查找）把每个可执行目标编出来并真链接（ninja -k 0，
     一个目标红不挡别的）；--gc-sections / --wrap 等链接参数照目标原样，所以「死代码里的引用不算缺」与 GNU ld 同义；
  3. 读 ld.lld 的 undefined symbol 报错，每个缺的符号打一行【红】：符号名、引用位置（文件:行）、所属目标。

与真 CTest 的差别（这一层只证明「链接得上」，不证明测试过）：
  - libc 是 bionic 不是 glibc：编译器包装去掉 -lpthread / -lrt / -pthread（bionic 都在 libc 里），并 -U__ANDROID__
    （放在最前，目标自己 -D__ANDROID__ 的源如 svc_mem.c 照样有；与 glibc 宿主上「缺省没有 __ANDROID__」一致）；
    glibc 有而 bionic（取 NDK 最高 API 级）没有的 libc 函数会误报红，反之漏报——源码里用 glibc 专属函数时以合入后的真 CTest 为准；
  - 去掉 -Werror：编译器不同告警不同，告警不是本核对的事；编译错误照报红（该目标链接核对不了）；
  - 不跑测试。
另做路由自检（--route-hook）：编译实际读到的仓库文件（ninja 依赖记录 + CMake 重跑依赖）都要被钩子文件头「# 门禁路由：」覆盖，
漏了打【红】，免得新链进来的共用文件改了却不触发本核对。

--parts apk：安卓单库 libs2arm_android.so（APK 里那个库）同一套真编真链。起因 #1086：共享前端 hle_nvn.c 每帧调用
nvn_be_tex_tick，只有 Apple 后端有定义，单库缺符号；core-link/link_symbols.py 的 -z defs 真链只在合入后层，合入前没拦住。
  1. 用 NDK 自带的 build/cmake/android.toolchain.cmake（与 Gradle 同一工具链、arm64-v8a、android-29 = minSdk、Debug）按
     android/CMakeLists.txt 的 if(ANDROID) 分支配置，APK_CONFIGS 七种组合各配一次：诊断 / 游戏 × 直连前端关 /
     nvn_vk 后端 / nvk 后端（S2A_ANDROID_VK_ENGINE 开时共享前端 hle_nvn.c 等才编进来），外加 Gradle 缺省的诊断构建
     （S2A_CS_SERVICES=0、不带探针，#557）；
  2. NativeAOT 静态服务层（libs2a_cs.a）不编：沿用 link_symbols.py 的 S2A_ANDROID_LINK_PROBE 探针——check_once 从真实
     C# 工程收出 DllImport 入口（探针引用它们）和 [UnmanagedCallersOnly] 导出（探针给空定义），write_probe 写探针；
  3. 只编 s2arm_android 目标（-Werror、-z defs 都照目标原样，不剥），ld.lld 每个缺的符号打一行【红】，探针引用的缺口
     改报 C# 引用位置；编译不过也报红。
单库与真 APK 构建的差别：C# 一侧是探针不是真归档（探针只证明「C# 引用的原生入口有定义、原生引用的 C# 导出存在」，
不链 NativeAOT 运行时 link.txt 里那些库）；s2a_avc_* 由 libavc 第三方归档提供，探针不引用（合入后层 link_symbols.py
带真归档链）；S2A_ANDROID_EXPERIMENTAL_TOUCH 只核关、VAMM 只核缺省 alias、s2arm_rewriter 不编；不打 APK、不要 SDK / Gradle。
--parts ctest-x86：同一份 Linux CTest 配置换 x86_64 目标，只编全部目标文件、不链接，-Werror 保留（上面 ctest 部件剥了它）。
起因 11-01：7360dfa77 把只被 aarch64 块调用的静态函数定义在块外，arm64 构建正常；贡献者在 x86_64 宿主跑 Linux CTest，
-Werror「定义未使用」编不过——门禁只在 arm64 上编，看不见。只编不链：x86_64 下链接核对重复 aarch64 那份，没有新信息。
退出码：0 绿；1 红；2 工具 / 环境缺失（报红，不跳过、不下载）。
"""
import argparse
import fnmatch
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "core-link"))
import link_symbols  # noqa: E402  与 Q19 同一套 NDK 查找（固定 29.0.14206865）与链接探针
from link_symbols import find_ndk  # noqa: E402

ROUTE = re.compile(r"^\s*#\s*门禁路由：\s*(.*?)\s*$")
FAILED = re.compile(r"^FAILED: (?:\[code=\d+\] )?(\S+)")
STEP = re.compile(r"^\[\d+/\d+\] ")
UNDEF = re.compile(r"^ld\.lld: error: undefined symbol: (.+?)\s*$")
REFBY = re.compile(r"^>>> referenced by (\S+?)(?: \((.+)\))?\s*$")
EXE = re.compile(r"^build (\S+): C(?:XX)?_EXECUTABLE_LINKER__")
OBJ_TARGET = re.compile(r"CMakeFiles/([^/]+)\.dir/")
# 单库配置：(模式, S2A_ANDROID_VK_ENGINE, S2ARM_ANDROID_VK_BACKEND, 标签, 是否带探针)。Gradle 缺省是引擎关；引擎开时
# 共享前端（graphics/nvn/gfx/nvn/hle_nvn.c 等）编进来，两种后端各自要给齐 nvn_be_* —— #1086 缺的就在这里。
APK_CONFIGS = (
    # 最后一项 = 带不带 link_symbols 探针（探针配置走 S2A_ANDROID_LINK_PROBE，等同服务层静态链入的源集合）
    ("diagnostic", "OFF", "nvn_vk", "诊断", True),
    ("diagnostic", "ON", "nvn_vk", "诊断 + nvn_vk 后端", True),
    ("game", "OFF", "nvn_vk", "游戏", True),
    ("game", "ON", "nvn_vk", "游戏 + nvn_vk 后端", True),
    # Gradle 缺省 assembleDebug：S2A_CS_SERVICES=0、没有探针，服务层不链入（#557：这条路缺 s2a_host_session_state，
    # 上面四种都带探针 = 走静态源集合，没拦住）。game 模式要求 static（CoreLink.cmake），只有诊断有这条路。
    ("diagnostic", "OFF", "nvn_vk", "诊断 · Gradle 缺省（服务层不链入）", False),
    # 产品游戏包走引擎开 + nvk（pack/targets/android/package.py）；同时覆盖诊断角色的 nvk 单库。
    ("game", "ON", "nvk", "游戏 + nvk 后端", True),
    ("diagnostic", "ON", "nvk", "诊断 + nvk 后端", True),
)
APK_LIB = "libs2arm_android.so"


def die(msg):
    print("【红】" + msg)
    sys.exit(2)


def ndk_tools(ndk):
    pre = sorted((ndk / "toolchains/llvm/prebuilt").glob("*/bin/clang"))
    if not pre:
        die("NDK %s 里找不到 toolchains/llvm/prebuilt/*/bin/clang" % ndk)
    base = pre[0].parent.parent
    libdir = base / "sysroot/usr/lib/aarch64-linux-android"
    apis = sorted(int(p.name) for p in libdir.iterdir() if p.name.isdigit()) if libdir.is_dir() else []
    if not apis:
        die("NDK sysroot 缺 aarch64 API 级目录：%s" % libdir)
    return base, apis[-1]


def write_toolchain(work, base, api, arch="aarch64", keep_werror=False):
    clang = shlex.quote(str(base / "bin/clang"))
    strip = "-lpthread|-lrt|-pthread" if keep_werror else "-Werror|-lpthread|-lrt|-pthread"
    for name, drv in (("cc", clang), ("cxx", clang + "++")):
        (work / name).write_text(
            "#!/bin/bash\n"
            "a=()\n"
            'for x in "$@"; do case $x in %s) ;; *) a+=("$x") ;; esac; done\n'
            'exec %s --target=%s-linux-android%d -U__ANDROID__ "${a[@]}"\n' % (strip, drv, arch, api))
        (work / name).chmod(0o755)
    tc = work / "toolchain.cmake"
    tc.write_text("\n".join([
        "set(CMAKE_SYSTEM_NAME Linux)",
        "set(CMAKE_SYSTEM_PROCESSOR %s)" % arch,
        'set(CMAKE_C_COMPILER "%s")' % (work / "cc"),
        'set(CMAKE_CXX_COMPILER "%s")' % (work / "cxx"),
        'set(CMAKE_SYSROOT "%s")' % (base / "sysroot"),
        'set(CMAKE_AR "%s")' % (base / "bin/llvm-ar"),
        'set(CMAKE_RANLIB "%s")' % (base / "bin/llvm-ranlib"),
        'set(CMAKE_NM "%s")' % (base / "bin/llvm-nm"),
        ""]))
    return tc


def parse_build(log):
    """ninja -k 0 的输出 → {目标: {"undef": [(符号, [引用…])], "compile": [对象…], "other": [行…], "first": 行}}"""
    out = {}
    cur = None
    sym = None
    for line in log.splitlines():
        m = FAILED.match(line)
        if m:
            path = m.group(1)
            t = OBJ_TARGET.search(path)
            target = t.group(1) if t else path
            cur = out.setdefault(target, {"undef": [], "compile": [], "other": [], "first": None})
            if t:
                cur["compile"].append(path)
            sym = None
            continue
        if cur is None:
            continue
        if STEP.match(line) or line.startswith("ninja: "):
            cur = sym = None
            continue
        m = UNDEF.match(line)
        if m:
            sym = (m.group(1), [])
            cur["undef"].append(sym)
            continue
        m = REFBY.match(line)
        if m and sym is not None:
            sym[1].append(m.group(2) or m.group(1))
            continue
        if re.search(r"\berror\b", line) and not line.startswith(("clang: error: linker command failed", ": && ")):
            if cur["first"] is None:
                cur["first"] = line.strip()
            if line.startswith("ld.lld: error:"):
                cur["other"].append(line.strip())
    return out


def rel(root, p):
    p = os.path.realpath(p)
    return p[len(root) + 1:] if p.startswith(root + "/") else None


def route_prefixes(hook):
    pre = []
    with open(hook, encoding="utf-8") as fh:
        for i, line in enumerate(fh):
            if i >= 40:   # 与 tools/gate_decl.py 只读文件头 HEAD 行一致
                break
            m = ROUTE.match(line)
            if m:
                pre += m.group(1).split()
    return pre


def covered(f, prefixes):   # 与 tools/gate_route.py 的命中规则一致：等于 / 前缀 / 通配
    return any(f == p or f.startswith(p) or fnmatch.fnmatch(f, p) for p in prefixes)


def build_inputs(bdir, root):
    files = set()
    deps = subprocess.run(["ninja", "-C", str(bdir), "-t", "deps"], capture_output=True, text=True).stdout
    for line in deps.splitlines():
        if line.startswith(" ") and line.strip():
            p = line.strip()
            r = rel(root, p if os.path.isabs(p) else os.path.join(bdir, p))
            if r:
                files.add(r)
    for line in (bdir / "build.ninja").read_text(encoding="utf-8", errors="replace").splitlines():
        if ": RERUN_CMAKE |" in line:
            for p in line.split("|", 1)[1].split():
                r = rel(root, p if os.path.isabs(p) else os.path.join(bdir, p))
                if r:
                    files.add(r)
    return sorted(files)


def report(fails, root, label=None, probe=None, cs_sites=None):
    """打 ninja 失败的【红】行，返回红行数。label：把这个构建目录的失败都记到一个目标名上（单库配置）；
    probe / cs_sites：探针文件路径与 {符号: [C# 引用位置…]}，探针里的引用改报 C# 位置。"""
    red = 0
    for target in sorted(fails):
        f = fails[target]
        name_t = label or target
        for name, refs in f["undef"]:
            for ref in refs or ["（ld.lld 没给引用位置）"]:
                m = re.match(r"(.+):(\d+)$", ref)
                if probe and os.path.basename(m.group(1) if m else ref) == os.path.basename(probe):
                    where = "、".join(cs_sites.get(name) or ["（探针，C# 位置不明）"]) + "（C#）"
                else:
                    where = "%s:%s" % (rel(root, m.group(1)) or m.group(1), m.group(2)) if m and os.path.isabs(m.group(1)) else ref
                print("【红】未定义符号 %s：%s 引用，目标 %s 的链接源里没有定义 / 替身" % (name, where, name_t))
                red += 1
        if f["compile"]:
            print("【红】目标 %s 编译不过（%s），链接核对不了：%s" % (name_t, f["compile"][0], f["first"] or "见 ninja 输出"))
            red += 1
        elif not f["undef"]:
            print("【红】目标 %s 链接失败：%s" % (name_t, "；".join(f["other"]) or f["first"] or "见 ninja 输出"))
            red += 1
    return red


def ctest_part(root, project, work, base, api, ndk, jobs):
    """返回 (目标数, 链过数, 红行数, 构建目录)；构建目录 None = 配置失败。"""
    tc = write_toolchain(work, base, api)
    bdir = work / "b"
    cfg = subprocess.run(["cmake", "-S", str(project), "-B", str(bdir), "-G", "Ninja",
                          "-DCMAKE_TOOLCHAIN_FILE=" + str(tc), "-DCMAKE_BUILD_TYPE=Debug"],
                         capture_output=True, text=True)
    if cfg.returncode:
        print("\n".join((cfg.stdout + cfg.stderr).strip().splitlines()[-20:]))
        print("【红】CMake 配置失败（CMAKE_SYSTEM_NAME=Linux aarch64，NDK %s API %d）" % (ndk.name, api))
        return 0, 0, 1, None
    exes = sorted(m.group(1) for m in map(EXE.match, (bdir / "build.ninja").read_text(
        encoding="utf-8", errors="replace").splitlines()) if m)
    bld = subprocess.run(["ninja", "-C", str(bdir), "-k", "0", "-j", str(jobs)],
                         capture_output=True, text=True)
    fails = parse_build(bld.stdout + bld.stderr)
    red = report(fails, root)
    linked = [e for e in exes if (bdir / e).is_file()]
    if bld.returncode and not fails:
        print("\n".join((bld.stdout + bld.stderr).strip().splitlines()[-20:]))
        print("【红】ninja 失败（rc=%d）但没解析出失败目标" % bld.returncode)
        red += 1
    for e in exes:
        if e not in linked and e not in fails:
            print("【红】目标 %s 没链出来、也不在失败列表里（射程不全）" % e)
            red += 1
    print("链接目标 %d 个（CMake Linux aarch64 配置，NDK %s API %d）：链过 %d、红 %d" % (
        len(exes), ndk.name, api, len(linked), len(exes) - len(linked)))
    if not exes:
        print("【红】CMake 没产出任何可执行目标（射程 0）")
        red += 1
    return len(exes), len(linked), red, bdir


def apk_probes(root, work, cs_project):
    """每种模式写一个链接探针（link_symbols.check_once + write_probe），返回 ({模式: 探针路径}, {符号: [C# 位置…]})。"""
    probes, sites, cache = {}, {}, {}
    projects = [Path(cs_project) if os.path.isabs(cs_project) else Path(root) / cs_project]
    for mode in sorted({c[0] for c in APK_CONFIGS if c[4]}):
        r = link_symbols.check_once(Path(root), mode, projects, None, False, "cmake", cache)
        # s2a_avc_* 由 libavc 第三方归档提供（合入后层带真归档链），合入前不链归档、探针不引用
        req = [i for i in r["required"] if i["library"] in link_symbols.TARGET_LIBS and not i["symbol"].startswith("s2a_avc_")]
        for i in req:
            sites.setdefault(i["symbol"], [])
            site = "%s:%d" % (i["path"], i["line"])
            if site not in sites[i["symbol"]]:
                sites[i["symbol"]].append(site)
        probes[mode] = work / ("probe_%s.c" % mode)
        link_symbols.write_probe(probes[mode], [i["symbol"] for i in req],
                                 [i["symbol"] for i in r["cs_exports"]], r["native_symbols"])
    return probes, sites


def apk_part(root, project, work, ndk, jobs, cs_project):
    """返回 (配置数, 链过数, 红行数, [构建目录…])。"""
    toolchain = ndk / "build/cmake/android.toolchain.cmake"
    if not toolchain.is_file():
        die("NDK 缺 CMake 工具链：%s" % toolchain)
    try:
        probes, sites = apk_probes(root, work, cs_project)
    except link_symbols.CheckError as e:
        print("【红】单库链接探针生成失败（C# 入口 / 导出收集）：%s" % " ".join(str(e).splitlines()))
        return len(APK_CONFIGS), 0, 1, []
    linked = red = 0
    bdirs = []
    for i, (mode, engine, backend, label, use_probe) in enumerate(APK_CONFIGS):
        tlabel = "%s（%s）" % (APK_LIB, label)
        bdir = work / ("apk%d" % i)
        cfg = subprocess.run([
            "cmake", "-S", str(project), "-B", str(bdir), "-G", "Ninja",
            "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain), "-DANDROID_ABI=arm64-v8a",
            "-DANDROID_PLATFORM=android-29", "-DCMAKE_BUILD_TYPE=Debug",
            "-DS2A_ANDROID_NATIVE_MODE=" + mode, "-DS2A_ANDROID_VK_ENGINE=" + engine,
            "-DS2ARM_ANDROID_VK_BACKEND=" + backend,
            "-DS2A_ANDROID_LINK_PROBE=" + (str(probes[mode]) if use_probe else ""), "-DS2A_CS_SERVICES=0"],
            capture_output=True, text=True)
        if cfg.returncode:
            print("\n".join((cfg.stdout + cfg.stderr).strip().splitlines()[-20:]))
            print("【红】目标 %s CMake 配置失败（NDK 安卓工具链 arm64-v8a android-29）" % tlabel)
            red += 1
            continue
        bdirs.append(bdir)
        bld = subprocess.run(["ninja", "-C", str(bdir), "-k", "0", "-j", str(jobs), "s2arm_android"],
                             capture_output=True, text=True)
        fails = parse_build(bld.stdout + bld.stderr)
        merged = {"undef": [], "compile": [], "other": [], "first": None}
        for f in fails.values():   # 一个构建目录 = 一个单库配置，失败都记到这个配置上
            merged["undef"] += f["undef"]
            merged["compile"] += f["compile"]
            merged["other"] += f["other"]
            merged["first"] = merged["first"] or f["first"]
        if fails:
            red += report({tlabel: merged}, root, tlabel, probes[mode] if use_probe else None, sites)
        elif bld.returncode:
            print("\n".join((bld.stdout + bld.stderr).strip().splitlines()[-20:]))
            print("【红】目标 %s ninja 失败（rc=%d）但没解析出失败目标" % (tlabel, bld.returncode))
            red += 1
        if (bdir / APK_LIB).is_file() and not bld.returncode:
            linked += 1
            print("【绿】目标 %s 链接成功" % tlabel)
        elif not fails and not bld.returncode:
            print("【红】目标 %s 没链出来（射程不全）" % tlabel)
            red += 1
    print("单库链接配置 %d 个（%s，NDK 安卓工具链 arm64-v8a android-29，诊断 / 游戏 × 直连前端关 / nvn_vk / nvk + 诊断 Gradle 缺省）：链过 %d、红 %d" % (
        len(APK_CONFIGS), APK_LIB, linked, len(APK_CONFIGS) - linked))
    return len(APK_CONFIGS), linked, red, bdirs


def ctest_x86_part(root, project, work, base, api, ndk, jobs):
    """Linux CTest 配置在 x86_64 目标下只编全部目标文件（-Werror 保留）。返回 (目标文件数, 编过数, 红行数, 构建目录)。"""
    xw = work / "x86"
    xw.mkdir()
    tc = write_toolchain(xw, base, api, arch="x86_64", keep_werror=True)
    bdir = xw / "b"
    cfg = subprocess.run(["cmake", "-S", str(project), "-B", str(bdir), "-G", "Ninja",
                          "-DCMAKE_TOOLCHAIN_FILE=" + str(tc), "-DCMAKE_BUILD_TYPE=Debug"],
                         capture_output=True, text=True)
    if cfg.returncode:
        print("\n".join((cfg.stdout + cfg.stderr).strip().splitlines()[-20:]))
        print("【红】CMake 配置失败（CMAKE_SYSTEM_NAME=Linux x86_64，NDK %s API %d）" % (ndk.name, api))
        return 0, 0, 1, None
    tg = subprocess.run(["ninja", "-C", str(bdir), "-t", "targets", "all"], capture_output=True, text=True).stdout
    objs = sorted({l.split(":")[0] for l in tg.splitlines() if l.split(":")[0].endswith(".o")})
    if not objs:
        print("【红】x86_64 配置一个目标文件都没列出来（射程 0）")
        return 0, 0, 1, bdir
    bld = subprocess.run(["ninja", "-C", str(bdir), "-k", "0", "-j", str(jobs)] + objs, capture_output=True, text=True)
    out = (bld.stdout + bld.stderr).splitlines()
    failed = [l[len("FAILED: "):].split()[0] for l in out if l.startswith("FAILED: ")]
    errs = [l.replace(str(root) + "/", "") for l in out if " error: " in l]
    for l in errs[:30]:
        print("【红】x86_64 编译：%s" % l)
    red = len(errs) or (1 if bld.returncode else 0)
    if bld.returncode and not errs:
        print("\n".join(out[-20:]))
        print("【红】x86_64 编译 ninja 失败（rc=%d）但没解析出错误行" % bld.returncode)
    print("x86_64 编译检查（CMake Linux x86_64 配置，NDK %s API %d，-Werror 保留，只编不链）：目标文件 %d 个，编不过 %d 个" % (
        ndk.name, api, len(objs), len(failed)))
    return len(objs), len(objs) - len(failed), red, bdir


def main(argv=None):
    ap = argparse.ArgumentParser(description="安卓链接符号核对（NDK 交叉真链接：Linux CTest 目标 + 单库 libs2arm_android.so）")
    ap.add_argument("--root", default=str(HERE.parents[1]), help="仓库根（引用位置按它写相对路径）")
    ap.add_argument("--project", help="CMake 工程目录（缺省 <根>/android）")
    ap.add_argument("--route-hook", help="核编译读到的仓库文件都被这个钩子文件头「# 门禁路由：」覆盖")
    ap.add_argument("--parts", default="ctest", help="ctest / apk / ctest-x86 的逗号组合（缺省 ctest）")
    ap.add_argument("--cs-project", default=link_symbols.DEFAULT_CS_PROJECT, help="单库探针读的 C# 工程（相对仓库根）")
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args(argv)
    parts = set(a.parts.split(","))
    if not parts or parts - {"ctest", "apk", "ctest-x86"}:
        ap.error("--parts 只认 ctest / apk / ctest-x86")
    t0 = time.time()
    root = os.path.realpath(a.root)
    project = Path(a.project or os.path.join(root, "android")).resolve()
    for tool in ("cmake", "ninja"):
        if not shutil.which(tool):
            die("缺 %s（brew install %s）；不跳过、不下载" % (tool, tool))
    ndk = find_ndk()
    if not ndk:
        die("找不到 NDK 29.0.14206865（查找顺序同 core-link/link_symbols.py：ANDROID_NDK_HOME / ANDROID_HOME / ~/Library/Android/sdk / Homebrew）")
    base, api = ndk_tools(ndk)
    work = Path(tempfile.mkdtemp(prefix="s2a-ctest-link-"))
    try:
        total = linked = red = 0
        bdirs = []
        if "ctest" in parts:
            n, ok, r, bdir = ctest_part(root, project, work, base, api, ndk, a.jobs)
            total, linked, red = total + n, linked + ok, red + r
            bdirs += [bdir] if bdir else []
        if "apk" in parts:
            n, ok, r, more = apk_part(root, project, work, ndk, a.jobs, a.cs_project)
            total, linked, red = total + n, linked + ok, red + r
            bdirs += more
        if "ctest-x86" in parts:
            n, ok, r, bdir = ctest_x86_part(root, project, work, base, api, ndk, a.jobs)
            total, linked, red = total + n, linked + ok, red + r
            bdirs += [bdir] if bdir else []
        routed = ""
        if a.route_hook:
            pre = route_prefixes(a.route_hook)
            inputs = sorted(set().union(*[build_inputs(b, root) for b in bdirs])) if bdirs else []
            for f in inputs:
                if not covered(f, pre):
                    print("【红】路由漏项：%s 被安卓链接目标编译 / 配置读到，%s 文件头「# 门禁路由：」没覆盖它" % (f, rel(root, a.route_hook) or a.route_hook))
                    red += 1
            routed = "，路由覆盖核 %d 个读入文件" % len(inputs)
        print("射程：目标 %d 个、链过 %d、红 %d%s；%.1f 秒" % (total, linked, total - linked, routed, time.time() - t0))
        return 1 if red else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
