// SPDX-License-Identifier: Apache-2.0
// Optional reflection of a direct compute dispatch. A zero extent means unknown.
// The result is relative to the Vulkan descriptor, including any alignment prefix.
#pragma once
#include "nvn_backend.h"
#include "spirv_workgroup.h"
#include <stdlib.h>

struct s2a_write_bound {
  uint32_t group_stride, end_first_group;
};

struct wb_id {
  const uint32_t* def;
  uint32_t set, binding, builtin, stride, entry_block, visit;
  uint32_t values[2], stores;
  unsigned escaped : 1, chained : 1, member_zero : 1, block : 1, init_checked : 1, initialized : 1;
};

struct wb_module {
  const uint32_t* words;
  size_t count;
  uint32_t bound, local_x, epoch, budget;
  struct wb_id* ids;
  uint32_t* todo;
};

static unsigned wb_op(const uint32_t* p) {
  return p[0] & 65535;
}
static unsigned wb_len(const uint32_t* p) {
  return p[0] >> 16;
}
static struct wb_id* wb_id(struct wb_module* m, uint32_t id) {
  return &m->ids[id < m->bound ? id : 0];
}
static const uint32_t* wb_def(struct wb_module* m, uint32_t id, unsigned op, unsigned min) {
  const uint32_t* p = wb_id(m, id)->def;
  return p && wb_op(p) == op && wb_len(p) >= min ? p : NULL;
}
static int wb_scalar(struct wb_module* m, uint32_t type) {
  const uint32_t* p = wb_id(m, type)->def;
  return p && (wb_op(p) == SpvOpTypeInt || wb_op(p) == SpvOpTypeFloat) && p[2] == 32;
}
static int wb_constant(struct wb_module* m, uint32_t id, uint32_t* value) {
  const uint32_t* p = wb_def(m, id, SpvOpConstant, 4);
  if (!p || !wb_def(m, p[1], SpvOpTypeInt, 4) || !wb_scalar(m, p[1]))
    return 0;
  *value = p[3];
  return 1;
}

// Index only the instructions we can prove. Unknown pointer uses (including
// pointer arguments, atomics and copies) invalidate the referenced pointer.
static int wb_index(struct wb_module* m) {
  const uint32_t* end = m->words + m->count;
  unsigned fn = 0, entry = 0;
  for (const uint32_t* p = m->words + 5; p < end; p += wb_len(p)) {
    unsigned len = wb_len(p), op = wb_op(p), result = 0, min = 1;
    if (!len || (size_t)(end - p) < len)
      return 0;
    switch (op) {
      case SpvOpTypeInt:
      case SpvOpTypePointer:
        result = 1;
        min = 4;
        break;
      case SpvOpTypeFloat:
      case SpvOpTypeRuntimeArray:
      case SpvOpTypeStruct:
        result = 1;
        min = 3;
        break;
      case SpvOpLabel:
        result = 1;
        min = 2;
        break;
      case SpvOpVariable:
      case SpvOpConstant:
      case SpvOpLoad:
      case SpvOpBitcast:
      case SpvOpCopyObject:
        result = 2;
        min = 4;
        break;
      case SpvOpAccessChain:
      case SpvOpInBoundsAccessChain:
      case SpvOpCompositeExtract:
      case SpvOpIMul:
      case SpvOpIAdd:
      case SpvOpISub:
      case SpvOpBitwiseAnd:
      case SpvOpShiftLeftLogical:
      case SpvOpShiftRightLogical:
        result = 2;
        min = 5;
        break;
      case SpvOpFunction:
        fn = 1;
        entry = 0;
        break;
      case SpvOpFunctionEnd:
        fn = 0;
        break;
      case SpvOpMemoryModel:
        if (len != 3 || p[1] != SpvAddressingModelLogical)
          return 0;
        break;
      case SpvOpDecorationGroup:
      case SpvOpGroupDecorate:
      case SpvOpGroupMemberDecorate:
      case SpvOpDecorateId:
        return 0;
      case SpvOpCapability:
        if (len != 2 || p[1] == SpvCapabilityVariablePointers ||
            p[1] == SpvCapabilityVariablePointersStorageBuffer ||
            p[1] == SpvCapabilityPhysicalStorageBufferAddresses)
          return 0;
        break;
      case SpvOpDecorate:
        if (len == 3 && (p[2] == SpvDecorationBufferBlock || p[2] == SpvDecorationBlock))
          wb_id(m, p[1])->block = 1;
        if (len == 4) {
          struct wb_id* d = wb_id(m, p[1]);
          if (p[2] == SpvDecorationDescriptorSet)
            d->set = p[3] + 1;
          if (p[2] == SpvDecorationBinding)
            d->binding = p[3] + 1;
          if (p[2] == SpvDecorationBuiltIn)
            d->builtin = p[3] + 1;
          if (p[2] == SpvDecorationArrayStride)
            d->stride = p[3];
        }
        break;
      case SpvOpMemberDecorate:
        if (len == 5 && !p[2] && p[3] == SpvDecorationOffset && !p[4])
          wb_id(m, p[1])->member_zero = 1;
        break;
    }
    if (len < min)
      return 0;
    if (result) {
      if (!p[result] || p[result] >= m->bound)
        return 0;
      struct wb_id* d = &m->ids[p[result]];
      if (d->def)
        return 0;
      d->def = p;
      if (op == SpvOpLabel && fn && !entry)
        entry = p[result];
      d->entry_block = entry;
    }
  }
  for (const uint32_t* p = m->words + 5; p < end; p += wb_len(p)) {
    unsigned op = wb_op(p);
    if (op == SpvOpFunction)
      fn = 1;
    if (op == SpvOpFunctionEnd)
      fn = 0;
    if (!fn || op == SpvOpLine || op == SpvOpNoLine)
      continue;
    for (unsigned j = 1; j < wb_len(p); ++j) {
      if (op == SpvOpExtInst && j == 4)
        continue;  // extended instruction number is a literal
      struct wb_id* d = wb_id(m, p[j]);
      if (!d->def || d->def == p)
        continue;
      unsigned def = wb_op(d->def);
      if (def != SpvOpVariable && def != SpvOpAccessChain && def != SpvOpInBoundsAccessChain)
        continue;
      if (op == SpvOpLoad && j == 3)
        continue;
      if (op == SpvOpStore && j == 1 && wb_len(p) >= 3) {
        if (d->stores < 2)
          d->values[d->stores] = p[2];
        d->stores++;
      } else if ((op == SpvOpAccessChain || op == SpvOpInBoundsAccessChain) && j == 3) {
        d->chained = 1;
      } else
        d->escaped = 1;
    }
  }
  return 1;
}

// Follow only paths on which the local has not yet been assigned. This also
// checks joins and back edges; a single static store alone proves no dominance.
static int wb_initialized(struct wb_module* m, uint32_t id) {
  struct wb_id* v = wb_id(m, id);
  if (v->init_checked)
    return v->initialized;
  v->init_checked = 1;
  if (!v->entry_block || !v->stores || v->stores > 2 || v->escaped || v->chained ||
      wb_len(v->def) != 4)
    return 0;
  unsigned n = 0;
  ++m->epoch;
  m->todo[n++] = v->entry_block;
  wb_id(m, v->entry_block)->visit = m->epoch;
  while (n) {
    const uint32_t* p = wb_def(m, m->todo[--n], SpvOpLabel, 2);
    if (!p)
      return 0;
    for (p += wb_len(p); p < m->words + m->count; p += wb_len(p)) {
      if (!m->budget)
        return 0;
      --m->budget;
      unsigned op = wb_op(p);
      if (op == SpvOpStore && wb_len(p) >= 3 && p[1] == id)
        break;
      if (op == SpvOpLoad && p[3] == id)
        return 0;
      if (op == SpvOpReturn || op == SpvOpReturnValue || op == SpvOpUnreachable)
        break;
      if (op == SpvOpBranch || op == SpvOpBranchConditional) {
        unsigned first = op == SpvOpBranch ? 1 : 2, last = op == SpvOpBranch ? 1 : 3;
        if (wb_len(p) <= last)
          return 0;
        for (unsigned j = first; j <= last; ++j) {
          struct wb_id* next = wb_id(m, p[j]);
          if (!wb_def(m, p[j], SpvOpLabel, 2))
            return 0;
          if (next->visit == m->epoch)
            continue;
          next->visit = m->epoch;
          m->todo[n++] = p[j];
        }
        break;
      }
      if (op == SpvOpSwitch || op == SpvOpFunctionEnd || op == SpvOpLabel)
        return 0;
    }
  }
  return v->initialized = 1;
}

// Value <= group * WorkgroupID.x + tail. A driver UBO word can be carried
// symbolically through addition until its aligned base is subtracted.
struct wb_expr {
  uint32_t group, tail, base, cleared;
};
static int wb_same_base(struct wb_module* m, uint32_t a, uint32_t b) {
  if (a == b)
    return a != 0;
  if (!a || !b)
    return 0;
  const uint32_t *x = wb_id(m, a)->def, *y = wb_id(m, b)->def;
  return wb_len(x) == wb_len(y) && !memcmp(x + 3, y + 3, (wb_len(x) - 3) * sizeof(*x));
}
static int wb_builtin(struct wb_module* m, uint32_t var, struct wb_expr* out) {
  if (!wb_def(m, var, SpvOpVariable, 4) || wb_id(m, var)->def[3] != SpvStorageClassInput)
    return 0;
  switch (wb_id(m, var)->builtin) {
    case SpvBuiltInGlobalInvocationId + 1:
      out->group = m->local_x;
      out->tail = m->local_x - 1;
      return 1;
    case SpvBuiltInWorkgroupId + 1:
      out->group = 1;
      return 1;
    case SpvBuiltInLocalInvocationId + 1:
      out->tail = m->local_x - 1;
      return 1;
    default:
      return 0;
  }
}
static int wb_eval(struct wb_module* m, uint32_t id, uint32_t driver, unsigned depth,
                   struct wb_expr* out) {
  const uint32_t* p = wb_id(m, id)->def;
  if (!p || depth > 72 || !m->budget)
    return 0;
  --m->budget;
  *out = (struct wb_expr){0};
  if (wb_constant(m, id, &out->tail))
    return 1;
  if (!wb_scalar(m, p[1]))
    return 0;
  struct wb_expr a, b;
  uint32_t k;
  switch (wb_op(p)) {
    case SpvOpCopyObject:
    case SpvOpBitcast:
      return wb_eval(m, p[3], driver, depth + 1, out);
    case SpvOpCompositeExtract: {
      const uint32_t* load = wb_def(m, p[3], SpvOpLoad, 4);
      return wb_len(p) == 5 && !p[4] && load && wb_builtin(m, load[3], out);
    }
    case SpvOpLoad: {
      struct wb_id* src = wb_id(m, p[3]);
      const uint32_t* q = src->def;
      if (!q)
        return 0;
      if (wb_op(q) == SpvOpAccessChain || wb_op(q) == SpvOpInBoundsAccessChain) {
        if (wb_len(q) == 5 && wb_constant(m, q[4], &k) && !k && wb_builtin(m, q[3], out))
          return 1;
        if (!driver || q[3] != driver)
          return 0;
        for (unsigned j = 4; j < wb_len(q); j++)
          if (!wb_constant(m, q[j], &k))
            return 0;
        out->base = p[3];
        return 1;
      }
      if (wb_op(q) != SpvOpVariable || q[3] != SpvStorageClassFunction ||
          !wb_initialized(m, p[3]) || !wb_eval(m, src->values[0], driver, depth + 1, out))
        return 0;
      if (src->stores == 2) {
        if (!wb_eval(m, src->values[1], driver, depth + 1, &b) || memcmp(out, &b, sizeof b))
          return 0;
      }
      return 1;
    }
    case SpvOpIAdd:
      if (!wb_eval(m, p[3], driver, depth + 1, &a) || !wb_eval(m, p[4], driver, depth + 1, &b) ||
          a.cleared || b.cleared || (a.base && b.base) || UINT32_MAX - a.group < b.group ||
          UINT32_MAX - a.tail < b.tail)
        return 0;
      *out = (struct wb_expr){a.group + b.group, a.tail + b.tail, a.base ? a.base : b.base, 0};
      return 1;
    case SpvOpBitwiseAnd:
      if (!wb_constant(m, p[4], &k) || !wb_eval(m, p[3], driver, depth + 1, &a) || !a.base ||
          a.cleared || a.group || a.tail || k == UINT32_MAX || !k || ((~k) & (1u + ~k)))
        return 0;
      *out = a;
      out->cleared = ~k;
      return 1;
    case SpvOpISub:
      if (!wb_eval(m, p[3], driver, depth + 1, &a) || !wb_eval(m, p[4], driver, depth + 1, &b) ||
          !wb_same_base(m, a.base, b.base) || a.cleared || !b.cleared ||
          UINT32_MAX - a.tail < b.cleared)
        return 0;
      out->group = a.group;
      out->tail = a.tail + b.cleared;
      return 1;
    case SpvOpShiftRightLogical:
      if (!wb_constant(m, p[4], &k) || k >= 32 || !wb_eval(m, p[3], driver, depth + 1, &a) ||
          a.base || (a.group & ((1u << k) - 1)))
        return 0;
      out->group = a.group >> k;
      out->tail = a.tail >> k;
      return 1;
    case SpvOpShiftLeftLogical:
    case SpvOpIMul: {
      uint32_t input = p[3];
      if (!wb_constant(m, p[4], &k)) {
        if (wb_op(p) != SpvOpIMul || !wb_constant(m, p[3], &k))
          return 0;
        input = p[4];
      }
      if (wb_op(p) == SpvOpShiftLeftLogical) {
        if (k >= 32)
          return 0;
        k = 1u << k;
      }
      if (!wb_eval(m, input, driver, depth + 1, &a) || a.base ||
          (uint64_t)a.group * k > UINT32_MAX || (uint64_t)a.tail * k > UINT32_MAX)
        return 0;
      out->group = a.group * k;
      out->tail = a.tail * k;
      return 1;
    }
    default:
      return 0;
  }
}

static uint32_t wb_descriptor(struct wb_module* m, unsigned set, unsigned binding) {
  uint32_t var = 0;
  for (uint32_t i = 1; i < m->bound; ++i) {
    if (m->ids[i].set != set + 1 || m->ids[i].binding != binding + 1)
      continue;
    if (var || !wb_def(m, i, SpvOpVariable, 4))
      return 0;
    var = i;
  }
  return var;
}
static struct s2a_write_bound wb_slot(struct wb_module* m, uint32_t var, uint32_t driver) {
  struct s2a_write_bound unknown = {0}, result = {0};
  struct wb_id* root = wb_id(m, var);
  if (!var || root->escaped || root->stores)
    return unknown;
  const uint32_t* ptr = wb_def(m, root->def[1], SpvOpTypePointer, 4);
  if (!ptr || (ptr[2] != SpvStorageClassUniform && ptr[2] != SpvStorageClassStorageBuffer))
    return unknown;
  const uint32_t* block = wb_def(m, ptr[3], SpvOpTypeStruct, 3);
  if (!block || wb_len(block) != 3 || !m->ids[ptr[3]].member_zero || !m->ids[ptr[3]].block)
    return unknown;
  const uint32_t* array = wb_def(m, block[2], SpvOpTypeRuntimeArray, 3);
  if (!array || m->ids[block[2]].stride != 4 || !wb_scalar(m, array[2]))
    return unknown;
  for (uint32_t i = 1; i < m->bound; ++i) {
    struct wb_id* d = &m->ids[i];
    const uint32_t* p = d->def;
    if (!p || (wb_op(p) != SpvOpAccessChain && wb_op(p) != SpvOpInBoundsAccessChain) || p[3] != var)
      continue;
    if (d->chained || d->escaped)
      return unknown;
    if (!d->stores)
      continue;
    uint32_t member;
    struct wb_expr value;
    if (wb_len(p) != 6 || !wb_constant(m, p[4], &member) || member ||
        !wb_eval(m, p[5], driver, 0, &value) || value.base || value.group > UINT32_MAX / 4 ||
        value.tail >= UINT32_MAX / 4)
      return unknown;
    uint32_t stride = value.group * 4, end = value.tail * 4 + 4;
    if (stride > result.group_stride)
      result.group_stride = stride;
    if (end > result.end_first_group)
      result.end_first_group = end;
  }
  return result;
}

static struct s2a_write_bound* s2a_spirv_write_bounds(const void* blob, size_t bytes,
                                                      const char* entry,
                                                      const nvn_gpu_layout_ent* ents,
                                                      uint32_t count) {
  if (!count || !blob || ((uintptr_t)blob & 3) || bytes < 20 || bytes > (4u << 20))
    return NULL;
  uint32_t local[3];
  if (s2a_spirv_workgroup_size_ex(blob, bytes, entry, 1, local) || !local[0])
    return NULL;
  struct wb_module m = {.words = blob, .count = bytes / 4, .local_x = local[0], .budget = 1000000};
  m.bound = m.words[3];
  if (!m.bound || m.bound > 65536 || count > 256)
    return NULL;
  m.ids = calloc(m.bound, sizeof(*m.ids));
  m.todo = malloc((size_t)m.bound * sizeof(*m.todo));
  struct s2a_write_bound* bounds = NULL;
  if (m.ids && m.todo && wb_index(&m)) {
    uint32_t driver = 0;
    for (unsigned i = 0; i < count; ++i)
      if (ents[i].src_kind == NVN_RB_DRV0)
        driver = wb_descriptor(&m, ents[i].set, ents[i].binding);
    for (unsigned i = 0; i < count; ++i) {
      if (ents[i].src_kind != NVN_RB_SBUFP || ents[i].count != 1 ||
          !(ents[i].access & NVN_GPU_ACC_W))
        continue;
      struct s2a_write_bound b =
          wb_slot(&m, wb_descriptor(&m, ents[i].set, ents[i].binding), driver);
      if (!b.end_first_group)
        continue;
      if (!bounds)
        bounds = calloc(count, sizeof(*bounds));
      if (!bounds)
        break;
      bounds[i] = b;
    }
  }
  free(m.todo);
  free(m.ids);
  return bounds;
}
