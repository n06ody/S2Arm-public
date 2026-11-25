#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Inspect a final fragment module and emit a candidate proof and SPIR-V.

The replay caller still needs to authenticate the source and variant,
resolve the reflected bindings, and prove publication of the actual domain.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
from hashlib import sha256
import json
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

U32 = (1 << 32) - 1


class Reject(Exception):
    pass


def require(cond, why):
    if not cond:
        raise Reject(why)


def tool(*args):
    return subprocess.check_output([str(x) for x in args], text=True)


@dataclass(frozen=True)
class Ins:
    result: str | None
    op: str
    args: tuple[str, ...]
    line: int


class Module:
    def __init__(self, text):
        self.lines = [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith(";")]
        self.ins = []
        self.defs = {}
        self.uses = defaultdict(list)
        self.dec = defaultdict(dict)
        self.member = defaultdict(dict)
        self.blocks = defaultdict(list)
        self.loc = {}
        block = None
        function = None
        self.entry = None
        self.entry_name = None
        for i, raw in enumerate(self.lines):
            tok = shlex.split(raw, posix=False)
            named = len(tok) > 2 and tok[1] == "="
            o = Ins(tok[0] if named else None, tok[2] if named else tok[0], tuple(tok[3:] if named else tok[1:]), i)
            self.ins.append(o)
            if o.result:
                self.defs[o.result] = o
            for arg in o.args:
                if arg.startswith("%"):
                    self.uses[arg].append(o)
            if o.op == "OpDecorate":
                self.dec[o.args[0]][o.args[1]] = o.args[2:]
            if o.op == "OpMemberDecorate":
                self.member[o.args[0], int(o.args[1])][o.args[2]] = o.args[3:]
            if o.op == "OpEntryPoint":
                require(o.args[0] == "Fragment" and self.entry is None, "entry-point")
                self.entry = o.args[1]
                self.entry_name = o.args[2][1:-1]
            if o.op == "OpFunction":
                function = o.result
            if o.op == "OpFunctionEnd":
                block = None
                function = None
            if o.op == "OpLabel":
                block = o.result
            if block and function == self.entry:
                self.blocks[block].append(o)
                self.loc[o.line] = block
        require(self.entry and self.blocks, "entry-point")
        self.succ = {b: self.edges(ins[-1]) for b, ins in self.blocks.items()}
        self.pred = defaultdict(set)
        for b, nxt in self.succ.items():
            for n in nxt:
                self.pred[n].add(b)
        start = next(iter(self.blocks))
        reach = self.reach(start)
        self.dom = {b: ({b} if b == start else set(reach)) for b in reach}
        changed = True
        while changed:
            changed = False
            for b in reach - {start}:
                new = {b} | set.intersection(*(self.dom[p] for p in self.pred[b] if p in reach))
                if new != self.dom[b]:
                    self.dom[b] = new
                    changed = True

    @staticmethod
    def edges(i):
        if i.op == "OpBranch":
            return i.args
        if i.op == "OpBranchConditional":
            return i.args[1:3]
        if i.op == "OpSwitch":
            return i.args[1:2] + i.args[3::2]
        return ()

    def reach(self, first, stop=None):
        seen, work = set(), [first]
        while work:
            b = work.pop()
            if b == stop or b in seen:
                continue
            seen.add(b)
            work.extend(self.succ[b])
        return seen

    def dominates(self, defining, use):
        a, b = self.loc.get(defining.line), self.loc.get(use.line)
        return bool(a and b and (defining.line < use.line if a == b else a in self.dom.get(b, ())))

    def check_side_effects(self):
        for i in self.ins:
            if (i.op.startswith(("OpAtomic", "OpDemote", "OpBeginInvocationInterlock", "OpEndInvocationInterlock")) or
                i.op in {"OpKill", "OpTerminateInvocation", "OpImageWrite", "OpControlBarrier", "OpMemoryBarrier",
                         "OpCopyMemory", "OpCopyMemorySized", "OpUndef"}):
                raise Reject("module-side-effect:" + i.op)
            require(i.op not in {"OpDecorationGroup", "OpGroupDecorate", "OpGroupMemberDecorate",
                                 "OpDecorateId"}, "unsupported-decorations")
            if i.op == "OpExtInst":
                require(self.defs[i.args[1]].args == ('"GLSL.std.450"',), "extension-set")
            if i.op == "OpStore":
                ptr = self.defs[i.args[0]]
                pty = self.defs[ptr.args[0]]
                require(pty.op == "OpTypePointer" and pty.args[0] in {"Function", "Private", "Output"}, "nonlocal-store")
            if i.op == "OpVariable" and i.args[1] == "Output":
                sensitive = {("FragDepth",), ("SampleMask",)}
                pointee = self.defs[i.args[0]].args[1]
                require(self.dec[i.result].get("BuiltIn") not in sensitive and
                        not any(d.get("BuiltIn") in sensitive for (root, _), d in self.member.items() if root == pointee),
                        "depth-or-coverage-output")

    def const(self, val):
        i = self.defs.get(val)
        if i and i.op == "OpConstant" and self.defs[i.args[0]].op == "OpTypeInt" and self.defs[i.args[0]].args[0] == "32":
            return int(i.args[1], 0) & U32
        return None

    def scalar32(self, val):
        i = self.defs.get(val)
        if not i or not i.args:
            return False
        t = self.defs.get(i.args[0])
        return bool(t and t.op in {"OpTypeInt", "OpTypeFloat"} and t.args[0] == "32")

    def single_store(self, ptr, load):
        v = self.defs[ptr]
        if v.op != "OpVariable" or v.args[1:] != ("Function",):
            return None
        # Permit metadata and direct non-volatile loads/stores only.
        stores = []
        for use in self.uses[ptr]:
            if use.op in {"OpName", "OpDecorate"}:
                continue
            if use.op == "OpStore" and len(use.args) == 2 and use.args[0] == ptr:
                stores.append(use)
            elif use.op != "OpLoad" or len(use.args) != 2 or use.args[1] != ptr:
                return None
        if len(stores) == 1 and self.dominates(stores[0], load):
            return stores[0].args[1]
        return None

    def binding(self, root):
        d = self.dec[root]
        require("DescriptorSet" in d and "Binding" in d, "descriptor-binding")
        slot = int(d["DescriptorSet"][0]), int(d["Binding"][0])
        matching = [r for r, v in self.dec.items() if v.get("DescriptorSet") == d["DescriptorSet"] and v.get("Binding") == d["Binding"]]
        require(matching == [root], "ambiguous-descriptor-root")
        v = self.defs[root]
        require(v.op == "OpVariable", "descriptor-root")
        pty = self.defs[v.args[0]]
        require(pty.op == "OpTypePointer", "descriptor-pointer")
        block = self.defs[pty.args[1]]
        require(block.op not in {"OpTypeArray", "OpTypeRuntimeArray"}, "descriptor-array")
        require(block.op == "OpTypeStruct" and len(block.args) == 1, "descriptor-block")
        dblock = self.dec[block.result]
        require(self.member[block.result, 0].get("Offset") == ("0",), "member-offset")
        array = self.defs[block.args[0]]
        require(array.op in {"OpTypeArray", "OpTypeRuntimeArray"}, "member-array")
        storage = pty.args[0]
        require(storage in {"Uniform", "StorageBuffer"}, "descriptor-storage")
        is_ssbo = "BufferBlock" in dblock if storage == "Uniform" else "Block" in dblock
        return slot, block.result, array, is_ssbo

    def pointer(self, pointer):
        a = self.defs.get(pointer)
        require(a and a.op in {"OpAccessChain", "OpInBoundsAccessChain"}, "noncanonical-address")
        root = a.args[1]
        slot, block, array, ssbo = self.binding(root)
        path = a.args[2:]
        require(path and self.const(path[0]) == 0, "member-index")
        return slot, root, block, array, ssbo, path[1:]

    def expr(self, val, depth=0):
        require(depth < 96, "expression-depth")
        c = self.const(val)
        if c is not None:
            return c
        i = self.defs.get(val)
        if not i:
            return ("opaque", val)
        recur = lambda x: self.expr(x, depth + 1)
        if i.op == "OpLoad" and len(i.args) == 2:
            ptr = i.args[1]
            local = self.single_store(ptr, i)
            if local:
                return recur(local)
            p = self.defs[ptr]
            if p.op in {"OpAccessChain", "OpInBoundsAccessChain"} and p.args[1] in self.dec:
                d = self.dec[p.args[1]]
                if "DescriptorSet" in d or "Binding" in d:
                    slot, root, block, array, ssbo, path = self.pointer(ptr)
                    if not ssbo and len(path) == 2 and "Block" in self.dec[block]:
                        require("Volatile" not in self.dec[root] and "Volatile" not in self.member[block, 0],
                                "prefix-volatile")
                        vec = self.defs[array.args[0]]
                        vector, component = (self.const(x) for x in path)
                        require(array.op == "OpTypeArray" and self.dec[array.result].get("ArrayStride") == ("16",) and
                                vec.op == "OpTypeVector" and vec.args[1] == "4" and self.defs[vec.args[0]].op in {"OpTypeFloat", "OpTypeInt"} and
                                self.defs[vec.args[0]].args[0] == "32" and
                                vector is not None and component is not None and vector < self.const(array.args[1]) and component < 4,
                                "prefix-layout")
                        return ("prefix", slot, vector * 16 + component * 4)
        if i.op == "OpBitcast" and self.scalar32(val) and self.scalar32(i.args[1]):
            return recur(i.args[1])
        if i.op == "OpExtInst" and len(i.args) == 5 and i.args[2] == "UMin":
            require(self.defs[i.args[1]].args == ('"GLSL.std.450"',), "extension-set")
            return ("min", recur(i.args[3]), recur(i.args[4]))
        syntax = {"OpIAdd": "add", "OpISub": "sub", "OpIMul": "mul",
                  "OpBitwiseAnd": "and", "OpShiftRightLogical": "lsr"}
        if i.op in syntax and len(i.args) == 3 and self.scalar32(val):
            return (syntax[i.op], recur(i.args[1]), recur(i.args[2]))
        return ("opaque", val)

    def count_load(self, val, depth=0):
        require(depth < 40, "count-depth")
        i = self.defs[val]
        t = self.defs.get(i.args[0])
        require(t and t.op == "OpTypeInt" and t.args == ("32", "0"), "not-unsigned-u32-count")
        if i.op == "OpExtInst" and len(i.args) == 5 and i.args[2] == "UMin":
            require(self.defs[i.args[1]].args == ('"GLSL.std.450"',), "extension-set")
            lhs, rhs = i.args[3:]
            if self.const(lhs) is not None:
                lhs, rhs = rhs, lhs
            cap = self.const(rhs)
            require(cap is not None and cap > 0, "count-transform")
            return self.count_load(lhs, depth + 1)
        require(i.op == "OpLoad" and len(i.args) == 2, "count-load")
        ptr = i.args[1]
        local = self.single_store(ptr, i)
        if local:
            return self.count_load(local, depth + 1)
        slot, root, block, array, ssbo, path = self.pointer(ptr)
        require(ssbo and len(path) == 1 and self.dec[array.result].get("ArrayStride") == ("4",)
                and self.defs[array.args[0]].args == ("32", "0"), "counter-layout")
        require("NonWritable" in self.dec[root] or "NonWritable" in self.member[block, 0], "counter-writable")
        require("Volatile" not in self.dec[root] and "Volatile" not in self.member[block, 0], "counter-volatile")
        fixed_words = self.const(array.args[1]) if array.op == "OpTypeArray" else None
        return slot, self.expr(path[0]), fixed_words

    def on_zero(self, val):
        i = self.defs[val]
        sense = True
        while i.op == "OpLogicalNot":
            sense = not sense
            i = self.defs[i.args[1]]
        require(len(i.args) == 3, "not-simple-zero-branch")
        lhs, rhs = i.args[1:]
        zero_is_true = i.op in {"OpIEqual", "OpULessThanEqual", "OpUGreaterThanEqual"}
        if i.op in {"OpIEqual", "OpINotEqual"}:
            if self.const(lhs) == 0:
                lhs, rhs = rhs, lhs
        elif i.op in {"OpUGreaterThanEqual", "OpULessThan"}:
            lhs, rhs = rhs, lhs
        else:
            require(i.op in {"OpULessThanEqual", "OpUGreaterThan"}, "not-simple-zero-branch")
        require(self.const(rhs) == 0, "not-zero-compared")
        return lhs, (zero_is_true == sense)


def bounded(expr):
    if isinstance(expr, int):
        return expr
    if expr[0] in {"min", "and"}:
        a, b = bounded(expr[1]), bounded(expr[2])
        return min(a, b)
    return U32


def two(expr, op):
    require(isinstance(expr, tuple) and expr[0] == op and len(expr) == 3, "address-" + op)
    return expr[1:]


def split_const(expr, op):
    a, b = two(expr, op)
    if isinstance(a, int):
        a, b = b, a
    require(isinstance(b, int), "nonconstant-" + op)
    return a, b


def domain(slot, expr, fixed_words):
    byte_expr, shift = two(expr, "lsr")
    require(shift == 2, "word-shift")
    prefix = None
    mask = 0
    if isinstance(byte_expr, tuple) and byte_expr[0] == "sub":
        added, high_bits = two(byte_expr, "sub")
        p, high = split_const(high_bits, "and")
        require(isinstance(p, tuple) and p[0] == "prefix", "prefix-source")
        a, b = two(added, "add")
        require(p == a or p == b, "prefix-mismatch")
        byte_expr = b if p == a else a
        mask = high ^ U32
        require(mask <= 255 and not (mask & (mask + 1)), "prefix-mask")
        prefix = {"set": p[1][0], "binding": p[1][1], "ubo_byte": p[2], "mask": mask,
                  "required_runtime_source": "same_direct_DRV0"}
    index_stride, field = split_const(byte_expr, "add")
    index, stride = split_const(index_stride, "mul")
    high_index = bounded(index)
    require(0 < stride <= U32 and stride % 4 == 0 and field % 4 == 0 and field < stride and high_index < 65536,
            "unbounded-or-unaligned-domain")
    end = ((high_index * stride + field + mask) & ~3) + 4
    require(end <= U32, "address-wrap")
    if fixed_words is not None:
        require(end <= fixed_words * 4, "declared-array-range")
    return {"set": slot[0], "binding": slot[1], "descriptor_count": 1, "stride_bytes": stride,
            "field_byte": field, "record_min": 0, "record_max": high_index, "max_end_byte": end, "prefix": prefix}


def candidate_regions(m):
    groups = defaultdict(list)
    rejects = defaultdict(int)
    for label, b in m.blocks.items():
        if len(b) < 3 or b[-2].op != "OpSelectionMerge" or b[-1].op != "OpBranchConditional":
            continue
        branch = b[-1]
        merge = b[-2].args[0]
        yes, no = branch.args[1:3]
        if (yes == merge) == (no == merge):
            continue
        enter = no if yes == merge else yes
        region = m.reach(enter, merge)
        loops = [o for r in region for o in m.blocks[r] if o.op == "OpLoopMerge"]
        if not loops:
            continue
        try:
            require(len(loops) == 1 and all(m.pred[r] <= region | {label} for r in region) and
                    all(k in region or k == merge for r in region for k in m.succ[r]), "not-single-entry-exit")
            # Reject unrelated work in the guarded prefix, including another return.
            require(all(m.succ[r] or r == merge for r in region), "guard-region-terminal")
            target, zero_takes_yes = m.on_zero(branch.args[0])
            require((yes if zero_takes_yes else no) == merge, "zero-enters-region")
            slot, addr, size = m.count_load(target)
            d = domain(slot, addr, size)
            ops = [o for r in region for o in m.blocks[r]]
            groups[json.dumps(d, sort_keys=True)].append({
                "branch_line": branch.line, "guard_block": label, "enter_block": enter, "merge_block": merge,
                "loop_merge_block": loops[0].args[0], "zero_takes_yes": zero_takes_yes,
                "instructions": len(ops),
                "sample_instructions": sum(o.op.startswith("OpImageSample") for o in ops)
            })
        except Reject as ex:
            rejects[str(ex)] += 1
    return groups, rejects


def specialize(m, groups, out):
    # Rank solely by the work represented in the proved cold region.
    chosen = max(groups, key=lambda d: (sum(x["sample_instructions"] for x in groups[d]),
                                        sum(x["instructions"] for x in groups[d]), d))
    source = m.lines.copy()
    made = {}
    for region in groups[chosen]:
        line = region["branch_line"]
        o = m.ins[line]
        cond = m.defs[o.args[0]]
        ty = cond.args[0]
        pred = "OpConstantTrue" if region["zero_takes_yes"] else "OpConstantFalse"
        const = next((o.result for o in m.ins if o.op == pred and o.args == (ty,)), None)
        if const is None:
            const = "%cold_ir_" + ("true" if region["zero_takes_yes"] else "false")
            made[const] = f"{const} = {pred} {ty}"
        source[line] = " ".join(("OpBranchConditional", const, *o.args[1:]))
    declaration_end = next(i.line for i in m.ins if i.op == "OpFunction")
    source[declaration_end:declaration_end] = made.values()
    asm, raw, final = [out / name for name in ("B.spvasm", "B.raw.spv", "B.spv")]
    asm.write_text("\n".join(source) + "\n")
    tool("spirv-as", "--preserve-numeric-ids", "--target-env", "vulkan1.1", asm, "-o", raw)
    tool("spirv-val", "--target-env", "vulkan1.1", raw)
    tool("spirv-opt", "--target-env=vulkan1.1", "--preserve-bindings", "--eliminate-dead-branches",
         "--eliminate-dead-functions", "--cfg-cleanup", raw, "-o", final)
    tool("spirv-val", "--target-env", "vulkan1.1", final)
    return chosen, final


def package_variant(source, candidate, program_identity, output):
    """Format matched by android/native/nvn/nvn_vk_cold_asset.h."""
    domain = candidate["selected_domain"]
    prefix = domain["prefix"]
    variant = (output / "B.spv").read_bytes()
    require(candidate["entry"] == "main" and len(variant) <= 16 * 1024 * 1024 - 184,
            "unsupported-package-variant")
    metadata = struct.pack(
        "<8sII32s32s32s10I", b"S2ACOLD1", len(variant), 0, program_identity,
        sha256(source).digest(), sha256(variant).digest(),
        domain["set"], domain["binding"], domain["field_byte"] // 4,
        domain["stride_bytes"] // 4, domain["record_max"] + 1,
        prefix["set"] if prefix else 0, prefix["binding"] if prefix else 0,
        prefix["ubo_byte"] if prefix else 0, prefix["mask"] if prefix else 0, 0,
    )
    encoded = metadata + sha256(metadata + variant).digest() + variant
    out = output / "s2a-cold-guard-v1"
    out.mkdir(exist_ok=True)
    name = f"{program_identity.hex()}.bin"
    with tempfile.NamedTemporaryFile(prefix=".asset-", dir=out, delete=False) as file:
        file.write(encoded)
    Path(file.name).replace(out / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--program-identity", type=bytes.fromhex,
                        help="emit an APK asset for this exact 32-byte canonical program identity")
    args = parser.parse_args()
    if args.program_identity is not None and (len(args.program_identity) != 32 or not any(args.program_identity)):
        parser.error("--program-identity must encode a 32-byte nonzero hex digest")
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    blob = args.input.read_bytes()
    proof = out / "proof.json"
    variant = out / "B.spv"
    observation = {"rule": "s2a-cold-guard-v1", "source_sha256": sha256(blob).hexdigest(), "status": "rejected"}
    try:
        with tempfile.TemporaryDirectory(dir=out) as directory:
            workspace = Path(directory)
            source = workspace / "source.spv"
            source.write_bytes(blob)
            tool("spirv-val", "--target-env", "vulkan1.1", source)
            m = Module(tool("spirv-dis", source))
            observation["entry"] = m.entry_name
            m.check_side_effects()
            groups, refused = candidate_regions(m)
            observation["refused"] = dict(refused)
            observation["groups"] = [{"domain": json.loads(d), "regions": v} for d, v in groups.items()]
            require(groups, "no-proved-zero-entry")
            chosen, b = specialize(m, groups, workspace)
            observation.update(status="candidate", selected_domain=json.loads(chosen),
                               selected_regions=groups[chosen], variant_sha256=sha256(b.read_bytes()).hexdigest())
            b.replace(variant)
            if args.program_identity is not None:
                package_variant(blob, observation, args.program_identity, out)
    except Reject as e:
        observation["reason"] = str(e)
        observation["status"] = "rejected"
        variant.unlink(missing_ok=True)
        if args.program_identity is not None:
            (out / "s2a-cold-guard-v1" / (args.program_identity.hex() + ".bin")).unlink(missing_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", prefix=".proof-", dir=out, delete=False) as file:
        file.write(json.dumps(observation, indent=2) + "\n")
    Path(file.name).replace(proof)
    print(observation["status"], observation.get("reason", observation.get("selected_domain")))


if __name__ == "__main__":
    main()
