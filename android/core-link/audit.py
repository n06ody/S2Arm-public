#!/usr/bin/env python3
"""Audit Android provider links, without claiming game readiness.

Static references come from the actual NativeAOT archive, not a maintained list
of C# declarations. The final library must define them locally. Literal runtime
lookups are reported separately: the linker cannot check dlsym consumers.
"""

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


class AuditError(Exception):
    pass


def identity(path):
    path = Path(path).resolve(strict=True)
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def android_build_id(readelf, library):
    if not readelf:
        raise AuditError('Android library build ID requires readelf')
    result = subprocess.run([readelf, '--notes', str(library)], capture_output=True, text=True, check=False)
    if result.returncode:
        raise AuditError('readelf failed to inspect Android library build ID')
    ids = re.findall(r'Build ID:\s*([0-9a-fA-F]+)', result.stdout)
    if len(ids) != 1 or not re.fullmatch(r'[0-9a-fA-F]{40}', ids[0]):
        raise AuditError('Android library missing unique SHA-1 GNU build ID')
    return ids[0].lower()


def symbols(nm, path, *, dynamic=False, include_local=False, include_all=False,
            strong_only=False):
    command = [nm, "--format=posix"]
    if not include_local:
        command.append("--extern-only")
    if dynamic:
        command.append("--dynamic")
    result = subprocess.run(command + [str(path)], capture_output=True, text=True, check=False)
    if result.returncode:
        raise AuditError(f"nm failed for {path}: {result.stderr.strip()}")
    defined, undefined = set(), set()
    for line in result.stdout.splitlines():
        # llvm-nm emits an archive/member heading ending in ':'. Its next lines
        # are POSIX symbol/type/value/size records. No guessed BSD column order.
        if not line.strip() or line.endswith(":"):
            continue
        # NativeAOT emits legal binary names containing spaces. Anchor the
        # numeric tail/type instead of splitting the symbol name into words.
        # Prefer full value/size records, then the shorter undefined forms.
        record = None
        for tail in (r"\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+", r"\s+[0-9a-fA-F]+", ""):
            record = re.fullmatch(r"(.+?)\s+([A-Za-z?])" + tail + r"\s*", line)
            if record:
                break
        if record is None:
            raise AuditError(f"unrecognized nm output for {path}: {line}")
        name, kind = record.groups()
        # Compare the exact binary names. Mach-O's C ABI underscore must not be
        # stripped from ELF names, where _s2a_x and s2a_x are distinct symbols.
        if not include_all and not name.startswith(("s2a_", "_s2a_")):
            continue
        if kind in ("U", "w", "v"):
            undefined.add(name)
        elif kind.upper() in ("T", "D", "B", "R", "S", "G", "C", "A", "W", "V", "I"):
            # Final static-archive definitions can be localized by --exclude-libs.
            # They still satisfy DirectPInvoke, but are not dynamic exports.
            if not strong_only or kind.upper() not in ("W", "V"):
                defined.add(name)
    return defined, undefined


def elf_data_strings(library):
    """NUL-delimited strings of the allocated, non-executable data sections of an ELF64 LE library.

    Symbol tables (.dynstr / .dynsym / hash / version) are left out: a name there is a symbol, not a
    literal the code can pass to dlsym. .symtab / .strtab are not allocated and are never read."""
    data = Path(library).read_bytes()
    if data[:4] != b"\x7fELF" or data[4:6] != b"\x02\x01":
        raise AuditError("hidden-lookup scan needs an ELF64 little-endian library")
    shoff = int.from_bytes(data[0x28:0x30], "little")
    shentsize = int.from_bytes(data[0x3A:0x3C], "little")
    shnum = int.from_bytes(data[0x3C:0x3E], "little")
    shstrndx = int.from_bytes(data[0x3E:0x40], "little")
    if not shoff or shentsize < 0x40 or shstrndx >= shnum:
        raise AuditError("ELF library has no usable section header table")
    def header(i):
        h = data[shoff + i * shentsize: shoff + (i + 1) * shentsize]
        return (int.from_bytes(h[0:4], "little"), int.from_bytes(h[4:8], "little"),
                int.from_bytes(h[8:16], "little"), int.from_bytes(h[24:32], "little"),
                int.from_bytes(h[32:40], "little"))
    _, _, _, names_off, names_size = header(shstrndx)
    names = data[names_off:names_off + names_size]
    skip = {".dynstr", ".dynsym", ".gnu.hash", ".hash", ".gnu.version", ".gnu.version_r", ".gnu.version_d"}
    strings = set()
    for i in range(shnum):
        name_off, kind, flags, offset, size = header(i)
        name = names[name_off:names.index(b"\0", name_off)].decode("latin-1")
        # SHF_ALLOC without SHF_EXECINSTR, and not SHT_NOBITS (.bss has no bytes in the file)
        if not flags & 0x2 or flags & 0x4 or kind == 8 or name in skip:
            continue
        strings.update(part.decode("latin-1") for part in data[offset:offset + size].split(b"\0") if part)
    return strings


def hidden_runtime_lookups(library, managed_defined, local_defined, exported):
    """Managed exports defined only as hidden symbols of this library whose exact name is also a data string.

    The single Android library links the NativeAOT archive with --exclude-libs,ALL: its exports satisfy
    direct references but are not in the dynamic table, so dlsym(RTLD_DEFAULT, "<name>") from native code
    in the same library returns NULL (010045B018EC2000: hle_nvn.c looked up S2A_NvMapFromGuest that way,
    got NULL on every Android run, gave the video player nvmap handle 0 and the game crashed in NvBlit).
    Such a literal can only feed a lookup that cannot succeed; a direct reference is the working form."""
    hidden = (managed_defined & local_defined) - exported
    return sorted(hidden & elf_data_strings(library))


def runtime_lookups(consumer_root):
    # This is an inventory of literal names, not a C# reachability/ABI proof.
    # A new nonliteral lookup must be reviewed; it is included as an unchecked
    # call site rather than silently counted as a resolved symbol.
    found, unchecked = {}, []
    call = re.compile(r"NativeLibrary\.(?:TryGetExport|GetExport)\s*\(")
    literal = re.compile(r',\s*"(s2a_[A-Za-z0-9_]+)"\s*[,)]')
    for source in sorted(Path(consumer_root).rglob("*.cs")):
        if any(part in ("obj", "bin", "build") for part in source.parts):
            continue
        text = source.read_text(encoding="utf-8")
        for match in call.finditer(text):
            # Each current lookup is contained in one statement. Bound the
            # scan at the next lookup too, so one unknown call cannot borrow
            # the following call's literal name.
            end = text.find(";", match.end())
            next_call = call.search(text, match.end())
            if end < 0:
                end = len(text)
            if next_call:
                end = min(end, next_call.start())
            name = literal.search(text, match.end(), end)
            site = {"path": str(source.resolve()), "line": text.count("\n", 0, match.start()) + 1}
            if name:
                found.setdefault(name.group(1), []).append(site)
            else:
                unchecked.append(site)
    return found, unchecked


def startup_roots(nm, readelf, link_input_list, library, final_defined, final_undefined):
    if not link_input_list:
        return {"checked": False, "reason": "No runtime link inputs supplied", "failures": []}
    inputs, roots, flags = [], set(), []
    for entry in Path(link_input_list).read_text(encoding="utf-8").splitlines():
        if not entry:
            continue
        if entry.startswith("-"):
            flags.append(entry)
            continue
        path = Path(entry)
        if not path.is_absolute():
            raise AuditError(f"runtime link input is not absolute: {entry}")
        inputs.append(identity(path))
        _, undefined = symbols(nm, path, include_all=True)
        roots.update(name for name in undefined if name.startswith(("__start_", "__stop_")))
    if not inputs or not roots:
        raise AuditError("runtime link inputs have no start/stop roots; wrong/empty input")
    if not readelf:
        raise AuditError("--readelf is required to inspect NativeAOT startup sections")
    result = subprocess.run([readelf, "--sections", "--symbols", "--wide", str(library)],
                            capture_output=True, text=True, check=False)
    if result.returncode:
        raise AuditError(f"readelf failed for {library}: {result.stderr.strip()}")
    sections = {}
    for match in re.finditer(r"^\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+([0-9a-fA-F]+)"
                            r"\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\S+\s+(\S+)",
                            result.stdout, re.MULTILINE):
        sections[match.group(2)] = {"index": match.group(1), "address": int(match.group(3), 16),
                                    "bytes": int(match.group(4), 16), "flags": match.group(5)}
    root_definitions = {name: set() for name in roots}
    for match in re.finditer(r"^\s*\d+:\s+([0-9a-fA-F]+)\s+\S+\s+\S+\s+\S+\s+\S+"
                            r"\s+(\S+)\s+(\S+)\s*$", result.stdout, re.MULTILINE):
        value, index, name = match.groups()
        if name in roots and index != "UND":
            root_definitions[name].add((int(value, 16), index))
    required_sections = {name.split("_", 3)[3] for name in roots}
    failures = []
    for name in sorted(roots):
        if name not in final_defined or name in final_undefined:
            failures.append(f"missing/unresolved NativeAOT startup symbol: {name}")
    for name in sorted(required_sections):
        if name not in sections or "A" not in sections[name]["flags"]:
            failures.append(f"NativeAOT startup section missing/not allocated: {name}")
    for name in sorted(roots):
        section = sections.get(name.split("_", 3)[3])
        if section is None:
            continue
        expected = section["address"] + (section["bytes"] if name.startswith("__stop_") else 0)
        definitions = root_definitions[name]
        # Linker-script absolute symbols are valid only at the exact boundary.
        # A same-valued symbol belonging to a different section is not a root.
        if not definitions or any(value != expected or index not in (section["index"], "ABS")
                                  for value, index in definitions):
            failures.append(f"NativeAOT startup boundary mismatch: {name}, expected 0x{expected:x} "
                            f"in section {section['index']} or ABS, found {sorted(definitions)}")
    if "__modules" in required_sections and not sections.get("__modules", {}).get("bytes", 0):
        failures.append("NativeAOT __modules section is empty/missing")
    return {"checked": True, "link_input_list": identity(link_input_list), "inputs": inputs,
            "listed_flags": flags, "required_symbols": sorted(roots),
            "sections": {name: sections.get(name) for name in sorted(required_sections)},
            "failures": failures}


def provider_sources(sources, role):
    stubs = [s for s in sources if Path(s["path"]).name == "s2a_native_stub.c"]
    if role == "diagnostic":
        if len(stubs) != 1:
            raise AuditError("expected exactly one diagnostic stub source in the CMake target")
        return stubs
    if stubs:
        raise AuditError("game provider cannot include s2a_native_stub.c")
    # Check the essential production seams in the actual flattened CMake list.
    # This is source provenance, not a replacement for archive-derived imports
    # or a claim that naming these files proves their runtime semantics.
    required = (
        "runtime/src/loader.c", "runtime/src/thread.c", "runtime/src/svc_mem.c",
        "runtime/src/gate.c", "runtime/src/entry.S", "runtime/src/guest_main.c",
        "graphics/track/track.c", "android/native/module.c",
        "android/native/plat_android.c",
        "android/native/runtime/runtime.c",
        "android/native/runtime/tracking/runtime.c",
        "android/native/runtime/fault/fault.c",
    )
    missing = [suffix for suffix in required
               if not any(Path(source["path"]).parts[-len(Path(suffix).parts):] == Path(suffix).parts
                          for source in sources)]
    if missing:
        raise AuditError("game CMake source inventory lacks production providers: " + ", ".join(missing))
    return stubs


def audit(*, archive, library, source_list, source_root, consumer_root, nm, android_elf=False,
          link_input_list=None, readelf=None, provider_role="diagnostic"):
    if provider_role not in ("diagnostic", "game"):
        raise AuditError(f"unknown provider role: {provider_role}")
    archive, library = Path(archive), Path(library)
    consumers = [p for p in sorted(Path(consumer_root).rglob("*.cs"))
                 if not any(part in ("obj", "bin", "build") for part in p.parts)]
    if not consumers:
        raise AuditError("consumer source inventory is empty or missing")
    archive_defined, archive_undefined = symbols(nm, archive)
    local_defined, local_undefined = symbols(nm, library, include_local=True, include_all=True,
                                            strong_only=provider_role == "game")
    # ELF dynamic exports are distinct from hidden definitions used to satisfy
    # DirectPInvoke. A local definition is not enough for a runtime lookup.
    header = library.read_bytes()[:20]
    is_elf = header[:4] == b"\x7fELF"
    is_macho = header[:4] in (b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
                              b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
                              b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                              b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca")
    if android_elf and (not is_elf or header[4:6] != b"\x02\x01"
                        or header[16:20] != b"\x03\x00\xb7\x00"):
        raise AuditError("final Android library must be ELF64 little-endian AArch64 ET_DYN")
    exported, _ = symbols(nm, library, dynamic=is_elf, include_all=True)
    # Source-level dlsym names use the platform's C ABI spelling in nm output.
    # This mapping is only for source lookup inventory, never binary comparison.
    def export_visible(name):
        return ("_" + name if is_macho else name) in exported
    required = archive_undefined - archive_defined
    if not required:
        raise AuditError("archive has no unresolved s2a_* imports; wrong/empty consumer input")
    sources = []
    for entry in Path(source_list).read_text(encoding="utf-8").splitlines():
        if not entry.strip():
            continue
        # CMake 给目标里由自定义命令生成的源（如 nvn_names_android.c）另记一条 <输出>.rule 伪源，
        # 磁盘上没有这个文件；生成出来的 .c 本身照常在清单里、照常审计。
        if entry.endswith(".rule"):
            continue
        path = Path(entry)
        if not path.is_absolute():
            path = Path(source_root) / path
        sources.append(identity(path))
    if not sources:
        raise AuditError("CMake source inventory is empty")
    stubs = provider_sources(sources, provider_role)
    lookups, unchecked = runtime_lookups(consumer_root)
    missing = required - local_defined
    unresolved = required & local_undefined
    hidden_lookups = []
    if android_elf:
        managed_all, _ = symbols(nm, archive, include_all=True)
        hidden_lookups = hidden_runtime_lookups(library, managed_all, local_defined, exported)
        # 原生对托管导出的弱引用（同库服务层的正确取法）必须在最终库里解析掉：归档定义了、库里还是 w / U，就是没链上
        hidden_lookups += sorted(f"{name} (unresolved reference)" for name in managed_all & local_undefined)
    startup = startup_roots(nm, readelf, link_input_list, library, local_defined, local_undefined)
    native_identity = identity(library)
    if android_elf:
        native_identity["build_id"] = android_build_id(readelf, library)
    link_complete = not missing and not unresolved and not startup["failures"] and not hidden_lookups
    return {
        "schema_version": 1,
        "scope": f"{provider_role}-link-only",
        "provider_role": provider_role,
        "game_ready": False,
        "runtime_export_evidence": "ELF dynsym" if is_elf else "host symbol table only",
        "artifacts": {"managed_archive": identity(archive), "native_library": native_identity,
                      "cmake_sources": identity(source_list)},
        "consumer_sources": [identity(p) for p in consumers],
        "selected_native_sources": sources,
        "diagnostic_stub_sources": stubs,
        "required_direct_symbols": sorted(required),
        "missing_direct_definitions": sorted(missing),
        "unresolved_direct_symbols": sorted(unresolved),
        "runtime_lookups": [{"name": name, "exported": export_visible(name), "sites": sites}
                            for name, sites in sorted(lookups.items())],
        "unchecked_runtime_lookups": unchecked,
        "missing_runtime_exports": sorted(name for name in lookups if not export_visible(name)),
        "hidden_runtime_lookups": hidden_lookups,
        "nativeaot_startup": startup,
        "service_entrypoints": {"S2A_Init": {"defined": ("_S2A_Init" if is_macho else "S2A_Init") in local_defined,
                                             "exported": export_visible("S2A_Init"),
                                             "executed": False}},
        "diagnostic_link_complete": provider_role == "diagnostic" and link_complete,
        "game_link_complete": provider_role == "game" and link_complete,
        "provider_link_complete": link_complete,
        "limits": ["Does not execute S2A_Init or a guest.",
                   "Runtime lookup visibility from the Android main-program handle is not tested.",
                   "Symbol presence does not prove ABI, provider semantics or platform closure.",
                   "Runtime lookup inventory is literal-source based, not managed reachability."],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--source-list", required=True)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--consumer-root", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--android-elf", action="store_true")
    parser.add_argument("--provider-role", choices=("diagnostic", "game"), default="diagnostic")
    parser.add_argument("--link-input-list", required=True)
    parser.add_argument("--readelf", required=True)
    args = vars(parser.parse_args())
    report_path = Path(args.pop("report"))
    # Never leave a previous successful report after this invocation fails.
    report_path.unlink(missing_ok=True)
    try:
        report = audit(**args)
    except (AuditError, OSError) as error:
        print(f"Android {args['provider_role']} core-link audit failed: {error}")
        return 1
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if not report["provider_link_complete"]:
        print(f"Android {report['provider_role']} core-link audit failed: missing/unresolved providers: "
              + ", ".join(sorted(set(report["missing_direct_definitions"])
                                 | set(report["unresolved_direct_symbols"]))))
        for failure in report["nativeaot_startup"]["failures"]:
            print(f"  {failure}")
        for name in report["hidden_runtime_lookups"]:
            if name.endswith(" (unresolved reference)"):
                print(f"  【红】 native reference to a managed export left unresolved in the final library: {name}")
            else:
                print(f"  【红】 native runtime lookup of a hidden in-library managed export: {name} "
                      f"(--exclude-libs,ALL keeps it out of the dynamic table; dlsym gets NULL — reference it directly)")
        return 1
    print(f"Android {report['provider_role']} link complete ({len(report['required_direct_symbols'])} imports); "
          f"game_ready=false; report: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
