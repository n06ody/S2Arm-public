// Debug pass metadata. The stage key is opaque byte order, the SHA is of the
// final code passed to vkCreateShaderModule (including any Android rewrite).
enum { PASS_FS_KILL = 1, PASS_FS_DEMOTE = 2, PASS_FS_DEPTH_OR_MASK = 4, PASS_FS_STORAGE = 8 };

static void pass_shader_dump(const struct nvn_gpu_shader_s *sh, const void *blob, size_t size) {
    if (sh->capture_key != g.pass_capture_key || !g.pass_capture_key) return;
    if (size > 256 * 1024) {
        nvn_plat_log(NVN_PLAT_LOG_WARN, "pass-spv", "selected module exceeds dump limit");
        return;
    }
    const unsigned char *b = blob;
    static const char hex[] = "0123456789abcdef";
    char line[900];
    for (size_t off = 0; off < size; off += 384) {
        size_t count = size - off < 384 ? size - off : 384;
        size_t used = (size_t)snprintf(line, sizeof line, "sha=%s bytes=%zu off=%zu data=", sh->capture_sha, size, off);
        for (size_t i = 0; i < count; i++) {
            line[used++] = hex[b[off + i] >> 4];
            line[used++] = hex[b[off + i] & 15];
        }
        line[used] = 0;
        nvn_plat_log(NVN_PLAT_LOG_INFO, "pass-spv", line);
    }
}

static void pass_shader_identify(struct nvn_gpu_shader_s *sh, const uint8_t key[8],
                                 const void *blob, size_t size) {
    const unsigned char *b = blob;
    for (unsigned i = 0; i < 8; i++) sh->capture_key = (sh->capture_key << 8) | key[i];
    s2a_sha256 ctx;
    uint8_t digest[32];
    s2a_sha256_init(&ctx);
    s2a_sha256_update(&ctx, blob, size);
    s2a_sha256_final(&ctx, digest);
    s2a_sha256_hex(digest, sh->capture_sha);
    // Some replay artifact paths have no guest stage key. Use the first eight
    // bytes of the exact Vulkan module digest for diagnostic selection.
    for (unsigned i = 0; i < 8; i++) sh->capture_digest_prefix = (sh->capture_digest_prefix << 8) | digest[i];
    if (!sh->capture_key) sh->capture_key = sh->capture_digest_prefix;
    if (g.pass_capture_delay <= 0) return;
    sh->capture_flags = s2a_spirv_storage_need(blob, size, 4) != 0 ? PASS_FS_STORAGE : 0;
    size_t n = size / 4;
    for (size_t i = 5; i < n;) {
        uint32_t word = s2a_spv_word(b + i * 4), len = word >> 16, op = word & 0xffffu;
        if (!len || len > n - i) { sh->capture_flags |= PASS_FS_STORAGE; break; }
        // SPIR-V core grammar: OpKill / TerminateInvocation / DemoteToHelperInvocation.
        if (op == 252 || op == 4416) sh->capture_flags |= PASS_FS_KILL;
        if (op == 5380) sh->capture_flags |= PASS_FS_DEMOTE;
        if (op == 16 && len >= 3) {   // OpExecutionMode: DepthReplacing / StencilRefReplacingEXT
            uint32_t mode = s2a_spv_word(b + (i + 2) * 4);
            if (mode == 12 || mode == 5027) sh->capture_flags |= PASS_FS_DEPTH_OR_MASK;
        }
        if ((op == 71 && len >= 4 && s2a_spv_word(b + (i + 2) * 4) == 11 &&
             s2a_spv_word(b + (i + 3) * 4) == 20) || // OpDecorate BuiltIn SampleMask
            (op == 72 && len >= 5 && s2a_spv_word(b + (i + 3) * 4) == 11 &&
             s2a_spv_word(b + (i + 4) * 4) == 20))   // OpMemberDecorate BuiltIn SampleMask
            sh->capture_flags |= PASS_FS_DEPTH_OR_MASK;
        i += len;
    }
    pass_shader_dump(sh, blob, size);
}

// Build a second pipeline with the same layout, render pass and static state.
// The cached game pipeline and descriptor preparation remain authoritative.
static void pass_build_constant(struct nvn_gpu_pipe_s *p, const nvn_gpu_pipe_desc *d,
                                const VkGraphicsPipelineCreateInfo *gp) {
    const struct nvn_gpu_shader_s *fs = d->st[NVN_GPU_SH_FS].sh;
    if (!g.pass_capture_blank || !fs || fs->capture_key != g.pass_capture_key) return;
    if (d->dfmt || d->sfmt || d->samples > 1 || fs->capture_flags & (PASS_FS_STORAGE | PASS_FS_DEPTH_OR_MASK))
        goto unsupported;
    const uint32_t n = gp->pColorBlendState->attachmentCount;
    nvn_fmt_info f0, f1;
    if ((n != 1 && n != 2) || !nvn_fmt_info_of(d->cfmt[0], &f0) ||
        f0.kind == NVN_FMT_UINT || f0.kind == NVN_FMT_SINT ||
        (n == 2 && (!nvn_fmt_info_of(d->cfmt[1], &f1) || f1.kind != NVN_FMT_UINT)))
        goto unsupported;
    for (uint32_t i = 0; i < n; i++) {
        const VkPipelineColorBlendAttachmentState *b = &gp->pColorBlendState->pAttachments[i];
        if (b->blendEnable && (b->srcColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
            b->dstColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
            b->srcAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
            b->dstAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR)) goto unsupported;
    }

    VkShaderModuleCreateInfo module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = n == 2 ? sizeof pass_constant_color_uint : sizeof pass_constant_color,
        .pCode = n == 2 ? pass_constant_color_uint : pass_constant_color};
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g.d, &module, NULL, &mod) != VK_SUCCESS) goto unsupported;
    VkPipelineShaderStageCreateInfo stages[5];
    memcpy(stages, gp->pStages, gp->stageCount * sizeof stages[0]);
    for (uint32_t i = 0; i < gp->stageCount; i++) {
        if (stages[i].stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
        stages[i].module = mod;
        stages[i].pName = "main";
        stages[i].pSpecializationInfo = NULL;
    }
    VkGraphicsPipelineCreateInfo alt = *gp;
    alt.pStages = stages;
    VkResult r = vkCreateGraphicsPipelines(g.d, VK_NULL_HANDLE, 1, &alt, NULL, &p->capture_constant);
    vkDestroyShaderModule(g.d, mod, NULL);
    if (r == VK_SUCCESS) return;
unsupported:
    nvn_plat_log(NVN_PLAT_LOG_WARN, "pass-spv", "constant replacement rejected: pipeline or fragment interface unsupported");
}
