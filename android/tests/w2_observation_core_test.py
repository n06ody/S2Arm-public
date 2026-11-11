#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Bounded host-only metadata/lifetime checks and assertion-based negative controls."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DIRECT = Path("android/native/direct")
TEST = Path("android/tests/w2_observation_core_test.c")


def check_build_modes(root, cc, implementation):
    """Separate caller/owner objects: release must have no observer references or state."""
    (root / DIRECT / "w2_observation.inc").write_text(implementation)
    caller = root / "build-caller.c"
    caller.write_text('''#include "android/native/direct/w2_observation.h"
int main(void) {
    struct s2a_w2_scope scope = {0}; struct s2a_w2_shader shader = {0};
    struct s2a_w2_window *window = s2a_w2_start(1, NULL);
    s2a_w2_scope_enter(&scope); s2a_w2_scope_expect_shader(&scope);
    s2a_w2_shader_begin(&shader, S2A_W2_GRAPHICS);
    s2a_w2_shader_lookup(NULL, NULL, S2A_W2_OFFLINE, 1);
    s2a_w2_shader_end(&shader, 1); s2a_w2_scope_leave(&scope, 1);
    s2a_w2_close(window);
    int result = window != NULL || s2a_w2_seal(window, 1, 1) != 0 ||
        s2a_w2_snapshot(window, NULL) != -1 || s2a_w2_ack(window, NULL, 1) != -1 ||
        s2a_w2_export(window, NULL, NULL) != -1;
    s2a_w2_release(window);
    return result;
}
''')
    owner = root / "build-owner.c"
    owner.write_text('#include "android/native/direct/w2_observation.inc"\n'
                     'int observation_owner_marker(void) { return 1; }\n')
    engine = "-DS2A_ANDROID_VK_ENGINE=1"
    backend = "-DS2A_ANDROID_VK_BACKEND_NVN_VK=1"
    debug = "-DS2A_ANDROID_DEBUG_ENV=1"
    modes = [("release", [engine, backend], False),
             ("debug", [engine, backend, debug], True),
             ("engine-off", ["-DS2A_ANDROID_VK_ENGINE=0", backend, debug], False),
             ("backend-off", [engine, "-DS2A_ANDROID_VK_BACKEND_NVN_VK=0", debug], False),
             ("engine-absent", [backend, debug], False),
             ("backend-absent", [engine, debug], False),
             ("all-absent", [], False)]
    nm = shlex.split(os.environ.get("NM", "nm"))
    for name, defines, active in modes:
        objects = []
        for kind, source in (("caller", caller), ("owner", owner)):
            obj = root / (name + "-" + kind + ".o")
            subprocess.run(cc + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic"] +
                           defines + ["-c", str(source), "-o", str(obj)], check=True, timeout=30)
            output = subprocess.run(nm + ["-a", str(obj)], check=True, text=True,
                                    capture_output=True, timeout=5).stdout
            symbols = {fields[-1].lstrip("_"): fields[-2]
                       for line in output.splitlines() if len(fields := line.split()) >= 2}
            observer = sorted(s for s in symbols if s.startswith(("s2a_w2_", "w2_")))
            if active:
                required = {"s2a_w2_start", "s2a_w2_close", "s2a_w2_seal", "s2a_w2_release",
                            "s2a_w2_scope_enter", "s2a_w2_scope_expect_shader", "s2a_w2_scope_leave",
                            "s2a_w2_shader_begin", "s2a_w2_shader_lookup", "s2a_w2_shader_end",
                            "s2a_w2_snapshot", "s2a_w2_ack", "s2a_w2_export"}
                expected_type = "U" if kind == "caller" else "T"
                if any(symbols.get(symbol) != expected_type for symbol in required):
                    raise SystemExit(f"FAIL build mode {name} {kind}: expected all 13 observer symbols as {expected_type}")
                if kind == "owner" and not {"w2_enabled", "w2_scope", "w2_shader"} <= symbols.keys():
                    raise SystemExit("FAIL build mode debug owner: real atomic/TLS state missing")
            elif observer:
                raise SystemExit(f"FAIL build mode {name} {kind}: unexpected observer symbols: {observer}")
            objects.append(str(obj))
        if not active:
            binary = root / (name + "-no-observer")
            # No pthread/provider link shim: the production inline no-ops must stand alone.
            subprocess.run(cc + objects + ["-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=5)
        print(f"PASS build mode {name}: " + ("real Debug symbols/state" if active else "no observer symbols/state; no-op link/run"), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-modes-only", action="store_true", help="only compile the production Debug/release contract")
    args = parser.parse_args()
    source = (ROOT / DIRECT / "w2_observation.inc").read_text()
    controls = [
        ("zero-observation-complete", "d->shaders > 0", "1", "test_zero_and_ordinary_packets"),
        ("retry-misclassified", "if (retry) {", "if (0 && retry) {", "test_compute_sync_retry"),
        ("short-identity-alias", "!memcmp(w->data.entries[i].identity, identity, 32)",
         "!memcmp(w->data.entries[i].identity, identity, 8)", "test_positive_sources_and_full_identity"),
        ("memo-relabelled-memory", "++w->data.entries[i].source_counts[source]",
         "++w->data.entries[i].source_counts[S2A_W2_MEMORY]", "test_positive_sources_and_full_identity"),
        ("rejected-counted-hit", "else ++w->data.entries[i].rejected_source_counts[source]",
         "else ++w->data.entries[i].source_counts[source]", "test_cohort_missing_rejected_and_failed"),
        ("stale-revision-ack", "s->instance != w->data.instance ||\n            s->revision != w->data.revision",
         "s->instance != w->data.instance", "test_receipts_and_output"),
        ("foreign-instance-ack", "s->instance != w->data.instance ||\n            s->revision != w->data.revision",
         "s->revision != w->data.revision", "test_receipts_and_output"),
        ("unbound-not-counted", "++w->data.unbound; t->blocked = 1; w2_gap(w);",
         "t->blocked = 1; w2_gap(w);", "test_early_nested_and_unbound"),
        ("cohort-pollution", "goto lookup_done; // Never attribute another cohort's identity to this window.",
         "if (0) goto lookup_done; // Deliberately wrong cohort attribution.",
         "test_cohort_missing_rejected_and_failed"),
        ("closing-new-lease", "w2_scope || w->data.state != S2A_W2_ADMIT || w->owner_released",
         "w2_scope || w->data.state == S2A_W2_SEALED || w->owner_released", "test_close_and_lease"),
        ("ineligible-progress-ack", "if (result != 0 || (s && s->metadata_complete)) result = s2a_w2_ack(w, s, result == 0);\n    else result = -1;",
         "result = s2a_w2_ack(w, s, result == 0);", "test_ineligible_snapshot_progress"),
    ]
    seal_controls = [
        ("repeated-seal-revision", "if (w->data.state != S2A_W2_SEALED) {",
         "if (1) {", "test_repeat_seal"),
        ("repeated-seal-clears-receipt", "else if (stop_ok && drained) {",
         "else if (stop_ok && drained) { w->receipt_revision = 0;", "test_repeat_seal"),
        ("successful-seal-erases-failures", "else if (stop_ok && drained) {",
         "else if (stop_ok && drained) { w->data.stop_failures = 0; w->data.drain_failures = 0; w->data.gaps = 0;",
         "test_repeat_seal"),
    ]
    for name, old, _, _ in controls:
        if source.count(old) != 1:
            raise SystemExit(name + ": mutation anchor changed; review the control")
    build=ROOT/"build/w2-observation-core";build.mkdir(parents=True,exist_ok=True);tempfile.tempdir=str(build)
    with tempfile.TemporaryDirectory(prefix="s2a-w2-core-") as directory:
        root = Path(directory)
        (root / DIRECT).mkdir(parents=True)
        (root / TEST.parent).mkdir(parents=True)
        for name in ("w2_observation.h", "compile_request.h"):
            shutil.copyfile(ROOT / DIRECT / name, root / DIRECT / name)
        shutil.copyfile(ROOT / TEST, root / TEST)
        cc = shlex.split(os.environ.get("CC", "cc"))
        check_build_modes(root, cc, source)
        if args.build_modes_only:
            return

        def run(name, implementation, arguments=()):
            (root / DIRECT / "w2_observation.inc").write_text(implementation)
            binary = root / name
            subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                 "-pthread", str(root / TEST), "-o", str(binary)],
                           check=True, timeout=30)
            result = subprocess.run([str(binary), *arguments], text=True, capture_output=True, timeout=15)
            print(name + ": " + (result.stdout + result.stderr).strip(), flush=True)
            return result

        # First show a functional red. Missing declarations or compile failures never count.
        name, old, new, _ = controls[0]
        result = run(name, source.replace(old, new))
        if result.returncode != 1 or "FAIL expect_incomplete" not in result.stderr:
            raise SystemExit(name + ": did not fail the zero-observation assertion")
        result = run("production-core", source)
        if result.returncode:
            raise SystemExit("production core failed")
        for name, old, new, expected in controls[1:]:
            result = run(name, source.replace(old, new))
            if result.returncode != 1 or "FAIL " + expected not in result.stderr:
                raise SystemExit(name + ": did not fail the expected functional assertion")

        for name, old, new, expected in seal_controls:
            if source.count(old) != 1:
                raise SystemExit(name + ": mutation anchor changed; review the control")
            result = run(name, source.replace(old, new), ("repeat-seal",))
            if result.returncode != 1 or "FAIL " + expected not in result.stderr:
                raise SystemExit(name + ": did not fail the expected repeat-seal assertion")

        # Test-only reduced capacity executes the real exporter failure branch, not a copied shim.
        capacity = "struct w2_json j = {NULL, 0, 65536, 0};"
        guard = "if (!j.failed) result = writer(opaque, j.text, j.used);"
        if source.count(capacity) != 1 or source.count(guard) != 1:
            raise SystemExit("tiny export anchors changed; review the controls")
        tiny = source.replace(capacity, "struct w2_json j = {NULL, 0, 32, 0};")
        result = run("tiny-export-buffer", tiny, ("tiny-export",))
        if result.returncode:
            raise SystemExit("tiny exporter failed to reject without invoking its sink")
        result = run("serialize-failure-ignored", tiny.replace(guard,
                     "result = writer(opaque, j.text, j.used);"), ("tiny-export",))
        if result.returncode != 1 or "FAIL test_tiny_export" not in result.stderr:
            raise SystemExit("ignored serialization failure did not fail the expected assertion")

        # Engine-off consumers compile and link without pthread or the provider implementation.
        off = root / "engine_off.c"
        off.write_text('''#include "android/native/direct/w2_observation.h"
int main(void) {
    struct s2a_w2_scope s; struct s2a_w2_shader t;
    if (s2a_w2_start(0, 0)) return 1;
    s2a_w2_scope_enter(&s); s2a_w2_scope_expect_shader(&s);
    s2a_w2_shader_begin(&t, S2A_W2_GRAPHICS);
    s2a_w2_shader_lookup(0, 0, S2A_W2_UNKNOWN, 0);
    s2a_w2_shader_end(&t, 0); s2a_w2_scope_leave(&s, 0);
    return 0;
}
''')
        subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                             str(off), "-o", str(root / "engine-off")], check=True, timeout=30)
        subprocess.run([str(root / "engine-off")], check=True, timeout=5)
        print("PASS: 46 metadata/lifetime/serialization cases, tiny exporter, 15 assertion-based negative controls, engine-off link; "
              "no device, GPU, request-set, strict or W2 acceptance claim")


if __name__ == "__main__":
    main()
