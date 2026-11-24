// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#pragma once
#include "../../../graphics/third_party/spirv_cross/spirv.h"

// A pool table describes all available views. Only promote an element to a
// sample when the installed module has one unconditional, constant choice.
// Unhandled SPIR-V leaves that binding unproven.
static uint32_t frame_pool_index(const uint32_t *words, size_t bytes, unsigned set, unsigned binding) {
    if (!words || bytes < 20 || bytes % 4 || words[0] != SpvMagicNumber ||
        !words[3] || words[3] > 65536) return UINT32_MAX;
    struct id_info { uint32_t literal; unsigned flags; };
    enum { SET=1, BIND=2, CONSTANT=4, POINTER=8, IMAGE=16, INT32=32 };
    uint32_t bound=words[3], var=0, index=UINT32_MAX;
    struct id_info *ids=calloc(bound,sizeof *ids);
    if (!ids) return UINT32_MAX;
    size_t count=bytes/4;
    for (size_t at=5; at<count;) {
        const uint32_t *q=words+at;
        unsigned n=q[0]>>16, op=q[0]&65535;
        if (!n || n>count-at) goto unknown;
        if (op==SpvOpDecorate && n==4 && q[1]<bound) {
            if (q[2]==SpvDecorationDescriptorSet && q[3]==set) ids[q[1]].flags |= SET;
            if (q[2]==SpvDecorationBinding && q[3]==binding) ids[q[1]].flags |= BIND;
        }
        if (op==SpvOpTypeInt && n==4 && q[1]<bound && q[2]==32) ids[q[1]].flags |= INT32;
        if (op==SpvOpConstant && n==4 && q[1]<bound && q[2]<bound && (ids[q[1]].flags & INT32)) {
            ids[q[2]].flags |= CONSTANT;
            ids[q[2]].literal=q[3];
        }
        at+=n;
    }
    for (unsigned i=1; i<bound; i++) if ((ids[i].flags & (SET|BIND))==(SET|BIND)) {
        if (var) goto unknown;
        var=i;
    }
    if (!var) goto unknown;
    unsigned functions=0, blocks=0, fetches=0;
    for (size_t at=5; at<count; at+=words[at]>>16) {
        const uint32_t *q=words+at;
        unsigned n=q[0]>>16, op=q[0]&65535;
        if (op==SpvOpFunction) functions++;
        if (!functions) continue;
        if (op==SpvOpLabel) blocks++;
        if (functions>1 || blocks>1 || op==SpvOpFunctionCall || op==SpvOpKill ||
            op==SpvOpTerminateInvocation || op==SpvOpDemoteToHelperInvocation) goto unknown;
        for (unsigned j=1; j<n; j++) if (q[j]==var) {
            if ((op!=SpvOpAccessChain && op!=SpvOpInBoundsAccessChain) ||
                n!=5 || j!=3 || q[2]>=bound || q[4]>=bound || !(ids[q[4]].flags & CONSTANT)) goto unknown;
            uint32_t elem=ids[q[4]].literal;
            if (elem==UINT32_MAX || (index!=UINT32_MAX && index!=elem)) goto unknown;
            index=elem;
            ids[q[2]].flags |= POINTER;
        }
        if (n>=4 && q[2]<bound && q[3]<bound) {
            if (op==SpvOpLoad && (ids[q[3]].flags & POINTER)) ids[q[2]].flags |= IMAGE;
            if ((op==SpvOpSampledImage || op==SpvOpImage) && (ids[q[3]].flags & IMAGE))
                ids[q[2]].flags |= IMAGE;
            if (op>=SpvOpImageSampleImplicitLod && op<=SpvOpImageDrefGather && (ids[q[3]].flags & IMAGE))
                fetches++;
        }
    }
    if (functions!=1 || blocks!=1 || !fetches) goto unknown;
    free(ids);
    return index;
unknown:
    free(ids);
    return UINT32_MAX;
}
