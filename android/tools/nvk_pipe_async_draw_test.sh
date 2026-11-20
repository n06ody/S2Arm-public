#!/usr/bin/env bash
# 新回放层首用管线异步的绘制级宿主用例（android/tests/nvk_pipe_async_draw_test.c）：
# 真 rp_draw.c / rp_pass.c + 真 nvn_vk_pipe_demand.c / nvn_pipe_pend.c / nvn_first_use.c，后端用 t_nvn_replay.c 的替身、建管线换成可控闸。
# 两组开关值各跑一个进程（main：item 30 / frame 100 / unsafe 300 ms；unbounded：unsafe 0 = 不设限），再在 ASan 下各跑一遍。
# 负对照（S2A_PA_NEG=skip_safe|budget|epoch）：把对应一处改成典型错误写法后编，期望红。无 GPU。
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cc="${CC:-cc}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
rp=graphics/nvn/replay
draw="$repo/$rp/rp_draw.c"; demand="$repo/android/native/nvn/nvn_vk_pipe_demand.c"
case "${S2A_PA_NEG:-}" in
    '') ;;
    skip_safe) python3 - "$draw" "$tmp/rp_draw.c" 'const int may_skip = rp_fu_skip_safe(rt);' 'const int may_skip = (rp_fu_skip_safe(rt), 1);' <<'PY'
import sys; src, dst, a, b = sys.argv[1:]; s = open(src).read()
if s.count(a) != 1: raise SystemExit('负对照锚点变了：' + a)
open(dst, 'w').write(s.replace(a, b))
PY
        draw="$tmp/rp_draw.c" ;;
    budget) python3 - "$draw" "$tmp/rp_draw.c" 'if (may_skip) s2a_fu_budget_spend(&g_fu_budget, frame, waited);' '(void)waited;' <<'PY'
import sys; src, dst, a, b = sys.argv[1:]; s = open(src).read()
if s.count(a) < 1: raise SystemExit('负对照锚点变了：' + a)
open(dst, 'w').write(s.replace(a, b))
PY
        draw="$tmp/rp_draw.c" ;;
    epoch) python3 - "$demand" "$tmp/nvn_vk_pipe_demand.c" 'static int dem_gen_live(uint64_t gen) { return atomic_load_explicit(&g_gen, memory_order_acquire) == gen; }' 'static int dem_gen_live(uint64_t gen) { (void)gen; return 1; }' <<'PY'
import sys; src, dst, a, b = sys.argv[1:]; s = open(src).read()
if s.count(a) != 1: raise SystemExit('负对照锚点变了：' + a)
open(dst, 'w').write(s.replace(a, b))
PY
        demand="$tmp/nvn_vk_pipe_demand.c" ;;
    *) echo "未知负对照 $S2A_PA_NEG" >&2; exit 2 ;;
esac
incs=(-I "$repo" -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/graphics/nvn/gfx/tex" -I "$repo/$rp" -I "$repo/android/native/nvn")
srcs=("$repo/android/tests/nvk_pipe_async_draw_test.c" "$repo/graphics/nvn/tests/nvn_clear_view_unused.c")
for f in rp_exec.c rp_copy_t2b.c rp_clear_tex.c rp_state.c rp_entry.c rp_res.c rp_buf.c rp_pass.c rp_present.c rp_sdx.c nvn_unimpl.c; do srcs+=("$repo/$rp/$f"); done
srcs+=("$draw" "$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c" "$demand" "$repo/android/native/nvn/nvn_pipe_pend.c" "$repo/graphics/nvn/gfx/nvn/nvn_first_use.c")
weak=(); [ "$(uname -s)" = Darwin ] && read -r -a weak <<< "$(cd "$repo" && python3 tools/weak_link_decl.py flags replay)"
flags=(-std=c11 -D_DEFAULT_SOURCE -DS2A_TEST_TS_ALLOC -O1 -g -pthread -Wall -Wextra -Werror)
"$cc" "${flags[@]}" "${incs[@]}" -o "$tmp/t" "${srcs[@]}" ${weak[@]+"${weak[@]}"} -lm
rc=0
(cd "$repo" && timeout 60 "$tmp/t" main) || rc=1
(cd "$repo" && timeout 60 "$tmp/t" unbounded) || rc=1
if [ -z "${S2A_PA_NEG:-}" ]; then
    "$cc" "${flags[@]}" -fsanitize=address -fno-omit-frame-pointer "${incs[@]}" -o "$tmp/ta" "${srcs[@]}" ${weak[@]+"${weak[@]}"} -lm
    leak=1; [ "$(uname -s)" = Darwin ] && leak=0
    (cd "$repo" && ASAN_OPTIONS=detect_leaks=$leak timeout 120 "$tmp/ta" main) || rc=1
    (cd "$repo" && ASAN_OPTIONS=detect_leaks=$leak timeout 120 "$tmp/ta" unbounded) || rc=1
fi
[ "$rc" = 0 ] && echo "nvk_pipe_async_draw: GREEN${S2A_PA_NEG:+（负对照 ${S2A_PA_NEG}）}" || echo "nvk_pipe_async_draw: RED${S2A_PA_NEG:+（负对照 ${S2A_PA_NEG}）}"
exit $rc
