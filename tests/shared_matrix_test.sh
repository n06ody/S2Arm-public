#!/bin/bash
# 门禁覆盖：tools/shared_matrix.py runtime/* graphics/* services/* pack/* android/* docs/安卓接入矩阵*
# 共用模块接入矩阵单测（tools/shared_matrix.py，约 30 秒：合成小仓 4 个场景 + 真仓 --check）：bash tests/shared_matrix_test.sh
# 期望：合成仓里——基线写完后既有缺口与已登记例外不拦门禁（绿）；新加一个安卓没链接没接线的共用模块 → --check 红；
#   该模块登记进例外表 → 绿；把基线缺口模块接上（进 core 名单 + 加调用方）→ 缺口收窄提示【黄】仍绿；
#   负对照：把工具 gap() 阉割成永远返回空 → --check 变绿（证明上面的红确实来自检测而不是误伤 / 常红）。
set -u; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); bad=0
trap 'rm -rf "$T"' EXIT
ck() { if eval "$2"; then echo "通 $1"; else echo "不通 $1"; bad=1; fi; }
sed_edit() { sed -i.s2a-bak "$1" "$2" && rm -f "$2.s2a-bak"; }   # GNU / BSD 通用的原地改

# ---------- 合成仓 ----------
R=$T/repo; mkdir -p "$R"/{runtime/src,android/{native,cs/aot},platform/apple,services/{S2A.Services,aot},graphics/{compiler,widen},pack/targets/{android,ios},docs,tools}
cat > "$R/tools/layer_map.tsv" <<'EOF'
graphics/	图形后端	图形构建	Metal档
graphics/widen/	核心	宽屏	共用层
runtime/src/	核心	原生	共用层
runtime/build.mk	工具与测试	构建	工具与文档
android/	平台壳	安卓	安卓自己
platform/apple/	平台壳	苹果	终端壳
services/	核心	服务	共用层
services/aot/	平台壳	苹果	终端壳
pack/	核心	公共出包	共用层
pack/targets/	平台壳	出包	按目标
docs/	文档与约定	文档	工具与文档
tools/	工具与测试	工具	工具与文档
EOF
cat > "$R/runtime/src/alpha.c" <<'EOF'   # 安卓已链接已接线：不该出现在缺口里
int s2a_alpha_tick(void) { return 1; }
EOF
cat > "$R/runtime/src/newmod.c" <<'EOF'  # 苹果通配进库、安卓没接：基线缺口
int s2a_newmod_ping(void) { return 2; }
EOF
cat > "$R/runtime/src/oldgap.c" <<'EOF'  # 基线缺口，后面把它接上看【黄】
int s2a_oldgap_kick(void) { return 3; }
EOF
cat > "$R/runtime/src/excmod.c" <<'EOF'  # 基线缺口但例外登记过
int s2a_excmod_zap(void) { return 4; }
EOF
echo 'HOST_SRCS := $(wildcard ../src/*.c)' > "$R/runtime/build.mk"   # 苹果：通配 runtime/src/*.c 全链
cat > "$R/android/CMakeLists.txt" <<'EOF'               # 安卓：core 名单只含 alpha
set(core alpha)
set(android_src native/caller.c native/oldgap_use.c)
foreach(x IN LISTS core)
  list(APPEND SRCS runtime/src/${x}.c)
endforeach()
EOF
cat > "$R/android/native/caller.c" <<'EOF'              # 安卓侧生产调用方
extern int s2a_alpha_tick(void);
int android_boot(void) { return s2a_alpha_tick(); }
EOF
echo 'int x(void){ return new SvcLib(), 0; }' > "$R/android/native/use.cs"
cat > "$R/android/cs/aot/S2A.Android.Aot.csproj" <<'EOF'
<Project><ItemGroup><ProjectReference Include="$(RepoRoot)services/S2A.Services/S2A.Services.csproj"/><ProjectReference Include="$(RepoRoot)graphics/compiler/../widen/Widen.csproj"/></ItemGroup></Project>
EOF
echo '<Project/>' > "$R/services/S2A.Services/S2A.Services.csproj"
echo '<Project/>' > "$R/graphics/widen/Widen.csproj"
cat > "$R/services/S2A.Services/Svc.cs" <<'EOF'                   # 服务层：安卓链上且有调用方
public class SvcLib {}
EOF
cat > "$R/services/S2A.Services/Orphan.cs" <<'EOF'                # 服务层：安卓链上但没接线
public class Orphan {}
EOF
cat > "$R/services/aot/S2A.Aot.csproj" <<'EOF'
<Project><ItemGroup><ProjectReference Include="$(RepoRoot)services/S2A.Services/S2A.Services.csproj"/></ItemGroup></Project>
EOF
echo 'int x(void){ return s2a_alpha_tick(); }' > "$R/platform/apple/host.c"
echo 'echo demo' > "$R/pack/stage_demo.sh"
cat > "$R/pack/refresh.sh" <<'EOF'
if [ "${S2A_PACK_PLAT:-ios}" = android ]; then
  bash "$H/stage_demo.sh" "$G"
  exit 0
fi
bash "$H/stage_demo.sh" "$G"
EOF
echo '{"stages":[{"implementation":"pack/refresh.sh"}]}' > "$R/pack/targets/android/target.json"
echo '{"stages":[{"implementation":"pack/stage_demo.sh"}]}' > "$R/pack/targets/ios/target.json"
cat > "$R/docs/安卓接入矩阵_例外.tsv" <<'EOF'
runtime/src/excmod.c	安卓明确不适用（合成例外）	tester	2026-01-01
EOF

run() { (cd "$R" && python3 "$H/tools/shared_matrix.py" --root "$R" "$@" 2>&1); }

# 地图最长前缀、C 符号和 Android 条件分支：旧扫描器会把函数名末字母及 Apple 代码误当调用。
PYTHONPATH="$H/tools" python3 - "$R" <<'PYEOF'
import pathlib, sys
import layer_map, shared_matrix as m
rows = layer_map.load(str(pathlib.Path(sys.argv[1]) / 'tools/layer_map.tsv'))
assert layer_map.lookup(rows, 'runtime/src/alpha.c')[2] == '原生'
assert layer_map.lookup(rows, 'graphics/widen/Widen.csproj')[2] == '宽屏'
table = pathlib.Path(sys.argv[1]) / 'tools/layer_map.tsv'
original = table.read_text()
assert 'runtime/src/alpha.c' in {v['id'] for v in m.enum_modules(sys.argv[1])}
try:
    table.write_text(original + 'runtime/src/alpha.c\t平台壳\t宿主\t终端壳\n')
    assert 'runtime/src/alpha.c' not in {v['id'] for v in m.enum_modules(sys.argv[1])}
finally:
    table.write_text(original)
refs, _ = m.csproj_closure(sys.argv[1], 'android/cs/aot/S2A.Android.Aot.csproj')
assert (pathlib.Path(sys.argv[1]) / 'graphics/widen/Widen.csproj').resolve() in refs
text = 'int s2a_real(void) { return 0; }\nstatic int s2a_hidden(void) { return 0; }\n'
assert m.c_exports(text)[0] == {'s2a_real'}
text = '#if defined(__APPLE__) // host\nwrong();\n#else\nright();\n#endif\n#if UNKNOWN\nmaybe();\n#endif\n'
t = m.android_known_inactive(text)
assert 'wrong()' not in t and 'right()' in t and 'maybe()' in t
PYEOF
map_test_rc=$?
ck "地图、C 函数名和 Android 条件分支" '[ $map_test_rc = 0 ]'

# ---------- 场景 ----------
o=$(run --write-baseline); r=$?
ck "合成仓 --write-baseline 能写基线（rc=${r}）" '[ $r = 0 ] && [ -f "$R/docs/安卓接入矩阵_基线.json" ]'

o=$(run --check); r=$?
ck "基线内缺口 + 例外登记不拦门禁（rc=${r}）" '[ $r = 0 ]'
ck "既有缺口仍被列出" 'echo "$o" | grep -q "runtime/src/newmod.c"'
ck "例外模块不进红名单" '! echo "$o" | grep "【红】" | grep -q excmod'

echo 'int s2a_late_jump(void) { return 5; }' > "$R/runtime/src/late.c"
o=$(run --check); r=$?
ck "新增共用模块安卓没接 → 红（rc=${r}）" '[ $r != 0 ] && echo "$o" | grep -q "【红】新增共用模块 runtime/src/late.c"'

printf 'runtime/src/late.c\t补登记\ttester\t2026-01-02\n' >> "$R/docs/安卓接入矩阵_例外.tsv"
o=$(run --check); r=$?
ck "新缺口登记例外后 → 绿（rc=${r}）" '[ $r = 0 ]'
sed_edit '/^runtime\/src\/late.c/d' "$R/docs/安卓接入矩阵_例外.tsv"

# 服务层 C# 只靠导出名对外（[UnmanagedCallersOnly(EntryPoint = "…")]）、由共用 C 代码按名字 dlsym 取：没人取 → 红；
#   安卓链上的 C 文件 dlsym(RTLD_DEFAULT, "那个名字") → 算接线、绿（11-14：EntryPoint 导出名原先从去了字符串的源里取，永远取不到；dlsym 取的也不认）
cat > "$R/services/S2A.Services/Exported.cs" <<'EOF2'
using System.Runtime.InteropServices;
static class Exported { [UnmanagedCallersOnly(EntryPoint = "S2A_TestExportOpen")] static uint Open() => 1; }
EOF2
o=$(run --check); r=$?
ck "服务层只有导出名、没人取 → 红（rc=${r}）" '[ $r != 0 ] && echo "$o" | grep -q "【红】新增共用模块 services/S2A.Services/Exported.cs"'
echo 'int y(void){ unsigned (*f)(void) = (unsigned (*)(void))dlsym(RTLD_DEFAULT, "S2A_TestExportOpen"); return f ? (int)f() : 0; }' >> "$R/android/native/caller.c"
o=$(run --check); r=$?
ck "安卓链上的 C 代码 dlsym 取导出名 → 算接线、不再红（前一场景的 late.c 还在，只看 Exported）" '! echo "$o" | grep "【红】" | grep -q Exported'
sed_edit '/S2A_TestExportOpen/d' "$R/android/native/caller.c"
# 同一个导出，安卓链上的 C 代码按链接期弱引用直接调（安卓单库里唯一取得到的方式：--exclude-libs,ALL 下 dlsym 恒为 NULL，
#   core-link 审计拦按名取隐藏导出）→ 也算接线；负对照：同样的调用放在只有苹果编的分支里 → 安卓侧不算，仍红
printf '%s\n' '#if defined(__APPLE__)' 'extern unsigned S2A_TestExportOpen(void) __attribute__((weak));' 'int z(void){ return S2A_TestExportOpen ? (int)S2A_TestExportOpen() : 0; }' '#endif' >> "$R/android/native/caller.c"
o=$(run --check); r=$?
ck "负对照：直接调用只在苹果分支里 → 安卓仍红（rc=${r}）" '[ $r != 0 ] && echo "$o" | grep "【红】" | grep -q Exported'
sed_edit '/S2A_TestExportOpen/d' "$R/android/native/caller.c"; sed_edit '/^#if defined(__APPLE__)$/d' "$R/android/native/caller.c"; sed_edit '/^#endif$/d' "$R/android/native/caller.c"
printf '%s\n' 'extern unsigned S2A_TestExportOpen(void) __attribute__((weak));' 'int z(void){ return S2A_TestExportOpen ? (int)S2A_TestExportOpen() : 0; }' >> "$R/android/native/caller.c"
o=$(run --check); r=$?
ck "安卓链上的 C 代码弱引用直接调导出 → 算接线、不再红" '! echo "$o" | grep "【红】" | grep -q Exported'
rm "$R/services/S2A.Services/Exported.cs"; sed_edit '/S2A_TestExportOpen/d' "$R/android/native/caller.c"

# 把 oldgap 接上：进 core 名单 + 安卓调用方 → 缺口收窄 → 基线条目变旧 → 【黄】但绿
sed_edit 's/set(core alpha)/set(core alpha oldgap)/' "$R/android/CMakeLists.txt"
echo 'extern int s2a_oldgap_kick(void); int a(void){ return s2a_oldgap_kick(); }' > "$R/android/native/oldgap_use.c"
rm "$R/runtime/src/late.c"
o=$(run --check); r=$?
ck "基线缺口接好后 → 绿且提示收紧（rc=${r}）" '[ $r = 0 ] && echo "$o" | grep -q "【黄】" && echo "$o" | grep -q oldgap'

# 负对照：把 gap() 阉割成永远返回空集 → --check 必须变绿；若仍红说明红与检测无关（常红 / 崩错），本单测必须红
cp "$H/tools/shared_matrix.py" "$T/shared_matrix_mut.py"
python3 - "$T/shared_matrix_mut.py" <<'PYEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); t = p.read_text()
old = "def gap(m):\n    g = set()"
assert old in t, "gap() 形状变了：负对照桩改不上，先修本单测"
p.write_text(t.replace(old, "def gap(m):\n    return set()  # 负对照桩\n    g = set()"))
PYEOF
o=$(cd "$R" && PYTHONPATH="$H/tools" python3 "$T/shared_matrix_mut.py" --root "$R" --check 2>&1); r=$?
ck "负对照：阉割 gap() 后 --check 应变绿（rc=${r}）" '[ $r = 0 ]'

# 只认提交内容（11-14：门禁机检出目录复用，未跟踪的生成文件曾让真仓 --check 时红时绿）：合成仓提交成 git 仓，
#   再丢一份未跟踪的生成 cmake（把 newmod 接进安卓）→ --check 输出必须与丢之前逐字相同
(cd "$R" && git init -q && git add -A && git -c user.name=t -c user.email=t@t commit -qm base) > /dev/null 2>&1
o1=$(run --check); mkdir -p "$R/android/.cxx"
printf 'set(core alpha oldgap newmod)\nforeach(x IN LISTS core)\n  list(APPEND SRCS runtime/src/${x}.c)\nendforeach()\n' > "$R/android/.cxx/gen.cmake"
echo 'extern int s2a_newmod_ping(void); int g(void){ return s2a_newmod_ping(); }' > "$R/android/native/gen_use.c"
o2=$(run --check)
ck "未跟踪的生成文件不改变 --check 结果" '[ "$o1" = "$o2" ]'

# ---------- 真仓门禁 ----------
o=$(cd "$H" && python3 tools/shared_matrix.py --check 2>&1); r=$?
ck "真仓 --check 与已提交基线一致（rc=${r}）" '[ $r = 0 ]'
# 红了把 --check 的【红】行原样打出来（11-14：只打「不通」一句，红行里没有肇事模块的路径，列车按单测读到的文件弱证据点名、点错了同批无关的支）
[ $r = 0 ] || grep '【红】' <<< "$o"

exit $bad
