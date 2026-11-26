// SPDX-License-Identifier: Apache-2.0
// Instrument existing SSA values. In particular, never issue a second sample:
// a second texture lookup could use different derivatives, coordinates or LOD.
#include "../../../graphics/third_party/spirv_cross/spirv.h"
enum { ROI_TRACE_CAP = 64, ROI_TRACE_WORDS = 9, ROI_TRACE_SITES = 96 };
struct roi_site {
  unsigned set, binding, offset, dimensions;  // dimensions=0 denotes a scalar constant load
};
struct roi_module {
  uint32_t* words;
  size_t n;
  unsigned sites;
  struct roi_site site[ROI_TRACE_SITES];
};
struct roi_ir_buf {
  uint32_t* p;
  size_t n, cap;
  int bad;
};
static void roi_ir_append(struct roi_ir_buf* b, const uint32_t* w, size_t n) {
  if (!n)
    return;
  if (b->bad || n > SIZE_MAX / 8 - b->n) {
    b->bad = 1;
    return;
  }
  if (n + b->n > b->cap) {
    size_t cap = 2 * (b->n + n) + 64;
    uint32_t* p = realloc(b->p, sizeof(*p) * cap);
    if (!p) {
      b->bad = 1;
      return;
    }
    b->p = p;
    b->cap = cap;
  }
  memcpy(b->p + b->n, w, n * sizeof(*w));
  b->n += n;
}
static void roi_ir_op(struct roi_ir_buf* b, SpvOp op, const uint32_t* args, size_t n) {
  uint32_t head = ((uint32_t)(n + 1) << 16) | op;
  roi_ir_append(b, &head, 1);
  if (n)
    roi_ir_append(b, args, n);
}
#define ROI_OP(b, op, ...)                                            \
  do {                                                                \
    const uint32_t args_[] = {__VA_ARGS__};                           \
    roi_ir_op((b), SpvOp##op, args_, sizeof(args_) / sizeof(*args_)); \
  } while (0)
struct roi_ir_id {
  const uint32_t* def;
  uint32_t set, binding, stride;
  int has_binding;
};
struct roi_ir {
  struct roi_ir_id* id;
  uint32_t bound, next;
  struct roi_ir_buf decl;
};
static const uint32_t* roi_def(const struct roi_ir* s, uint32_t id) {
  return id < s->bound ? s->id[id].def : NULL;
}
static int roi_opcode(const uint32_t* p) {
  return p ? (int)(p[0] & 65535) : -1;
}
static uint32_t roi_type(struct roi_ir* s, SpvOp op, const uint32_t* args, unsigned n) {
  for (unsigned i = 1; i < s->bound; i++) {
    const uint32_t* p = s->id[i].def;
    if (p && p[0] == ((n + 2) << 16 | op) && !memcmp(p + 2, args, n * 4))
      return i;
  }
  // Callers request each missing type only once.
  uint32_t id = s->next++, head = (n + 2) << 16 | op;
  roi_ir_append(&s->decl, &head, 1);
  roi_ir_append(&s->decl, &id, 1);
  if (n)
    roi_ir_append(&s->decl, args, n);
  return id;
}
static uint32_t roi_const(struct roi_ir* s, uint32_t type, uint32_t v) {
  uint32_t id = s->next++;
  ROI_OP(&s->decl, Constant, type, id, v);
  return id;
}
static const uint32_t* roi_image_variable(struct roi_ir* s, uint32_t image) {
  const uint32_t* p = roi_def(s, image);
  if (roi_opcode(p) == SpvOpSampledImage)
    p = roi_def(s, p[3]);
  if (roi_opcode(p) != SpvOpLoad)
    return NULL;
  p = roi_def(s, p[3]);
  return roi_opcode(p) == SpvOpVariable ? p : NULL;
}
// Resolve the common std140 block { vec4 values[N]; }. Require the actual
// layout decorations and constant indices rather than inferring a byte offset
// from an identifier or a compiler's resource name.
static int roi_ubo_offset(struct roi_ir* s, const uint32_t* w, size_t n, const uint32_t* load,
                          uint32_t* var, uint32_t* byte) {
  const uint32_t* a = roi_def(s, load[3]);
  if (roi_opcode(a) != SpvOpAccessChain || a[0] >> 16 != 7)
    return 0;
  const uint32_t* v = roi_def(s, a[3]);
  if (roi_opcode(v) != SpvOpVariable || v[3] != SpvStorageClassUniform)
    return 0;
  const uint32_t* ptr = roi_def(s, v[1]);
  if (roi_opcode(ptr) != SpvOpTypePointer)
    return 0;
  const uint32_t *block = roi_def(s, ptr[3]), *idx[3];
  if (roi_opcode(block) != SpvOpTypeStruct)
    return 0;
  for (unsigned k = 0; k < 3; k++) {
    idx[k] = roi_def(s, a[4 + k]);
    if (roi_opcode(idx[k]) != SpvOpConstant || idx[k][0] >> 16 != 4)
      return 0;
  }
  uint32_t member = idx[0][3];
  if (member >= (block[0] >> 16) - 2 || idx[2][3] > 3)
    return 0;
  const uint32_t* array = roi_def(s, block[2 + member]);
  if (roi_opcode(array) != SpvOpTypeArray)
    return 0;
  const uint32_t* vec = roi_def(s, array[2]);
  if (roi_opcode(vec) != SpvOpTypeVector || vec[3] != 4 || vec[2] != load[1])
    return 0;
  uint32_t stride = s->id[array[1]].stride;
  if (!stride)
    return 0;
  for (size_t i = 5; i < n; i += w[i] >> 16) {
    const uint32_t* p = w + i;
    if (roi_opcode(p) == SpvOpMemberDecorate && p[0] >> 16 == 5 && p[1] == block[1] &&
        p[2] == member && p[3] == SpvDecorationOffset) {
      uint64_t off = p[4] + (uint64_t)idx[1][3] * stride + 4 * idx[2][3];
      if (off > UINT32_MAX)
        return 0;
      *byte = (uint32_t)off;
      *var = v[2];
      return 1;
    }
  }
  return 0;
}
// The module comes from the same validated bytes used to create the original
// VkShaderModule. Unsupported descriptor expressions stay unobserved.
static int roi_instrument(const uint32_t* w, size_t n, unsigned set, unsigned px, unsigned py,
                          unsigned ubo_set, unsigned ubo_binding, unsigned ubo_byte,
                          struct roi_module* out) {
  if (n < 5 || w[0] != SpvMagicNumber || !w[3] || w[3] > 1000000 || n > 4000000)
    return 0;
  struct roi_ir s = {.bound = w[3], .next = w[3]};
  struct roi_ir_buf dec = {0}, body = {0}, fn = {0}, dst = {0};
  s.id = calloc(s.bound, sizeof(*s.id));
  if (!s.id)
    return 0;
  size_t types = 0, funcs = 0, entry = 0;
  uint32_t frag = 0;
  int ok = 0;
  for (size_t i = 5; i < n;) {
    const uint32_t* p = w + i;
    unsigned z = p[0] >> 16, op = p[0] & 65535;
    if (!z || z > n - i)
      goto done;
    if (op == SpvOpEntryPoint) {
      if (entry || z < 4 || p[1] != SpvExecutionModelFragment)
        goto done;
      entry = i;
    }
    if (op == SpvOpFunction && !funcs)
      funcs = i;
    if (op >= SpvOpTypeVoid && op <= SpvOpTypeFunction && !types)
      types = i;
    unsigned at = 0;
    if (op >= SpvOpTypeVoid && op <= SpvOpTypeFunction)
      at = 1;
    else if (op == SpvOpConstant || op == SpvOpVariable || op == SpvOpLoad ||
             op == SpvOpAccessChain || op == SpvOpSampledImage || op == SpvOpCompositeConstruct ||
             op == SpvOpVectorShuffle || op == SpvOpSelect || op == SpvOpCopyObject ||
             op == SpvOpFAdd || op == SpvOpFMul)
      at = 2;
    if (at && z > at && p[at] < s.bound)
      s.id[p[at]].def = p;
    if (op == SpvOpDecorate && z == 4 && p[1] < s.bound) {
      struct roi_ir_id* d = s.id + p[1];
      if (p[2] == SpvDecorationBinding) {
        d->binding = p[3];
        d->has_binding = 1;
      }
      if (p[2] == SpvDecorationDescriptorSet)
        d->set = p[3];
      if (p[2] == SpvDecorationArrayStride)
        d->stride = p[3];
      if (p[2] == SpvDecorationBuiltIn && p[3] == SpvBuiltInFragCoord)
        frag = p[1];
    }
    i += z;
  }
  if (!types || !funcs || !entry)
    goto done;
#define TY(op, ...)                                            \
  roi_type(&s, SpvOpType##op, (const uint32_t[]){__VA_ARGS__}, \
           sizeof((const uint32_t[]){__VA_ARGS__}) / 4)
  uint32_t void_t = roi_type(&s, SpvOpTypeVoid, (uint32_t[]){0}, 0);
  uint32_t bool_t = roi_type(&s, SpvOpTypeBool, (uint32_t[]){0}, 0);
  uint32_t u = TY(Int, 32, 0), f = TY(Float, 32), v4 = TY(Vector, f, 4);
  uint32_t input = TY(Pointer, SpvStorageClassInput, v4),
           function_type = TY(Function, void_t, u, v4, v4);
  uint32_t storage = w[1] >= 0x10300 ? SpvStorageClassStorageBuffer : SpvStorageClassUniform;
  uint32_t arr = s.next++, block = s.next++, blockp = s.next++, uintp = TY(Pointer, storage, u);
  uint32_t buf = s.next++, helper = s.next++;
  ROI_OP(&s.decl, TypeRuntimeArray, arr, u);
  ROI_OP(&s.decl, TypeStruct, block, u, arr);
  ROI_OP(&s.decl, TypePointer, blockp, storage, block);
  ROI_OP(&s.decl, Variable, blockp, buf, storage);
  ROI_OP(&dec, Decorate, arr, SpvDecorationArrayStride, 4);
  ROI_OP(&dec, Decorate, block,
         storage == SpvStorageClassUniform ? SpvDecorationBufferBlock : SpvDecorationBlock);
  ROI_OP(&dec, MemberDecorate, block, 0, SpvDecorationOffset, 0);
  ROI_OP(&dec, MemberDecorate, block, 1, SpvDecorationOffset, 4);
  ROI_OP(&dec, Decorate, buf, SpvDecorationDescriptorSet, set);
  ROI_OP(&dec, Decorate, buf, SpvDecorationBinding, 0);
  int new_frag = !frag;
  if (new_frag) {
    frag = s.next++;
    ROI_OP(&s.decl, Variable, input, frag, SpvStorageClassInput);
    ROI_OP(&dec, Decorate, frag, SpvDecorationBuiltIn, SpvBuiltInFragCoord);
  }
  uint32_t zero = roi_const(&s, u, 0), one = roi_const(&s, u, 1),
           cap = roi_const(&s, u, ROI_TRACE_CAP);
  uint32_t stride = roi_const(&s, u, ROI_TRACE_WORDS), tx = roi_const(&s, u, px),
           ty = roi_const(&s, u, py);
  uint32_t f0 = roi_const(&s, f, 0), zeros = s.next++;
  ROI_OP(&s.decl, ConstantComposite, v4, zeros, f0, f0, f0, f0);
  for (size_t i = funcs; i < n; i += w[i] >> 16) {
    const uint32_t* p = w + i;
    unsigned op = p[0] & 65535, z = p[0] >> 16;
    roi_ir_append(&body, p, z);
    uint32_t var = 0, offset = 0, dim = 0, coord = 0, value = 0;
    if ((op == SpvOpImageSampleImplicitLod || op == SpvOpImageSampleExplicitLod) && z >= 5 &&
        p[1] == v4) {
      const uint32_t *v = roi_image_variable(&s, p[3]), *src = roi_def(&s, p[4]);
      if (!v || !src)
        continue;
      const uint32_t* t = roi_def(&s, src[1]);
      if (roi_opcode(t) != SpvOpTypeVector || t[2] != f || (t[3] != 2 && t[3] != 3))
        continue;
      var = v[2];
      dim = t[3];
      coord = s.next++;
      value = p[2];
      ROI_OP(&body, VectorShuffle, v4, coord, p[4], zeros, 0, 1, dim == 3 ? 2 : dim, dim);
    } else if (op == SpvOpLoad && z >= 4 && p[1] == f &&
               roi_ubo_offset(&s, w, n, p, &var, &offset) && s.id[var].set == ubo_set &&
               s.id[var].binding == ubo_binding && offset == ubo_byte) {
      coord = zeros;
      value = s.next++;
      ROI_OP(&body, CompositeConstruct, v4, value, p[2], f0, f0, f0);
    } else
      continue;
    if (out->sites >= ROI_TRACE_SITES || !s.id[var].has_binding)
      goto done;
    unsigned ix = out->sites++;
    out->site[ix] = (struct roi_site){s.id[var].set, s.id[var].binding, offset, dim};
    uint32_t tag = roi_const(&s, u, ix);
    ROI_OP(&body, FunctionCall, void_t, s.next++, helper, tag, coord, value);
  }
  if (!out->sites)
    goto done;
  uint32_t tag = s.next++, coord = s.next++, value = s.next++;
  ROI_OP(&fn, Function, void_t, helper, 0, function_type);
  ROI_OP(&fn, FunctionParameter, u, tag);
  ROI_OP(&fn, FunctionParameter, v4, coord);
  ROI_OP(&fn, FunctionParameter, v4, value);
  ROI_OP(&fn, Label, s.next++);
  uint32_t fc = s.next++, match[2];
  ROI_OP(&fn, Load, v4, fc, frag);
  for (unsigned k = 0; k < 2; k++) {
    uint32_t c = s.next++, ic = s.next++;
    match[k] = s.next++;
    ROI_OP(&fn, CompositeExtract, f, c, fc, k);
    ROI_OP(&fn, ConvertFToU, u, ic, c);
    ROI_OP(&fn, IEqual, bool_t, match[k], ic, k ? ty : tx);
  }
  uint32_t xy = s.next++, hit = s.next++, end = s.next++;
  ROI_OP(&fn, LogicalAnd, bool_t, xy, match[0], match[1]);
  ROI_OP(&fn, SelectionMerge, end, 0);
  ROI_OP(&fn, BranchConditional, xy, hit, end);
  ROI_OP(&fn, Label, hit);
  uint32_t head = s.next++, row = s.next++, room = s.next++, store = s.next++, join = s.next++;
  ROI_OP(&fn, AccessChain, uintp, head, buf, zero);
  ROI_OP(&fn, AtomicIAdd, u, row, head, one, zero, one);
  ROI_OP(&fn, ULessThan, bool_t, room, row, cap);
  ROI_OP(&fn, SelectionMerge, join, 0);
  ROI_OP(&fn, BranchConditional, room, store, join);
  ROI_OP(&fn, Label, store);
  uint32_t base = s.next++;
  ROI_OP(&fn, IMul, u, base, row, stride);
  for (unsigned k = 0; k < ROI_TRACE_WORDS; k++) {
    uint32_t idx = s.next++, ptr = s.next++, v = tag;
    uint32_t delta = roi_const(&s, u, k);
    ROI_OP(&fn, IAdd, u, idx, base, delta);
    ROI_OP(&fn, AccessChain, uintp, ptr, buf, one, idx);
    if (k) {
      uint32_t part = s.next++;
      v = s.next++;
      ROI_OP(&fn, CompositeExtract, f, part, k <= 4 ? coord : value, (k - 1) % 4);
      ROI_OP(&fn, Bitcast, u, v, part);
    }
    ROI_OP(&fn, Store, ptr, v);
  }
  ROI_OP(&fn, Branch, join);
  ROI_OP(&fn, Label, join);
  ROI_OP(&fn, Branch, end);
  ROI_OP(&fn, Label, end);
  roi_ir_op(&fn, SpvOpReturn, NULL, 0);
  roi_ir_op(&fn, SpvOpFunctionEnd, NULL, 0);
  roi_ir_append(&dst, w, 5);
  for (size_t i = 5; i < funcs; i += w[i] >> 16) {
    unsigned z = w[i] >> 16;
    if (i == types)
      roi_ir_append(&dst, dec.p, dec.n);
    if (i == entry) {
      uint32_t h = w[i] + ((new_frag + (w[1] >= 0x10400)) << 16);
      roi_ir_append(&dst, &h, 1);
      roi_ir_append(&dst, w + i + 1, z - 1);
      if (new_frag)
        roi_ir_append(&dst, &frag, 1);
      if (w[1] >= 0x10400)
        roi_ir_append(&dst, &buf, 1);
    } else
      roi_ir_append(&dst, w + i, z);
  }
  roi_ir_append(&dst, s.decl.p, s.decl.n);
  roi_ir_append(&dst, body.p, body.n);
  roi_ir_append(&dst, fn.p, fn.n);
  if (!dst.bad && !s.decl.bad && !dec.bad && !body.bad && !fn.bad) {
    dst.p[3] = s.next;
    out->words = dst.p;
    out->n = dst.n;
    dst.p = NULL;
    ok = 1;
  }
done:
  free(s.id);
  free(s.decl.p);
  free(dec.p);
  free(body.p);
  free(fn.p);
  free(dst.p);
  return ok;
#undef TY
}
#undef ROI_OP
