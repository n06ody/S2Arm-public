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
    ('no-legacy-fallback', [('    if (!capture && rf && e == ENOENT) {', '    if (0 && rf && e == ENOENT) {')], 'legacy-named manifest of the same closure is accepted'),
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
# 负对照（管线清单满表淘汰规则：先挑本轮没碰过的旧条目）：各改被测实现一处，整套用例必须红在对应的断言上（编译失败不算红）
python3 - "$repo" "$tmp" <<'PY'
import os, pathlib, re, subprocess, sys
repo, tmp = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
src, test = repo/'android/native/nvn/nvn_vk_pipe_warm.c', repo/'android/tests/nvn_vk_pipe_warm_test.c'
text = src.read_text()
inc = []
for d in ('android/native', 'android/native/nvn', 'android/native/direct', 'graphics/nvn/replay', 'graphics/nvn/gfx/nvn', 'graphics/nvn/gfx/nvn/include', 'graphics/nvn/gfx/tex'):
    inc += ['-I', str(repo/d)]
INS = '    g.fresh[slot] = 1;   // 新记进来的算本轮碰过：旧条目挤光之前不会被下一条新配方挤掉\n'
TCH = '    g.fresh[slot] = 1;   // 命中（含 memo / 桶链 / 标签快路径）也算本轮碰过\n'
LOAD = '    memset(g.fresh, 0, g.cap);   // 载入进来的都是旧条目：一次都还没被本轮碰过\n'
RESET = '    memset(g.fresh, 0, g.cap);   // 全表都碰过了：新一轮，下一次起重新分「碰过 / 没碰过」\n    g.st.epochs++;\n'
SEL = '        uint32_t *best = g.fresh[i] ? &hot : &cold;\n'
CMP = '        if (*best == UINT32_MAX || g.uses[i] < g.uses[*best] || (g.uses[i] == g.uses[*best] && g.seen[i] < g.seen[*best])) *best = i;\n'
TAG = '        pw_touch_locked(slot1 - 1);\n        g.st.tag_hits++;\n'
# (名字, 改动, 必须全部出现的断言片段)——产品里只有一条规则；满表淘汰以前的老规则 = 改坏项 no-fresh-preference
cases = [
    ('new-not-fresh', [(INS, '')], ['aging thrash:']),                                  # 新记进来不算碰过：新配方又会互相挤
    ('hit-not-fresh', [(TCH, '')], ['aging touch:']),                                    # 命中不算碰过：刚被命中的旧条目照样被挤
    ('load-keeps-fresh', [(LOAD, '')], ['aging thrash:']),                               # 载入不清标记：旧条目一进来就当碰过
    ('no-new-round', [(RESET, '    g.st.epochs++;\n')], ['aging epoch:']),              # 全表碰过时不开新一轮：工作集换批后新配方互相挤
    ('no-epoch-count', [(RESET, '    memset(g.fresh, 0, g.cap);\n')], ['epochs want']),  # 开了新一轮但不计数
    ('select-inverted', [(SEL, '        uint32_t *best = !g.fresh[i] ? &hot : &cold;\n')], ['aging thrash:']),   # 先挑碰过的
    ('no-fresh-preference', [(SEL, '        uint32_t *best = &cold;\n')],                                         # 去掉「先挑没碰过的」= 老规则：新配方被互相挤掉
     ['aging thrash: each of the', 'aging thrash: new recipe', 'aging touch:', 'aging epoch:', 'note/memo/chain want 23/14/0']),
    ('uses-inverted', [(CMP, '        if (*best == UINT32_MAX || g.uses[i] > g.uses[*best] || (g.uses[i] == g.uses[*best] && g.seen[i] < g.seen[*best])) *best = i;\n')],
     ['aging thrash:']),                                                                  # 同一类里挑 uses 最大的
    ('seen-only', [(CMP, '        if (*best == UINT32_MAX || g.seen[i] < g.seen[*best]) *best = i;\n')], ['aging epoch:']),    # 只看最近序号，不看 uses
    ('tag-touch-not-fresh', [(TAG, '        g.uses[slot1 - 1]++; g.seen[slot1 - 1] = (uint32_t)++g.clock; g.recs[slot1 - 1].uses = g.uses[slot1 - 1]; g.dirty = 1;\n        g.st.tag_hits++;\n')], ['不同']),   # 标签快路径不标碰过
]
for name, edits, expects in cases:
    t = text
    for old, new in edits:
        assert t.count(old) == 1, (name, 'mutation anchor', old[:70])
        t = t.replace(old, new)
    m = tmp / f'pipe_warm_{name}.c'; m.write_text(t)
    exe = tmp / f't_pipe_{name}'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread', '-D_GNU_SOURCE', *inc, '-o', str(exe), str(test), str(m)], check=True)
    d = tmp / f'pw_dir_{name}'; d.mkdir()
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300, env=dict(os.environ, S2A_PW_DIR=str(d)))
    out = r.stdout + r.stderr
    assert r.returncode != 0 and all(e in out for e in expects), (name, [e for e in expects if e not in out], out[-800:])
    fails = []   # 红在测试文件的哪几行（去重，取前 3 条）
    for line_no, text_ in re.findall(r'FAIL \S*nvn_vk_pipe_warm_test\.c:(\d+): ([^\n]{0,64})', out):
        if line_no not in [f[0] for f in fails]: fails.append((line_no, text_))
    hit = next((l.strip()[:90] for l in out.splitlines() if expects[0] in l), expects[0])
    where = '；'.join(f'nvn_vk_pipe_warm_test.c:{n}「{t_}」' for n, t_ in fails[:3]) + f'（共 {len(fails)} 处）' if fails else f'「{hit}」'
    print(f'管线清单淘汰规则 负对照 {name}：红在 {where}')
print(f'PASS: 管线清单淘汰规则负对照 {len(cases)} 项全部变红')
PY
"$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -pthread -D_GNU_SOURCE \
  -I "$android/native/nvn" -o "$tmp/t_once" \
  "$android/tests/nvn_vk_pipe_once_test.c" "$android/native/nvn/nvn_vk_pipe_once.c"
"$tmp/t_once"
# 计算管线现建慢行文本（带阶段键与出库标记）+ 负控：出库 / 现编标记取反必须红
"$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -pthread -D_GNU_SOURCE \
  -I "$android/native/nvn" -o "$tmp/t_diag" "$android/tests/nvn_vk_pipe_diag_test.c"
"$tmp/t_diag"
python3 - "$repo" "$tmp" <<'PY'
import os, pathlib, subprocess, sys
repo, tmp = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
hdr = (repo/'android/native/nvn/nvn_vk_pipe_diag.h').read_text()
old = 'lib ? " 出库 摘要'
assert hdr.count(old) == 1, 'diag mutation anchor'
mut = tmp/'diag_mutant'; mut.mkdir(exist_ok=True)
(mut/'nvn_vk_pipe_diag.h').write_text(hdr.replace(old, '!lib ? " 出库 摘要'))
exe = mut/'t_diag'
subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(mut), '-o', str(exe), str(repo/'android/tests/nvn_vk_pipe_diag_test.c')], check=True)
r = subprocess.run([str(exe)], capture_output=True, text=True)
assert r.returncode != 0 and 'FAIL' in r.stderr and 'live line' in r.stderr, r.stderr
print('计算管线慢行文本 负对照 lib-flag-inverted：红在 live line')
PY
# 首局预热随包种子（nvn_vk_{prog,pipe}_warm.h「随包种子」）：两个模块一起链，本机清单为空读种子 / 本机优先 / 坏种子打黄不用 / 坏条目逐条跳过 /
#   程序没定时管线稍后重试 / 种子配方逐字段对上手写字节
"$cc" -std=gnu11 -O1 -Wall -Wextra -Werror -pthread -D_GNU_SOURCE \
  -I "$android/native" -I "$android/native/nvn" -I "$android/native/direct" \
  -I "$repo/graphics/nvn/replay" -I "$repo/graphics/nvn/gfx/nvn" -I "$repo/graphics/nvn/gfx/nvn/include" -I "$repo/graphics/nvn/gfx/tex" \
  -o "$tmp/t_seed" "$android/tests/nvn_vk_warm_seed_test.c" "$android/native/nvn/nvn_vk_prog_warm.c" "$android/native/nvn/nvn_vk_pipe_warm.c" "$android/native/direct/program_cache.c" "$android/native/direct/sha256.c"
"$tmp/t_seed"
# Run the actual installer first so the reader sees the same relative object links as Android.
mkdir -p "$tmp/java" "$tmp/games" "$tmp/cache"
"$tmp/t_seed" --emit "$tmp"
javac -encoding UTF-8 -sourcepath "$android/app/src/main/java" -d "$tmp/java" "$android/tests/GameInstallTest.java"
java -Djava.io.tmpdir="$tmp" -cp "$tmp/java" org.s2arm.android.GameInstallTest --install-warm-seeds \
  "$tmp/input/rev" "$tmp/games" "$tmp/revision"
revision="$(cat "$tmp/revision")"
# Invalid links must never turn identical valid fixture bytes into a seed.
python3 - "$tmp" "$revision" <<'VERIFY'
import os, pathlib, subprocess, sys
from contextlib import contextmanager
tmp, revision = map(pathlib.Path, sys.argv[1:])
program, = revision.glob('shaders/direct/warm-vk/*/programs-*.bin')
pipe = program.with_name('pipes-v1-352.bin')
def check(programs, pipes):
    subprocess.run([tmp/'t_seed', '--installed', revision, tmp/'cache', str(programs), str(pipes)], check=True, timeout=10)
    print(f'loaded programs={programs} pipes={pipes}', flush=True)

@contextmanager
def saved_entry(path):
    # Move the real installer entry aside; never modify the object through its revision link.
    saved = tmp/'original-entry'
    path.rename(saved)
    try:
        yield
    finally:
        if path.is_dir() and not path.is_symlink():
            path.rmdir()
        elif path.is_symlink() or path.exists():
            path.unlink()
        saved.rename(path)

def rejected(case):
    print(f'negative: {case}', flush=True)
    check(0, 0)

original = os.readlink(program)
obj = program.resolve(strict=True)
payload = obj.read_bytes()
bucket, digest = obj.parent.name, obj.name
wrong_bucket = '00' if bucket != '00' else '01'
# Change one letter after the bucket prefix so only the lowercase requirement is violated.
i = next(i for i, char in enumerate(digest) if i >= 2 and char in 'abcdef')
mixed_case = digest[:i] + digest[i].upper() + digest[i + 1:]
for case, target in [
    ('bucket mismatch', original.rsplit('/', 2)[0] + '/' + wrong_bucket + '/' + digest),
    ('uppercase digest', original.rsplit('/', 1)[0] + '/' + mixed_case),
    ('one extra parent', '../' + original),
    ('one missing parent', original.removeprefix('../')),
]:
    # Put valid manifest bytes at the alternative destination, so a missing target
    # cannot produce a false green. On case-insensitive volumes the mixed name already exists.
    alternate = pathlib.Path(os.path.normpath(program.parent / target))
    alternate.parent.mkdir(parents=True, exist_ok=True)
    if not alternate.exists():
        alternate.write_bytes(payload)
    assert alternate.read_bytes() == payload
    with saved_entry(program):
        program.symlink_to(target)
        rejected(case)

for kind in ('directory', 'FIFO'):
    with saved_entry(obj):
        if kind == 'directory':
            obj.mkdir()
        else:
            os.mkfifo(obj)
        rejected('object is ' + kind)

for directory in (obj.parent.parent, obj.parent):
    with saved_entry(directory):
        directory.symlink_to(os.path.relpath(tmp/'original-entry', directory.parent), target_is_directory=True)
        assert program.read_bytes() == payload
        rejected('aliased directory ' + directory.name)

print('positive: restored installer leaves', flush=True)
check(2, 2)
for leaf, counts in [(program, (0, 0)), (pipe, (2, 0))]:
    original = os.readlink(leaf)
    copy = tmp/leaf.name
    copy.write_bytes(leaf.read_bytes())
    for target in [os.path.relpath(copy, leaf.parent), leaf.name, str(leaf.resolve())]:
        leaf.unlink()
        leaf.symlink_to(target)
        check(*counts)
        leaf.unlink()
        leaf.symlink_to(original)
cohort = program.parent
saved = cohort.with_name('saved')
cohort.rename(saved)
cohort.symlink_to(saved.name, target_is_directory=True)
check(0, 0)
print('PASS: installer leaves accepted; external, cyclic, absolute and directory links rejected')
VERIFY
# nowindow/ 补件走同一套受控读取：程序结果（s2a_program_cache_load_installed）与管线配方（pw_read_file installed=1）。
# 安装器放下的叶子要能载入（跨资源根换包后补件仍可用）；库外链接 / 环 / 绝对路径 / 目录别名 / 桶不符一律拒；
# 然后各撤一处（受控读取改回普通 open / 管线读退回普通 open），整套必须红在对应用例（编译失败不算红）。
python3 - "$repo" "$tmp" "$revision" <<'NOWINDOW'
import os, pathlib, subprocess, sys
from contextlib import contextmanager
repo, tmp, revision = map(pathlib.Path, sys.argv[1:])
warm_vk = revision/'shaders/direct/warm-vk'
for entry in list(warm_vk.iterdir()):   # 上一段把 cohort 目录换成了别名链接（真目录改名 saved）：还原后再测补件
    if entry.is_symlink():
        entry.unlink()
        (warm_vk/'saved').rename(entry)
seed_dir, = warm_vk.iterdir()
nw = seed_dir/'nowindow'
nw_prog, = nw.glob('*.program')
nw_pipe = nw/'pipes-v1-352.bin'
assert nw_prog.is_symlink() and nw_pipe.is_symlink(), 'installer must publish nowindow leaves as object links'

class Red(Exception):
    pass

def check(exe, case, progs, pipes, results):
    r = subprocess.run([str(exe), '--installed-nowindow', str(revision), str(tmp/'cache'), str(progs), str(pipes), str(results)],
                       capture_output=True, text=True, timeout=20)
    if r.returncode != 0:
        raise Red(f'{case}: {r.stderr.strip()[-300:]}')

@contextmanager
def replaced(path, make):
    saved = path.with_name(path.name + '.saved')
    path.rename(saved)
    try:
        make()
        yield
    finally:
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            path.rmdir()
        saved.rename(path)

def scenario(exe):
    check(exe, 'installer leaves', 2, 1, 1)      # 安装器放下的补件：程序结果载入、替换管线清单（1 条，不是上一级的 2 条）
    for leaf, expect in ((nw_prog, (2, 1, 0)), (nw_pipe, (2, 0, 1))):
        original = os.readlink(leaf)
        copy = tmp/('copy-' + leaf.name)
        copy.write_bytes(leaf.read_bytes())
        digest = pathlib.Path(original).name
        wrong = '00' if pathlib.Path(original).parent.name != '00' else '01'
        for case, target in [('external relative link', os.path.relpath(copy, leaf.parent)),
                             ('cyclic link', leaf.name),
                             ('absolute link', str(copy)),
                             ('bucket mismatch', original.rsplit('/', 2)[0] + '/' + wrong + '/' + digest)]:
            with replaced(leaf, lambda t=target: leaf.symlink_to(t)):
                check(exe, f'{leaf.name[-12:]} {case}', *expect)
    saved = nw.with_name('nowindow.saved')
    with replaced(nw, lambda: nw.symlink_to(saved.name, target_is_directory=True)):
        check(exe, 'aliased nowindow directory', 2, 0, 0)
    check(exe, 'restored leaves', 2, 1, 1)

scenario(tmp/'t_seed')
print('PASS: nowindow installer leaves accepted; external, cyclic, absolute, bucket and directory aliases rejected', flush=True)

prog_cache, pipe_warm = repo/'android/native/direct/program_cache.c', repo/'android/native/nvn/nvn_vk_pipe_warm.c'
cases = [   # (名字, 被改文件, 改动, 必须出现在红因里的片段)
    ('program-plain-open', prog_cache, [('int fd = installed ? s2a_installed_seed_open(path) : open(path, O_RDONLY | O_CLOEXEC);', 'int fd = installed ? open(path, O_RDONLY | O_CLOEXEC) : open(path, O_RDONLY | O_CLOEXEC);')],
     'program result: expected rejected'),     # 程序结果改回普通 open：库外 / 绝对链接照收
    ('pipe-plain-read', pipe_warm, [('int rc = pw_read_file(seed_path, 1, &buf, &bytes, &se);', 'int rc = pw_read_file(seed_path, 0, &buf, &bytes, &se);')],
     'installer leaves'),                       # 管线读退回普通 open：碰到安装器相对链接的 ELOOP，补件整份读不到
    ('pipe-follows-links', pipe_warm, [('int rc = pw_read_file(seed_path, 1, &buf, &bytes, &se);', 'int rc = pw_read_file(seed_path, 0, &buf, &bytes, &se);'),
                                       ('O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);\n    if (fd < 0) { if (errno == ENOENT)', 'O_RDONLY | O_CLOEXEC | O_NONBLOCK);\n    if (fd < 0) { if (errno == ENOENT)')],
     'recipes: expected'),                      # 普通 open 且跟随任意链接：安装器叶子能读，库外 / 绝对链接也照收
]
inc = []
for d in ('android/native', 'android/native/nvn', 'android/native/direct', 'graphics/nvn/replay', 'graphics/nvn/gfx/nvn', 'graphics/nvn/gfx/nvn/include', 'graphics/nvn/gfx/tex'):
    inc += ['-I', str(repo/d)]
for name, src, edits, expect in cases:
    text = src.read_text()
    for old, new in edits:
        assert text.count(old) == 1, (name, 'mutation anchor', old[:70])
        text = text.replace(old, new)
    m = tmp/f'nowindow_{name}_{src.name}'; m.write_text(text)
    srcs = [str(m) if s == src else str(s) for s in (repo/'android/native/nvn/nvn_vk_prog_warm.c', pipe_warm, prog_cache)]
    exe = tmp/f't_seed_nowindow_{name}'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread', '-D_GNU_SOURCE', *inc, '-o', str(exe),
                    str(repo/'android/tests/nvn_vk_warm_seed_test.c'), *srcs, str(repo/'android/native/direct/sha256.c')], check=True)
    try:
        scenario(exe)
    except Red as red:
        assert expect in str(red), (name, expect, str(red))
        print(f'nowindow 负对照 {name}：红在「{red}」'[:420])
    else:
        raise SystemExit(f'nowindow mutant {name} stayed green')
print(f'PASS: nowindow 受控读取负对照 {len(cases)} 项全部变红')
NOWINDOW
# 负对照：各撤一处，t_seed 必须红在对应断言（编译失败不算红）
python3 - "$repo" "$tmp" <<'PY'
import os, pathlib, subprocess, sys
repo, tmp = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
prog, pipe = repo/'android/native/nvn/nvn_vk_prog_warm.c', repo/'android/native/nvn/nvn_vk_pipe_warm.c'
cases = [
    ('seed-even-with-local', prog, [('    if (!capture && rf && e == ENOENT && !seed_dir_of(seed_dir', '    if (!capture && !seed_dir_of(seed_dir')], '有本机程序清单：用本机的 1 条'),   # 不判本机清单是否为空
    ('desc-field-swap', pipe, [('    d->dfmt = pw_get32(b, &at);\n    d->sfmt = pw_get32(b, &at);', '    d->sfmt = pw_get32(b, &at);\n    d->dfmt = pw_get32(b, &at);')], 'dfmt 0x3e / sfmt 0 / samples 4'),   # 配方映射错一个字段
    ('bad-row-kept', prog, [('        if (!rec_well_formed(&s.rec)) { filtered++; continue; }', '')], '坏条目跳过：3 条收 2 条'),   # 坏行不跳过
    ('pipe-row-unchecked', pipe, [('        if (from_seed && !(nvk_prog_warm_contains && nvk_prog_warm_contains(e))) { dropped++; continue; }', '')], '管线清单从种子收下 2 条'),   # 种子里程序不在的管线条目不跳过
    ('pipe-no-wait', pipe, [('        if (s < 0) return -1;', '')], '程序清单还没载入：管线 load 返回 -1 稍后重试'),   # 程序没定就当没有种子
]
inc = []
for d in ('android/native', 'android/native/nvn', 'android/native/direct', 'graphics/nvn/replay', 'graphics/nvn/gfx/nvn', 'graphics/nvn/gfx/nvn/include', 'graphics/nvn/gfx/tex'):
    inc += ['-I', str(repo/d)]
for name, src, edits, expect in cases:
    t = src.read_text()
    for old, new in edits:
        assert t.count(old) == 1, (name, 'mutation anchor', old[:60])
        t = t.replace(old, new)
    m = tmp / f'seed_{name}_{src.name}'; m.write_text(t)
    others = [str(m) if s == src else str(s) for s in (prog, pipe)]
    exe = tmp / f't_seed_{name}'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread', '-D_GNU_SOURCE', *inc, '-o', str(exe),
                    str(repo/'android/tests/nvn_vk_warm_seed_test.c'), *others, str(repo/'android/native/direct/program_cache.c'), str(repo/'android/native/direct/sha256.c')], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert r.returncode != 0 and expect in r.stderr, (name, r.stdout[-400:], r.stderr[-800:])
    print(f'随包种子 负对照 {name}：红在「{expect}」')
print(f'PASS: 随包种子负对照 {len(cases)} 项全部变红')
PY
