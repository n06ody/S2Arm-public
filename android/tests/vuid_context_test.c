// SPDX-License-Identifier: Apache-2.0
#include "nvn_vuid_diag.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int checks, bad;
#define CHECK(x) do { checks++; if (!(x)) { bad++; fprintf(stderr, "【红】line %d: %s\n", __LINE__, #x); } } while (0)
static _Atomic int concurrent_reports;
static void *worker(void *p) {
    (void)p;
    struct s2a_vuid_context c = {.kind = S2A_VUID_COMPILE, .stages = 1, .bound = 1};
    c.keys[0][0] = 0x2a;
    s2a_vuid_exchange(&c, NULL);
    for (int i = 0; i < 128; i++) {
        char line[1200];
        if (s2a_vuid_observe(99, "VUID-test", "vkCreateShaderModule(): test", line, sizeof line)) {
            if (!strstr(line, " exact ") || !strstr(line, "st0=2a00000000000000")) atomic_fetch_add(&concurrent_reports, 1000);
            atomic_fetch_add(&concurrent_reports, 1);
        }
    }
    s2a_vuid_exchange(NULL, NULL);
    return NULL;
}
int main(void) {
    char line[1200];
    struct s2a_vuid_context outer = {.kind = S2A_VUID_DRAW, .stages = 3, .bound = 3}, inner = {.kind = S2A_VUID_COMPILE, .bound = 1}, save;
    outer.keys[0][0] = 0x12; outer.keys[1][0] = 0x34;
    s2a_vuid_start(0);
    s2a_vuid_exchange(&outer, NULL);
    CHECK(!s2a_vuid_observe(1, "VUID-test", "vkCmdDraw(): test", line, sizeof line));
    s2a_vuid_start(1); s2a_vuid_reset_report();
    CHECK(s2a_vuid_observe(1, "VUID-test", "vkCmdDraw(): test", line, sizeof line));
    CHECK(strstr(line, " exact ") && strstr(line, "st0=1200000000000000") && strstr(line, "st1=3400000000000000"));
    s2a_vuid_exchange(&inner, &save);
    CHECK(s2a_vuid_observe(2, "VUID-test", "vkCreateShaderModule(): test", line, sizeof line));
    CHECK(strstr(line, " unknown ") && strstr(line, "st0=unknown"));
    s2a_vuid_exchange(&save, NULL);
    CHECK(s2a_vuid_current()->kind == S2A_VUID_DRAW && s2a_vuid_current()->keys[1][0] == 0x34);
    // 范围里出了不是本范围会调的入口：留着程序键，但只能是 partial，并点名入口
    CHECK(s2a_vuid_observe(3, "VUID-test", "vkQueueSubmit(): test", line, sizeof line));
    CHECK(strstr(line, " partial ") && strstr(line, "api=vkQueueSubmit(不是本范围的入口)") && strstr(line, "st0=1200000000000000"));
    // 00328 一类：绘制范围里写描述符（后端在绘制调用里 vkUpdateDescriptorSets）：精确
    CHECK(s2a_vuid_observe(5, "VUID-VkWriteDescriptorSet-descriptorType-00328", "vkUpdateDescriptorSets(): pDescriptorWrites[0].pBufferInfo[0].offset", line, sizeof line));
    CHECK(strstr(line, " exact ") && strstr(line, "api=vkUpdateDescriptorSets ") && strstr(line, "st1=3400000000000000"));
    // 消息不以入口名开头：范围在也不能算精确
    CHECK(s2a_vuid_observe(6, "VUID-test", "Validation Error: no entry point", line, sizeof line));
    CHECK(strstr(line, " partial ") && strstr(line, "api=?"));
    outer.stages = 1; s2a_vuid_exchange(&outer, NULL);
    CHECK(s2a_vuid_observe(4, "VUID-test", "vkCmdDrawIndexed(): test", line, sizeof line));
    CHECK(strstr(line, " partial ") && strstr(line, "st1=unknown"));
    s2a_vuid_exchange(NULL, NULL);
    // 07991 一类：建管线时按所链着色器的程序键关联（任意线程）；着色器销毁后句柄不再带键
    { static int sh_a, sh_b, sh_c; const uint8_t ka[8] = {0x55}, kb[8] = {0x66}, kc[8] = {0x77};
      s2a_vuid_note_shader(&sh_a, 0, ka); s2a_vuid_note_shader(&sh_b, 1, kb); s2a_vuid_note_shader(&sh_c, 2, kc);
      const void *pipe[5] = {&sh_a, &sh_b};
      struct s2a_vuid_context old;
      CHECK(s2a_vuid_pipe_begin(pipe, 5, &old));
      CHECK(s2a_vuid_observe(7991, "VUID-VkGraphicsPipelineCreateInfo-layout-07991", "vkCreateGraphicsPipelines(): pCreateInfos[0].pStages[1]", line, sizeof line));
      CHECK(strstr(line, " exact ") && strstr(line, "kind=4") && strstr(line, "st0=5500000000000000") && strstr(line, "st1=6600000000000000") && !strstr(line, "st2="));
      s2a_vuid_exchange(&old, NULL);
      s2a_vuid_forget_shader(&sh_a);
      CHECK(s2a_vuid_pipe_begin(pipe, 5, &old));
      CHECK(s2a_vuid_observe(7991, "VUID-VkGraphicsPipelineCreateInfo-layout-07991", "vkCreateGraphicsPipelines(): pCreateInfos[0].pStages[1]", line, sizeof line));
      CHECK(strstr(line, " partial ") && strstr(line, "st0=unknown") && strstr(line, "st1=6600000000000000"));
      s2a_vuid_exchange(&old, NULL);
      s2a_vuid_forget_shader(&sh_b); s2a_vuid_forget_shader(&sh_c); }
    // 表的删除不能断开其它句柄的探测链：同一个桶里挤 64 个，删掉一半，剩下的都还查得到
    { static char many[64][16 * 8192]; const uint8_t k[8] = {0x99};   // 地址相差 16*8192 字节：散列后落进同一个桶附近
      for (int i = 0; i < 64; i++) s2a_vuid_note_shader(many[i], 0, k);
      for (int i = 0; i < 64; i += 2) s2a_vuid_forget_shader(many[i]);
      int found = 0, gone = 0;
      for (int i = 0; i < 64; i++) { const void *one[1] = {many[i]}; struct s2a_vuid_context old;
          s2a_vuid_pipe_begin(one, 1, &old); const struct s2a_vuid_context *c = s2a_vuid_current();
          if (i & 1) found += c->stages == 1 && c->keys[0][0] == 0x99; else gone += c->stages == 0;
          s2a_vuid_exchange(&old, NULL); }
      CHECK(found == 32 && gone == 32);
      for (int i = 1; i < 64; i += 2) s2a_vuid_forget_shader(many[i]); }
    s2a_vuid_reset_report();
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(threads[i], NULL);
    CHECK(atomic_load(&concurrent_reports) == 10); // 512 calls: 1,2,4,...,512
    s2a_vuid_reset_report();
    for (int i = 0; i < 512; i++) CHECK(s2a_vuid_observe(i, "VUID-test", "vkQueueSubmit(): test", line, sizeof line));
    CHECK(s2a_vuid_observe(512, "VUID-test", "vkQueueSubmit(): test", line, sizeof line) && strstr(line, "表满"));
    printf("vuid_context: checks=%d bad=%d concurrent_reports=%d\n", checks, bad, atomic_load(&concurrent_reports));
    return bad != 0;
}
