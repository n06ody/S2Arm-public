#!/usr/bin/env bash
# 首次使用准备下沉的三个纯逻辑模块（宿主，无 GPU）：
#   nvn_vk_prog_warm  跨会话程序清单：逐字段往返 / c1 合并不丢 / uses 去重 / 上限淘汰 / 坏文件整份拒绝 / 本局有效性过滤
#   nvn_vk_pipe_warm  跨会话管线配方：紧凑序列化 216 个字段变体 / 碰撞不去重 / 阶段位一致性 / 坏文件拒绝
#   nvn_vk_pipe_once  「同一配方只有一份」双检：4 线程并发只装一份、被丢弃的新建对象必须销毁
# 合同见 android/native/nvn/nvn_vk_{prog,pipe}_warm.h 与 nvn_vk_pipe_once.h。
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
android="$repo/android"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cc="${CC:-cc}"
"$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -pthread -D_GNU_SOURCE \
  -I "$android/native" -I "$android/native/nvn" -I "$android/native/direct" \
  -I "$repo/graphics/nvn/replay" -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/graphics/nvn/gfx/tex" \
  -o "$tmp/t_prog" "$android/tests/nvn_vk_prog_warm_test.c" "$android/native/nvn/nvn_vk_prog_warm.c" "$android/native/direct/sha256.c"
"$tmp/t_prog"
# 负对照（程序清单按闭包分文件、先比闭包，offline-shader-miss 修法）：各撤一处，t_prog 必须红在对应断言（编译失败不算红）
python3 - "$android/native/nvn/nvn_vk_prog_warm.c" "$tmp" <<'PY'
import pathlib, subprocess, sys
src, tmp = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
repo = src.parents[3]
text = src.read_text()
closure = '        if (memcmp(hdr_closure, closure_sha, 8)) { bad = "翻译器闭包不同（换了 APK / 编译器版本，清单是另一版翻译器记的）"; mismatch = 1; }\n'
profile = '        else if (memcmp(hdr_profile, profile_sha, 32)) { bad = "设备 profile 不同（同一版翻译器；设备能力 / 驱动 / 画质开关变了）"; mismatch = 1; }\n'
cases = [
    ('profile-first', [(closure, profile.replace('        else if', '        if')), (profile, closure.replace('        if', '        else if'))], 'closure is reported before profile'),
    ('one-file', [('"%s/" NVK_PROG_WARM_PREFIX "%s.bin", dir, hx', '"%s/programs-v1.bin%.0s", dir, hx')], 'manifest file exists after save'),   # 文件名回到一款一份：按闭包命名的那份根本不出现
    ('no-prune', [('    if (!rc) warm_prune(dir, path);', '    if (0) warm_prune(dir, path);')], 'the oldest (A) is pruned'),
    ('prune-keeps-legacy', [('    { char pth[NVK_PROG_WARM_MAX_PATH]; if (snprintf(pth, sizeof pth, "%s/%s", dir, NVK_PROG_WARM_FILE) < (int)sizeof pth) unlink(pth); }\n', '')], 'legacy-named file is removed'),
    ('no-legacy-fallback', [('    if (rf && e == ENOENT) {', '    if (0 && rf && e == ENOENT) {')], 'legacy-named manifest of the same closure is accepted'),
    ('mismatch-red', [('nvn_plat_log(mismatch ? NVN_PLAT_LOG_WARN : NVN_PLAT_LOG_RED, "warm", m);', 'nvn_plat_log(NVN_PLAT_LOG_RED, "warm", m);')], 'yellow line with its own reason'),
]
for name, edits, expect in cases:
    t = text
    for old, new in edits:
        assert t.count(old) == 1, (name, 'mutation anchor', old[:60])
        t = t.replace(old, new)
    m = tmp / f'prog_warm_{name}.c'; m.write_text(t)
    exe = tmp / f't_prog_{name}'
    inc = ['-I', str(repo/'android/native'), '-I', str(repo/'android/native/nvn'), '-I', str(repo/'android/native/direct'),
           '-I', str(repo/'graphics/nvn/replay'), '-I', str(repo/'graphics/nvn/gfx/nvn'), '-I', str(repo/'graphics/nvn/gfx/nvn/include')]
    subprocess.run([__import__('os').environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread', '-D_GNU_SOURCE', *inc, '-o', str(exe),
                    str(repo/'android/tests/nvn_vk_prog_warm_test.c'), str(m), str(repo/'android/native/direct/sha256.c')], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert r.returncode != 0 and expect in r.stderr, (name, r.stdout[-400:], r.stderr[-800:])
    print(f'prog_warm 负对照 {name}：红在「{expect}」')
print(f'PASS: 程序清单负对照 {len(cases)} 项全部变红')
PY
"$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -pthread -D_GNU_SOURCE \
  -I "$android/native" -I "$android/native/nvn" -I "$android/native/direct" \
  -I "$repo/graphics/nvn/replay" -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/graphics/nvn/gfx/tex" \
  -o "$tmp/t_pipe" "$android/tests/nvn_vk_pipe_warm_test.c" "$android/native/nvn/nvn_vk_pipe_warm.c"
"$tmp/t_pipe"
"$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -pthread -D_GNU_SOURCE \
  -I "$android/native/nvn" -o "$tmp/t_once" \
  "$android/tests/nvn_vk_pipe_once_test.c" "$android/native/nvn/nvn_vk_pipe_once.c"
"$tmp/t_once"
