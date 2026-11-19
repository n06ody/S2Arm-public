#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capability failures are environment-unverified, never fixture failures."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CHECK = ROOT / "android/tests/vk/syncval_capability.py"
KEY = "syncval_shader_accesses_heuristic"
MODES = ("inbound-dependency", "inbound-attachment", "inbound-attachment-compute", "inbound-indirect",
         "cgr-narrow", "cgr-fallback", "cgr-multi-omit", "cgr-write-control", "cgr-unknown-control")


def manifest(version="1.4.341", key=KEY, name="VK_LAYER_KHRONOS_validation"):
    return {"layer": {"name": name, "api_version": version, "features": {"settings": [{"key": key}]}}}


class CapabilityTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.path = self.folder / "VkLayer_khronos_validation.json"

    def run_check(self, value):
        self.path.write_text(json.dumps(value), encoding="utf-8")
        return subprocess.run([sys.executable, str(CHECK), str(self.path)], text=True, capture_output=True)

    def test_exact_version_and_real_feature_required(self):
        good = self.run_check(manifest())
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertIn("仍须通过实际正负控", good.stdout)
        for value in (manifest("1.3.275"), manifest(key="unrelated"), manifest(name="other"), manifest(version="bad"), {}, {"layer": None}):
            with self.subTest(value=value):
                failed = self.run_check(value)
                self.assertEqual(failed.returncode, 75)
                self.assertIn("【红】同步校验能力不足（需 ≥1.4.341），未验证", failed.stderr)
                self.assertIn("CI 同版本容器", failed.stderr)

    def test_missing_or_corrupt_manifest_is_unverified(self):
        for content in (None, "{"):
            with self.subTest(content=content):
                if content is None:
                    self.path.unlink(missing_ok=True)
                else:
                    self.path.write_text(content)
                result = subprocess.run([sys.executable, str(CHECK), str(self.path)], text=True, capture_output=True)
                self.assertEqual(result.returncode, 75)
                self.assertIn("【红】同步校验能力不足", result.stderr)

    def test_every_entry_refuses_old_layer_before_compiling(self):
        self.path.write_text(json.dumps(manifest("1.3.275", "unrelated")), encoding="utf-8")
        test_env = dict(os.environ, S2A_VK_LAYER_DIR=str(self.folder), S2A_LAVAPIPE_ICD=str(self.folder / "dummy.json"))
        for mode in MODES:
            with self.subTest(mode=mode):
                script = ROOT / "android/tests/vk/standalone" / (mode + ".sh")
                result = subprocess.run(["bash", str(script)], env=test_env, text=True, capture_output=True)
                self.assertEqual(result.returncode, 75, result.stdout + result.stderr)
                self.assertIn("【红】同步校验能力不足（需 ≥1.4.341），未验证", result.stderr)
                self.assertIn("CI 同版本容器", result.stderr)


if __name__ == "__main__":
    unittest.main()
