#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Actual compute bridge/identity/lookup observation, controlled Vulkan/compiler IO.

The existing bridge fixture remains the production-result oracle. We add only
observation setup and binary result capture; no production branch is replaced.
"""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent)); import host_link   # 链接清单按符号闭包推导、弱引用自动放行（见 host_link.py）

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "android/native"
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "android/core-link"))
import link_symbols
spec = importlib.util.spec_from_file_location("program", HERE / "w2_compute_program_test.py")
program = importlib.util.module_from_spec(spec)
spec.loader.exec_module(program)


def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


def harness():
    text = (HERE / "w2_compute_bridge_test.c").read_text()
    text = replace_once(text, "static int acquired, released;", r'''#include "direct/w2_observation.inc"
static int observation_sink(void *opaque, const char *data, size_t size) {
    (void)opaque;
    assert(size && strstr(data, "receipt_not_persisted"));
    assert(!strstr(data, "\"window_complete\":true"));
    return 0;
}
static int acquired, released;''')
    text = replace_once(text, "    int rc=nvn_vk_compute_build(&device,&state,&memory,&out,&result,NULL);", r'''    int enabled = getenv("W2_ON") != NULL;
    int unsealed = getenv("W2_UNSEALED") != NULL;
    struct s2a_w2_window *window = s2a_w2_start(enabled, &pc);
    struct s2a_w2_scope scope = {0};
    s2a_w2_scope_enter(&scope);
    s2a_w2_scope_expect_shader(&scope);
    memset(&out, 0, sizeof out);
    memset(&result, 0, sizeof result); result.executed = 123;
    int rc=nvn_vk_compute_build(&device,&state,&memory,&out,&result,NULL);
    if (mode == 31 && rc == S2A_RECORD_DISPATCH_RETRY_SYNC) {
        if (enabled) {
            struct s2a_w2_snapshot interim;
            assert(s2a_w2_snapshot(window, &interim) == 0);
            assert(interim.shaders == 0 && interim.observations == 0 &&
                   interim.accepted_builds == 0 && interim.build_failures == 0 && interim.rejects == 0);
        }
        state.fq_pool = 0; memset(&out, 0, sizeof out);
        rc=nvn_vk_compute_build(&device,&state,&memory,&out,&result,NULL);
    }
    s2a_w2_scope_leave(&scope, rc == 0);
    if (enabled) {
        struct s2a_w2_snapshot snapshot;
        assert(window);
        s2a_w2_close(window);
        if (!unsealed) (void)s2a_w2_seal(window, 1, 1);
        assert(s2a_w2_snapshot(window, &snapshot) == 0);
        assert(snapshot.shaders == 1 && snapshot.finished_shaders == 1);
        assert(!snapshot.request_set_proven && !snapshot.strict_proven &&
               !snapshot.gpu_proven && !snapshot.w2_acceptance_proven);
        if (rc == 0) {
            assert(snapshot.observations == 1 && snapshot.entry_count == 1);
            assert(snapshot.entries[0].kind == S2A_W2_COMPUTE);
            assert(!memcmp(snapshot.entries[0].identity, out.diagnostic.identity, 32));
            assert(snapshot.entries[0].source_counts[mode == 31 ? S2A_W2_MEMO : (mode == 13 || mode == 14) ? S2A_W2_OFFLINE : S2A_W2_LIVE] == 1);
            assert(snapshot.metadata_complete == !unsealed);
        } else assert(!snapshot.metadata_complete);
        if (mode == 15) assert(snapshot.build_failures == 1 && snapshot.observations == 1);
        (void)s2a_w2_export(window, observation_sink, NULL);
        s2a_w2_release(window);
    }
    /* Initialized stable output bytes and IO boundary counts, excluding the
       ASLR-sensitive callback pointer. No pointer is dereferenced by the oracle. */
    struct s2a_record_dispatch_output stable = out; stable.finish = NULL;
    FILE *capture = fopen(getenv("W2_RESULT"), "wb"); assert(capture);
    assert(fwrite(&rc, sizeof rc, 1, capture) == 1);
    assert(fwrite(&result, sizeof result, 1, capture) == 1);
    assert(fwrite(&stable, sizeof stable, 1, capture) == 1);
    int counts[] = {provider_calls, offline, disk, worker, pipeline, pauses, resumes, stopped, acquired, released};
    assert(fwrite(counts, sizeof counts, 1, capture) == 1);
    assert(fclose(capture) == 0);''')
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", help="compare the unchanged bridge at this frozen git commit")
    args = parser.parse_args()
    ndk = link_symbols.find_ndk()
    if ndk is None:
        raise RuntimeError("W2 observation host test needs pinned NDK29; no download or skip")
    host = "darwin-x86_64" if sys.platform == "darwin" else "linux-x86_64"
    headers = ndk / "toolchains/llvm/prebuilt" / host / "sysroot/usr/include"
    fixture = program.fixture()
    fixture.update(session=11, generation=22)
    # 真实编译结果的 transport 是嵌套对象；bridge 夹具的回填桩（echo_request_id）按 REQUEST
    # 顶层 requestId 替换 <RID> 占位。这里必须照同一个形状给占位，否则现编成功路径会因为
    # “结果没回显请求身份”被 accept() 准确拒掉——那是夹具缺项，不是生产能力差异。
    fixture["transport"] = dict(session=11, generation=22, requestId="<RID>")
    fixture["content"]["identity"] = dict(directABI=1, translatorClosure="closure",
        runtimeCompilerABI="s2a-direct-compile-v1", profileHash="sha256-" + "a" * 64)
    unaligned = copy.deepcopy(fixture)
    unaligned["content"]["hasUnalignedStorageBuffer"] = True
    texture = copy.deepcopy(fixture)
    texture["content"]["bindings"].append(dict(stage="cs", kind="texture", set=2, binding=3, slot=8, count=1))
    # argv[3] 是 mode 14 的“离线库里的坏条目”。它必须真的过不了 accept(fresh=0)：
    # 早先直接拿 texture.json 当这里，但那一份对 fresh=0 的持久化条目完全合法（内容身份
    # 与当前 provider 一致），于是 mode 14 会真的成功——用例自己先红。改成“同一份内容、
    # 身份 profileHash 属于另一个 profile”，这才是离线库必须拒掉的那类陈旧条目。
    bad = copy.deepcopy(texture)
    bad["content"]["identity"]["profileHash"] = "sha256-" + "b" * 64
    build=ROOT/"build/w2-observation-compute";build.mkdir(parents=True,exist_ok=True);tempfile.tempdir=str(build)
    with tempfile.TemporaryDirectory(prefix="w2-observe-compute-") as directory:
        td = Path(directory)
        inc = td / "include"; inc.mkdir()
        (inc / "vulkan").symlink_to(headers / "vulkan")
        if (headers / "vk_video").exists(): (inc / "vk_video").symlink_to(headers / "vk_video")
        (inc / "android").mkdir()
        (inc / "android" / "native_window.h").write_text("#pragma once\ntypedef struct ANativeWindow ANativeWindow;\n")
        for name, value in (("good", fixture), ("unaligned", unaligned), ("texture", texture), ("bad", bad)):
            (td / (name + ".json")).write_text(json.dumps(value))
        (td / "fixture.c").write_text(harness())
        (td / "source-store.c").write_text('#define s2a_source_acquire real_source_acquire\n#define s2a_source_release real_source_release\n#include "' + str(NATIVE / "direct/source_store.c") + '"\n')
        flags = [os.environ.get("CC", "cc"), "-DS2A_COMPILE_EXPORT_HOST_TEST=1", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-pthread"]
        flags += ["-DS2A_ANDROID_VK_ENGINE=1", "-DS2A_ANDROID_VK_BACKEND_NVN_VK=1", "-DS2A_ANDROID_DEBUG_ENV=1"]
        flags += ["-I" + str(p) for p in (inc, NATIVE, NATIVE / "nvn", NATIVE / "direct", ROOT / "graphics/nvn/gfx/nvn", ROOT / "graphics/nvn/gfx/nvn/include")]
        files = [str(td / "fixture.c"), str(td / "source-store.c")]
        # 合并取并集：本支 bridge 的消费点纹理快照（nvn_vk_compute_texture.c）与 compile facts
        # 依赖的格式/BCRT 视图换算（nvn_vk_format.c / nvn_vk_mrt.c）必须保留；main 新增的
        # compile_export_w2.c / w2_single_query.c 是 compile_export 拆分后的生产依赖，也保留。
        files += [str(NATIVE / "nvn" / p) for p in ("nvn_vk_compute_bridge.c", "nvn_vk_compute_request.c",
            "nvn_vk_compute_lookup.c", "nvn_vk_program_result.c", "nvn_vk_compile_facts.c",
            "nvn_vk_compute_texture.c", "nvn_vk_format.c", "nvn_vk_mrt.c")]
        files += [str(NATIVE / "direct" / p) for p in ("control.c", "diag.c", "compile_export.c", "compile_export_w2.c", "w2_single_query.c", "input_identity.c", "texture_facts.c", "sha256.c", "json.c")]
        executable = td / "compute"
        host_link.run(flags + files + ["-lm", "-o", str(executable)], check=True, timeout=60)
        bridge_file = str(NATIVE / "nvn/nvn_vk_compute_bridge.c")
        current = Path(bridge_file).read_text()
        variants = {"withdraw-lookup": current.replace('#include "direct/w2_observation.h"',
            '#include "direct/w2_observation.h"\n#define s2a_w2_shader_lookup(...) ((void)0)')}
        if args.baseline_ref:
            variants["baseline"] = subprocess.check_output(["git", "show", args.baseline_ref + ":android/native/nvn/nvn_vk_compute_bridge.c"], cwd=ROOT, text=True)
        binaries = {}
        for name, text in variants.items():
            source = td / (name + ".c"); source.write_text(text)
            binary = td / name; binaries[name] = binary
            inputs = [str(source) if f == bridge_file else f for f in files]
            host_link.run(flags + inputs + ["-lm", "-o", str(binary)], check=True, timeout=60)
        red_env = os.environ.copy()
        red_env.update(W2_ON="1", W2_RESULT=str(td / "red.bin"))
        red_env.pop("W2_UNSEALED", None)
        for name, binary in binaries.items():
            red = subprocess.run([str(binary), "0", str(td / "good.json"), str(td / "texture.json"), str(td / ("diag-red-" + name))],
                env=red_env, capture_output=True, text=True, timeout=5)
            assert red.returncode != 0 and "Assertion" in red.stderr, (name, red.stderr)
            print(name, "expected functional red:", red.stderr.strip())
        for mode in list(range(21)) + [31]:
            observed = []
            for setting in ("off", "on", "unsealed"):
                env = os.environ.copy()
                env.pop("W2_ON", None); env.pop("W2_UNSEALED", None)
                if setting != "off": env["W2_ON"] = "1"
                if setting == "unsealed": env["W2_UNSEALED"] = "1"
                output = td / (str(mode) + setting + ".bin"); env["W2_RESULT"] = str(output)
                subprocess.run([str(executable), str(mode), str(td / ("unaligned.json" if mode in (1, 2) else "good.json")), str(td / "bad.json"), str(td / ("diag" + str(mode)))], env=env, check=True, timeout=5)
                observed.append(output.read_bytes())
            assert observed[0] == observed[1] == observed[2], (mode, "production result bytes changed")
            if "baseline" in binaries:
                env = os.environ.copy(); env.pop("W2_ON", None); env.pop("W2_UNSEALED", None)
                output = td / (str(mode) + "baseline.bin"); env["W2_RESULT"] = str(output)
                subprocess.run([str(binaries["baseline"]), str(mode), str(td / ("unaligned.json" if mode in (1, 2) else "good.json")), str(td / "texture.json"), str(td / ("diag" + str(mode)))], env=env, check=True, timeout=5)
                assert output.read_bytes() == observed[0], (mode, "frozen baseline output differs")
        print("PASS: 22 actual compute cases x off/on/unsealed; stable output bytes and IO counts identical")


if __name__ == "__main__":
    main()
