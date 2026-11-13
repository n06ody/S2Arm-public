// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Synthetic test: exercises first_draw binding and support bytes, not shader sampling.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_vk_first_draw.h"
#include "nvn_vk_texcache.h"
#include "nvn_obj.h"
static struct nvn_vk_tex *g_rt, *g_tex, *g_tex2;
static int phase;
int __wrap_s2a_vk_adapter_tex_by_id(int id, struct nvn_vk_tex **out, char *w, size_t n) {
  (void)w;
  (void)n;
  printf("resolved id=%d\n", id);
  *out = id == 1 ? g_tex : g_tex2;
  return (id == 1 || id == 2) ? 0 : -1;
}
int __wrap_s2a_vk_adapter_samp_by_id(int id, samp_par *p) {
  (void)id;
  memset(p, 0, sizeof *p);
  p->minf = NVN_MIN_FILTER_LINEAR;
  p->magf = NVN_MAG_FILTER_LINEAR;
  p->ws = p->wt = p->wr = NVN_WRAP_MODE_REPEAT;
  p->lod_max = 10;
  return 0;
}
int __wrap_s2a_vk_adapter_tex_view_by_id(int id, view_t *v) {
  if (!phase || getenv("BUFFER"))
    return 0;
  memset(v, 0, sizeof *v);
  v->set_target = 1;
  v->target = id == 1 ? 1 : 7;
  return 1;
}
VkImageView __wrap_s2a_vk_adapter_tex_view(struct nvn_vk_tex *t, const view_t *v, char *w,
                                           size_t n) {
  (void)v;
  (void)w;
  (void)n;
  return t->view;
}
struct nvn_vk_tex *__wrap_s2a_vk_adapter_tex(const tex_t *t, char *w, size_t n) {
  (void)t;
  (void)w;
  (void)n;
  return g_rt;
}
VkImageView __wrap_s2a_vk_adapter_attachment_view(struct nvn_vk_tex *t, const view_t *v,
                                                  VkFormat *f, char *w, size_t n) {
  (void)v;
  (void)w;
  (void)n;
  *f = VK_FORMAT_R8G8B8A8_UNORM;
  return t->view_att ? t->view_att : t->view;
}
enum { N = 128, SZ = 2608 };
static struct {
  VkDeviceMemory mem;
  VkDeviceSize size, off, mapped_size;
  unsigned char *map;
} M[N];
static struct {
  VkBuffer b;
  VkDeviceMemory mem;
  VkDeviceSize off;
} B[N];
static int nm, nb;
static unsigned char *ptr[4], frozen[4][SZ];
static VkDeviceSize offsets[4];
static int nsnap, draw_calls;
// The fixture owns these three texcache entries; observe their destruction
// without taking over ownership or hiding validation messages.
static VkImage owned_images[3];
static VkImageView owned_views[6];
static VkDeviceMemory owned_memory[3];
static unsigned images_destroyed[3], views_destroyed[6], memory_freed[3];
static int nviews;
void __real_vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *);
void __wrap_vkDestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks *a) {
  for (int i = 0; i < 3; ++i)
    if (owned_images[i] && owned_images[i] == image)
      ++images_destroyed[i];
  __real_vkDestroyImage(d, image, a);
}
void __real_vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *);
void __wrap_vkDestroyImageView(VkDevice d, VkImageView view, const VkAllocationCallbacks *a) {
  for (int i = 0; i < nviews; ++i)
    if (owned_views[i] == view)
      ++views_destroyed[i];
  __real_vkDestroyImageView(d, view, a);
}
void __real_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
void __wrap_vkFreeMemory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks *a) {
  for (int i = 0; i < 3; ++i)
    if (owned_memory[i] && owned_memory[i] == memory)
      ++memory_freed[i];
  __real_vkFreeMemory(d, memory, a);
}
static void remember_view(VkImageView view) {
  if (!view)
    return;
  for (int i = 0; i < nviews; ++i)
    if (owned_views[i] == view)
      return;
  owned_views[nviews++] = view;
}
void __real_vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
void __wrap_vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t first,
                      uint32_t base) {
  ++draw_calls;
  __real_vkCmdDraw(cmd, vertices, instances, first, base);
}
VkResult __real_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *,
                                 const VkAllocationCallbacks *, VkDeviceMemory *);
VkResult __wrap_vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *a,
                                 const VkAllocationCallbacks *c, VkDeviceMemory *m) {
  VkResult r = __real_vkAllocateMemory(d, a, c, m);
  if (!r && nm < N) {
    M[nm].mem = *m;
    M[nm++].size = a->allocationSize;
  }
  return r;
}
VkResult __real_vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags,
                            void **);
VkResult __wrap_vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize z,
                            VkMemoryMapFlags f, void **p) {
  VkResult r = __real_vkMapMemory(d, m, o, z, f, p);
  if (!r)
    for (int i = 0; i < nm; i++)
      if (M[i].mem == m) {
        M[i].map = *p;
        M[i].off = o;
        M[i].mapped_size = z == VK_WHOLE_SIZE ? M[i].size - o : z;
      }
  return r;
}
VkResult __real_vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
VkResult __wrap_vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize o) {
  VkResult r = __real_vkBindBufferMemory(d, b, m, o);
  if (!r && nb < N) {
    B[nb].b = b;
    B[nb].mem = m;
    B[nb++].off = o;
  }
  return r;
}
void __real_vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet *, uint32_t,
                                   const VkCopyDescriptorSet *);
void __wrap_vkUpdateDescriptorSets(VkDevice d, uint32_t n, const VkWriteDescriptorSet *w,
                                   uint32_t nc, const VkCopyDescriptorSet *c) {
  for (unsigned i = 0; i < n; i++)
    if (w[i].dstBinding == 0 && w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
        w[i].pBufferInfo && w[i].pBufferInfo->range == SZ && nsnap < 4) {
      const VkDescriptorBufferInfo *x = w[i].pBufferInfo;
      for (int j = 0; j < nb; j++)
        if (B[j].b == x->buffer)
          for (int k = 0; k < nm; k++)
            if (M[k].mem == B[j].mem && M[k].map) {
              VkDeviceSize o = B[j].off + x->offset;
              if (o >= M[k].off && o <= M[k].size && SZ <= M[k].size - o &&
                  o - M[k].off <= M[k].mapped_size && SZ <= M[k].mapped_size - (o - M[k].off)) {
                offsets[nsnap] = x->offset;
                ptr[nsnap] = M[k].map + (o - M[k].off);
                memcpy(frozen[nsnap], ptr[nsnap], SZ);
                nsnap++;
              }
            }
    }
  __real_vkUpdateDescriptorSets(d, n, w, nc, c);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  FILE *f = fopen(argv[1], "rb");
  if (!f || fseek(f, 0, SEEK_END))
    return 2;
  long n = ftell(f);
  if (n <= 0)
    return 2;
  rewind(f);
  char *p = malloc((size_t)n);
  if (!p || fread(p, 1, (size_t)n, f) != (size_t)n)
    return 2;
  fclose(f);
  char why[512] = {0};
  struct s2a_vk_device d;
  if (s2a_vk_device_create(&d, why, sizeof why)) {
    printf("device %s\n", why);
    return 2;
  }
  struct nvn_vk_texcache c;
  nvn_vk_texcache_init(&c, &d, NULL, 0);
  unsigned char pixels[16] = {0};
  g_rt = nvn_vk_texcache_get(&c, 0x1000, 1, 0x25, 2, 2, pixels, sizeof pixels);
  if (!g_rt) {
    puts("no rt");
    return 2;
  }
  g_tex = nvn_vk_texcache_get(&c, 0x2000, 2, 0x25, 2, 2, pixels, sizeof pixels);
  if (!g_tex)
    return 3;
  g_tex2 = nvn_vk_texcache_get(&c, 0x3000, 3, 0x25, 2, 2, pixels, sizeof pixels);
  if (!g_tex2)
    return 3;
  struct nvn_vk_tex *owned[3] = {g_rt, g_tex, g_tex2};
  for (int i = 0; i < 3; ++i) {
    owned_images[i] = owned[i]->image;
    owned_memory[i] = owned[i]->memory;
    remember_view(owned[i]->view);
    remember_view(owned[i]->view_att);
  }
  g_tex->nvn_target = getenv("BUFFER") ? 10 : 7;
  g_tex2->nvn_target = 1;
  tex_t t = {0};
  t.magic = 0x00584554;
  t.d.format = 0x25;
  t.serial = 1;
  struct s2a_record_draw_state s = {0};
  s.stages = 3;
  s.rt_count = 1;
  s.count = 3;
  s.instances = 1;
  s.rt[0].key = (uintptr_t)&t;
  s.prim = 4;
  s.vp_seen = 1;
  s.vp_n = 1;
  s.vp_w = 2;
  s.vp_h = 2;
  s.vp_far = 1;
  s.sc_seen = 1;
  s.sc_w = 2;
  s.sc_h = 2;
  if (getenv("BIND") || getenv("PARTIAL")) {
    s.tex_present[1][0] = 1;
    s.tex_handle[1][0] = 1 | ((uint64_t)1 << 20);
    if (!getenv("PARTIAL")) {
      s.tex_present[1][1] = 1;
      s.tex_handle[1][1] = 2 | ((uint64_t)1 << 20);
    }
  }
  int r = nvn_vk_first_draw(&d, &s, (void *)p, n, NULL, NULL, 2, 2, "synthetic", why, sizeof why);
  if (r) printf("first rc=%d why=%s\n", r, why);
  int ok = 0;
  if (getenv("BIND")) {
    d.shader_draw_parameters_enabled = 0;
    phase = 1;
    int r2 =
        nvn_vk_first_draw(&d, &s, (void *)p, n, NULL, NULL, 2, 2, "synthetic2", why, sizeof why);
    uint32_t x0 = 0, x1 = 0;
    if (nsnap == 2) {
      memcpy(&x0, frozen[0] + 1392, 4);
      memcpy(&x1, frozen[1] + 1392, 4);
    }
    unsigned lane = getenv("LANE") ? (unsigned)atoi(getenv("LANE")) : 1;
    uint32_t z0 = 0, z1 = 0;
    if (nsnap == 2) {
      memcpy(&z0, frozen[0] + 1396 + 4 * lane, 4);
      memcpy(&z1, frozen[1] + 1396 + 4 * lane, 4);
    }
    uint32_t want0 = getenv("M0") ? (uint32_t)strtoul(getenv("M0"), NULL, 0) : 1;
    uint32_t want1 = getenv("M1") ? (uint32_t)strtoul(getenv("M1"), NULL, 0) : 2;
    for (unsigned l = 0; l < 3 && nsnap == 2; l++) {
      uint32_t v0, v1;
      memcpy(&v0, frozen[0] + 1396 + 4 * l, 4);
      memcpy(&v1, frozen[1] + 1396 + 4 * l, 4);
      if (l != lane && (v0 || v1)) {
        z0 = ~want0;
        break;
      }
    }
    ok = z0 == want0 && z1 == want1 && !r && !r2 && draw_calls == 2 && nsnap == 2 &&
         ptr[0] != ptr[1] && offsets[0] != offsets[1] && !memcmp(ptr[0], frozen[0], SZ) &&
         !memcmp(ptr[1], frozen[1], SZ) && ((x0 ^ x1) == 2);
    if (nsnap == 2) {
      unsigned char expected[2][SZ] = {{0}};
      uint32_t one = 0x3f800000u, two = 2;
      memcpy(expected[0] + 1392, &two, 4);
      for (int tcase = 0; tcase < 2; ++tcase) {
        memcpy(expected[tcase] + 1408, &one, 4);
        for (int i = 0; i < 73; ++i) {
          memcpy(expected[tcase] + 192 + 16 * i, &one, 4);
          memcpy(expected[tcase] + 1440 + 16 * i, &one, 4);
        }
      }
      memcpy(expected[0] + 1396 + 4 * lane, &want0, 4);
      memcpy(expected[1] + 1396 + 4 * lane, &want1, 4);
      ok &= !memcmp(frozen[0], expected[0], SZ) && !memcmp(frozen[1], expected[1], SZ);
    }
    if (nsnap == 2) {
      unsigned char a[SZ], b[SZ];
      memcpy(a, frozen[0], SZ);
      memcpy(b, frozen[1], SZ);
      memset(a + 1392, 0, 4);
      memset(b + 1392, 0, 4);
      memset(a + 1396, 0, 12);
      memset(b + 1396, 0, 12);
      ok &= !memcmp(a, b, SZ);
    }
    printf("second=%d snaps=%d offsets=%llu,%llu x=%u,%u z=%u,%u ok=%d\n", r2, nsnap,
           (unsigned long long)offsets[0], (unsigned long long)offsets[1], x0, x1, z0, z1, ok);
  } else {
    const char *missing = getenv("PARTIAL") ? "texture slot 1" : "texture slot 0";
    int partial_ok = 1;
    if (getenv("PARTIAL")) {
      int seen = 0;
      for (int i = 0; i < nm; i++)
        if (M[i].map && M[i].off == 0 && M[i].mapped_size >= (4u << 20)) {
          uint32_t word = 0, scale = 0;
          memcpy(&word, M[i].map + 1400, 4);
          memcpy(&scale, M[i].map + 192, 4);
          printf("failed draw arena Z=%u (missing binding bit=%u)\n", word, (word >> 1) & 1);
          if (word != 0 || scale != 0x3f800000u)
            partial_ok = 0;
          ++seen;
        }
      if (seen != 1)
        partial_ok = 0;
    }
    ok = partial_ok && r != 0 && strstr(why, missing) && nsnap == 0 && draw_calls == 0;
    printf("missing rc=%d why=%s ok=%d\n", r, why, ok);
  }
  int flush = nvn_vk_first_draw_flush();
  if (flush != 0) {
    fprintf(stderr, "flush failed; retaining in-flight resources\n");
    return 1;
  }
  printf("fixture pins before present_done: %u/%u/%u\n", g_rt->pins, g_tex->pins, g_tex2->pins);
  // flush proved completion; only now may the fixture release the draw pins.
  nvn_vk_first_draw_present_done();
  if (g_rt->pins || g_tex->pins || g_tex2->pins) {
    fprintf(stderr, "fixture textures remain pinned: %u/%u/%u\n", g_rt->pins, g_tex->pins,
            g_tex2->pins);
    return 1;
  }
  nvn_vk_first_draw_release_device(&d);
  nvn_vk_texcache_destroy(&c);
  int owned_ok = nviews >= 3;
  for (int i = 0; i < 3; ++i)
    owned_ok &=
        owned_images[i] && owned_memory[i] && images_destroyed[i] == 1 && memory_freed[i] == 1;
  for (int i = 0; i < nviews; ++i)
    owned_ok &= views_destroyed[i] == 1;
  printf("fixture release: images=%u/%u/%u views=%d memory=%u/%u/%u ok=%d\n", images_destroyed[0],
         images_destroyed[1], images_destroyed[2], nviews, memory_freed[0], memory_freed[1],
         memory_freed[2], owned_ok);
  ok &= owned_ok;
  s2a_vk_device_destroy(&d, 1);
  free(p);
  printf("flush=%d\n", flush);
  return ok && flush == 0 ? 0 : 1;
}
uint64_t s2a_poolahb_begin_seq(void) {
  return 0;
}
void s2a_poolahb_complete_seq(uint64_t s) {
  (void)s;
}
int s2a_poolahb_lookup(uint64_t a, uint64_t n, VkBuffer *b, uint64_t *o) {
  (void)a;
  (void)n;
  (void)b;
  (void)o;
  return 0;
}
