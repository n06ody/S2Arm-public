//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
// Tegra X1 block-linear layout (public descriptions: deko3d / Ryujinx BlockLinearLayout):
//   GOB = 64 bytes × 8 rows = 512 bytes; offset of (x byte, y row) within a GOB =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   block = 1 GOB wide × 2^tileH GOBs high × 2^tileD GOBs deep, ordered y first then z within a block; blocks fill the level in x → y → z row-major order.
// Level / layer start offsets and the actual block height and block depth of this level come from nvn_tex.c; here we only do intra-level addressing and copying.
#include "nvn_deswizzle.h"
#include <string.h>

// Switch 2 (NVN2) orders a GOB's 16-byte pieces otherwise: (x%64/32)·256 + (y%8/4)·128 + (x%32/16)·64 + (y%4/2)·32 + (y%2)·16 + x%16 (nvn_gob_switch2; the reference port fdb3710d, MIT)
int nvn_gob_switch2;   // see nvn_deswizzle.h

static uint32_t gob_off(uint32_t x, uint32_t y) {
    if (nvn_gob_switch2) return (x % 64 / 32) * 256 + (y % 8 / 4) * 128 + (x % 32 / 16) * 64 + (y % 4 / 2) * 32 + (y % 2) * 16 + x % 16;
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;                                  // bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // which block row
         + (x / 64) * block                                            // which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // which GOB within the block
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// Geometry of this level: width-in-blocks byte count, block row count, slice count, and where this level and layer starts in storage; returns -1 for unsupported textures
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || (d->flags & 0x10) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

// 线性排布（LINEAR 0x10 / LINEAR_RENDER_TARGET 0x40，单级 2D，nvn_tex_layout 同口径：行距 = stride 或按 32 / 128 对齐）：逐行拷贝，不是块线性。
//   11-09 质量线（#255 / #262）：原先这里对线性贴图直接返回 -1，宿主建不了零拷贝视图的线性贴图（iPhone 行距要 64 字节对齐、行距不够 Metal 要求）走上传路径时内容一字节没传
//   （哈迪斯 II iPhone 日志「deswizzle failed format 0x9a target 1」）。行距放不下一行像素（stride < 宽 × 每像素字节）返回 -1，不越界读写
static int move_linear(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *mem, uint8_t *lin, size_t pitch, int to_linear) {
    if (level || layer || d->target != 1 || d->samples > 1) return -1;
    int bw, bh; uint32_t bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) return -1;
    uint32_t row_bytes = (d->width + bw - 1) / bw * bpb, rows = (d->height + bh - 1) / bh;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    uint64_t sp = rows ? L.size / rows : 0;
    if (!rows || sp < row_bytes || pitch < row_bytes) return -1;
    for (uint32_t y = 0; y < rows; y++) { if (to_linear) memcpy(lin + (size_t)y * pitch, mem + y * sp, row_bytes); else memcpy(mem + y * sp, lin + (size_t)y * pitch, row_bytes); }
    return 0;
}
static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear, int level_local) {
    if ((d->flags & (0x10 | 0x40)) && d->target != 10) return move_linear(d, level, layer, bl, lin, pitch, to_linear);
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    if (!level_local) bl += base;
    uint32_t lb = nvn_tex_level_gobs(d, level) * 64;   // the row as laid out (padded in a sparse texture's tiled levels)
    if (lb < row_bytes) lb = row_bytes;
    // A whole GOB column's four 16-byte pieces (x%64 = 0/16/32/48) lie inside one GOB, at the x terms of the layout formula only:
    //   the y terms of gob_off are common to all four, so from the column's start (gob_off(0, y)) they sit at gob_off(16, 0) / gob_off(32, 0) / gob_off(48, 0)
    //   — taken from the same formula, not hand-derived (the GPU kernel k_nvn_deswz does the same addressing per 16-byte piece). Computed once per call
    uint32_t o1 = gob_off(16, 0), o2 = gob_off(32, 0), o3 = gob_off(48, 0);
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *row = lin + ((size_t)z * rows + y) * pitch;
            uint32_t x = 0;
            for (; x + 64 <= row_bytes; x += 64) {                    // whole GOB columns: the four 16-byte pieces straight to their addresses, one offset computation per column
                uint8_t *p = bl + nvn_bl_offset(x, y, z, lb, rows, th, td);
                if (to_linear) {
                    memcpy(row + x, p, 16); memcpy(row + x + 16, p + o1, 16); memcpy(row + x + 32, p + o2, 16); memcpy(row + x + 48, p + o3, 16);
                } else {
                    memcpy(p, row + x, 16); memcpy(p + o1, row + x + 16, 16); memcpy(p + o2, row + x + 32, 16); memcpy(p + o3, row + x + 48, 16);
                }
            }
            for (; x < row_bytes; x += 16) {                          // 16-byte segments: 16 bytes are contiguous within a GOB, and 16 divides all block widths
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint8_t *p = bl + nvn_bl_offset(x, y, z, lb, rows, th, td);
                if (to_linear) memcpy(row + x, p, n); else memcpy(p, row + x, n);
            }
        }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1, 0);
}
int nvn_deswizzle_level(const nvn_texdesc *d, unsigned level, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, 0, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0, 0);
}
