// Shader reflection json → fixed-length C arrays (A: PC chain reflection; B: reflection produced by translating the original Switch shaders, route = "B", output of tools/dmsh_build.py).
// Kinds added by B: SBUFP = storage buffer device pointer (NVN storage buffer slot); SUPPORT = Ryujinx auxiliary constants; C1 = the shader's own constant segment (c[1]);
//            DRV0 / DRV2 = NVN driver constant segments c[0] (storage buffer address table) / c[2] (texture and sampler handle table)
//            PUSH = NVN2 (Ampere) push-constant slot reg, read as constant bank c[24 + reg] (nvn2_cmd.h; filled from PushGraphics / ComputeConstants, nvn_mtl.m encode_args)
//            SBUFQ = a storage buffer whose 64-bit GPU address the game pushed into NVN2 push-constant slot reg at byte off (compute push block 1 = slot 1, graphics stage i's bank
//                    c[25 + i] = slot 1 + i; Dsh.cs sbuf_push), read like SBUFU but from the pushed bytes
// PC shader reflection json → fixed-length C arrays (converted once when the shader is fetched; each draw only walks the arrays and does not touch NSDictionary / NSString).
// Unit test tests/t_refl.m computes the expectations by hand from real reflection.
#pragma once
#include <stdint.h>
// SPILL：计算核共享内存超过设备上限时挪出来的线程组数组（安卓直连编译器 DirectSharedSpill，同 iOS SpillFix 的 spill_slot）：每个线程组一片，回放按派发组数从暂存区分配

enum { NVN_RB_CBUF = 1, NVN_RB_SAMP, NVN_RB_TEX, NVN_RB_IMG, NVN_RB_TBUF,
       NVN_RB_SBUFP, NVN_RB_SUPPORT, NVN_RB_C1, NVN_RB_DRV0, NVN_RB_DRV2, NVN_RB_SAMPT, NVN_RB_UBOP, NVN_RB_TEXU, NVN_RB_SAMPU, NVN_RB_SAMPI, NVN_RB_SBUFU, NVN_RB_TEXD, NVN_RB_SAMPD, NVN_RB_IMGA, NVN_RB_TEXA, NVN_RB_SAMPA, NVN_RB_GEOMETRY, NVN_RB_UVX, NVN_RB_LB, NVN_RB_UBOLEN, NVN_RB_SBSZ, NVN_RB_PUSH, NVN_RB_TBUFU, NVN_RB_IMGU, NVN_RB_GMEM, NVN_RB_GMEML, NVN_RB_TEXP, NVN_RB_SAMPP, NVN_RB_UBOA, NVN_RB_TBLEAD, NVN_RB_SBUFQ, NVN_RB_SPILL };
enum { NVN_REFL_PUSH_SLOTS = 8, NVN_REFL_PUSH_BYTES = 256 };   // SBUFQ bounds: NVN2 push-constant slots and their bytes (= nvn2_cmd.h NVN2_PUSH_SLOTS / NVN2_PUSH_BYTES, asserted in nvn_mtl.m)   // UBOA: 计算存储缓冲来自「用户 UBO 内 8 字节 guest 裸指针」（map 3，UBO 槽 src_reg + 字节偏移 src_off）   // UVX: S2Arm widen 16:9 sampling remap table (dmsh_build.py fs_uvx; widen_glue.inc uvx_fill); LB: #800 sampler LOD bias table (dmsh_red.py fs_lb; nvn_mtl.m lb_fill)
// TEXA / SAMPA: an array of n handles in the driver table c2 that the shader indexes at run time (dmsh_build.py c2_array): element k = the texture / sampler of the slot whose c2 word lies
// at byte off + 8k (combined, else separate); words outside both tables give the dummy. Luigi's Mansion 3's shadow cascades
// IMGA: SPIRV-Cross's MSL 2.3 image atomics: a device atomic_uint / atomic_int pointer (<image name>_atomic) beside the image of NVN image slot r; the replay binds the image's storage address
// TEXD / SAMPD: Ryujinx's fallback for a texture handle it cannot trace to one slot (handle type 4, <stage>_<t|tb|s>_cb0_40000000): an array of n textures / samplers indexed by
// the handle's texture / sampler ID (capped at n - 1); the replay binds slot k at element k and puts slot indices in c2 (profile key tex_dyn_slots, Octopath Traveler 0)
// TEXP / SAMPP: the texture / sampler pool as one array indexed by the handle's ID, for a sampled texture handle the translator traced to no slot (Dsh.cs PoolArrays; upstream reference
//   implementation (MIT) the reference port nvn_refl.h TEXP / SAMPP): one argument buffer entry, the GPU address of the table the replay keeps for the element type dtt / dpf and the pool bound at the draw
//   (nvn_mtl.m pool_gpu), laid out as DMSH_POOL_HEAD bytes of head (the bound n, the read bitmap's address) and then one 8-byte resource ID per ID. Buffer textures keep TEXD's slot arrays
#define DMSH_POOL_HEAD 16   // = Dsh.cs POOL_HEAD: uint n, 4 bytes of padding, ulong address of the read bitmap (bit i of 32-bit word i / 32: a shader read texture ID i, Dsh.cs POOL_MARK / nvn_mtl.m pool_seen_scan)
// IMGU: an image whose handle is in a uniform buffer (uniform slot r, byte off; the texture ID is the low 20 bits of that word, as TBUFU; No Man's Sky 11-05). rw as IMG
// TBUFU: a buffer texture whose handle is in a uniform buffer (uniform slot r, byte off; Ryujinx ReadPackedId: the texture ID is the low 20 bits of that word, a buffer texture takes no sampler)
// SAMPI: the sampler ID is a constant in the shader (reg = the ID; Ryujinx's SeparateConstantSamplerHandle: the handle ORs a constant sampler into a bound texture, Monster Hunter Rise)
// UBOP: device pointer of compute-stage uniform slot r (the Maxwell compute stage reads uniforms via the c0 address table + global memory)   // SAMPT: the sampler is taken from the combined handle of texture slot r (high bits = sampler ID)   // Binding kind (type in the json); IMG = writable texture (D3D u_r, NVN image slot r); TBUF = typed buffer (D3D t_r Buffer<T>, buffer texture in NVN texture slot r)
// GMEM: the host's raw memory pool table for global memory the translator traced to no storage buffer (an address the shader loaded from memory; Dsh.cs Reflect "gmem_map", nvn_mtl.m gmem_map):
//   the shader may read, and with rw write, anything in any pool that was given an address. GMEML: the write log of those stores ("gmem_log", a bitmap of table entries, nvn_mtl.m gml_take)
enum { NVN_DPF_FLOAT = 0, NVN_DPF_UINT, NVN_DPF_SINT, NVN_DPF_DEPTH };            // Dummy texture pixel class (determined by msl_type)
enum { NVN_CT_FLOAT = 0, NVN_CT_UINT, NVN_CT_INT };                               // Vertex input component type
#define NVN_REFL_MAXARG 1024   // argument buffer entries of one shader (an array binding takes n of them)
// 11-02 从 192 放宽：192 是 10-24 随上游 参考实现 搬来的常量，不是 Metal 的限制（参数缓冲 Tier 2 每阶段可访问的纹理 / 缓冲数以十万计，参数块只是一段 8 字节一项的缓冲）。
//   马力欧派对 超级巨星 真机 64 支片元整支不可用（编号到 198–207）：5 个 Ryujinx 无法追溯句柄的回退数组（TEXD / SAMPD，各 32 项）占 160 项，
//   其后的图像 / 界长表 / 宽屏表顶过 191。宿主按它开的定长表（encode_args slots、len_fill、lb_fill、se_bind）随之放大，栈上合计约 12 KB；aid 是 uint16
// #1204 T1 bounds hardening (graphics/metal/gal_spvx.m, direct form): UBOLEN = gal_ubo_len, uint[NVN uniform slot 0..NVN_UBOLEN_N-1] = vec4 count of the bound uniform buffer;
//   SBSZ = SPIRV-Cross's spvBufferSizeConstants, uint[argument id of each storage buffer] = its bound byte size. Both filled per draw from the current bindings (nvn_mtl.m encode_args)
#define NVN_UBOLEN_N 14   // = gal_abi.h NXGAL_DIRECT_UBOLEN_N (tests/dsh_harden_test.sh checks they agree)
#define NVN_REFL_MAXV 32

typedef struct {
    uint8_t kind, rw;        // rw: storage buffer access is read_write / write / device
    uint8_t dtt, dpf;        // Dummy texture when the texture slot is unbound: MTLTextureType value, pixel class
    uint16_t reg, aid;       // D3D register number, index within the argument buffer
    uint16_t off;            // TEXU / SAMPU / TBUFU: byte offset of the handle within the uniform buffer; SBUFU: of the 64-bit storage buffer address
    uint8_t n;               // TEXD / SAMPD, and TEXU / SAMPU arrays (element k = the handle at byte 8k of the uniform buffer): array length (argument buffer entries aid .. aid + n - 1)
    uint8_t sid;             // SAMPU: the uniform word is a sampler ID, not a handle (Ryujinx SeparateSamplerId: shifted into the handle's high bits; The Witcher 3)
    uint8_t f1d;             // TEX: read by 1D integer fetches as 2D rows (json "fetch1d"): a buffer texture here is bound as rows (nvn_mtl.m tbuf_as_2d); 1 = rows of 16384 texels (Maxwell),
                             //   2 = exact rows, W * H = the element count ("fetch1d_exact", Ampere: the shader reads W and H from the form and 0 past them)
    uint8_t atomic_image;    // IMG / IMGU: native texture atomic calls; requires ShaderAtomic on this image's storage, not every writable texture
    uint8_t unb;             // TEX: 1 + the support buffer s2a_flags.yzw bit of this binding (json unnorm_bit = SupportBuffer.S2ATexUnnormBit(SPIR-V binding), #968); 0 = none (older library, or not covered)
    // Single TEXU / SAMPU / TBUFU / IMGU: the other word of the same Ryujinx handle (nvn_refl.m pair_from_name, from the binding's name). Ryujinx ReadPackedId reads the
    //   texture-buffer word and the sampler-buffer word and ORs them (the sampler one shifted by 20 for SeparateSamplerId): which word holds the texture ID is the game's, not the
    //   slot order's (Civilization VI: texture IDs in uniform slot 13, sampler handles in slot 12, the shader's OR names slot 12 first). pair = 0: no other half (read as before)
    uint8_t pair, pair_sid;  // pair_sid: the other half is a sampler ID word, shifted by 20 (its SAMPU sid)
    uint16_t pair_reg, pair_off;
    // #798 TBLEAD（gal_spvx.c spv_tbias，Dsh.cs kind tblead）：一个纹素缓冲的前导元素数（uint，着色器取数下标加它、取宽度减它）。宿主把视图建在向下取整到线性贴图对齐的边界上、
    //   宽度多出这么多元素，参数块里这一项指向常量表里值为它的一格（nvn_mtl.m tbl_bind）。tbl：TBUF / TBUFU / TEX 上 = 1 + 它那一项在 b[] 里的下标（0 = 没有，走 GPU 对齐副本）；
    //   TBLEAD 上 = 1 + 它所属纹素缓冲在 b[] 里的下标
    uint16_t tbl;
} nvn_rbind;
// SBSZ 表一项一项怎么填（nvn_mtl.m len_fill 调用；tests/t_sbsz.c 手算核）：q[成员的参数号] = 该存储缓冲当前绑定的字节数，返回表长（至少 1）。
//   SBUFP / UBOP：bound(ctx, b) 给出当前绑定的字节数（没绑 0，游戏没给长度 UINT32_MAX & ~15 = 不夹）；GMEM / GMEML（原始内存池表与写日志）：bound 给整段长度；SBUFU：不夹。
//   GEOMETRY（几何着色器模拟的辅助缓冲，reg 0 顶点捕获 / 1 几何顶点输出 / 2 几何索引输出 / 3 信息 / 4 装配表）：geo[reg] = 本笔临时区里该段的字节数。
//   11-06 鬼灭 #37：#1204 加固（11-02 de14970a6）只填前三类，几何缓冲一项没有；几何内核只有几何缓冲时表长 1，内核读 [2..4] 读到表外的环形临时区残值，
//   按它夹写：读到 0 一条不写、读到小值只写一部分，没写的索引槽留着上一次的内容被打包成三角形（越界索引、顶满容量把真三角形挤掉）→ 人物脸空白、手缺失
typedef uint32_t (*nvn_sbsz_bound_fn)(void *ctx, const nvn_rbind *b);
static inline int nvn_sbsz_fill(const nvn_rbind *rb, int nb, nvn_sbsz_bound_fn bound, void *ctx, const uint64_t geo[5], uint32_t *q) {
    const uint32_t nolim = UINT32_MAX & ~15u; int n = 0;
    for (int i = 0; i < nb; i++) {
        const nvn_rbind *b = &rb[i]; uint32_t v;
        if (b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_UBOP || b->kind == NVN_RB_GMEM || b->kind == NVN_RB_GMEML) v = bound(ctx, b);
        else if (b->kind == NVN_RB_SBUFU || b->kind == NVN_RB_SBUFQ) v = nolim;
        else if (b->kind == NVN_RB_GEOMETRY) { uint64_t z = b->reg < 5 ? geo[b->reg] : 0; v = z > nolim ? nolim : (uint32_t)z; }
        else continue;
        if (b->aid >= NVN_REFL_MAXARG) continue;
        for (; n <= b->aid; n++) q[n] = 0;
        q[b->aid] = v;
    }
    if (!n) q[n++] = 0;
    return n;
}
typedef struct {
    int nb, abi, nv;         // Binding count, argument buffer slot, vertex input count
    nvn_rbind *b;             // nb bindings, allocated to the reflection's count (freed with c1): a fixed 64 refused Monster Hunter Rise's 71-binding shaders (12) and Diablo II's 68 (24)
    struct { uint8_t loc, ct, nvn, sh[3]; } v[NVN_REFL_MAXV];   // loc: Metal vertex attribute slot (DXBC input register); nvn: NVN vertex attribute number (the n of HLSL semantic ATTRn);
                                                                // sh[ct]: Metal attribute index of the input's shadow of that component type in the <key>_bv library (0xff = none; dmsh_vin.py)
    uint8_t has_vin;         // has a shadow-input library <key>_bv (nvn_mtl.m vin_fn)
    uint16_t tg[3];          // Compute thread group size (none = 0)
    // Compute: stores the shader makes on its first invocation only, which the build took out of the shader (S.Dsh first_store; upstream reference implementation, MIT, c8521eeb / e07cba1c):
    //   word `word` of storage slot `reg` gets `value`, made by the host before each dispatch (nvn_mtl.m dispatch). At most 4
    struct { uint8_t reg; uint16_t word; uint32_t value; } first[4]; uint8_t nfirst;
    uint8_t route_b, has_c1; // Route B; has a c1 constant segment
    uint8_t bindless;        // Has TEXU / SAMPU / TBUFU bindings (which texture is read is known only at replay time)
    uint8_t dyn;             // Has TEXD / SAMPD bindings: c2's texture and sampler tables hold slot indices for this shader
    uint8_t pool;            // Has TEXP / SAMPP bindings (it also sets bindless): its argument block holds pool table addresses, which change when a table is rebuilt (nvn_mtl.m g_psnap_gen)
    uint8_t has_atomic_image; // Native image atomic binding; ordinary draw/dispatch needs no atomic prescan
    uint8_t has_tbl;         // #798: some texel buffer has a lead entry (TBLEAD; nvn_mtl.m tbl_bind)
    uint8_t has_unb;         // some TEX binding has an unnorm_bit (nvn_mtl.m ub_*: the host fills s2a_flags.yzw per draw); 0 = byte-for-byte the support buffer as before
    uint16_t unb_uncov;      // TEX bindings whose unnorm_bit is -1 (binding outside the bitmap: > 31, geometry, no unique mapping): normalization stays compiled in
    uint8_t has_user;        // bit 0: user_out known (vertex json user_out), bit 1: user_in known (pixel json user_in); a library without them leaves both 0
    uint32_t user_out, user_in;   // user(locn) positions as bits: what the vertex shader writes / the pixel shader reads (dmsh_build.py user_locs; nvn_mtl.m pso_pair_ok)
    uint8_t drv_read[3];     // B: reads driver segments c0 / c2 at runtime (indices 0, 2; gathered statically from the MSL at packaging time, tools/dmsh_build.py drv_reads)
    uint8_t drv_unfilled;    // B: also reads driver-segment locations the replay layer does not fill (drv_unfillable): they read 0, the shader is used anyway (err holds the list)
    uint16_t c1_len;         // Length of the c1 constant segment (B; 0 / 256 / 512 / 768 across the whole library)
    // For the fully-covering-first-draw proof (dmsh_build.py raster_facts; missing field = not provable): vertex - position xyz taken as-is from attribute pos_attr (-1 = no), w is the constant 1 (pos_w1) or taken from the same attribute's .w;
    // fragment - may discard, writes sample_mask, bitmap of the color numbers written
    int8_t pos_attr; uint8_t pos_w1, may_discard, writes_smask, color_mask, color_known;   // color_known: the color list was recorded at packaging (color_mask can be trusted)
    // Merge B step 2 (readable-tile version <key>_bf, dmsh_fetch.py): fragment - nfc redirectable samples (the i-th: texture slot fc_reg, interpolated output fc_locn),
    // bitmap fc_mixed of texture slots with mixed use (these slots must not be redirected to read the tile), nfo outputs (each with color number fo_color); vertex - .xy of interpolated output locn is taken as-is from attribute uv_attr[locn] (-1 = no)
    uint8_t nfc, fc_reg[32], fc_locn[32], nfo, fo_color[8]; uint64_t fc_mixed[4]; int8_t uv_attr[32];   // fc_mixed: a bit per texture slot 0-255 (slots 128+ are separate texture binds, NVN_SEPTEX_SLOT0)
    // Vertex - .x / .y of interpolated output locn = fma(.x / .y of attribute uv_aff_attr[locn], scale, offset), where scale / offset reference a uniform: (slot << 8) | (vector << 2) | component
    int8_t uv_aff_attr[32]; uint32_t uv_aff[32][4];   // [sx, ox, sy, oy]; a uniform reference: (slot << 16) | (vector 0..4095 << 2) | component, the whole 64 KB constant buffer
    // 10-30 遍内自采样改片上读取（nvn_mtl.m fb_tile_try）：fragment - fc_proj 第 i 个可改采样是投影屏幕坐标（in.m.xy / in.m.w，dmsh_fetch proj_uv）；
    //   vertex - scr_uv 第 L 位 = 插值量 locn L 是本顶点裁剪坐标的屏幕坐标（.x = (P.x + P.w) / 2、.y = (P.w - P.y) / 2、.w = P.w，main0 与 main0_z 都成立，dmsh_build scr_uv_locns）
    uint32_t fc_proj, scr_uv;
    // #1152：fragment - fc_ndc 第 i 个可改采样是片元算的 NDC→uv（fma(x / w, 0.5, 0.5)、fma(y / w, -0.5, 0.5)），证明看顶点 scr_pos；vertex - scr_pos 第 L 位 = 插值量 locn L 就是 gl_Position
    uint32_t fc_ndc, scr_pos;
    uint8_t *c1;             // c1 constant segment as-is (B): c1_len bytes, malloc'd by nvn_refl_from_json (NULL without one). It was a 4 KB array; Hyrule Warriors: Age of Calamity has 6 KB segments (its soldiers' vertex shaders)
} nvn_refl;

#ifdef __OBJC__
#import <Foundation/Foundation.h>
// 0 on success; on failure returns non-0 and writes the reason into err (unknown binding type, fixed-length limit exceeded, missing field)
int nvn_refl_from_json(NSDictionary *j, nvn_refl *out, char *err, int errlen);
#endif
