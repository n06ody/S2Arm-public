# Android NDK 宿主工具链解析：直连探针脚本共用。
# 以前每个脚本都写死 toolchains/llvm/prebuilt/linux-x86_64；换到 macOS 会直接找不到编译器。
# 先认 $ANDROID_HOME / $ANDROID_SDK_ROOT / 常见 SDK 目录下的固定版本，没有再认
# ANDROID_NDK_HOME / ANDROID_NDK / ANDROID_NDK_ROOT，最后 /opt/android-ndk；再按宿主实际存在的
# prebuilt 目录选（darwin-arm64 / darwin-x86_64 / linux-x86_64），
# 找不到就返回非零，由调用方报红退出，不猜路径。
#
# 用法：
#   . "$(dirname "$0")/ndk_toolchain.sh"
#   ndk=$(s2a_ndk_resolve) || { echo "no NDK" >&2; exit 2; }
#   cc=$(s2a_ndk_clang "$ndk" 29) || { echo "no cc" >&2; exit 2; }
#   cxx=$(s2a_ndk_clangxx "$ndk" 29)
#   glslc=$(s2a_ndk_glslc "$ndk")

# 直接执行不会定义任何函数、什么都不做还 rc=0，调用方会把「没编」当成「编过」（坑记录 11-20）：
# 检测到非 source 调用就报错退出，不留假绿出口。
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    echo "ndk_toolchain.sh 只能 source 用：. \"$0\" 后调 s2a_ndk_resolve / s2a_ndk_clang / s2a_ndk_glslc" >&2
    exit 2
fi

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/versions.sh"

s2a_ndk_resolve() {
    # 钉版目录优先，环境变量兜底：托管 macOS 镜像的 ANDROID_NDK_HOME 指向自带的旧 NDK（27.x），
    # 先认它会在钉版已装的机器上用错编译器（NDK 29 才认的告警选项当场 -Werror 失败）。
    local ndk= candidate
    for candidate in \
        "${ANDROID_HOME:-/nonexistent}/ndk/$S2A_ANDROID_NDK" \
        "${ANDROID_SDK_ROOT:-/nonexistent}/ndk/$S2A_ANDROID_NDK" \
        "$HOME/Android/Sdk/ndk/$S2A_ANDROID_NDK" \
        "$HOME/Library/Android/sdk/ndk/$S2A_ANDROID_NDK"; do
        [ -d "$candidate" ] && { ndk=$candidate; break; }
    done
    [ -n "$ndk" ] || ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK:-${ANDROID_NDK_ROOT:-}}}
    [ -n "$ndk" ] || { [ -d /opt/android-ndk ] && ndk=/opt/android-ndk; }
    [ -n "$ndk" ] || return 1
    printf '%s\n' "$ndk"
}

s2a_ndk_prebuilt_tag() {
    local ndk=$1 tag
    for tag in darwin-arm64 darwin-x86_64 linux-x86_64; do
        [ -d "$ndk/toolchains/llvm/prebuilt/$tag" ] && { printf '%s\n' "$tag"; return 0; }
    done
    return 1
}

s2a_ndk_clang() {
    local ndk=$1 api=$2 tag
    tag=$(s2a_ndk_prebuilt_tag "$ndk") || return 1
    printf '%s\n' "$ndk/toolchains/llvm/prebuilt/$tag/bin/aarch64-linux-android$api-clang"
}

s2a_ndk_clangxx() {
    local ndk=$1 api=$2 tag
    tag=$(s2a_ndk_prebuilt_tag "$ndk") || return 1
    printf '%s\n' "$ndk/toolchains/llvm/prebuilt/$tag/bin/aarch64-linux-android$api-clang++"
}

# NDK 自带的 prebuilt/<host>/bin（host 侧工具）；没有就返回非零。
s2a_ndk_host_bin() {
    local ndk=$1 tag
    tag=$(s2a_ndk_prebuilt_tag "$ndk") || return 1
    [ -d "$ndk/toolchains/llvm/prebuilt/$tag/bin" ] || return 1
    printf '%s\n' "$ndk/toolchains/llvm/prebuilt/$tag/bin"
}

# 并行编译数：Linux 用 nproc，macOS 用 sysctl。
s2a_jobs() {
    if command -v nproc >/dev/null 2>&1; then nproc
    elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.ncpu
    else echo 2; fi
}

# sha256：Linux 用 sha256sum，macOS 用 shasum -a 256。
s2a_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"
    else shasum -a 256 "$@"; fi
}

# 「<hex>  <file>」清单校验（BSD shasum 没有 -c）：按第一个双空格拆分，文件名可含单空格。
_s2a_sha_check_stdin() {
    local bits=$1 line expected file actual rc=0
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        expected=${line%%  *}
        file=${line#*  }
        actual=$(shasum -a "$bits" "$file" 2>/dev/null | cut -d' ' -f1) || return 1
        { [ -n "$actual" ] && [ "$actual" = "$expected" ]; } || rc=1
    done
    return "$rc"
}

# s2a_sha256_check [manifest]：校验「<sha256>  <file>」清单（默认 stdin）。
s2a_sha256_check() {
    local manifest=${1:--}
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum -c --quiet "$manifest"
    elif [ "$manifest" = - ]; then
        _s2a_sha_check_stdin 256
    else
        _s2a_sha_check_stdin 256 < "$manifest"
    fi
}

# s2a_sha512_check [manifest]：校验「<sha512>  <file>」清单（默认 stdin）。
s2a_sha512_check() {
    local manifest=${1:--}
    if command -v sha512sum >/dev/null 2>&1; then
        sha512sum -c --quiet "$manifest"
    elif [ "$manifest" = - ]; then
        _s2a_sha_check_stdin 512
    else
        _s2a_sha_check_stdin 512 < "$manifest"
    fi
}

# GNU timeout 的跨宿主替身：secs 秒后杀子进程并以 124 退出（只杀直接子进程，与 timeout 一致）。
# 需要 python3；调用方本来就靠它做归档/清单校验。
s2a_timeout() {
    local secs=$1
    shift
    python3 - "$secs" "$@" <<'PY'
import subprocess, sys
try:
    sys.exit(subprocess.run(sys.argv[2:], timeout=int(sys.argv[1])).returncode)
except subprocess.TimeoutExpired:
    sys.exit(124)
PY
}

s2a_ndk_glslc() {
    local ndk=$1 tag candidate
    if tag=$(s2a_ndk_prebuilt_tag "$ndk") && [ -x "$ndk/shader-tools/$tag/glslc" ]; then
        printf '%s\n' "$ndk/shader-tools/$tag/glslc"
        return 0
    fi
    candidate=$(command -v glslc 2>/dev/null || true)
    [ -n "$candidate" ] || return 1
    printf '%s\n' "$candidate"
}
