// SPDX-License-Identifier: Apache-2.0
// One private pipeline and one coherent result buffer for this recorded draw.
// The existing command-end GPU-write -> host-read barrier and submission fence
// cover these writes, just like attachment readback.
static void roi_fragment_free(struct roi_fragment* f) {
  if (f->pool)
    vkDestroyDescriptorPool(g.d, f->pool, NULL);
  destroy_pipe_objects(f->variant);
  if (f->buffer)
    nvn_gpu_buf_release(f->buffer, NULL, NULL);
  free(f->module.words);
  free(f);
}

static void roi_fragment_report(nvn_gpu_cmd c, struct roi_fragment* f, uint64_t signal) {
  const uint32_t* result = (const uint32_t*)(const void*)f->buffer->mapped;
  unsigned count = result[0];
  if (!count || count > ROI_TRACE_CAP)
    roi_message("event=gap frame=%d pass=%u draw=%u pixel=%u,%u count=%u reason=%s", roi_target,
                f->pass, f->draw, f->x, f->y, count,
                count ? "fragment_buffer_full" : "no_fragment_events");
  for (unsigned i = 0; i < count && i < ROI_TRACE_CAP; ++i) {
    const uint32_t* record = result + 1 + i * ROI_TRACE_WORDS;
    if (record[0] >= f->module.sites) {
      roi_gap(c, "unknown_fragment_site");
      continue;
    }
    const struct roi_site* site = &f->module.site[record[0]];
    float v[8];
    memcpy(v, record + 1, sizeof v);
    if (!site->dimensions) {
      roi_message(
          "event=shader_constant frame=%d cmd=%u pass=%u draw=%u signal=%llu pixel=%u,%u "
          "set=%u binding=%u offset=%u value=%.9g",
          roi_target, c->roi->id, f->pass, f->draw, (unsigned long long)signal, f->x, f->y,
          site->set, site->binding, site->offset, v[4]);
    } else {
      char coords[64];
      if (site->dimensions == 3)
        snprintf(coords, sizeof coords, "%.9g,%.9g,%.9g", v[0], v[1], v[2]);
      else
        snprintf(coords, sizeof coords, "%.9g,%.9g", v[0], v[1]);
      roi_message(
          "event=shader_sample frame=%d cmd=%u pass=%u draw=%u signal=%llu pixel=%u,%u "
          "set=%u binding=%u view=%llu coord=%s rgba=%.9g,%.9g,%.9g,%.9g",
          roi_target, c->roi->id, f->pass, f->draw, (unsigned long long)signal, f->x, f->y,
          site->set, site->binding, (unsigned long long)f->view[record[0]], coords, v[4], v[5],
          v[6], v[7]);
    }
  }
}

static uint64_t roi_fragment_view(nvn_gpu_cmd c, nvn_gpu_pipe p, const struct roi_site* site) {
  const unsigned st = NVN_GPU_SH_FS;
  for (unsigned i = 0; i < c->nbinds[st]; ++i) {
    const nvn_gpu_binding* b = &c->binds[st][i].b;
    if (b->ent >= p->stage_nent[st] || b->elem || (b->ent < 64 && c->pool_binds[st][b->ent]))
      continue;
    const nvn_gpu_layout_ent* e = &p->stage_ents[st][b->ent];
    if (e->set != site->set || e->binding != site->binding || e->count > 1)
      continue;
    nvn_gpu_view v = b->cls == NVN_GPU_RES_TEX        ? b->u.view
                     : b->cls == NVN_GPU_RES_TEX_SAMP ? b->u.ts.view
                                                      : NULL;
    if (v)
      return (uintptr_t)feedback_view(c, v);
  }
  return 0;
}

static int roi_fragment_pipeline(nvn_gpu_cmd c, nvn_gpu_pipe p, struct roi_fragment* f) {
  const nvn_gpu_shader fs = p->key.st[NVN_GPU_SH_FS].sh;
  if (!fs || !fs->roi_spv ||
      !roi_instrument(fs->roi_spv, fs->roi_words, p->nset, f->x, f->y, roi_ubo_set, roi_ubo_binding,
                      roi_ubo_byte, &f->module)) {
    roi_gap(c, "fragment_module_or_sample_site");
    return 0;
  }
  nvn_gpu_layout_ent* ent = calloc(fs->nent + 1, sizeof *ent);
  if (!ent) {
    roi_gap(c, "fragment_layout_allocation");
    return 0;
  }
  if (fs->nent)
    memcpy(ent, fs->ents, fs->nent * sizeof *ent);
  ent[fs->nent] = (nvn_gpu_layout_ent){.set = p->nset,
                                       .cls = NVN_GPU_RES_SSBO,
                                       .count = 1,
                                       .stages = 1u << NVN_GPU_SH_FS,
                                       .access = NVN_GPU_ACC_R | NVN_GPU_ACC_W};
  nvn_gpu_layout layout = {
      .ent = ent, .nent = fs->nent + 1, .push_off = fs->push_off, .push_bytes = fs->push_bytes};
  nvn_gpu_shader_src src = {.kind = NVN_GPU_BLOB_SPIRV,
                            .stage = NVN_GPU_SH_FS,
                            .entry = fs->entry,
                            .blob = f->module.words,
                            .bytes = f->module.n * sizeof(uint32_t),
                            .layout = &layout};
  nvn_gpu_shader shader = NULL;
  nvn_gpu_err err = {0};
  nvn_gpu_rc r = nvn_gpu_shader_load(&src, &shader, &err);
  free(ent);
  free(f->module.words);
  f->module.words = NULL;
  if (nvn_gpu_ok(r)) {
    nvn_gpu_pipe_desc desc = p->key;
    desc.st[NVN_GPU_SH_FS].sh = shader;
    int serial = pipe_serial_on();
    if (serial)
      pthread_mutex_lock(&g_create_mu);
    r = pipe_build(&desc, &f->variant, &err);
    if (serial)
      pthread_mutex_unlock(&g_create_mu);
    nvn_gpu_shader_release(shader);
  }
  if (!nvn_gpu_ok(r)) {
    roi_message("event=gap frame=%d pass=%u reason=fragment_pipeline detail=%s", roi_target,
                c->roi->pass, err.why);
    return 0;
  }
  // Vulkan set compatibility also requires identical push constant ranges.
  // All original stage ranges were copied by pipe_build; verify both here.
  if (f->variant->nset != p->nset + 1 ||
      memcmp(p->dsl, f->variant->dsl, p->nset * sizeof p->dsl[0]) ||
      memcmp(p->stage_push_off, f->variant->stage_push_off, sizeof p->stage_push_off) ||
      memcmp(p->stage_push_bytes, f->variant->stage_push_bytes, sizeof p->stage_push_bytes)) {
    roi_gap(c, "fragment_layout_prefix");
    return 0;
  }
  return 1;
}

static void roi_fragment_begin(nvn_gpu_cmd c, nvn_gpu_pipe p) {
  unsigned pass = c->roi->pass;
  if (!pass || (pass != (unsigned)roi_volume_pass && pass != (unsigned)roi_uv_pass))
    return;
  if (!g.dev->core_features.fragmentStoresAndAtomics || p->nset >= 4 ||
      p->nset >= g.caps.max_sets) {
    roi_gap(c, "fragment_storage_or_descriptor_limit");
    return;
  }
  struct roi_fragment* f = calloc(1, sizeof *f);
  if (!f) {
    roi_gap(c, "fragment_allocation");
    return;
  }
  f->pass = pass;
  f->draw = c->roi->draws + 1;
  f->x = pass == (unsigned)roi_volume_pass ? roi_src_x : roi_x;
  f->y = pass == (unsigned)roi_volume_pass ? roi_src_y : roi_y;
  if (!roi_fragment_pipeline(c, p, f))
    goto fail;
  VkDeviceSize bytes = sizeof(uint32_t) * (1 + ROI_TRACE_WORDS * ROI_TRACE_CAP);
  nvn_gpu_err err;
  if (!nvn_gpu_ok(buf_create_impl(bytes, NVN_GPU_MEM_SHARED, NULL, 0, &f->buffer, &err))) {
    roi_gap(c, "fragment_readback_buffer");
    goto fail;
  }
  memset(f->buffer->mapped, 0, bytes);
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                   .maxSets = 1,
                                   .poolSizeCount = 1,
                                   .pPoolSizes = &size};
  if (vkCreateDescriptorPool(g.d, &pi, NULL, &f->pool) != VK_SUCCESS) {
    roi_gap(c, "fragment_descriptor_pool");
    goto fail;
  }
  VkDescriptorSet set;
  VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                    .descriptorPool = f->pool,
                                    .descriptorSetCount = 1,
                                    .pSetLayouts = &f->variant->dsl[p->nset]};
  if (vkAllocateDescriptorSets(g.d, &ai, &set) != VK_SUCCESS) {
    roi_gap(c, "fragment_descriptor_set");
    goto fail;
  }
  VkDescriptorBufferInfo bi = {f->buffer->buffer, 0, bytes};
  VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                .pBufferInfo = &bi};
  vkUpdateDescriptorSets(g.d, 1, &write, 0, NULL);
  for (unsigned i = 0; i < f->module.sites; ++i) {
    if (!f->module.site[i].dimensions)
      continue;
    f->view[i] = roi_fragment_view(c, p, &f->module.site[i]);
    if (!f->view[i])
      roi_gap(c, "fragment_descriptor_identity");
  }
  f->next = c->roi->fragments;
  c->roi->fragments = f;
  vkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, f->variant->pipe);
  vkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, f->variant->layout, p->nset, 1,
                          &set, 0, NULL);
  c->es.gpipe = VK_NULL_HANDLE;  // next ordinary draw restores its own pipeline
  return;
fail:
  roi_fragment_free(f);
}
