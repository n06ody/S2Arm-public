#!/bin/bash
# 门禁项：「上游 + 补丁 = 树内源码」机械核对（方案 §2 铁律 3、§5b-6）。
#   上游   = compat/UPSTREAM 记录的对象号（git mktree 复原上游子集根树，不依赖网络）
#   补丁   = compat/patches/series 依序 git apply（不许模糊匹配）
#   树内   = compat/tree
# 另核：patches/ 下每个 .patch 都在 series 里、series 里每条都有文件、每条在 compat/改动清单.md 有一行。
# 用法：verify.sh                核工作区（含未提交改动，开发自查）
#       verify.sh --rev <提交>   核某提交（门禁用：HEAD 或待合入提交）
#       verify.sh --trees        只打印「上游+补丁树 树内树」两个树对象号（patch_new.sh 用），不判
# 退出码：0 一致；1 不一致（打印差异文件）；2 用法 / 环境错。
set -euo pipefail
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel); cd "$ROOT"
REV=""; TREES=0
case "${1:-}" in
    --trees) TREES=1 ;;
    --rev) REV=${2:?"--rev 要给提交"}; git rev-parse -q --verify "$REV^{commit}" >/dev/null || { echo "无此提交：$REV" >&2; exit 2; } ;;
    "") ;;
    *) echo "用法：verify.sh [--rev <提交>]" >&2; exit 2 ;;
esac
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ryuverify.XXXXXX"); trap 'rm -rf "$TMP"' EXIT

# 读文件：工作区或某提交
rd() { if [ -n "$REV" ]; then git show "$REV:$1" 2>/dev/null; else cat "$1" 2>/dev/null; fi; }
ls_patches() {
    if [ -n "$REV" ]; then git ls-tree --name-only "$REV" compat/patches/ 2>/dev/null | sed 's|.*/||' | grep '\.patch$' || true
    else ls compat/patches 2>/dev/null | grep '\.patch$' || true; fi
}

# 1. 复原上游子集根树
rd compat/UPSTREAM > "$TMP/UPSTREAM" || { echo "缺 compat/UPSTREAM" >&2; exit 2; }
COMMIT=$(awk -F'\t' '$1=="commit"{print $2}' "$TMP/UPSTREAM")
: > "$TMP/root.mk"; : > "$TMP/src.mk"
while IFS=$'\t' read -r k p id mode; do
    [ "$k" = path ] || continue
    [ -n "$mode" ] || { echo "UPSTREAM 的 path 行缺模式列（${p}）：用新版 import_upstream.sh 重新生成" >&2; exit 2; }
    git cat-file -e "$id" 2>/dev/null || { echo "仓库里没有上游对象 ${id}（${p}）：先跑 compat/fetch_base.sh 取上游原样对象" >&2; exit 2; }
    type=blob; [ "$mode" = 040000 ] && type=tree
    case "$p" in
        src/*/*) echo "不支持的导入路径深度：$p" >&2; exit 2 ;;
        src/*) printf '%s %s %s\t%s\n' "$mode" "$type" "$id" "${p#src/}" >> "$TMP/src.mk" ;;
        */*) echo "不支持的导入路径：$p" >&2; exit 2 ;;
        *) printf '%s %s %s\t%s\n' "$mode" "$type" "$id" "$p" >> "$TMP/root.mk" ;;
    esac
done < "$TMP/UPSTREAM"
SRC_TREE=$(git mktree < "$TMP/src.mk")
printf '040000 tree %s\tsrc\n' "$SRC_TREE" >> "$TMP/root.mk"
PRISTINE=$(git mktree < "$TMP/root.mk")

# 2. 补丁序列与清单的一致性
rd compat/patches/series | sed 's/#.*//; s/[[:space:]]*$//' | grep -v '^$' > "$TMP/series" || true
rd compat/改动清单.md > "$TMP/list" || : > "$TMP/list"
ls_patches | LC_ALL=C sort > "$TMP/files"
LC_ALL=C sort "$TMP/series" > "$TMP/series.sorted"
err=0
if [ "$(uniq -d "$TMP/series.sorted")" != "" ]; then echo "series 里有重复条目：$(uniq -d "$TMP/series.sorted" | tr '\n' ' ')"; err=1; fi
for f in $(LC_ALL=C comm -23 "$TMP/files" "$TMP/series.sorted"); do echo "补丁文件不在 series 里：$f"; err=1; done
for f in $(LC_ALL=C comm -13 "$TMP/files" "$TMP/series.sorted"); do echo "series 列了但没有文件：$f"; err=1; done
while read -r f; do grep -qF "$f" "$TMP/list" || { echo "改动清单缺这一条：$f"; err=1; }; done < "$TMP/series"

# 3. 上游 + 补丁
export GIT_INDEX_FILE="$TMP/expect.idx"
git read-tree "$PRISTINE"
n=0
while read -r f; do
    n=$((n+1))
    rd "compat/patches/$f" > "$TMP/p" || { err=1; continue; }
    if ! git apply --cached -p1 --whitespace=nowarn "$TMP/p" 2> "$TMP/apply.err"; then
        echo "补丁打不上：$f"; sed 's/^/    /' "$TMP/apply.err"; err=1
    fi
done < "$TMP/series"
EXPECT=$(git write-tree)

# 4. 树内源码
if [ -n "$REV" ]; then
    CUR=$(git rev-parse "$REV:compat/tree")
else
    # 以真索引为底（已跟踪文件即使匹配 .gitignore 也保留），再收工作区的增删改
    unset GIT_INDEX_FILE
    REAL_IDX=$(git rev-parse --path-format=absolute --git-path index)
    export GIT_INDEX_FILE="$TMP/cur.idx"
    cp "$REAL_IDX" "$GIT_INDEX_FILE"
    git add -A -- compat/tree
    CUR=$(git write-tree --prefix=compat/tree/)
fi
unset GIT_INDEX_FILE
if [ $TREES -eq 1 ]; then echo "$EXPECT $CUR"; exit $err; fi

if [ "$EXPECT" = "$CUR" ] && [ $err -eq 0 ]; then
    echo "兼容层核对：一致（上游 ${COMMIT:0:12} + $n 个补丁 = 树内源码${REV:+，提交 $(git rev-parse --short "$REV")}）"
    exit 0
fi
if [ "$EXPECT" != "$CUR" ]; then
    echo "兼容层核对：树内源码 ≠ 上游 + 补丁。差异文件（A = 树内多出 / D = 树内缺 / M = 内容不同）："
    git diff-tree -r --name-status "$EXPECT" "$CUR" | sed 's/^/    /' | head -50
    echo "  改了 compat/tree 就要用 compat/patch_new.sh 生成补丁并在改动清单写理由。"
fi
exit 1
