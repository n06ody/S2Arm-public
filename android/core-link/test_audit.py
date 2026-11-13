#!/usr/bin/env python3
"""Host-built archive/library fixtures exercise the actual nm/CLI path."""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("core_link_audit", HERE / "audit.py")
audit_module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit_module)


class BuildIdTest(unittest.TestCase):
    def test_readelf_requires_one_sha1_build_id(self):
        expected = '0123456789abcdef' * 2 + '01234567'
        for output, valid in ((f'  Build ID: {expected.upper()}\n', True),
                              ('', False),
                              (f'Build ID: {expected}\nBuild ID: {expected}\n', False),
                              ('Build ID: aa\n', False)):
            with self.subTest(output=output), patch.object(audit_module.subprocess, 'run',
                    return_value=subprocess.CompletedProcess([], 0, output, '')):
                if valid:
                    self.assertEqual(audit_module.android_build_id('readelf', 'lib.so'), expected)
                else:
                    with self.assertRaises(audit_module.AuditError):
                        audit_module.android_build_id('readelf', 'lib.so')


def elf_fixture(sections):
    """Minimal ELF64 LE AArch64 ET_DYN with the given (name, sh_type, sh_flags, bytes) sections.

    Field offsets follow the ELF64 spec (Elf64_Ehdr: e_shoff 0x28, e_shentsize 0x3A, e_shnum 0x3C,
    e_shstrndx 0x3E; Elf64_Shdr: name 0, type 4, flags 8, offset 24, size 32), written independently of
    the parser under test."""
    sections = [("", 0, 0, b"")] + list(sections)
    shstr = b"\0" + b"".join(n.encode() + b"\0" for n, *_ in sections[1:]) + b".shstrtab\0"
    sections.append((".shstrtab", 3, 0, shstr))
    body, offsets, pos = b"", [], 0x40
    for _, _, _, content in sections:
        offsets.append(pos if content else 0)
        body += content
        pos += len(content)
    shoff = (pos + 7) & ~7
    body += b"\0" * (shoff - pos)
    name_at, cursor = [0], 1
    for n, *_ in sections[1:-1]:
        name_at.append(cursor)
        cursor += len(n) + 1
    name_at.append(cursor)
    headers = b""
    for i, (_, kind, flags, content) in enumerate(sections):
        headers += (name_at[i].to_bytes(4, "little") + kind.to_bytes(4, "little") + flags.to_bytes(8, "little")
                    + (0).to_bytes(8, "little") + offsets[i].to_bytes(8, "little") + len(content).to_bytes(8, "little")
                    + bytes(24))
    ehdr = (b"\x7fELF\x02\x01\x01" + bytes(9) + (3).to_bytes(2, "little") + (0xB7).to_bytes(2, "little")
            + (1).to_bytes(4, "little") + bytes(16) + shoff.to_bytes(8, "little") + bytes(4)
            + (0x40).to_bytes(2, "little") + bytes(4) + (0x40).to_bytes(2, "little")
            + len(sections).to_bytes(2, "little") + (len(sections) - 1).to_bytes(2, "little"))
    assert len(ehdr) == 0x40
    return ehdr + body + headers


class HiddenLookupTest(unittest.TestCase):
    """010045B018EC2000: native dlsym(RTLD_DEFAULT, "S2A_NvMapFromGuest") inside the single Android library
    got NULL (--exclude-libs,ALL hides the NativeAOT export). Expected sets are written by hand from the
    fixture, not from the scanner's output."""
    ALLOC, EXEC, PROGBITS, NOBITS, STRTAB = 0x2, 0x4, 1, 8, 3

    def scan(self, sections, managed, local, exported):
        with tempfile.TemporaryDirectory(prefix="s2a-hidden-") as d:
            lib = Path(d) / "lib.so"
            lib.write_bytes(elf_fixture(sections))
            return audit_module.hidden_runtime_lookups(lib, set(managed), set(local), set(exported))

    def test_literal_of_hidden_managed_export_is_reported(self):
        rodata = b"\0S2A_NvMapFromGuest\0no service layer: S2A_SaveAtomicState missing\0S2A_Exported\0"
        found = self.scan([(".rodata", self.PROGBITS, self.ALLOC, rodata)],
                          managed={"S2A_NvMapFromGuest", "S2A_SaveAtomicState", "S2A_Exported", "S2A_Unused"},
                          local={"S2A_NvMapFromGuest", "S2A_SaveAtomicState", "S2A_Exported", "S2A_Unused"},
                          exported={"S2A_Exported"})
        # 只有整串等于隐藏导出名的才算；嵌在日志句子里的不算；动态表里有的不算；没出现字面量的不算
        self.assertEqual(found, ["S2A_NvMapFromGuest"])

    def test_symbol_tables_code_and_bss_are_not_literals(self):
        name = b"S2A_NvMapFromGuest\0"
        sections = [(".dynstr", self.STRTAB, self.ALLOC, b"\0" + name),
                    (".text", self.PROGBITS, self.ALLOC | self.EXEC, b"\0" + name),
                    (".strtab", self.STRTAB, 0, b"\0" + name),
                    (".bss", self.NOBITS, self.ALLOC | 0x1, b"")]
        self.assertEqual(self.scan(sections, {"S2A_NvMapFromGuest"}, {"S2A_NvMapFromGuest"}, set()), [])

    def test_native_definitions_are_not_managed_exports(self):
        # 同名字面量但符号不是托管归档定义的（原生自己的符号）：不在本检查范围
        found = self.scan([(".rodata", self.PROGBITS, self.ALLOC, b"\0s2a_native_thing\0")],
                          managed=set(), local={"s2a_native_thing"}, exported=set())
        self.assertEqual(found, [])

    def test_non_elf_is_refused(self):
        with tempfile.TemporaryDirectory(prefix="s2a-hidden-") as d:
            lib = Path(d) / "lib.dylib"
            lib.write_bytes(b"\xcf\xfa\xed\xfe" + bytes(60))
            with self.assertRaises(audit_module.AuditError):
                audit_module.hidden_runtime_lookups(lib, set(), set(), set())


class CoreLinkAuditTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = os.environ.get("CC", "cc")
        cls.nm = os.environ.get("NM", "nm")
        cls.symbol_prefix = "_" if sys.platform == "darwin" else ""
        for tool in (cls.cc, cls.nm, "ar", "cmake"):
            if not shutil.which(tool):
                raise RuntimeError(f"required test tool is missing: {tool}")
        cls.temp = tempfile.TemporaryDirectory(prefix="s2a-core-link-")
        cls.root = Path(cls.temp.name)
        cls.consumer = cls.root / "consumer"
        cls.consumer.mkdir()
        (cls.consumer / "Native.cs").write_text(
            'NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), '
            '"s2a_guest_tick", out nint tick);\n'
            'NativeLibrary.TryGetExport(program, symbolName, out nint unknown);\n',
            encoding="utf-8")
        cls.stub = cls.root / "s2a_native_stub.c"
        cls.stub.write_text("int s2a_handle_new(void) { return 17; }\n", encoding="utf-8")
        cls.sources = cls.root / "sources.txt"
        cls.sources.write_text("s2a_native_stub.c\n", encoding="utf-8")
        cls.archive = cls.root / "consumer.a"
        cls.compile_archive("extern int s2a_handle_new(void);\n"
                            "int invoke(void) { return s2a_handle_new(); }\n", cls.archive)
        cls.library = cls.compile_library(cls.stub, "complete")
        # Authored source inventory models flattened game CMake inputs. The
        # binary fixtures below still determine whether imports are satisfied.
        cls.game_sources = cls.root / "game-sources.txt"
        cls.game_paths = [
            "runtime/src/loader.c", "runtime/src/thread.c", "runtime/src/svc_mem.c",
            "runtime/src/gate.c", "runtime/src/entry.S", "runtime/src/guest_main.c",
            "graphics/track/track.c", "android/native/module.c",
            "android/native/plat_android.c",
            "android/native/runtime/runtime.c",
            "android/native/runtime/tracking/runtime.c",
            "android/native/runtime/fault/fault.c",
        ]
        for name in cls.game_paths:
            path = cls.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* Authored provenance fixture, not a native core. */\n", encoding="utf-8")
        cls.game_sources.write_text("\n".join(cls.game_paths) + "\n", encoding="utf-8")
        cls.game_provider = cls.root / "runtime/src/gate.c"
        cls.game_provider.write_text("int s2a_handle_new(void) { return 17; }\n", encoding="utf-8")
        cls.game_library = cls.compile_library(cls.game_provider, "game-complete")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def compile_archive(cls, text, archive):
        source = archive.with_suffix(".c")
        obj = archive.with_suffix(".o")
        source.write_text(text, encoding="utf-8")
        subprocess.run([cls.cc, "-fPIC", "-c", str(source), "-o", str(obj)], check=True)
        subprocess.run(["ar", "rcs", str(archive), str(obj)], check=True)

    @classmethod
    def compile_library(cls, source, name):
        library = cls.root / (name + ".so")
        flag = "-dynamiclib" if sys.platform == "darwin" else "-shared"
        subprocess.run([cls.cc, flag, "-fPIC", str(source), "-o", str(library)], check=True)
        return library

    def run_audit(self, **changes):
        args = {"archive": self.archive, "library": self.library, "source_list": self.sources,
                "source_root": self.root, "consumer_root": self.consumer, "nm": self.nm}
        args.update(changes)
        return audit_module.audit(**args)

    def test_direct_import_comes_from_compiled_archive(self):
        report = self.run_audit()
        self.assertTrue(report["diagnostic_link_complete"])
        self.assertFalse(report["game_ready"])
        self.assertEqual(report["required_direct_symbols"], [self.symbol_prefix + "s2a_handle_new"])
        self.assertEqual(report["missing_runtime_exports"], ["s2a_guest_tick"])
        self.assertEqual(len(report["unchecked_runtime_lookups"]), 1)
        self.assertEqual(report["selected_native_sources"][0], audit_module.identity(self.stub))

    def test_missing_real_definition_is_not_satisfied_by_source_inventory(self):
        source = self.root / "unrelated.c"
        source.write_text("int unrelated(void) { return 0; }\n", encoding="utf-8")
        report = self.run_audit(library=self.compile_library(source, "unrelated"))
        self.assertFalse(report["diagnostic_link_complete"])
        self.assertEqual(report["missing_direct_definitions"], [self.symbol_prefix + "s2a_handle_new"])

    def test_new_archive_import_needs_a_provider_without_updating_a_list(self):
        archive = self.root / "new-consumer.a"
        self.compile_archive("extern int s2a_new_required_provider(void);\n"
                             "int invoke_new(void) { return s2a_new_required_provider(); }\n", archive)
        report = self.run_audit(archive=archive)
        self.assertFalse(report["diagnostic_link_complete"])
        self.assertEqual(report["missing_direct_definitions"], [self.symbol_prefix + "s2a_new_required_provider"])

    def test_real_phase_owner_closes_import_but_source_name_alone_does_not(self):
        # The shared GAL introduced this DirectPInvoke even when ALT is unset.
        # Use the actual owner, not a fixture that merely exports the same name.
        source_root = HERE.parents[1]
        owner = source_root / "runtime/src/alt.c"
        clock_owner = source_root / "runtime/src/os.c"
        archive = self.root / "phase-consumer.a"
        self.compile_archive("extern int s2a_handle_new(void);\n"
                             "extern int s2a_alt_phase(void);\n"
                             "int invoke_phase(void) { return s2a_handle_new() + s2a_alt_phase(); }\n", archive)
        clock = self.root / "phase-clock.c"
        clock.write_text("#include <stdint.h>\nuint64_t plat_now_ns(void) { return 5000000000ull; }\n")
        sources = self.root / "phase-sources.txt"
        sources.write_text(f"{self.stub}\n{owner}\n{clock_owner}\n{clock}\n")
        missing = self.run_audit(archive=archive, source_list=sources)
        self.assertFalse(missing["diagnostic_link_complete"])
        self.assertEqual(missing["missing_direct_definitions"], [self.symbol_prefix + "s2a_alt_phase"])

        library = self.root / "real-phase.so"
        flag = "-dynamiclib" if sys.platform == "darwin" else "-shared"
        # 相位与 now_ns 都来自真实 owner；只在平台边界提供可控时钟输入。
        subprocess.run([self.cc, flag, "-fPIC", "-I", str(source_root / "runtime/include"),
                        str(self.stub), str(owner), str(clock_owner), str(clock), "-o", str(library)], check=True)
        report = self.run_audit(archive=archive, source_list=sources, library=library)
        self.assertTrue(report["diagnostic_link_complete"])
        self.assertFalse(report["game_ready"])
        self.assertIn(audit_module.identity(owner), report["selected_native_sources"])
        self.assertIn(audit_module.identity(clock_owner), report["selected_native_sources"])
        self.assertIn(self.symbol_prefix + "s2a_alt_phase", report["required_direct_symbols"])

    def test_symbols_satisfied_inside_archive_are_not_external_requirements(self):
        archive = self.root / "internal.a"
        self.compile_archive("extern int s2a_handle_new(void);\n"
                             "extern int s2a_internal(void);\n"
                             "int invoke(void) { return s2a_handle_new() + s2a_internal(); }\n", archive)
        provider = self.root / "internal-provider.c"
        obj = provider.with_suffix(".o")
        provider.write_text("int s2a_internal(void) { return 1; }\n", encoding="utf-8")
        subprocess.run([self.cc, "-fPIC", "-c", str(provider), "-o", str(obj)], check=True)
        subprocess.run(["ar", "rcs", str(archive), str(obj)], check=True)
        report = self.run_audit(archive=archive)
        self.assertEqual(report["required_direct_symbols"], [self.symbol_prefix + "s2a_handle_new"])

    def test_required_android_library_rejects_a_relocatable_object(self):
        with self.assertRaisesRegex(audit_module.AuditError, "ELF64"):
            self.run_audit(library=self.archive.with_suffix(".o"), android_elf=True)

    def test_missing_consumer_inventory_fails(self):
        with self.assertRaisesRegex(audit_module.AuditError, "consumer source inventory"):
            self.run_audit(consumer_root=self.root / "absent-consumers")

    def test_source_change_changes_report_identity(self):
        extra = self.root / "provider-source.c"
        extra.write_text("/* revision one */\n", encoding="utf-8")
        sources = self.root / "changing-sources.txt"
        sources.write_text(f"{self.stub}\n{extra}\n", encoding="utf-8")
        first = self.run_audit(source_list=sources)
        extra.write_text("/* revision two */\n", encoding="utf-8")
        second = self.run_audit(source_list=sources)
        self.assertNotEqual(first["selected_native_sources"][1]["sha256"],
                            second["selected_native_sources"][1]["sha256"])

    def test_runtime_export_is_inventoried_without_promoting_game_readiness(self):
        source = self.root / "visible-clock.c"
        source.write_text("int s2a_handle_new(void) { return 17; }\n"
                          "unsigned long long s2a_guest_tick(void) { return 12; }\n",
                          encoding="utf-8")
        report = self.run_audit(library=self.compile_library(source, "visible-clock"))
        self.assertEqual(report["missing_runtime_exports"], [])
        self.assertFalse(report["game_ready"])

    def test_binary_symbol_spelling_is_preserved(self):
        source = self.root / "distinct-names.c"
        source.write_text("int s2a_handle_new(void) { return 17; }\n"
                          "int _s2a_handle_new(void) { return 23; }\n", encoding="utf-8")
        defined, _ = audit_module.symbols(self.nm, self.compile_library(source, "distinct-names"),
                                         include_all=True)
        self.assertIn(self.symbol_prefix + "s2a_handle_new", defined)
        self.assertIn(self.symbol_prefix + "_s2a_handle_new", defined)

    def test_binary_symbols_with_spaces_are_not_split(self):
        name = "Internal_CompilerGenerated__Module___<CalliWithRuntimeMarshalling>UnmanagedCallingConvention, Static<Int32__IntPtr__UInt64>"
        source = self.root / "space-symbols.s"
        source.write_text(f'.text\n.globl "{name}"\n"{name}":\n.byte 0\n'
                          '.data\n.quad "s2a_missing symbol"\n', encoding="utf-8")
        obj = self.root / "space-symbols.o"
        subprocess.run([self.cc, "-c", str(source), "-o", str(obj)], check=True, capture_output=True)
        defined, undefined = audit_module.symbols(self.nm, obj, include_all=True)
        self.assertIn(name, defined)
        self.assertIn("s2a_missing symbol", undefined)

    def test_missing_or_wrong_managed_archive_fails(self):
        archive = self.root / "empty.a"
        self.compile_archive("int unrelated(void) { return 0; }\n", archive)
        with self.assertRaisesRegex(audit_module.AuditError, "no unresolved"):
            self.run_audit(archive=archive)

    def test_unrecognized_provider_mode_cannot_be_certified(self):
        sources = self.root / "no-stub.txt"
        sources.write_text(str(self.consumer / "Native.cs") + "\n", encoding="utf-8")
        with self.assertRaisesRegex(audit_module.AuditError, "diagnostic stub"):
            self.run_audit(source_list=sources)

    def test_game_link_is_explicit_and_never_promotes_game_readiness(self):
        report = self.run_audit(provider_role="game", source_list=self.game_sources,
                                library=self.game_library)
        self.assertEqual(report["scope"], "game-link-only")
        self.assertEqual(report["provider_role"], "game")
        self.assertTrue(report["game_link_complete"])
        self.assertTrue(report["provider_link_complete"])
        self.assertFalse(report["diagnostic_link_complete"])
        self.assertFalse(report["game_ready"])
        self.assertEqual(report["diagnostic_stub_sources"], [])
        self.assertEqual(len(report["selected_native_sources"]), len(self.game_paths))
        self.assertEqual(report["missing_runtime_exports"], ["s2a_guest_tick"])

    def test_game_rejects_diagnostic_stub_even_with_core_sources(self):
        sources = self.root / "game-with-stub.txt"
        sources.write_text(self.game_sources.read_text() + str(self.stub) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(audit_module.AuditError, "cannot include s2a_native_stub"):
            self.run_audit(provider_role="game", source_list=sources)

    def test_game_rejects_each_missing_core_seam(self):
        sources = self.root / "incomplete-game.txt"
        for missing in self.game_paths:
            with self.subTest(missing=missing):
                sources.write_text("\n".join(p for p in self.game_paths if p != missing) + "\n",
                                   encoding="utf-8")
                with self.assertRaisesRegex(audit_module.AuditError, "lacks production providers"):
                    self.run_audit(provider_role="game", source_list=sources)

    def test_game_source_inventory_cannot_replace_binary_definitions(self):
        source = self.root / "game-unrelated.c"
        source.write_text("int unrelated(void) { return 0; }\n", encoding="utf-8")
        report = self.run_audit(provider_role="game", source_list=self.game_sources,
                                library=self.compile_library(source, "game-unrelated"))
        self.assertFalse(report["game_link_complete"])
        self.assertFalse(report["provider_link_complete"])
        self.assertEqual(report["missing_direct_definitions"], [self.symbol_prefix + "s2a_handle_new"])

    @unittest.skipIf(sys.platform == "darwin", "POSIX Mach-O nm does not expose weak-definition flags")
    def test_game_import_cannot_be_satisfied_by_weak_provider(self):
        source = self.root / "weak-provider.c"
        source.write_text("__attribute__((weak)) int s2a_handle_new(void) { return 17; }\n",
                          encoding="utf-8")
        library = self.compile_library(source, "weak-provider")
        self.assertTrue(self.run_audit(library=library)["diagnostic_link_complete"])
        report = self.run_audit(provider_role="game", source_list=self.game_sources, library=library)
        self.assertFalse(report["game_link_complete"])
        self.assertEqual(report["missing_direct_definitions"], ["s2a_handle_new"])

    def test_unknown_provider_role_is_rejected(self):
        with self.assertRaisesRegex(audit_module.AuditError, "unknown provider role"):
            self.run_audit(provider_role="games")

    @unittest.skipIf(sys.platform == "darwin", "ELF section-boundary fixture runs on Linux")
    def test_game_cli_keeps_nativeaot_startup_boundaries_as_a_gate(self):
        archive = self.root / "game-startup.a"
        self.compile_archive("extern char __start___modules[], __stop___modules[];\n"
                             "extern int s2a_handle_new(void);\n"
                             "long startup(void) { return __stop___modules - __start___modules"
                             " + s2a_handle_new(); }\n", archive)
        modules = self.root / "game-modules.c"
        modules.write_text('__attribute__((used, section("__modules"))) '
                           'const void *module_pointer = (void *)1234;\n', encoding="utf-8")
        inputs = self.root / "game-link-inputs.txt"
        inputs.write_text(str(archive.with_suffix(".o")) + "\n", encoding="utf-8")
        report = self.root / "game-audit.json"
        library = self.root / "game-startup.so"
        command = [sys.executable, str(HERE / "audit.py"), "--provider-role", "game",
                   "--archive", str(archive), "--library", str(library),
                   "--source-list", str(self.game_sources), "--source-root", str(self.root),
                   "--consumer-root", str(self.consumer), "--nm", self.nm,
                   "--report", str(report), "--link-input-list", str(inputs),
                   "--readelf", "readelf"]
        link = [self.cc, "-shared", "-fPIC", str(archive.with_suffix(".o")),
                str(modules), str(self.game_provider), "-Wl,-z,defs", "-o", str(library)]
        subprocess.run(link, check=True)
        subprocess.run(command, check=True, capture_output=True)
        accepted = json.loads(report.read_text())
        self.assertTrue(accepted["game_link_complete"])
        self.assertTrue(accepted["nativeaot_startup"]["checked"])
        self.assertFalse(accepted["game_ready"])
        subprocess.run(link + ["-Wl,--defsym=__start___modules=0",
                               "-Wl,--defsym=__stop___modules=0"], check=True)
        result = subprocess.run(command, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        rejected = json.loads(report.read_text())
        self.assertFalse(rejected["game_link_complete"])
        self.assertTrue(any("boundary mismatch" in x for x in rejected["nativeaot_startup"]["failures"]))

    def test_cli_does_not_leave_stale_pass_report_after_invalid_input(self):
        report = self.root / "report.json"
        command = [sys.executable, str(HERE / "audit.py"), "--archive", str(self.archive),
                   "--library", str(self.library), "--source-list", str(self.sources),
                   "--source-root", str(self.root), "--consumer-root", str(self.consumer),
                   "--nm", self.nm, "--report", str(report),
                   "--link-input-list", str(self.root / "runtime-link.txt"), "--readelf", "readelf"]
        report.write_text(json.dumps(self.run_audit()) + "\n", encoding="utf-8")
        self.assertTrue(json.loads(report.read_text())["diagnostic_link_complete"])
        command[command.index("--archive") + 1] = str(self.root / "absent.a")
        result = subprocess.run(command, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(report.exists())

    def test_mode_gate_requires_android_static_game_before_any_core_execution(self):
        for arguments in (("game",), ("typo",), ("game", "-DANDROID=TRUE"),
                          ("game", "-DS2A_CS_SERVICES=static"),
                          ("game", "-DANDROID=TRUE", "-DS2A_CS_SERVICES=dynamic")):
            with self.subTest(arguments=arguments):
                result = subprocess.run(["cmake", f"-DS2A_ANDROID_NATIVE_MODE={arguments[0]}",
                                         *arguments[1:],
                                         "-P", str(HERE / "CoreLink.cmake")], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
        subprocess.run(["cmake", "-DS2A_ANDROID_NATIVE_MODE=diagnostic", "-P",
                        str(HERE / "CoreLink.cmake")], check=True)
        subprocess.run(["cmake", "-DS2A_ANDROID_NATIVE_MODE=game", "-DANDROID=TRUE",
                        "-DS2A_CS_SERVICES=static", "-P", str(HERE / "CoreLink.cmake")], check=True)


if __name__ == "__main__":
    unittest.main()
