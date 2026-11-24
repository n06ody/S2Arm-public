// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 同一次绘制读写附件：GENERAL 不是像素级反馈保证。照 iOS fb_prepare，
// 在 draw 前把采样范围存成独立图像；同一逻辑通道下一笔重新取快照。
// 此处在 nvn_gpu_* 契约之下，使用真实 VkImage / 子资源范围与段完成寿命。
static int feedback_overlap(struct nvn_gpu_view_s *v, struct nvn_gpu_view_s *a) {
    if (!v || !a || !v->tex || !a->tex || v->tex->image != a->tex->image ||
        !(v->aspect & a->tex->aspect) || a->base_level < v->base_level ||
        (uint64_t)a->base_level >= (uint64_t)v->base_level + v->levels) return 0;
    // view_attachment 挂的是一级一层的 2D 形态；3D 采样形态覆盖该级全部 z。
    if (v->type == VK_IMAGE_VIEW_TYPE_3D) return 1;
    uint32_t layer = a->tex->target == NVN_TEXTURE_TARGET_3D && a->tex->depth > 1 ? a->att_layer : a->base_layer;
    return layer >= v->base_layer && (uint64_t)layer < (uint64_t)v->base_layer + v->layers;
}
static int feedback_attached(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *v) {
    const nvn_gpu_pass_desc *d = &c->pass_desc;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++) if (feedback_overlap(v, d->color[i].view)) return 1;
    return feedback_overlap(v, d->depth.view) || feedback_overlap(v, d->stencil.view);
}
// 采用当前绘制的有效写状态，「本笔没有产生像素」也不能证明只读。
// 深度模板两面共享一个物理附件；任一面可能写，都不能用 NONE 豁免另一面。
static uint16_t feedback_writes(struct nvn_gpu_cmd_s *c) {
    const nvn_gpu_pipe_desc *p = &c->pipe->key;
    const ds_st ds = draw_depth_stencil(c);
    uint16_t writes = 0;
    for (uint32_t i = 0; i < 8; i++)
        if (p->cfmt[i] && ((p->chmask.mask >> (4 * i)) & 15)) writes |= 1u << i;
    if (p->dfmt && ds.dt && ds.dw) writes |= 1u << 8;
    if (p->sfmt && ds.st) {
        uint32_t front = c->has_dyn ? c->dyn.st_wmask[0] & 255 : 255;
        uint32_t back = c->has_dyn ? c->dyn.st_wmask[1] & 255 : 255;
        if ((front && (vk_stencil_op(ds.fail_f) != VK_STENCIL_OP_KEEP ||
                       vk_stencil_op(ds.zfail_f) != VK_STENCIL_OP_KEEP ||
                       vk_stencil_op(ds.zpass_f) != VK_STENCIL_OP_KEEP)) ||
            (back && (vk_stencil_op(ds.fail_b) != VK_STENCIL_OP_KEEP ||
                      vk_stencil_op(ds.zfail_b) != VK_STENCIL_OP_KEEP ||
                      vk_stencil_op(ds.zpass_b) != VK_STENCIL_OP_KEEP))) writes |= 1u << 8;
    }
    return writes;
}
// from_pool = 1：这个视图来自整池数组（NVN_RB_TEXP，整张贴图池一个绑定），不是着色器声明过的某个具体绑定。
//   整池数组只表示「可能被按运行时下标采样到」：池里登记着本通道正在画的那张图（几乎每张渲染目标都在池里），不等于这一笔会读它。
//   照 Ryujinx：贴图数组（DescriptorSetUpdater.InsertBindingBarriers 的 segment.IsArray 分支）只排队写后读屏障，不查反馈环、不拷快照
//   （非数组才记 FeedbackLoopHazards）。所以整池里与「本笔写着的附件」重叠的视图不拷快照、不拆通道、不把整组描述符改成快照（*skipped 记一笔供读数）。
//   与「本笔不写的附件」重叠的照旧（只读通道 / 快照）：那是深度测试不写深度同时采样深度这类合法用法，拆通道把前面的写落地才读得对。
//   nvk_pool_fb=1 回到旧行为；nvk_opt_layout 开着时池图像必须是 GENERAL（pool_check_layouts），重叠的视图要换成快照才合规，也保持旧行为。
// 免拷只在「GENERAL 档（nvk_opt_layout=0）且没开旧行为开关（nvk_pool_fb=0）」成立；这一条写成显式函数，夹具案例 18 按 opt 0 / 1 各断言一遍。
static inline int feedback_pool_skip_allowed(const struct nvn_gpu_cmd_s *c) { return !c->pool_fb_on && !c->opt; }
// 免拷日志限流：每个（管线、图）首次一行，最多 FB_SKIP_LOG_MAX 行，写满后只说一句「不再逐条记」；读数仍在分时行的 pskip。
// 管线键 = 建管线时的描述散列（p->hash）；图编号 = 贴图的访问编号（access_id，会话内不复用）。查表无锁：空槽 CAS 占位，占到的那一方打日志。
#define FB_SKIP_LOG_MAX 64u
static uint64_t access_id(uint64_t *slot);
static uint64_t g_fb_skip_seen[FB_SKIP_LOG_MAX];
static uint32_t g_fb_skip_full;
static void feedback_pool_skip_note(const struct nvn_gpu_cmd_s *c, const struct nvn_gpu_view_s *v) {
    struct nvn_gpu_tex_s *t = v->tex;
    if (!t) return;
    const uint64_t id = access_id(&t->access_id);
    const uint64_t key = (((uint64_t)c->pipe->hash << 32) ^ id) * 0x9E3779B97F4A7C15ull | 1ull;
    for (uint32_t i = 0; i < FB_SKIP_LOG_MAX; i++) {
        uint64_t cur = __atomic_load_n(&g_fb_skip_seen[i], __ATOMIC_RELAXED);
        if (cur == key) return;
        if (cur) continue;
        if (!__atomic_compare_exchange_n(&g_fb_skip_seen[i], &cur, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            if (cur == key) return;   // 别的线程刚占了同一个键
            continue;
        }
        char line[200];
        snprintf(line, sizeof line, "整池数组免拷快照（与本笔写着的附件重叠，nvk_pool_fb=1 恢复旧行为）：管线 %08x，图 #%llu %ux%u 格式 %#x",
                 c->pipe->hash, (unsigned long long)id, t->w, t->h, t->nvn_format);
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", line);
        return;
    }
    if (!__atomic_exchange_n(&g_fb_skip_full, 1u, __ATOMIC_RELAXED))
        nvn_plat_log(NVN_PLAT_LOG_INFO, "gpu", "整池数组免拷快照：不同（管线、图）已记满，之后不再逐条记（计数见分时行 pskip）");
}
static int feedback_add(struct nvn_gpu_cmd_s *c, struct nvn_gpu_view_s *v, uint32_t set,
                        uint16_t writes, uint16_t *read_mask, int from_pool, int *skipped) {
    uint16_t attached = 0;
    const nvn_gpu_pass_desc *d = &c->pass_desc;
    for (uint32_t i = 0; i < d->ncolor && i < 8; i++)
        if (feedback_overlap(v, d->color[i].view)) attached |= 1u << i;
    if (feedback_overlap(v, d->depth.view) || feedback_overlap(v, d->stencil.view)) attached |= 1u << 8;
    if (!attached) return 1;
    if (from_pool && (attached & writes) && feedback_pool_skip_allowed(c)) { feedback_pool_skip_note(c, v); *skipped = 1; return 1; }
    if (c->feedback_read_on && !(attached & writes)) {
        *read_mask |= attached;
        return 1;
    }
    c->desc_dirty |= 1u << set;
    for (struct feedback_copy *f = c->feedback; f != c->feedback_stop; f = f->next)
        if (f->source == v) return 1;
    struct feedback_copy *f = calloc(1, sizeof *f);
    if (!f) { set_sticky(c, NVN_GPU_E_OOM, "feedback snapshot list allocation failed"); return 0; }
    f->source = v; f->next = c->feedback; c->feedback = f;
    return 1;
}
// 同一段里同一张源图已经有过快照：复用那张图（重新拷一遍），不再每笔新建。快照图由本段的 REF_TEX 保活到 fence，
// 段内只要有一笔反馈采样就新建一张整图大小的快照，长段里（读档时一段录几百笔）会攒到几 GB：
// 1024x1024x8 R16 四级的影子图每笔 21 MiB，一段 182 笔 = 3.8 GB（真机读档时观察到：悬着的引用全在正在录制的这一段上）。
// 复用是安全的：前面那几笔对旧内容的采样在命令顺序上更早，image_barrier 从 GENERAL 出来等 ALL_COMMANDS（WAR 只要执行依赖）；
// 新一笔的描述符随 desc_dirty 重写成本笔的快照视图。只比指针、核对形状，不解引用 o->snap_src。
static struct nvn_gpu_tex_s *feedback_snap_reuse(struct nvn_gpu_cmd_s *c, struct feedback_copy *f, struct nvn_gpu_tex_s *s) {
    for (struct feedback_copy *o = c->feedback; o; o = o->next) {
        struct nvn_gpu_tex_s *t = o->snap;
        if (o == f || !t || o->snap_src != s) continue;
        if (t->format == s->format && t->samples == s->samples && t->w == s->w && t->h == s->h && t->depth == s->depth &&
            t->layers == s->layers && t->levels == s->levels && t->target == s->target && t->aspect == s->aspect) return t;
    }
    return NULL;
}
// ---- 快照池：快照图跨段复用（同 iOS nvn_mtl.m 的 dsnap_storage / fb_snap_shape：按形状留存储、原地复用、满了换掉最久没用的）----
// 起因：段内复用（上面的 feedback_snap_reuse）只在同一个提交段里查，段一完成快照随段引用销毁，下一段同一张源图又整张新建。
//   一台骁龙 8 至尊版手机（Adreno 830）初始山洞场景 simpleperf 30 秒：游戏线程 47% 的样本在
//   nvn_gpu_tex_create → vkAllocateMemory → kgsl_sharedmem_alloc → ioctl（高通驱动每次分配都进内核并清零），来源就是这里每段新建的快照图。
// 寿命：快照图只被它所在的那一段引用（cmd_ref REF_TEX；建它的 feedback_make 末尾已 release，released = 1）。段完成时 finish_cmd
//   （executed = 1 只在完成线程等到栅栏之后）给它记 fb_ready，随后归还最后一份引用：unref_locked 看到 released && refs == 0，
//   先问 fbpool_put_locked——收下就不销毁，图连同显存、默认视图原样留在 g.texs 上。所以池里的图 GPU 一定已经用完。
//   提交线程 feedback_make 取图、完成线程放回，两边都在 g_mu 下动池。取出后图回到「刚建好、本函数持一份建图所有权」的状态
//   （released = 0），与新建走同一条路：cmd_ref 进本段、末尾 release。
// 形状键 = 建它时的整份建图请求（目标 / NVN 格式 / 宽高深 / 级数 / 样本数 / 用途位含 VIEW_FMT）：nvn_gpu_tex_create 在一个会话里
//   是这份请求的确定函数，Vulkan 格式、面、层数、usage、MUTABLE / 格式列表、内存类型都随之相同；取图时再核一遍与源图的
//   格式 / 样本 / 层 / 面（宁可少复用，不错复用）。
// 内容：取出的图里是上一次的旧内容，本函数随后的 vkCmdCopyImage 覆盖 v 的 [base_level, +levels) × [base_layer, +layers)（3D 为整级全部 z）
//   × v->aspect 的每个面，快照视图的 subresourceRange 恰是同一段，采样永远看不到没覆盖的部分。
// 布局：段与段之间的静止布局一律 GENERAL（本函数末尾转回 GENERAL，段末 lay_rest_at_commit 兜底），image_barrier 按段跟踪、段首按 GENERAL 起算；
//   放回时图已转出 UNDEFINED（fb_ready 只在段执行过才置），与取出后第一次 image_barrier 的前提一致。
// 同一段里同一形状同时要两张：取走的就不在池里了，第二张池里没有就照旧新建。会话关闭 / 设备丢失：池清空（关闭时随 g.texs 一起销毁）。
static int fbpool_key_eq(const struct fb_key *a, const struct fb_key *b) {
    return a->target == b->target && a->nvn_format == b->nvn_format && a->w == b->w && a->h == b->h && a->depth == b->depth &&
           a->levels == b->levels && a->samples == b->samples && a->usage == b->usage;
}
static void fbpool_evict0_locked(struct nvn_gpu_vk *vk) {   // 调用方持 g_mu：淘汰最久没用的那张
    struct nvn_gpu_tex_s *t = g_fbpool[0];
    g_fbpool_bytes -= t->mem_size;
    memmove(&g_fbpool[0], &g_fbpool[1], (g_fbpool_n - 1) * sizeof g_fbpool[0]); g_fbpool_n--;
    __atomic_fetch_add(&g_fbpool_cnt[2], 1, __ATOMIC_RELAXED);
    destroy_tex_locked(vk, t);
}
// 调用方持 g_mu，t 已 released 且 refs == 0。收下返回 1（不销毁）；不收返回 0，调用方照旧销毁。
static int fbpool_put_locked(struct nvn_gpu_vk *vk, struct nvn_gpu_tex_s *t) {
    if (!t->fb_snap || !t->fb_ready || !g_fbpool_cap || vk->closing || vk->device_lost || t->mem_size > g_fbpool_cap) return 0;
    while (g_fbpool_n && (g_fbpool_n == FBPOOL_N || g_fbpool_bytes + t->mem_size > g_fbpool_cap)) fbpool_evict0_locked(vk);
    g_fbpool[g_fbpool_n++] = t;
    g_fbpool_bytes += t->mem_size;
    return 1;
}
static void fbpool_drain_locked(struct nvn_gpu_vk *vk) {   // 调用方持 g_mu
    while (g_fbpool_n) fbpool_evict0_locked(vk);
}
// 提交线程：取一张同形状的（从最近放回的找起）。取出的图 released = 0、refs = 0，归调用方所有，与刚建好的图一样处理。
static struct nvn_gpu_tex_s *fbpool_take(const struct fb_key *k, const struct nvn_gpu_tex_s *s) {
    if (!g_fbpool_cap) return NULL;
    struct nvn_gpu_tex_s *t = NULL;
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = g_fbpool_n; i-- > 0;) {
        struct nvn_gpu_tex_s *p = g_fbpool[i];
        if (!fbpool_key_eq(&p->fbk, k) || p->format != s->format || p->samples != s->samples ||
            p->layers != s->layers || p->aspect != s->aspect) continue;
        t = p;
        g_fbpool_bytes -= p->mem_size;
        memmove(&g_fbpool[i], &g_fbpool[i + 1], (g_fbpool_n - i - 1) * sizeof g_fbpool[0]); g_fbpool_n--;
        t->released = 0; t->fb_ready = 0;
        break;
    }
    pthread_mutex_unlock(&g_mu);
    if (t) __atomic_fetch_add(&g_fbpool_cnt[1], 1, __ATOMIC_RELAXED);
    return t;
}
// 读数（缺省常开）：out = 新建 / 池命中 / 淘汰（会话起累计、只增）、池里张数、池里字节。上屏的 5 秒读数行与宿主用例用。
void nvn_vk_fbpool_stat(uint64_t out[5]) {
    for (int i = 0; i < 3; i++) out[i] = __atomic_load_n(&g_fbpool_cnt[i], __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_mu); out[3] = g_fbpool_n; out[4] = g_fbpool_bytes; pthread_mutex_unlock(&g_mu);
}
static int feedback_make(struct nvn_gpu_cmd_s *c, struct feedback_copy *f) {
    fr_gap(c,"feedback_snapshot");
    struct nvn_gpu_view_s *v = f->source;
    struct nvn_gpu_tex_s *s = v->tex;
    struct nvn_gpu_tex_s *t = feedback_snap_reuse(c, f, s);
    const int fresh = !t;   // 新建 / 从快照池取的要在本函数末尾 release（段引用保活）；段内复用的不归本函数管
    if (fresh) {
        nvn_gpu_tex_desc td = {0};
        td.d.target = s->target; td.d.format = s->nvn_format;
        td.d.width = s->w; td.d.height = s->h; td.d.depth = s->depth;
        td.d.levels = s->levels; td.d.samples = s->samples;
        td.usage = NVN_GPU_TEX_SAMPLED | ((s->view_fmt || s->fmt_n) ? NVN_GPU_TEX_VIEW_FMT : 0);   // 列表模式的源（保 UBWC）也要能按孪生格式看
        const struct fb_key key = {(uint32_t)td.d.target, (uint32_t)td.d.format, (uint32_t)td.d.width, (uint32_t)td.d.height,
                                   (uint32_t)td.d.depth, (uint32_t)td.d.levels, (uint32_t)td.d.samples, (uint32_t)td.usage};
        t = fbpool_take(&key, s);
        if (!t) {
            nvn_gpu_tex th = NULL; nvn_gpu_err err = {0};
            nvn_gpu_rc r = nvn_gpu_tex_create(&td, &th, &err);
            if (!nvn_gpu_ok(r)) { set_sticky(c, r.code, "feedback texture: %s", err.why); return 0; }
            t = th;
            t->fb_snap = 1; t->fbk = key;
            __atomic_fetch_add(&g_fbpool_cnt[0], 1, __ATOMIC_RELAXED);
            if (g_draw_tm) __atomic_fetch_add(&g_feedback_snap[0], 1, __ATOMIC_RELAXED);
        }
        cmd_ref(c, t, REF_TEX);
        if (!rc_is(c->sticky, NVN_GPU_OK)) { nvn_gpu_tex_release(t); return 0; }
        // 深度 / 多采样保持物理格式和样本数，绝不 resolve / 近似格式替代。
        if (t->format != s->format || t->samples != s->samples) {
            set_sticky(c, NVN_GPU_E_UNSUPPORTED, "feedback snapshot format/sample mismatch"); nvn_gpu_tex_release(t); return 0;
        }
    } else if (g_draw_tm) __atomic_fetch_add(&g_feedback_snap[1], 1, __ATOMIC_RELAXED);
    f->snap = t; f->snap_src = s;   // 新建的此刻已登记进本段引用表，下面失败也活到段结束
    VkImageViewCreateInfo vi = {0}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image; vi.viewType = v->type; vi.format = v->format; vi.components = v->components;
    vi.subresourceRange = (VkImageSubresourceRange){v->aspect, v->base_level, v->levels, v->base_layer, v->layers};
    VkResult vr = vkCreateImageView(g.d, &vi, NULL, &f->view);
    if (vr != VK_SUCCESS) {
        set_sticky(c, NVN_GPU_E_UNSUPPORTED, "feedback vkCreateImageView failed %d", vr); if (fresh) nvn_gpu_tex_release(t); return 0;
    }
    image_barrier(c, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(c, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (!rc_is(c->sticky, NVN_GPU_OK)) { if (fresh) nvn_gpu_tex_release(t); return 0; }
    for (uint32_t lv = v->base_level; lv < v->base_level + v->levels; lv++) {
        // 3D 图像的 arrayLayer 必须为 0；范围位于 z，保留整级的全部切片。
        const int image3d = s->target == NVN_TEXTURE_TARGET_3D;
        uint32_t w = s->w >> lv, h = s->h >> lv, z = image3d ? s->depth >> lv : 1;
        for (VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT; aspect <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect <<= 1) {
            if (!(v->aspect & aspect)) continue; // Vulkan 拷贝的每条 region 只能指定一个面
            VkImageCopy copy = {0};
            copy.srcSubresource = (VkImageSubresourceLayers){aspect, lv, image3d ? 0 : v->base_layer, image3d ? 1 : v->layers};
            copy.dstSubresource = copy.srcSubresource;
            copy.extent = (VkExtent3D){w ? w : 1, h ? h : 1, z ? z : 1};
            vkCmdCopyImage(c->cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            if (g_draw_tm) __atomic_fetch_add(&g_feedback_stats[1], 1, __ATOMIC_RELAXED);
        }
    }
    image_barrier(c, s, VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(c, t, VK_IMAGE_LAYOUT_GENERAL);
    if (fresh) nvn_gpu_tex_release(t); // cmd 引用保活到 fence；raw view 在 finish_cmd 中先释放
    return rc_is(c->sticky, NVN_GPU_OK);
}
static int feedback_restart(struct nvn_gpu_cmd_s *c, uint16_t read_mask, int copy) {
    // 同一个 Vulkan 查询不能跨 VkRenderPass，NVN 的遮挡计数可以：活动的查询在旧通道末尾（nvn_gpu_pass_end）结束，新通道里换一个续段槽接着数，
    // 读回时并回原槽（occl_spare_*），样本数连续。不截断查询后假报成功：续段槽要不到（保险丝到顶 / 分配或建池失败）时，
    // 只这一个查询续不下去（occl_spare_reserve 已记下，它的读数按未完成处理），拆通道照做、整段不失败。
    // occl_spare_reserve 只在主机侧预留、不录命令，所以放在拆通道之前。
    const int32_t occ = c->q_active;
    struct occl_cont cont = {0};
    const int cont_ok = occ >= 0 && occl_spare_reserve(c, occ, &cont);
    if (g_draw_tm) __atomic_fetch_add(&g_feedback_stats[2], 1, __ATOMIC_RELAXED);
    nvn_gpu_pass_desc d = c->pass_desc;
    for (uint32_t i = 0; i < d.ncolor && i < 8; i++) if (d.color[i].view) {
        d.color[i].load = NVN_GPU_LOAD_LOAD;
    }
    if (d.depth.view) d.depth.load = NVN_GPU_LOAD_LOAD;
    if (d.stencil.view) d.stencil.load = NVN_GPU_LOAD_LOAD;
    nvn_gpu_pass_end(c); // CLEAR / 前面几笔写已落地；只读的新物理通道从 LOAD 开始（活动的遮挡查询在这里结束）
    if (cont_ok) occl_spare_reset(c, &cont);   // 通道外：新要的续段块整块重置（重置只能在通道外）
    if (copy) for (struct feedback_copy *f = c->feedback; f != c->feedback_stop; f = f->next)
        if (!feedback_make(c, f)) return 0;
    c->feedback_read_mask = read_mask;
    nvn_gpu_pass_begin(c, &d);
    if (cont_ok && rc_is(c->sticky, NVN_GPU_OK)) occl_spare_begin(c, occ, &cont);   // 新通道里接着数
    return rc_is(c->sticky, NVN_GPU_OK);
}
// 这一笔绑着的存储图像（图形阶段）与本通道的某个附件是同一张图的同一段子资源？只看当前管线布局认得的绑定，口径同 feedback_prepare 的主循环。
// 一笔绘制表达不了这种读写：存储写进采样快照等于丢掉，直接用原图则存储访问与附件写的先后没有定义。放在主循环之前单独扫一遍：
// 命中就在动任何快照链表之前整笔跳过，同一段里后面的笔接着用没被动过的状态。
static int feedback_image_overlap(struct nvn_gpu_cmd_s *c) {
    for (int st = 0; st < 5; st++)
        for (uint32_t i = 0; i < c->nbinds[st]; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            if (b->cls != NVN_GPU_RES_IMAGE || b->ent >= c->pipe->stage_nent[st]) continue;
            if (c->pipe->stage_ents[st][b->ent].cls != b->cls) continue;
            if (feedback_attached(c, b->u.view)) return 1;
        }
    return 0;
}
static int feedback_prepare(struct nvn_gpu_cmd_s *c, int compute) {
    // 退出反馈或进入计算时，恢复原视图描述符。原绑定表永不被快照改写。
    if (c->feedback != c->feedback_stop) c->desc_dirty = 0xfu;
    c->feedback_stop = c->feedback;
    if (compute || !c->feedback_on || !c->in_pass) return 1;
    // 图形阶段的存储图像与附件重叠：具名跳过这一笔并计数（同 graphics_image_unresolved 一类），不设粘性错误——粘性错误会让整段提交失败、
    // 时间线进终态，而这里只是这一笔画不了；同一段里后面的笔照常画。真正支持这种重叠（附件与存储写共用 GENERAL 加同步，
    // 或反馈环布局）另立卡（#999）。
    if (feedback_image_overlap(c)) {
        nvn_unimpl("draw", "graphics_image_feedback_overlap", NULL);
        return 0;
    }
    uint16_t writes = feedback_writes(c);
    // 每笔由实际采样集合重建：不再采样（包括没有描述符组）就退出 NONE。
    // 不遗留旧槽位的只读状态，避免另一附件别名到同一范围并开始写时漏恢复。
    uint16_t read_mask = 0;
    int pool_skipped = 0;   // 这一笔里整池数组与写着的附件重叠而免拷的视图有没有（读数）
    for (int st = 0; st < 5; st++) {
        for (uint32_t i = 0; i < c->nbinds[st]; i++) {
            const nvn_gpu_binding *b = &c->binds[st][i].b;
            if (b->ent >= c->pipe->stage_nent[st]) continue;
            const nvn_gpu_layout_ent *e = &c->pipe->stage_ents[st][b->ent];
            if (e->cls != b->cls) continue;
            struct nvn_gpu_view_s *v = b->cls == NVN_GPU_RES_TEX_SAMP ? b->u.ts.view :
                (b->cls == NVN_GPU_RES_TEX || b->cls == NVN_GPU_RES_IMAGE) ? b->u.view : NULL;
            if (b->cls != NVN_GPU_RES_IMAGE && !feedback_add(c, v, e->set, writes, &read_mask, 0, &pool_skipped)) return 0;
        }
        if (c->pool_active) for (uint32_t i = 0; i < c->pipe->stage_nent[st] && i < 64; i++) {
            nvn_gpu_pool_table t = c->pool_binds[st][i];
            if (!t || t->kind != NVN_GPU_POOL_IMAGE) continue;
            const uint32_t set = c->pipe->stage_ents[st][i].set;
            if (!t->ikey) {   // 没有按图索引的表：逐项扫
                for (uint32_t j = 0; j <= t->count; j++)
                    if (!feedback_add(c, t->objects[j], set, writes, &read_mask, 1, &pool_skipped)) return 0;
                continue;
            }
            // 有索引：只看和本通道附件同一张图的视图（附件至多 9 个，深度与模板常同图，查过的图不重复查）
            const nvn_gpu_pass_desc *d = &c->pass_desc;
            struct nvn_gpu_view_s *att[10]; uint32_t na = 0;
            for (uint32_t k = 0; k < d->ncolor && k < 8; k++) if (d->color[k].view) att[na++] = d->color[k].view;
            if (d->depth.view) att[na++] = d->depth.view;
            if (d->stencil.view) att[na++] = d->stencil.view;
            for (uint32_t k = 0; k < na; k++) {
                const uint64_t img = att[k]->tex ? (uint64_t)(uintptr_t)att[k]->tex->image : 0;
                uint32_t q = 0; while (q < k && !(att[q]->tex && (uint64_t)(uintptr_t)att[q]->tex->image == img)) q++;
                if (!img || q < k) continue;
                for (uint32_t j = pool_img_first(t, img); j != UINT32_MAX; j = t->unext[j])
                    if (!feedback_add(c, t->uviews[j], set, writes, &read_mask, 1, &pool_skipped)) return 0;
            }
        }
    }
    if (pool_skipped && g_draw_tm) __atomic_fetch_add(&g_feedback_stats[3], 1, __ATOMIC_RELAXED);
    if (read_mask && g_draw_tm) __atomic_fetch_add(&g_feedback_stats[0], 1, __ATOMIC_RELAXED);
    if (c->feedback == c->feedback_stop && read_mask == c->feedback_read_mask) return 1;
    return feedback_restart(c, read_mask, 1);
}
