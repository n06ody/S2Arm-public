// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// #674 (11-15): a 2D / 2D array texture in a slot the shader declares 3D.
//   Metal has no texture view from 2D to 3D, so tex_fit found nothing and the slot read the zero texture: a game that
//   keeps a small 2D placeholder in the 3D slots of its lighting shaders got "no light" there, and the lit surfaces
//   went black for the frames the placeholder was bound. Hardware samples the bound texture's own texels, so the host
//   copies them into a 3D texture of the same format (one depth plane per layer) and binds that.
//   The decisions are here, pure C, so they can be tested offline (graphics/nvn/tests/t_tex3d_bridge.c);
//   the Metal side is tex3d_prepare / tex_fit in nvn_mtl.m.
#ifndef S2A_NVN_TEX3D_BRIDGE_H
#define S2A_NVN_TEX3D_BRIDGE_H
#include <stdint.h>

enum { T3B_TYPE_2D = 2, T3B_TYPE_2D_ARRAY = 3, T3B_TYPE_3D = 7 };   // MTLTextureType raw values

// -> the depth of the 3D copy, 0 = no bridge (the caller keeps today's behaviour: the zero texture and a report).
//   Only a single-sample 2D / 2D array texture read as 3D, of a format a 3D texture can take (pf_plain: not
//   compressed, not depth / stencil). A 2D texture is one plane, a 2D array one plane per layer.
static inline unsigned t3b_depth(int have, int want, unsigned samples, unsigned layers, int pf_plain) {
    if (want != T3B_TYPE_3D || samples != 1 || !pf_plain) return 0;
    if (have == T3B_TYPE_2D) return 1;
    if (have == T3B_TYPE_2D_ARRAY) return layers ? layers : 1;
    return 0;
}

enum { T3B_USE = 0, T3B_COPY = 1 };
// What a draw that reads the bridge does before it binds. made = frame of the last copy + 1 (0 = never copied).
//   Never copied: copy now, even if that ends the open pass (a blit cannot start inside one), or the draw reads 0.
//   Copied this frame: use it. Copied in an earlier frame: refresh it, but only where no pass is open — ending a
//   pass to refresh costs a store and a load of its attachments, and the copy is at most a frame old meanwhile.
static inline int t3b_action(uint64_t made, uint64_t frame, int pass_open) {
    if (!made) return T3B_COPY;
    if (made == frame + 1) return T3B_USE;
    return pass_open ? T3B_USE : T3B_COPY;
}
#endif
