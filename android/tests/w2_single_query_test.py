#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Standalone synthetic W2 diagnostic contract; assertion-based mutation controls."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DIRECT = ROOT / "android/native/direct"
TEST = ROOT / "android/tests/w2_single_query_test.c"


def main():
    source = (DIRECT / "w2_single_query.c").read_text()
    controls = [
        ("short-full-identity", "!memcmp(encoded, original, n)",
         "!memcmp(encoded, original, 8)", "identity"),
        ("stale-admitted-to-resolver", "if (!stamp_equal(&input->original.stamp, &input->current))",
         "if (0 && !stamp_equal(&input->original.stamp, &input->current))", "stale"),
        ("existing-query-reresolved", "if (already_answered(input->identity, &query))",
         "if (0 && already_answered(input->identity, &query))", "missing"),
        ("same-handle-fallback-removed", "if (!fact && query->coordinate.cbuf_slot >= 0 &&\n        (query->method != S2A_W2_QUERY_SAMPLED_FORMAT || query->coordinate.cbuf_slot == 2)) fact = direct;",
         "if (0 && !fact && query->coordinate.cbuf_slot >= 0 &&\n        (query->method != S2A_W2_QUERY_SAMPLED_FORMAT || query->coordinate.cbuf_slot == 2)) fact = direct;", "missing"),
        ("fallback-overrides-exact", "if (!fact && query->coordinate.cbuf_slot >= 0 &&\n        (query->method != S2A_W2_QUERY_SAMPLED_FORMAT || query->coordinate.cbuf_slot == 2)) fact = direct;",
         "if (query->coordinate.cbuf_slot >= 0 &&\n        (query->method != S2A_W2_QUERY_SAMPLED_FORMAT || query->coordinate.cbuf_slot == 2)) fact = direct;", "missing"),
        ("sampled-c4-borrowed", "(query->method != S2A_W2_QUERY_SAMPLED_FORMAT || query->coordinate.cbuf_slot == 2)", "1", "missing"),
        ("unknown-format-is-answer", 'strcmp(fact->format, "Unknown") != 0', "1", "missing"),
        ("cross-stage-fact-borrowed", "stage_number(f->stage) != (int)query->coordinate.stage ||",
         "0 ||", "missing"),
        ("cross-handle-fact-borrowed", "f->handle != query->coordinate.handle) continue;",
         "0) continue;", "positive"),
        ("source-stamp-ignored", "!stamp_equal(&source->binding.stamp, stamp) ||",
         "(0 && !stamp_equal(&source->binding.stamp, stamp)) ||", "source"),
        ("short-source-identity", "memcmp(source->binding.canonical, canonical, bytes) ||",
         "memcmp(source->binding.canonical, canonical, 8) ||", "source"),
        ("source-handle-ignored", "r->stage != q->stage || r->handle != q->handle ||",
         "r->stage != q->stage ||", "source"),
        ("source-query-ignored", "!query_equal(&source->requested, query) || !source->source_id",
         "!source->source_id", "source"),
        ("missing-layers-defaulted", "raw->present == S2A_W2_RAW_COMPLETE && raw->layers > 0",
         "raw->present == S2A_W2_RAW_COMPLETE", "source"),
        ("absent-view-stale-values", "return source->view.present == 0 && source->view.format == 0 &&\n        source->view.target == 0 && source->view.layers == 0;",
         "return 1;", "view"),
        ("partial-failure-receipt", "memset(receipt, 0, sizeof *receipt);", "/* wrong: retain prior receipt */", "disabled"),
        ("facts-mutated-after-observation", "receipt->observed = 1;",
         "((struct s2a_compile_texture_src *)input->identity->textures)[0].normalized ^= 1;\n    receipt->observed = 1;", "positive"),
        ("postcheck-bool-canonical-alias", "rc = facts_valid(input->identity);\n    if (rc) return rc;\n    if (!source_matches",
         "if (!source_matches", "postcheck"),
        ("duplicate-coordinate-accepted", "return S2A_W2_QUERY_AMBIGUOUS;", "return 0;", "invalid"),
        ("disabled-optin-ignored", "if (input->enabled != 1) return S2A_W2_QUERY_DISABLED;",
         "if (0 && input->enabled != 1) return S2A_W2_QUERY_DISABLED;", "disabled"),
        ("resolver-failure-ignored", "if (resolver(snapshot, &query, &source)) return S2A_W2_QUERY_NO_SOURCE;",
         "(void)resolver(snapshot, &query, &source);", "source"),
        ("postcheck-facts-skipped", "!canonical_matches(input->identity, original, bytes)) return S2A_W2_QUERY_IDENTITY;\n    rc",
         "0) return S2A_W2_QUERY_IDENTITY;\n    rc", "postcheck"),
    ]
    for name, old, _, _ in controls:
        if source.count(old) != 1:
            raise SystemExit(f"FAIL {name}: mutation anchor changed; review the control")
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    with tempfile.TemporaryDirectory(prefix="s2a-w2-single-query-") as directory:
        root = Path(directory)
        implementation = root / "w2_single_query.c"
        shutil.copyfile(DIRECT / "w2_single_query.h", root / "w2_single_query.h")

        def run(name, text, arguments=()):
            implementation.write_text(text)
            binary = root / name
            subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic"] + flags +
                           ["-I" + str(DIRECT), "-I" + str(DIRECT.parent),
                            "-I" + str(ROOT / "graphics/nvn/gfx/nvn/include"), str(TEST),
                            str(implementation), str(DIRECT / "input_identity.c"),
                            str(DIRECT / "texture_facts.c"), "-o", str(binary)],
                           check=True, timeout=30)
            result = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=10)
            print(name + ": " + (result.stdout + result.stderr).strip(), flush=True)
            return result

        # Functional red, not a missing symbol/build failure: bypass the full
        # canonical comparison and require the mismatch assertion to reject it.
        name, old, new, group = controls[0]
        result = run(name, source.replace(old, new), (group,))
        if result.returncode != 1 or "FAIL run " not in result.stderr:
            raise SystemExit(f"FAIL {name}: expected identity assertion did not fail")
        result = run("production", source)
        if result.returncode or result.stdout.count("PASS ") != 12:
            raise SystemExit("FAIL production: expected all 12 host groups")
        for name, old, new, group in controls[1:]:
            result = run(name, source.replace(old, new), (group,))
            function = {"missing": "missing_semantics", "source": "source_contract"}.get(group, group)
            if result.returncode != 1 or "FAIL test_" + function + " " not in result.stderr:
                raise SystemExit(f"FAIL {name}: no expected functional assertion failure in {group}")
        print(f"PASS: 12 synthetic host groups, {len(controls)} assertion-based mutation controls; "
              "no device, frozen-producer, query translation, compile/cache, request-set or W2 acceptance claim")


if __name__ == "__main__":
    main()
