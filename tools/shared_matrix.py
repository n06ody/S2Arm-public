#!/usr/bin/env python3
"""共用模块接入矩阵（#1574 P0-M1）：自动产出「共用模块 × 平台」接入矩阵，并做门禁——
   新增的共用模块安卓没链接 / 没接线、又不在例外表里 → 红；已有缺口记进基线快照，不拦今天的门禁，缺口只能收窄不能变宽。

   python3 tools/shared_matrix.py                    生成 docs/安卓接入矩阵.md 与 docs/安卓接入矩阵.json（人读 + 机读）
   python3 tools/shared_matrix.py --check            门禁口径：对 docs/安卓接入矩阵_基线.json 与 docs/安卓接入矩阵_例外.tsv 核，退出码 0 / 1
   python3 tools/shared_matrix.py --write-baseline   把当前缺口快照写成基线（缺口减少时收紧；不许变宽——新缺口会被 --check 拦）
   通用参数：--root <仓库根>（缺省取脚本上两级）、--out-md <路径>、--out-json <路径>、--baseline <路径>、--exceptions <路径>

   枚举的共用模块：
     runtime/src/*.c                    纯 C 决策核（fps / qprompt / init_fail / hud / direct_prompt / widen / cpatch / dguard / loadprog / caps …）
     graphics/**/*.c 与 graphics/**/*.csproj     显卡线共用实现（不含 */tests/ 下的单测工程）
     services/S2A.Services/*.cs        服务层共用 C# 实现（Host.cs 的 HostUi 等）
     pack/stage_*.sh                   出包共享阶段脚本
   两级判定（每平台）：
     链接 = 模块进了该平台的构建 / 出包清单。安卓看 android/** 的 CMakeLists.txt 与 *.cmake（含 Runtime.cmake 的 core 名单）
       与 cs/aot/S2A.Android.Aot.csproj 的 ProjectReference / Compile 闭包；苹果看 Makefile / */build.mk / gfx 各 Makefile 的源清单
       与 services/aot/S2A.Aot.csproj 闭包；出包阶段看 pack/targets/<平台>/target.json 与 pack/refresh.sh 平台分支。
     接线 = 模块的对外入口在该平台代码里有生产调用方（单测只叫不算接线，记「仅测试引用」）：C 模块取其非 static 函数定义，
       在平台可调到的代码里查 sym( 调用 / &sym 引用（函数原型声明与另一定义不算）；C# 模块取其公开类型名与 [UnmanagedCallersOnly] 导出名；
       阶段脚本看 refresh.sh 的安卓 / 苹果分支实调（target.json enabled=false 的分支标「开关关着」）。模块自己就是入口（定义 main）时链接即接线。
   平行实现：安卓侧存在与共用实现同接口的另一份实现——C/C++ 同名函数定义重叠 ≥2（mtl_vk_shim.c 对 nvn_mtl.m 的 mtl_*），
     C# 实现同一接口（AndroidHostUi : IHostUIHandler 对 Host.cs HostUi）或同名类型；单列一节给 M3 收敛清单用，不算红。
   例外登记 docs/安卓接入矩阵_例外.tsv：模块 <制表符> 理由 <制表符> 登记人 <制表符> 日期；登记的模块不红，照样列进矩阵。
   基线 docs/安卓接入矩阵_基线.json：随仓库。--check 时：模块缺口 ⊆ 基线缺口 = 已知缺口（列出、不红）；超集 = 新增缺口 / 缺口变宽 = 红；
   基线里已没有缺口的条目 / 已删模块只打【黄】提醒重新 --write-baseline（不红，合并冲突比门禁便宜）。
"""
import fnmatch
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import layer_map

ROOT = Path(__file__).resolve().parents[1]
OUT_MD = "docs/安卓接入矩阵.md"
OUT_JSON = "docs/安卓接入矩阵.json"
BASELINE = "docs/安卓接入矩阵_基线.json"
EXCEPTIONS = "docs/安卓接入矩阵_例外.tsv"

CODE_EXT = {".c", ".cc", ".cpp", ".h", ".hpp", ".m", ".mm", ".cs", ".java"}
TEST_PATH = re.compile(r"(^|/)(tests?|w4-tests|[^/]*-tests|androidTest)/|_test[._]|/t_[^/]*\.")   # 单测目录 / 文件名：不算生产接线
C_KW = {"if", "for", "while", "switch", "return", "sizeof", "case", "do", "else"}


def read(p):
    try:
        return Path(p).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def strip_comments(src):
    """注释与字符串整段换空格 / 清空：调用方扫描不看提示文字里的名字"""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    out = []
    for l in src.split("\n"):
        l = re.sub(r"//.*$", "", l)
        l = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', l)
        l = re.sub(r"'(?:[^'\\\n]|\\.)*'", "''", l)
        out.append(l)
    return "\n".join(out)


_LS = {}


def ls_files(root):
    """仓库里已跟踪的文件（相对路径，排序）。11-14：只认提交内容——门禁机检出目录复用，别的分支 / 并行构建
    留下的未跟踪文件（生成的 .cmake、obj 下的 .cs 等）曾让真仓 --check 时红时绿（两天害红二十多批）。同一 root 只问一次 git"""
    key = str(Path(root).resolve())
    if key not in _LS:
        _LS[key] = _ls_files(root)
    return _LS[key]


def tracked_glob(root, pattern):
    """在已跟踪文件里按通配找（代替 Path.glob 直接扫磁盘）→ 绝对 Path 列表，排序"""
    d, base = os.path.split(pattern)
    return [Path(root) / f for f in ls_files(root) if os.path.dirname(f) == d and fnmatch.fnmatchcase(os.path.basename(f), base)]


def _ls_files(root):
    r = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True)
    if r.returncode == 0:
        return sorted(f for f in r.stdout.decode("utf-8", "surrogateescape").split("\0") if f)
    out = []   # 合成仓库（单测）：按目录走
    for d, ds, fs in os.walk(root):
        ds[:] = [x for x in ds if x not in (".git", "build", "obj", "bin")]
        out += [os.path.relpath(os.path.join(d, f), root) for f in fs]
    return sorted(out)


# ---------- 模块枚举 ----------


def enum_modules(root):
    """按分层地图选模块，kind 仅描述分析方法；最长前缀与分类共用 layer_map。"""
    rows = layer_map.load(str(Path(root) / layer_map.TABLE))
    # 图形构建是地图登记的整个图形目录；细分归属仍以最长前缀为准。
    gfx_roots = [r[0] for r in rows if r[2] == "图形构建" and r[0].endswith("/")]
    mods = []
    for f in ls_files(root):
        row = layer_map.lookup(rows, f)
        if row is None:
            raise ValueError(f"分层地图未覆盖 {f}")
        prefix, layer, module, follow, _ = row
        rel = f[len(prefix):] if prefix.endswith("/") else ""
        kind = None
        if module == "原生" and follow == "共用层" and "/" not in rel and f.endswith(".c"):
            kind = "native-c"
        elif any(layer_map.match(p, f) for p in gfx_roots) and follow in ("共用层", "Metal档") and not TEST_PATH.search(f):
            if f.endswith(".c"):
                kind = "gfx-c"
            elif f.endswith(".csproj"):
                kind = "gfx-csproj"
        elif module == "服务" and follow == "共用层" and f.endswith(".cs") and Path(f).parent.name == "S2A.Services":
            kind = "service-cs"
        elif module == "公共出包" and follow == "共用层" and "/" not in rel and re.fullmatch(r"stage_[^/]+\.sh", rel):
            kind = "pack-stage"
        if kind:
            mods.append({"id": f, "kind": kind, "path": f,
                         "layer": layer, "module": module, "follow": follow, "map_prefix": prefix})
    mods.sort(key=lambda m: (m["kind"], m["id"]))
    return mods


# ---------- 入口提取 ----------


CDEF_HEAD = re.compile(r"(?m)^([A-Za-z_][\w]*(?:[ \t]+[\w\*]+)*?)(?:[ \t]+|[ \t]*\*+[ \t]*|[ \t]*\n[ \t]*)([A-Za-z_]\w*)[ \t]*\(")


def c_exports(text):
    """非 static 的 C 函数定义名 + 是否定义 main → (symbols, has_main)。
    定义 = 行首的「类型 名(参数) {」：名字含控制关键字不算、ret 含 static 不算；
    从 ( 手数括号配对到 )，后面（容许 __attribute__）跟 { 才算定义（; 是声明）。"""
    src = strip_comments(text)
    syms, main = set(), False
    for m in CDEF_HEAD.finditer(src):
        ret, name = m.group(1), m.group(2)
        if name in C_KW or name.startswith("t_") or "static" in ret.split():
            continue
        i, depth = m.end(), 1
        while i < len(src) and depth:
            ch = src[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch in ";{}":
                break
            i += 1
        if depth:
            continue
        if re.match(r"\s*(?:__attribute__\s*\(\([^)]*\)\)\s*)*\{", src[i:i + 160]):
            syms.add(name)
            main = main or name == "main"
    return syms, main


def cs_types(text):
    """→ (类型名集, UnmanagedCallersOnly 导出名集, {类: [接口]})"""
    src = strip_comments(text)
    types = set(re.findall(
        r"\b(?:(?:public|internal|sealed|static|abstract|partial|unsafe|readonly|ref|new|file)\s+)*"
        r"(?:class|interface|struct|enum|record)\s+([A-Za-z_]\w*)", src))
    # 导出名在字符串里：从原文取（src 已把字符串清空，原先这一条永远取不到任何 EntryPoint 导出）
    eps = set(re.findall(r"\[UnmanagedCallersOnly\(EntryPoint\s*=\s*\"([A-Za-z_]\w*)\"", re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.S)))
    for m in re.finditer(
            r"\[UnmanagedCallersOnly\]\s*(?:\[[^\]\n]*\]\s*)*(?:public|internal|private|static|extern|unsafe|\s)+"
            r"[\w<>\[\],\. ]+?\s+(\w+)\s*\(", src):
        eps.add(m.group(1))
    impl = {}
    for m in re.finditer(r"\b(?:class|struct)\s+(\w+)[^\{;]{0,200}?:(.{0,400}?)\{", src, re.S):
        bases = [b.strip().split("<")[0].split(".")[-1] for b in m.group(2).split(",")]
        impl[m.group(1)] = [b for b in bases if re.match(r"^I[A-Z]\w*$", b)]
    return types, eps, impl


def module_entries(root, mod):
    """模块对外入口符号集（判定「接线」用）→ (符号集, 定义了 main)"""
    if mod["kind"] in ("native-c", "gfx-c"):
        return c_exports(read(Path(root) / mod["path"]))
    if mod["kind"] == "service-cs":
        types, eps, _ = cs_types(read(Path(root) / mod["path"]))
        return types | eps, False
    if mod["kind"] == "gfx-csproj":
        syms = set()
        for cs in tracked_glob(root, os.path.dirname(mod["path"]) + "/*.cs"):
            t, e, _ = cs_types(read(cs))
            syms |= t | e
        return syms, False
    return set(), False


# ---------- 链接判定：安卓（cmake） ----------


def cmake_spans(lines):
    """每行生效的 if() 条件名集合（只收含 S2A_/ANDROID/option 变量的条件）→ [ {条件} ]"""
    conds, stack = [], []
    for l in lines:
        m = re.match(r"\s*if\s*\(([^)]*)\)", l, re.I)
        if m:
            stack.append(m.group(1).strip())
        elif (m := re.match(r"\s*elseif\s*\(([^)]*)\)", l, re.I)) and stack:
            stack[-1] = m.group(1).strip()
        elif re.match(r"\s*else\b", l, re.I):
            if stack:
                stack[-1] = "NOT " + stack[-1]
        elif re.match(r"\s*endif\b", l, re.I):
            if stack:
                stack.pop()
        conds.append({c for c in stack if re.search(r"S2A_|ANDROID|\$\{", c)})
    return conds


def android_cmake(root):
    """→ ({cmake 文件: 文本}, {文件: [core 名单]})；全部 android 的 CMakeLists.txt 与 *.cmake"""
    texts, sets = {}, {}
    for f in ls_files(root):
        if f.startswith("android/") and (f.endswith("/CMakeLists.txt") or f.endswith(".cmake")):
            texts[f] = read(Path(root) / f)
    for f, t in texts.items():
        for m in re.finditer(r"foreach\((\w+)\s+IN\s+LISTS\s+(\w+)\)", t):
            end = t.find("endforeach", m.end())
            body = t[m.end():end if end > 0 else len(t)]
            if f"runtime/src/${{{m.group(1)}}}.c" not in body:
                continue
            sm = re.search(rf"set\(\s*{m.group(2)}\s+([^)]*)\)", t[:m.start()])
            if sm:
                sets[f] = sm.group(1).split()
    return texts, sets


def android_linked_c(mod, texts, sets):
    """native-c / gfx-c 的安卓链接判定 → (linked, evidence, [条件])"""
    for f, names in sets.items():   # Runtime.cmake core 名单（裸名 → runtime/src/<名>.c）
        if mod["kind"] == "native-c" and os.path.basename(mod["path"])[:-2] in names:
            return True, f"{f} core 名单", []
    hits = []
    for f, t in texts.items():
        for n, l in enumerate(t.split("\n"), 1):
            if mod["path"] in l:
                hits.append((f, n))
    if not hits:
        return False, "安卓构建清单无此源", []
    f0, n0 = hits[0]
    conds = sorted({c.split(" ")[0] for f, n in hits for c in cmake_spans(texts[f].split("\n"))[n - 1] if c.startswith("S2A_")})
    return True, f"{f0}:{n0}", conds


# ---------- 链接判定：苹果（Makefile / build.mk） ----------


def apple_linked_c(mod, mk_files):
    """native-c / gfx-c 的苹果链接判定 → (linked, evidence)"""
    rel = mod["path"]
    if mod["kind"] == "native-c":
        return True, "runtime/build.mk 通配 runtime/src/*.c → HOST_SRCS（宿主 + iOS App）"
    if re.match(r"graphics/metal/gal_\w+\.c$", rel):
        return True, "graphics/metal/Makefile：gal_*.c 由 gal_*.m #include 编译进 libnxgal"
    for f, t in mk_files.items():
        d = os.path.dirname(f)
        sub = os.path.relpath(rel, d) if d else rel
        if rel in t or sub in t:
            return True, f"{f} 点名"
        for w in re.findall(r"\$\(wildcard ([^)]+)\)", t):
            for pat in w.split():
                if fnmatch.fnmatch(sub, pat) or fnmatch.fnmatch(rel, pat):
                    return True, f"{f} 通配 {pat}"
    return False, "苹果构建清单无此源"


# ---------- 链接判定：C#（AOT 工程闭包） ----------


def csproj_closure(root, entry):
    """入口 csproj 的 ProjectReference 传递闭包 + UnmanagedEntryPointsAssembly → (csproj 绝对路径集, 程序集名集)"""
    refs, eps, seen, todo = set(), set(), set(), [Path(root) / entry]
    while todo:
        pj = todo.pop()
        if not pj.is_absolute():
            pj = (Path(root) / pj).resolve()
        pj = pj.resolve()
        if str(pj) in seen or not pj.is_file():
            continue
        seen.add(str(pj))
        t = read(pj)
        base = pj.parent
        props = {"RepoRoot": str(Path(root).resolve()) + "/", "MSBuildThisFileDirectory": str(base) + "/"}
        for pm in re.finditer(r"<([A-Za-z_]\w*)[^>]*>([^<]*)</\1>", t):
            name, val = pm.group(1), pm.group(2).strip()
            if name in props or not val:
                continue
            gm = re.fullmatch(r"\$\(\s*\[?System\.IO\.Path\]?::\s*GetFullPath\s*\(\s*'([^']+)'\s*\)\s*\)", val)
            raw = gm.group(1) if gm else val
            for pn, pv in props.items():
                raw = raw.replace(f"$({pn})", pv)
            if "$(" not in raw and (".." in raw or "/" in raw or "\\" in raw):
                props[name] = str((base / raw.replace("\\", "/")).resolve()) + "/"
        for m in re.finditer(r'<(ProjectReference|UnmanagedEntryPointsAssembly)\s+Include="([^"]+)"', t):
            inc = m.group(2).replace("\\", "/")
            for pn, pv in props.items():
                inc = inc.replace(f"$({pn})", pv)
            if m.group(1) == "ProjectReference":
                q = Path(inc)
                q = (q if q.is_absolute() else base / q).resolve()
                refs.add(q)
                todo.append(q)
            else:
                eps.add(inc)
    return refs, eps


# ---------- 出包阶段接线 ----------


FORVAR = re.compile(r"for\s+(\w+)\s+in\s+([A-Za-z_][\w ]*)")


def refresh_wiring(root):
    """pack/refresh.sh 平台分支解析 → {阶段名: {apple: set, android: set, common: set}}。
    安卓分支 = 「if [ … = android ]; then … exit 0」块；块前 = 两平台共用段；块后 = 苹果路线"""
    lines = read(Path(root) / "pack/refresh.sh").split("\n")
    a0 = a1 = None
    for i, l in enumerate(lines):
        if a0 is None and re.match(r"\s*if\b", l) and re.search(r"(?<![!=~])=\s*\"?android\"?\s*\]", l):
            a0 = i
        elif a0 is not None and re.match(r"\s*exit\s+0\b", l):
            a1 = i
            break
    sect = lambda i: "common" if a0 is None or i < a0 else ("android" if i < a1 else "apple")
    out = {}
    def note(stage, i, how):
        out.setdefault(stage, {}).setdefault(sect(i), set()).add(how)
    loops = {}
    for i, l in enumerate(lines):
        m = FORVAR.search(l)
        if m:
            loops[m.group(1)] = m.group(2).split()
    for i, l in enumerate(lines):
        for m in re.finditer(r"stage_(\w+)\.sh", l):
            # 只认调用形态：bash "$H/stage_x.sh" … / st1 x / …；消息文字里的 pack/stage_x.sh 不算
            if re.search(r'bash\s+"?\$H/stage_|st1\s+\w|\$H/stage_\w+\.sh"\s', l):
                note(m.group(1), i, "refresh.sh")
        for m in re.finditer(r"stage_\$(\w+)\.sh|stage_\$\{(\w+)\}\.sh", l):
            for w in loops.get(m.group(1) or m.group(2), []):
                note(w, i, f"refresh.sh 循环 ${m.group(1) or m.group(2)}")
        m = re.search(r"\bst1\s+(\w+)", l)
        if m:
            note(m.group(1), i, "refresh.sh st1")
    return out


def target_stages(root, plat):
    """pack/targets/<plat>/target.json → {阶段名: enabled}（只收实现指向 pack/stage_*.sh 的）"""
    out = {}
    try:
        doc = json.loads(read(Path(root) / f"pack/targets/{plat}/target.json"))
    except Exception:
        return out
    for s in doc.get("stages", []):
        m = re.search(r"(?:^|/)stage_(\w+)\.sh$", s.get("implementation", "") or "")
        if m:
            out[m.group(1)] = s.get("enabled", True)
    return out


def stage_linked_wired(mod, wiring, tg_a, tg_i):
    """pack-stage 模块两级判定 → {apple: (linked, wired, ev), android: (…)}"""
    stem = os.path.basename(mod["path"])[6:-3]   # pack/stage_x.sh → x
    w = wiring.get(stem, {})
    res = {}
    for plat, tg, key in (("apple", tg_i, "apple"), ("android", tg_a, "android")):
        called = bool(set(w.get("common", ())) | set(w.get(key, ())))
        declared = stem in tg
        enabled = tg.get(stem, True)
        if plat == "android" and stem == "extract" and tg_a.get("direct") is False:
            enabled = False   # refresh.sh：抽取只喂直连库，direct.enabled=false 时一起跳过
        ev = []
        if called:
            ev.append("refresh.sh " + "/".join(sorted(set(w.get("common", ())) | set(w.get(key, ())))))
        if declared:
            ev.append("target.json")
        if called and not enabled:
            ev.append("enabled=false")
        res[plat] = ((called or declared), (called and enabled), "；".join(ev))
    return res


# ---------- 调用方语料（接线判定） ----------


def android_known_inactive(src):
    """仅排除能证明在 Android 无效的条件分支；未知条件保留，不把文本命中称作运行证据。"""
    stack, active, out = [], True, []
    for line in src.split("\n"):
        m = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if m:
            op = m.group(1)
            expr = re.sub(r"/\*.*?\*/|//.*$", "", m.group(2)).strip()
            if op in ("if", "ifdef", "ifndef"):
                value = None
                if op in ("ifdef", "ifndef") and expr in ("__APPLE__", "__ANDROID__"):
                    value = (expr == "__ANDROID__") ^ (op == "ifndef")
                elif op == "if":
                    simple = re.fullmatch(r"(!?)\s*defined\s*(?:\(\s*(__(?:APPLE|ANDROID)__)\s*\)|(__(?:APPLE|ANDROID)__))", expr)
                    if simple:
                        value = (simple.group(2) or simple.group(3)) == "__ANDROID__"
                        if simple.group(1): value = not value
                    elif "||" not in expr and re.match(r"defined\s*\(\s*__APPLE__\s*\)\s*&&", expr):
                        value = False
                stack.append((active, value))
                active = active and value is not False
            elif op == "else" and stack:
                parent, value = stack[-1]
                active = parent and value is not True
                stack[-1] = (parent, None if value is None else not value)
            elif op == "elif" and stack:
                parent, value = stack[-1]
                active = parent and value is not True
                stack[-1] = (parent, None)
            elif op == "endif" and stack:
                active, _ = stack.pop()
            out.append("")
        else:
            out.append(line if active else "")
    return "\n".join(out)


def corpus(root, names, android=False):
    result = {}
    for f in sorted(names):
        text = read(Path(root) / f)
        if android and Path(f).suffix in (".c", ".cc", ".cpp", ".h", ".hpp", ".m", ".mm"):
            text = android_known_inactive(text)
        text = DLSYM_SRC.sub(lambda m: m.group(1) + "S2A_DLSYM__" + m.group(2), text)   # 去字符串之前把 dlsym 取的名字留成标记（DLSYM）
        result[f] = strip_comments(text)
    return result


PROTO_TAIL = re.compile(r"\)\s*(?:;|\{)\s*$")
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
# 按名字取的入口：dlsym(句柄, "名字")。服务层 C# 的 [UnmanagedCallersOnly] 导出常由共用 C 代码这样取（nvn2.c 取 S2A_CameraSessionOpen）——
#   那份 C 代码进了哪个平台的链，导出就在哪个平台接了线；原先只认 .cs 里的使用，这类导出一律被判「找不到调用方」
DLSYM_SRC = re.compile(r'(\bdlsym\s*\([^,()]*,\s*)"([A-Za-z_]\w*)"')   # 原文里的形态（corpus 在去掉字符串之前换成标记）
DLSYM = re.compile(r"\bS2A_DLSYM__([A-Za-z_]\w*)")
# 链接期弱引用：extern … 名字(…) __attribute__((weak))。安卓上服务层与 C 代码静态并进同一个库（--exclude-libs,ALL），
#   dlsym 取不到隐藏导出，同库取服务层导出要这样写（hle_nvn.c S2A_NvMapFromGuest、nvn2.c S2A_CameraSessionOpen）——算接线，同 dlsym
WEAKREF = re.compile(r"\bextern\b[^;{}]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*__attribute__\s*\(\(\s*weak\s*\)\)")
C_CALLER_SUFFIXES = (".c", ".cc", ".cpp", ".m", ".mm")   # 直接调 C# 导出算接线的调用方文件（C 系；.cs 另走使用形）
USE = [re.compile(r"\bnew\s+([A-Za-z_]\w*)"),
       re.compile(r":\s*([A-Za-z_]\w*)\b"),
       re.compile(r"\b([A-Za-z_]\w*)\s*\."),
       re.compile(r"\b([A-Za-z_]\w*)\s*<"),
       re.compile(r"\b([A-Za-z_]\w*)\s+\w+\s*=[^=]")]
WORD = re.compile(r"[\w\*<>\[\],:]*")


def scan_callers(corpus_map):
    """语料一遍扫完 → ({符号: [(文件,行)]} C 调用形, {名字: [(文件,行)]} C# 使用形）。
    C：sym( 是调用 / &sym 是引用；前缀全是类型词、尾巴是 ) ; 或 ) { 的是原型声明 / 重定义，不算调用。"""
    calls, uses = {}, {}
    for f, t in corpus_map.items():
        for n, l in enumerate(t.split("\n"), 1):
            for m in CALL.finditer(l):
                prefix = l[:m.start()].strip()
                words = prefix.split()
                if prefix and "(" not in prefix and all(WORD.fullmatch(w) for w in words) \
                        and words[-1] not in C_KW and PROTO_TAIL.search(l[m.start():]):
                    continue
                calls.setdefault(m.group(1), []).append((f, n))
            for p in USE:
                for m in p.finditer(l):
                    uses.setdefault(m.group(1), []).append((f, n))
            for m in DLSYM.finditer(l):
                calls.setdefault("dlsym:" + m.group(1), []).append((f, n))   # 单独的键：普通 C 调用形不受影响
            for m in WEAKREF.finditer(l):
                calls.setdefault("weak:" + m.group(1), []).append((f, n))    # 同上：链接期弱引用单独记
    return calls, uses


def wired_of(mod, syms, has_main, linked, prod_calls, prod_uses, test_calls):
    """→ (wired, evidence, 仅测试)。C 走 calls，C# 走 calls ∪ uses（导出名按调用形、类型名按使用形）。
    自己定义 main 只在该平台链上时才算接线——苹果才编的宿主工具不算安卓接线。"""
    own = mod["path"] if mod["kind"] != "gfx-csproj" else os.path.dirname(mod["path"]) + "/"

    def not_self(f):
        return f != own and not (own.endswith("/") and f.startswith(own))

    if has_main:
        return linked, "自身是入口（main），" + ("链接即接线" if linked else "本平台不编"), False
    if not syms:
        return False, "无可查入口符号", False
    hits, thits = [], []
    for s in syms:
        hits += [f"{f}:{n}" for f, n in prod_calls.get(s, []) + (prod_uses.get(s, []) if mod["kind"] in ("service-cs", "gfx-csproj") else [])
                 if not_self(f) and (mod["kind"] not in ("service-cs", "gfx-csproj") or f.endswith(".cs"))]
        if mod["kind"] in ("service-cs", "gfx-csproj"):   # C# 导出被平台可调到的 C 代码按名字 dlsym 取、或链接期弱引用：算接线（不限 .cs）
            hits += [f"{f}:{n}" for f, n in prod_calls.get("dlsym:" + s, []) + prod_calls.get("weak:" + s, []) if not_self(f)]
            # 平台可调到的 C 代码直接调这个导出（不经 dlsym、也不经弱引用声明，如 lifecycle.c 安卓下强引用调 S2A_CancelWaits()）：也算接线
            hits += [f"{f}:{n}" for f, n in prod_calls.get(s, []) if not_self(f) and f.endswith(C_CALLER_SUFFIXES)]
        thits += [f for f, _n in test_calls.get(s, []) if not_self(f)]
    hits = sorted(set(hits))
    if hits:
        return True, "文本候选（未证运行路径） " + " ".join(hits[:3]) + (" 等" if len(hits) > 3 else ""), False
    if thits:
        return False, "仅测试引用 " + " ".join(sorted(set(thits))[:3]), True
    return False, "平台代码里找不到调用方", False


# ---------- 平行实现 ----------


def parallel_impls(root, mods, files):
    """→ [(安卓侧, 共用侧, [共同符号 / 接口])]"""
    # 共用侧 = 矩阵里的共用模块文件 + graphics/ 下的 .m 共用实现；platforms/<平台> 文件是平台契约实现（plat_*），不算共用侧
    shared = [m["path"] for m in mods if m["kind"] in ("native-c", "gfx-c")]
    shared += [f for f in files if re.match(r"graphics/.*\.m$", f) and not TEST_PATH.search(f)]
    shared_defs = {}
    for f in sorted(set(shared)):
        for s in c_exports(read(Path(root) / f))[0]:
            if len(s) >= 4:   # 单字母误报（ObjC 方法形等）不算接口
                shared_defs.setdefault(s, f)
    out = []
    afiles = [f for f in files if f.startswith("android/")
              and Path(f).suffix in (".c", ".cc", ".cpp", ".m", ".mm") and not TEST_PATH.search(f)]
    pairs = {}
    for af in afiles:
        common = {s for s in c_exports(read(Path(root) / af))[0] if s in shared_defs and len(s) >= 4}
        for s in common:
            pairs.setdefault((af, shared_defs[s]), set()).add(s)
    out += [(a, s, sorted(v)) for (a, s), v in sorted(pairs.items()) if len(v) >= 2]
    # C#：安卓类实现的接口与共用侧类实现同一接口（BCL 通用接口不算）；同名类型也算
    DENY = {"IDisposable", "IAsyncDisposable", "IEnumerable", "ICloneable", "IComparable", "IEquatable",
            "INotifyPropertyChanged", "ISerializable", "ICollection", "IList", "IDictionary"}
    shared_impl, shared_ty = {}, {}
    for m in mods:
        if m["kind"] != "service-cs":
            continue
        ty, _e, impl = cs_types(read(Path(root) / m["path"]))
        for c, ifs in impl.items():
            for i in ifs:
                shared_impl.setdefault(i, []).append(f"{c}（{m['path']}）")
        for t in ty:
            shared_ty.setdefault(t, m["path"])
    for f in files:
        if not (f.startswith("android/") and Path(f).suffix == ".cs" and not TEST_PATH.search(f)):
            continue
        ty, _e, impl = cs_types(read(Path(root) / f))
        for c, ifs in impl.items():
            for i in ifs:
                if i in DENY or i not in shared_impl:
                    continue
                for s in shared_impl[i]:
                    out.append((f"{c}（{f}）", s, [f": {i}"]))
        for t in ty:
            if t in shared_ty:
                out.append((f"{t}（{f}）", f"同名类型（{shared_ty[t]}）", [t]))
    return out


# ---------- 例外 / 基线 ----------


def exceptions(root, path):
    """TSV：模块 <制表符> 理由 <制表符> 登记人 <制表符> 日期（# 注释）"""
    out = {}
    for l in read(Path(root) / path).split("\n"):
        if not l.strip() or l.startswith("#"):
            continue
        c = l.split("\t")
        if len(c) >= 2:
            out[c[0].strip()] = {"理由": c[1].strip(), "登记人": c[2].strip() if len(c) > 2 else "",
                                  "日期": c[3].strip() if len(c) > 3 else ""}
    return out


def load_baseline(root, path):
    try:
        return json.loads(read(Path(root) / path)).get("modules")
    except Exception:
        return None


def gap(m):
    g = set()
    if not m["android_linked"]:
        g.add("未链接")
    if not m["android_wired"]:
        g.add("未接线")
    return g


# ---------- 主流程 ----------


def build_matrix(root):
    files = ls_files(root)
    mk_files = {f: read(Path(root) / f) for f in files if f.endswith("build.mk") or os.path.basename(f) == "Makefile"}
    mods = enum_modules(root)
    texts, sets = android_cmake(root)
    a_refs, _a_eps = csproj_closure(root, "android/cs/aot/S2A.Android.Aot.csproj")
    p_refs, _p_eps = csproj_closure(root, "services/aot/S2A.Aot.csproj")
    wiring = refresh_wiring(root)
    tg_a, tg_i = target_stages(root, "android"), target_stages(root, "ios")
    a_prod_names, a_test_names, p_names = set(), set(), set()
    # Android native source candidates must appear in a CMake file; an unused
    # backend sitting under android is not a production caller.
    cmake_sources = "\n".join(re.sub(r"#.*", "", text) for text in texts.values())
    for f in files:
        if Path(f).suffix not in CODE_EXT:
            continue
        if f.startswith("android/"):
            if TEST_PATH.search(f):
                a_test_names.add(f)
            elif Path(f).suffix in (".c", ".cc", ".cpp", ".m", ".mm"):
                relative = f[len("android/"):]
                if relative in cmake_sources or f in cmake_sources:
                    a_prod_names.add(f)
            else:
                a_prod_names.add(f)
        elif f.startswith("compat/tree/") or f.startswith("tests/") or "/tests/" in f or TEST_PATH.search(f):
            continue
        else:
            p_names.add(f)
    for m in mods:
        if m["kind"] in ("native-c", "gfx-c"):
            linked, ev, cond = android_linked_c(m, texts, sets)
            m["android_linked"], m["android_ev"] = linked, ev + ("（" + " ".join(cond) + "）" if cond else "")
            if linked:
                a_prod_names.add(m["path"])   # 安卓链上的共用源自己也是安卓侧的调用方
            m["apple_linked"], m["apple_ev"] = apple_linked_c(m, mk_files)
        elif m["kind"] in ("service-cs", "gfx-csproj"):
            proj = (Path(root) / ("services/S2A.Services/S2A.Services.csproj" if m["kind"] == "service-cs" else m["path"])).resolve()
            m["android_linked"] = proj in a_refs
            m["android_ev"] = "S2A.Android.Aot ProjectReference" if m["android_linked"] else "安卓 AOT 闭包外"
            m["apple_linked"] = proj in p_refs
            m["apple_ev"] = "S2A.Aot ProjectReference" if m["apple_linked"] else "苹果 AOT 闭包外"
            if m["android_linked"]:
                if m["kind"] == "service-cs":
                    a_prod_names.add(m["path"])   # 安卓链上的共用 .cs 自己也是安卓侧调用方
                else:
                    a_prod_names.update(
                        os.path.relpath(str(cs), root)
                        for cs in tracked_glob(root, os.path.dirname(m["path"]) + "/*.cs"))
        elif m["kind"] == "pack-stage":
            sw = stage_linked_wired(m, wiring, tg_a, tg_i)
            for plat in ("apple", "android"):
                l, w, ev = sw[plat]
                m[f"{plat}_linked"], m[f"{plat}_wired"], m[f"{plat}_ev"] = l, w, ev
            m["entries"] = []
    a_calls, a_uses = scan_callers(corpus(root, a_prod_names, android=True))
    t_calls, _t_uses = scan_callers(corpus(root, a_test_names))
    p_calls, p_uses = scan_callers(corpus(root, p_names))
    for m in mods:
        if m["kind"] == "pack-stage":
            continue
        syms, has_main = module_entries(root, m)
        m["entries"] = sorted(syms)
        m["android_wired"], m["android_wired_ev"], _ = wired_of(m, syms, has_main, m["android_linked"], a_calls, a_uses, t_calls)
        m["apple_wired"], m["apple_wired_ev"], _ = wired_of(m, syms, has_main, m["apple_linked"], p_calls, p_uses, {})
    par = parallel_impls(root, mods, files)
    par_of = {}
    for a, s, _syms in par:
        if a.startswith("android/"):
            par_of.setdefault(s, []).append(a)
    for m in mods:
        m["parallel"] = par_of.get(m["path"], [])
    return mods, par


def cell(s):
    return str(s).replace("|", "｜")


def emit_md(root, mods, par, exc):
    sha = subprocess.run(["git", "-C", str(root), "rev-parse", "--short=9", "HEAD"],
                         capture_output=True, text=True).stdout.strip() or "（非 git 仓库）"
    gapped = [m for m in mods if gap(m)]
    L = ["# 安卓接入矩阵（自动生成，勿手改）", "",
         f"由 `tools/shared_matrix.py` 生成 @ {sha}。两级静态判定：链接 = 进平台构建 / 出包清单；接线 = 模块入口有生产代码文本候选（仅测试引用不算）；未知宏、弱符号和动态加载的实际路径未证。",
         "",
         f"共用模块 {len(mods)} 个：安卓链接和文本候选均有 {len(mods) - len(gapped)} 个、有缺口 {len(gapped)} 个（例外登记 {len([m for m in gapped if m['id'] in exc])} 个、平行实现 {len(par)} 处）。",
         "", "## 主表", "",
         "| 模块 | 地图层 / 模块 / 跟进 | 类别 | 苹果 链接 | 苹果 接线 | 安卓 链接 | 安卓 接线 | 备注 |",
         "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    for m in mods:
        note = []
        if m["id"] in exc:
            note.append("例外：" + exc[m["id"]]["理由"])
        if m.get("parallel"):
            note.append("平行实现")
        if m["android_linked"] and not m["android_wired"]:
            note.append("链接了没接线")
        L.append(f"| `{m['id']}` | {cell(m['layer'])} / {cell(m['module'])} / {cell(m['follow'])} | {m['kind']} | {'✓' if m['apple_linked'] else '—'} | {'✓' if m['apple_wired'] else '—'} "
                 f"| {'✓' if m['android_linked'] else '—'} | {'✓' if m['android_wired'] else '—'} | {cell('；'.join(note))} |")
    L += ["", "## 安卓有缺口的模块", "", "| 模块 | 安卓 链接 | 安卓 接线 | 证据 |", "| --- | --- | --- | --- |"]
    for m in gapped:
        ev = m.get("android_wired_ev", "") if m["android_linked"] else m.get("android_ev", "")
        L.append(f"| `{m['id']}` | {'✓' if m['android_linked'] else '—'} | {'✓' if m['android_wired'] else '—'} | {cell(ev)} |")
    L += ["", "## 链接了没接线（安卓）", "", "| 模块 | 接线证据 |", "| --- | --- |"]
    for m in mods:
        if m["android_linked"] and not m["android_wired"]:
            L.append(f"| `{m['id']}` | {cell(m.get('android_wired_ev', ''))} |")
    L += ["", "## 平行实现（安卓侧与共用实现同接口的另一份）", "", "| 安卓侧 | 共用侧 | 共同接口 |", "| --- | --- | --- |"]
    for a, s, syms in par:
        L.append(f"| `{a}` | `{s}` | {cell(' '.join(syms[:12]))}{' …' if len(syms) > 12 else ''} |")
    L += ["", "## 例外登记（docs/安卓接入矩阵_例外.tsv）", "", "| 模块 | 理由 | 登记人 | 日期 |", "| --- | --- | --- | --- |"]
    for k, v in sorted(exc.items()):
        L.append(f"| `{k}` | {cell(v['理由'])} | {cell(v['登记人'])} | {cell(v['日期'])} |")
    L.append("")
    return "\n".join(L)


def emit_json(mods, par, exc):
    return json.dumps({
        "schema": "s2a.shared_matrix.v1",
        "modules": [{
            "id": m["id"], "kind": m["kind"],
            "layer_map": {"layer": m["layer"], "module": m["module"], "follow": m["follow"], "prefix": m["map_prefix"]},
            "apple": {"linked": m["apple_linked"], "wired": m["apple_wired"]},
            "android": {"linked": m["android_linked"], "wired": m["android_wired"],
                        "evidence": m.get("android_ev", ""), "wired_evidence": m.get("android_wired_ev", "")},
            "exception": m["id"] in exc,
            "parallel": m.get("parallel", []),
        } for m in mods],
        "parallel_impls": [{"android": a, "shared": s, "interface": v} for a, s, v in par],
        "exceptions": exc,
    }, ensure_ascii=False, indent=1)


def check(root, base_path, exc_path):
    """门禁：新增共用模块安卓有缺口且不在例外表 → 红；缺口比基线变宽 → 红"""
    mods, par = build_matrix(root)
    exc = exceptions(root, exc_path)
    base = load_baseline(root, base_path)
    reds, yellows, known = [], [], []
    if base is None:
        yellows.append(f"【黄】基线 {base_path} 读不出：所有缺口按「新增」算——先跑 tools/shared_matrix.py --write-baseline")
        base = {}
    cur = {m["id"]: m for m in mods}
    for m in mods:
        g = gap(m)
        if not g or m["id"] in exc:
            continue
        b = base.get(m["id"])
        if b is None:
            reds.append(f"【红】新增共用模块 {m['id']} 安卓{'、'.join(sorted(g))}"
                        f"（{m.get('android_ev', '')}；{m.get('android_wired_ev', '')}）："
                        "接上构建与调用方，或在 docs/安卓接入矩阵_例外.tsv 登记理由")
            continue
        bg = set(b.get("gaps", []))
        if g - bg:
            reds.append(f"【红】共用模块 {m['id']} 安卓缺口变宽：基线 {'、'.join(sorted(bg)) or '无'} → 现在 {'、'.join(sorted(g))}"
                        "（只许收窄不许变宽；确属不适用就在例外表登记）")
        else:
            known.append(f"{m['id']}（{'、'.join(sorted(g))}）")
    stale = [k for k, v in base.items() if v.get("gaps") and k in cur and not gap(cur[k])]
    gone = [k for k in base if k not in cur]
    if stale:
        yellows.append(f"【黄】基线里 {len(stale)} 个条目已无缺口：跑 tools/shared_matrix.py --write-baseline 收紧"
                       f"（{' '.join(stale[:5])}{' …' if len(stale) > 5 else ''}）")
    if gone:
        yellows.append(f"【黄】基线里 {len(gone)} 个模块已不在仓库：{' '.join(gone[:5])}")
    for r in reds:
        print(r)
    for y in yellows:
        print(y)
    if known:
        print(f"基线已有缺口（不红）：{len(known)} 个：{' '.join(known[:12])}{' …' if len(known) > 12 else ''}")
    print(f"共用模块矩阵：{len(mods)} 个模块，安卓缺口 {len([m for m in mods if gap(m)])} 个"
          f"（例外 {len([m for m in mods if gap(m) and m['id'] in exc])} 个、基线 {len(known)} 个、平行实现 {len(par)} 处）")
    return 1 if reds else 0


def write_baseline(root, mods, path):
    doc = {"schema": "s2a.shared_matrix_baseline.v1", "modules": {}}
    sha = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if sha:
        doc["from"] = sha
    for m in mods:
        g = gap(m)
        doc["modules"][m["id"]] = {"gaps": sorted(g)} if g else {"gaps": []}
    Path(root, path).write_text(json.dumps(doc, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"基线已写 {path}：{len(mods)} 个模块、{len([m for m in mods if gap(m)])} 个有缺口")


def main(argv):
    root, check_f, wb = ROOT, False, False
    out_md, out_json, base, excp = OUT_MD, OUT_JSON, BASELINE, EXCEPTIONS
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--check":
            check_f = True
        elif a == "--write-baseline":
            wb = True
        elif a == "--root":
            i += 1
            root = Path(argv[i])
        elif a == "--out-md":
            i += 1
            out_md = argv[i]
        elif a == "--out-json":
            i += 1
            out_json = argv[i]
        elif a == "--baseline":
            i += 1
            base = argv[i]
        elif a == "--exceptions":
            i += 1
            excp = argv[i]
        else:
            print(__doc__, file=sys.stderr)
            return 2
        i += 1
    if check_f:
        return check(root, base, excp)
    mods, par = build_matrix(root)
    exc = exceptions(root, excp)
    if wb:
        write_baseline(root, mods, base)
        return 0
    Path(root, out_md).parent.mkdir(parents=True, exist_ok=True)
    Path(root, out_md).write_text(emit_md(root, mods, par, exc), encoding="utf-8")
    Path(root, out_json).write_text(emit_json(mods, par, exc) + "\n", encoding="utf-8")
    gapped = [m for m in mods if gap(m)]
    print(f"共用模块矩阵已生成 {out_md} / {out_json}：{len(mods)} 个模块，安卓缺口 {len(gapped)} 个、"
          f"平行实现 {len(par)} 处、例外 {len(exc)} 条")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
