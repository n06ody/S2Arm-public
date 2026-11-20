#!/usr/bin/env bash
# 存储前瞻索引的绘制级宿主用例（android/tests/rp_sdx_test.c）：真 rp_sdx.c / rp_pass.c / rp_draw.c / rp_exec.c 等，
# 后端用 t_nvn_replay.c 的替身，程序 peek 是测试文件的桩。
# 两组开关值各跑一个进程（main：nvk_sdx=1 缺省；off：nvk_sdx=0 索引不建），再在 ASan 下各跑一遍。
# 负对照（S2A_SDX_NEG=first_clear）：把「首个有效事件是整屏清除 → 可 DONTCARE」改成一律 STORE，期望正例用例变红。
# fd_no_mask：fd_cov 掩码消费取反 → 正反两头红；fd_vid_swap：vidgen 求值表查表分量轮转 → 序号正例证不出变红。无 GPU。
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cc="${CC:-cc}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
rp=graphics/nvn/replay
sdx="$repo/$rp/rp_sdx.c"
pass="$repo/$rp/rp_pass.c"
draw="$repo/$rp/rp_draw.c"
case "${S2A_SDX_NEG:-}" in
    '') ;;
    first_clear) python3 - "$sdx" "$tmp/rp_sdx.c" 'if (!hit) continue;
        return e->kind == SDX_CLEAR ? 2 : 2 << 2;' 'if (!hit) continue;
        return 2 << 2;' <<'PY'
import sys; src, dst, a, b = sys.argv[1:]; s = open(src).read()
if s.count(a) != 1: raise SystemExit('负对照锚点变了：' + a)
open(dst, 'w').write(s.replace(a, b))
PY
        sdx="$tmp/rp_sdx.c" ;;
    fd_no_mask) python3 - "$pass" "$tmp/rp_pass.c" 'd.color[i].load == NVN_GPU_LOAD_LOAD && (fd_cov >> i & 1)' 'd.color[i].load == NVN_GPU_LOAD_LOAD && !(fd_cov >> i & 1)' <<'PY'
import sys; src, dst, a, b = sys.argv[1:]; s = open(src).read()
if s.count(a) != 1: raise SystemExit('负对照锚点变了：' + a)
open(dst, 'w').write(s.replace(a, b))
PY
        pass="$tmp/rp_pass.c" ;;
    fd_vid_swap) python3 - "$draw" "$tmp/rp_draw.c" 'pos_vtab[(size_t)vid * 4 + c]' 'pos_vtab[(size_t)vid * 4 + ((c + 1) & 3)]' <<'PY'
import sys; src, dst, a, b = sys.argv[1:]; s = open(src).read()
if s.count(a) != 1: raise SystemExit('负对照锚点变了：' + a)
open(dst, 'w').write(s.replace(a, b))
PY
        draw="$tmp/rp_draw.c" ;;
    *) echo "未知负对照 $S2A_SDX_NEG" >&2; exit 2 ;;
esac
incs=(-I "$repo" -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/graphics/nvn/gfx/tex" -I "$repo/$rp" -I "$repo/android/native/nvn")
srcs=("$repo/android/tests/rp_sdx_test.c" "$repo/graphics/nvn/tests/nvn_clear_view_unused.c")
for f in rp_exec.c rp_copy_t2b.c rp_clear_tex.c rp_state.c rp_entry.c rp_res.c rp_buf.c rp_present.c nvn_unimpl.c; do srcs+=("$repo/$rp/$f"); done
srcs+=("$draw")
srcs+=("$pass")
srcs+=("$sdx" "$repo/graphics/nvn/gfx/nvn/nvn_tex.c" "$repo/graphics/nvn/gfx/tex/nvn_deswizzle.c" "$repo/graphics/nvn/gfx/nvn/nvn_first_use.c")
weak=(); [ "$(uname -s)" = Darwin ] && read -r -a weak <<< "$(cd "$repo" && python3 tools/weak_link_decl.py flags replay)"
flags=(-std=c11 -D_DEFAULT_SOURCE -DS2A_TEST_TS_ALLOC -O1 -g -pthread -Wall -Wextra -Werror)
"$cc" "${flags[@]}" "${incs[@]}" -o "$tmp/t" "${srcs[@]}" ${weak[@]+"${weak[@]}"} -lm
rc=0
(cd "$repo" && timeout 60 "$tmp/t" main) || rc=1
(cd "$repo" && timeout 60 "$tmp/t" off) || rc=1
(cd "$repo" && timeout 60 "$tmp/t" capsoff) || rc=1
if [ -z "${S2A_SDX_NEG:-}" ]; then
    "$cc" "${flags[@]}" -fsanitize=address -fno-omit-frame-pointer "${incs[@]}" -o "$tmp/ta" "${srcs[@]}" ${weak[@]+"${weak[@]}"} -lm
    leak=1; [ "$(uname -s)" = Darwin ] && leak=0
    (cd "$repo" && ASAN_OPTIONS=detect_leaks=$leak timeout 120 "$tmp/ta" main) || rc=1
    (cd "$repo" && ASAN_OPTIONS=detect_leaks=$leak timeout 120 "$tmp/ta" off) || rc=1
    (cd "$repo" && ASAN_OPTIONS=detect_leaks=$leak timeout 120 "$tmp/ta" capsoff) || rc=1
fi
[ "$rc" = 0 ] && echo "nvk_sdx: GREEN${S2A_SDX_NEG:+（负对照 ${S2A_SDX_NEG}）}" || echo "nvk_sdx: RED${S2A_SDX_NEG:+（负对照 ${S2A_SDX_NEG}）}"
exit $rc
