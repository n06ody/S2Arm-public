#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
# 门禁间接：android/tests/vk/run.sh 默认档按 standalone/*.sh 通配逐个跑（run.sh 末段 for 循环），单独跑用 run.sh --<名>
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/vk-feedback-core"
mkdir -p "$out/shaders"
: "${S2A_LAVAPIPE_ICD:?lavapipe required}"
: "${S2A_VK_LAYER_DIR:?validation required}"
[ -f "$repo/build/c883-ndk/vulkan/vulkan_core.h" ] || { echo 'NDK 29 headers missing' >&2; exit 2; }
glslc=${GLSLC:-glslc}
for shader in feedback feedback_pool feedback_pool_phantom feedback_pool_dyn feedback_combined feedback_depth; do
    "$glslc" --target-env=vulkan1.1 "$repo/android/tests/vk/shaders/$shader.frag" -o "$out/shaders/$shader.frag.spv"
done
"$glslc" --target-env=vulkan1.1 "$repo/android/native/nvn/blit_shaders/fullscreen.vert" -o "$out/shaders/fullscreen.vert.spv"
inc=()
for d in android/tests/vkhost build/c883-ndk graphics/nvn graphics/nvn/replay graphics/nvn/gfx/nvn graphics/nvn/gfx/nvn/include android/native android/native/nvn android/native/direct runtime/include; do inc+=(-I "$repo/$d"); done
src=("$repo/android/tests/vk/t_feedback.c" "$repo/graphics/nvn/replay/nvn_unimpl.c")
for f in android/native/nvn/backend_vulkan.c android/native/nvn/nvn_pipe_flight.c android/native/nvn/nvn_vk_pipe_once.c android/native/direct/vk_device.c android/native/direct/gs_path.c android/native/capprobe/capprobe_host.c android/native/nvn/nvn_vk_format.c graphics/nvn/gfx/nvn/nvn_fmt_expand.c android/native/nvn/nvn_vk_bcdec.c android/native/nvn/nvn_vk_driver_cache.c android/native/direct/sha256.c; do src+=("$repo/$f"); done
wrap=()
for fn in vkCreateImageView vkCreateFramebuffer vkCmdBeginRenderPass vkUpdateDescriptorSets vkCmdBindDescriptorSets vkCmdDraw vkCmdCopyImage vkGetDeviceProcAddr vkDestroyImageView vkWaitForFences; do wrap+=(-Wl,--wrap="$fn"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 \
    -DS2A_NVK_POOL_HOST_TEST=1 "${inc[@]}" "${src[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test"
export VK_ICD_FILENAMES="$S2A_LAVAPIPE_ICD" VK_LAYER_PATH="$S2A_VK_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
# run.sh 在调用此独立入口之后才开同步校验，不能依赖父进程或本地包装器。
# 同计算写入负控一样同时打开 shader 访问启发式；仅加载 validation 层不够。
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
cat > "$out/vk_layer_settings.txt" <<'SETTINGS'
khronos_validation.validate_sync = true
khronos_validation.syncval_shader_accesses_heuristic = true
SETTINGS
export VK_LAYER_SETTINGS_PATH="$out/vk_layer_settings.txt"
printf 'feedback validation: sync=1 shader_accesses_heuristic=1 settings=%s\n' "$VK_LAYER_SETTINGS_PATH"
if [[ ${S2A_TEST_PASS_DIAG_ONLY:-} == 1 ]]; then
    for dq in 0 1; do
        for arm in ${S2A_TEST_PASS_DIAG_ARMS:-off pass snapshot both}; do
            log="$out/pass-diag-dq$dq-$arm.log"
            S2A_TEST_PASS_DIAG="$arm" timeout 120 "$out/test" "$out/shaders" "$dq" 0 1 1 2>&1 | tee "$log"
            python3 "$repo/android/tests/vk/check_pass_diag.py" "$arm" "$log"
        done
    done
    exit 0
fi
failed=0
# 案例数：0–11（12）+ 深度 2 + 中止 1 + 16 / 17 + 守卫 18 + 整池组回收 20 = 19；opt=0 另有 15 / 19（要求池里的图都是 GENERAL），共 21；opt=1 共 19。
cases_want() { echo $((21 - 2 * $1)); }
# 白名单（只用于缺省档 opt=0 / pool_fb=0）：整池数组与「本笔写着的附件」别名，着色器真去采样它时，同步校验必然（也应当）报「采样读 ↔ 颜色附件写」冒险。
# 只放过这一类：案例 10 / 15 / 18 / 19 里，校验 ID 为 SYNC-HAZARD-READ-AFTER-WRITE 或 SYNC-HAZARD-WRITE-AFTER-READ、命令是 vkCmdDraw、
# 一方是 SYNC_FRAGMENT_SHADER_SHADER_SAMPLED_READ、另一方是 SYNC_COLOR_ATTACHMENT_OUTPUT_COLOR_ATTACHMENT_WRITE。
# 理由：整池数组按设计不追踪反馈环（对齐 Ryujinx 的数组段），描述符里留着原图，读不读由运行时下标决定；这类冒险是该设计的固有结果，不是漏了屏障。
# 其余一切（别的案例、别的访问类型、别的命令、核心校验报错）照红。打印不在白名单里的报错行；全在白名单里返回 0；白名单命中数打在「hazard-whitelist」行。
hazard_check() {
    awk -v allowed='10 15 18 19' '
        BEGIN { n = split(allowed, a, " "); for (i = 1; i <= n; i++) ok_case[a[i]] = 1 }
        /^feedback begin / { if (match($0, /case=[0-9]+/)) cur = substr($0, RSTART + 5, RLENGTH - 5); next }
        /Validation Error|VUID-/ {
            if (/^ /) next
            white = (cur in ok_case) && ($0 ~ /SYNC-HAZARD-(READ-AFTER-WRITE|WRITE-AFTER-READ)/) && ($0 ~ /vkCmdDraw/) &&
                    ($0 ~ /SYNC_FRAGMENT_SHADER_SHADER_SAMPLED_READ/) && ($0 ~ /SYNC_COLOR_ATTACHMENT_OUTPUT_COLOR_ATTACHMENT_WRITE/)
            if (white) hit[cur]++; else { print "case=" cur ": " substr($0, 1, 300); bad++ }
        }
        END { for (c in hit) printf "hazard-whitelist case=%s hits=%d\n", c, hit[c]; exit bad ? 1 : 0 }
    ' "$1"
}
# 旧行为（nvk_pool_fb=1）：整池数组也为写着的附件拷快照。同步校验开着，必须零冒险、零校验报错。
for dq in 0 1; do
    for opt in 0 1; do
        set +e
        timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" 1 1 2>&1 | tee "$out/dq$dq-opt$opt.log"
        rc=${PIPESTATUS[0]}
        set -e
        if [ "$rc" -ne 0 ] || grep -qE 'Validation Error|VUID-' "$out/dq$dq-opt$opt.log" ||
           ! grep -q "feedback cases=$(cases_want $opt) failures=0" "$out/dq$dq-opt$opt.log"; then
            echo "【红】feedback DQ=$dq OPT=$opt pool_fb=1 rc=$rc or validation failure" >&2; failed=1
        fi
    done
done
# 缺省（nvk_pool_fb=0）：整池数组与写着的附件重叠，不拷快照、不拆通道（对齐 Ryujinx：数组段不做反馈环追踪）。同步校验照开。
#   opt=0：案例 10（常量下标真读本通道附件）、15（只登记、不读）、18（守卫探针）、19（推送常量下标真读）故意保留描述符别名。
#          同步校验照开：只放过上面白名单里那一类（采样读 ↔ 颜色附件写），其余冒险与全部核心校验报错照红。
#          本机校验层（1.3.275）不跟踪整池数组的描述符访问，这一类实际 0 条；换成会跟踪的校验层时靠白名单放行。
#   opt=1：沿用旧行为，同步校验照开，必须零冒险（案例 15 / 19 要求池里的图都是 GENERAL，该模式不跑；免拷的守卫由案例 18 在 opt=1 下钉住：照旧拷快照）。
#   案例 16 / 17：池里登记的是「本笔没写」的附件，缺省与旧行为一样照旧拷快照、拆通道。
for dq in 0 1; do
    for opt in 0 1; do
        set +e
        timeout 120 "$out/test" "$out/shaders" "$dq" "$opt" 1 0 2>&1 | tee "$out/dq$dq-opt$opt-poolskip.log"
        rc=${PIPESTATUS[0]}
        set -e
        log="$out/dq$dq-opt$opt-poolskip.log"
        if [ "$rc" -ne 0 ] || ! grep -q "feedback cases=$(cases_want $opt) failures=0" "$log"; then
            echo "【红】feedback DQ=$dq OPT=$opt pool_fb=0 rc=$rc or case failure" >&2; failed=1
        fi
        if [ "$opt" -eq 0 ]; then
            if ! rep=$(hazard_check "$log"); then
                echo "【红】feedback DQ=$dq OPT=0 pool_fb=0：白名单之外的校验报错：" >&2; echo "$rep" | head -n 12 >&2; failed=1
            elif [ -n "$rep" ]; then
                echo "$rep" | sort | sed "s/^/DQ=$dq OPT=0 /"
            else
                # 本机校验层（1.3.275）不跟踪整池数组（partially-bound / update-after-bind 的变长描述符）的描述符访问：
                # 案例 10 / 15 / 19 的别名对同步校验不可见（对照：下面「快照路径整条关掉」的负控里，显式绑定的案例 2 / 3 / 12 / 13 报了冒险，池案例一条都没有）。
                # 所以这一档零报错，白名单一条没用上；换成会跟踪池数组的校验层时，那一类冒险走白名单放行，其余照红。
                echo "DQ=$dq OPT=0 hazard-whitelist: 0 条命中（本机校验层不跟踪整池数组的描述符访问）"
            fi
        elif grep -qE 'Validation Error|VUID-' "$log"; then
            echo "【红】feedback DQ=$dq OPT=1 pool_fb=0：校验报错（该档沿用旧行为，必须零冒险）" >&2; failed=1
        fi
    done
done
# 缺省与旧行为的读数必须真的不同（防止开关没接上、两边测的是同一件事）。逐项写死：
#   案例 15 / 19：缺省拷 0 份、整个命令只开 3 次通道；旧行为拷 2 份、多重开 2 次（19：真读附件，旧行为经快照读，像素值有定义）。
#   案例 18（守卫，只录制不提交）：opt=0 缺省拷 0 份开 3 次；opt=1 与 pool_fb=1 照旧拷 2 份开 5 次——同一份录制，只差 opt 档。
#   免拷逐条日志：缺省每个案例恰好 1 行（两笔 draw 只记一次），旧行为 0 行。
pairs=(
  "$out/dq0-opt0-poolskip.log|pool_fb=0 case=15 draws=2 live-overlap=2 copies=0 copy-bad=0 begins=3 "
  "$out/dq0-opt0-poolskip.log|pool_fb=0 case=19 draws=2 live-overlap=2 copies=0 copy-bad=0 begins=3 "
  "$out/dq0-opt0.log|pool_fb=1 case=15 draws=2 live-overlap=0 copies=2 copy-bad=0 begins=5 "
  "$out/dq0-opt0.log|pool_fb=1 case=19 draws=2 live-overlap=0 copies=2 copy-bad=0 begins=5 "
  "$out/dq0-opt0-poolskip.log|feedback guard dq=0 opt=0 pool_fb=0 case=18 draws=2 live-overlap=4 copies=0 copy-bad=0 begins=3 begins-bad=0 overlap-bad=0 skip-logs=1 logs-bad=0"
  "$out/dq0-opt1-poolskip.log|feedback guard dq=0 opt=1 pool_fb=0 case=18 draws=2 live-overlap=0 copies=2 copy-bad=0 begins=5 begins-bad=0 overlap-bad=0 skip-logs=0 logs-bad=0"
  "$out/dq0-opt0.log|feedback guard dq=0 opt=0 pool_fb=1 case=18 draws=2 live-overlap=0 copies=2 copy-bad=0 begins=5 begins-bad=0 overlap-bad=0 skip-logs=0 logs-bad=0"
  "$out/dq1-opt1-poolskip.log|feedback guard dq=1 opt=1 pool_fb=0 case=18 draws=2 live-overlap=0 copies=2 copy-bad=0 begins=5 begins-bad=0 overlap-bad=0 skip-logs=0 logs-bad=0"
  "$out/dq0-opt0-poolskip.log|pool_fb=0 case=10 draws=2 live-overlap=4 copies=0 copy-bad=0 begins=3 begins-bad=0 overlap-bad=0 pixel-bad=0 skip-logs=1 logs-bad=0"
  "$out/dq0-opt0.log|pool_fb=1 case=10 draws=2 live-overlap=0 copies=2 copy-bad=0 begins=5 begins-bad=0 overlap-bad=0 pixel-bad=0 skip-logs=0 logs-bad=0"
)
for p in "${pairs[@]}"; do
    f=${p%%|*}; pat=${p#*|}
    if ! grep -qF "$pat" "$f"; then
        echo "【红】feedback 读数没有按档分开：$(basename "$f") 缺「${pat}」" >&2; failed=1
    fi
done
# 案例 20（整池组回收，反馈补丁段完成时还原）：每一档都必须读到「第 2 段取回 1 个回收组、4 条补丁、没有已销毁的视图被采样」。
for p in dq0-opt0 dq0-opt1 dq1-opt0 dq1-opt1 dq0-opt0-poolskip dq0-opt1-poolskip dq1-opt0-poolskip dq1-opt1-poolskip; do
    if ! grep -qE 'feedback rec-restore .* case=20 draws=4 copies=2 dead-samples=0 overlap=0 rec-hit=0/1 patches=4 draws-bad=0 copy-bad=0 dead-bad=0 overlap-bad=0 hit-bad=0 patch-bad=0' "$out/$p.log"; then
        echo "【红】feedback 案例 20（整池组回收）没有读到预期：$p" >&2; grep 'rec-restore' "$out/$p.log" >&2 || true; failed=1
    fi
done
# 整池组回收的负控：段完成时不还原反馈补丁 → 放回空闲串的组里还是已销毁的快照视图，第 2 段取回来就在用已销毁的视图。同一份夹具，只改这一处后端源码。
mut="$out/mut"; mkdir -p "$mut"
cp "$repo/android/native/nvn/backend_vulkan.c" "$mut/backend_vulkan.c"
python3 - "$repo/android/native/nvn/backend_vulkan_pool.inc" "$mut/backend_vulkan_pool.inc" <<'PYMUT'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = 'if (!u->npatch) { free(u->patch); u->patch = NULL; continue; }'
if s.count(a) != 1: raise SystemExit('rec-restore mutation anchor changed')
pathlib.Path(sys.argv[2]).write_text(s.replace(a, 'if (1) { free(u->patch); u->patch = NULL; continue; }'))
PYMUT
msrc=(); for f in "${src[@]}"; do [ "$f" = "$repo/android/native/nvn/backend_vulkan.c" ] && f="$mut/backend_vulkan.c"; msrc+=("$f"); done
cc -D_POSIX_C_SOURCE=200809L -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
    -ffunction-sections -fdata-sections -DS2A_ANDROID_VK_BACKEND_NVN_VK=1 -DS2A_NVK_POOL_HOST_TEST=1 \
    "${inc[@]}" "${msrc[@]}" "${wrap[@]}" -Wl,--gc-sections -lvulkan -lpthread -lm -o "$out/test-mut"
set +e
timeout 120 "$out/test-mut" "$out/shaders" 0 0 1 1 > "$out/rec-restore-neg.log" 2>&1
mut_rc=$?
set -e
if [ "$mut_rc" -eq 0 ] || ! grep -qE 'feedback rec-restore .* case=20 .* dead-samples=[1-9][0-9]* .* dead-bad=1' "$out/rec-restore-neg.log"; then
    echo '【红】feedback 整池组回收负控：补丁不还原没有被案例 20 判红' >&2; tail -n 12 "$out/rec-restore-neg.log" >&2; failed=1
else
    echo 'feedback rec-restore negative: PASS (补丁不还原 → 第 2 段采样到已销毁的快照视图，案例 20 判红)'
fi
# Negative control disables the whole snapshot path (copy + barriers + descriptor
# substitution), with the same binary and real driver / validation configuration.
set +e
timeout 120 "$out/test" "$out/shaders" 1 0 0 1 > "$out/negative.log" 2>&1
negative_rc=$?
set -e
if [ "$negative_rc" -ne 1 ] ||
   ! grep -q 'case=3 draws=2 live-overlap=2 copies=0 copy-bad=1 begins=[0-9]* begins-bad=0 overlap-bad=1 pixel-bad=0' "$out/negative.log" ||
   ! grep -q "feedback cases=$(cases_want 0) failures=" "$out/negative.log" ||
   ! grep -q 'SYNC-HAZARD-READ-AFTER-WRITE' "$out/negative.log"; then
    echo '【红】feedback negative did not reach the expected alias failure' >&2
    tail -n 40 "$out/negative.log";failed=1
else
    echo 'feedback negative: PASS (same binary, copy off, live alias + VVL hazard detected)'
fi
# 白名单负控：同一份「快照整条路径关掉」的日志里，冒险出在案例 3 等白名单之外的案例，hazard_check 必须判红（返回非 0 并列出报错行）；
# 判不红 = 白名单放得太宽，缺省档的同步校验形同虚设。
if rep=$(hazard_check "$out/negative.log"); then
    echo '【红】feedback 白名单太宽：快照路径整条关掉后的冒险（案例 3 等）没有被判红' >&2; failed=1
elif ! echo "$rep" | grep -q '^case=3: '; then
    echo '【红】feedback 白名单负控：判红了，但报错行里没有案例 3' >&2; echo "$rep" | head -n 6 >&2; failed=1
else
    echo "feedback whitelist negative: PASS ($(echo "$rep" | grep -c '^case=') 条白名单之外的冒险被判红，含案例 3)"
fi
exit "$failed"
