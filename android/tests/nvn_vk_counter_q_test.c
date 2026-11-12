// SPDX-License-Identifier: Apache-2.0
// Exercise the real first-draw timestamp entry with a pre-reset query in an open pass.
// Vulkan entry points are compiled against NDK headers by the Python driver.
#define main clear_ownership_fixture_main
#include "nvn_vk_clear_enqueue_ownership_test.c"
#undef main

static unsigned query_reset[2], timestamp_count;
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool,
                                               uint32_t first, uint32_t count) {
    assert(cmd == (VkCommandBuffer)5 && pool == (VkQueryPool)3);
    assert(!(g_op.open && !test_end_passes) && "query reset inside render pass");
    assert(first <= 2 && count <= 2 - first);
    for (uint32_t i = first; i < first + count; i++) query_reset[i] = 1;
}
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage,
                                                VkQueryPool pool, uint32_t slot) {
    assert(cmd == (VkCommandBuffer)5 && pool == (VkQueryPool)3);
    assert(stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT && slot < 2 && query_reset[slot]);
    query_reset[slot] = 0;
    timestamp_count++;
}

int main(void) {
    mode = "counter-on";
    struct s2a_vk_device d = {0};
    d.device = (VkDevice)1; d.queue = (VkQueue)1; d.command_pool = (VkCommandPool)1;
    g_fq.dev = &d; g_fq.dev_handle = d.device;
    VkCommandBuffer cmd = (VkCommandBuffer)5;
    VkQueryPool pool = (VkQueryPool)3;
    // The pool was reset before vkCmdBeginRenderPass, as Vulkan requires.
    vkCmdResetQueryPool(cmd, pool, 0, 2);
    g_batch_cb = cmd; g_op.open = 1; g_op.cmd = cmd; g_op.dev = &d;
    char why[256] = {0};
    assert(nvn_vk_first_draw_timestamp_top(&d, pool, 0, why, sizeof why) == 0);
    assert(g_op.open && test_end_passes == 0 && "timestamp closed the active render pass");
    assert(g_batch_cb == cmd && submitted == 0 && "timestamp sealed/submitted the active batch");
    assert(timestamp_count == 1 && !query_reset[0]);
    puts("counter-q in-pass timestamp preserved active batch");
    return 0;
}
