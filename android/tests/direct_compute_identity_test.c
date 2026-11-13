// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Hand-specified S2AI1/v3 wire fixtures, not an alternate encoder or captured output.
#include <stdio.h>
#include <string.h>
#include "direct/input_identity.h"

static int checks, fails;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++fails; printf("FAIL: %s\n", m); } } while (0)

int main(void) {
    struct s2a_compile_graphics_src gfx = {0};
    gfx.topology = "";
    gfx.alpha_test_compare = "";
    for (int i = 0; i < 8; ++i) gfx.fragment_output_types[i] = "";
    for (int i = 0; i < 32; ++i) gfx.attribute_types[i] = "";
    struct s2a_compile_texture_src tex = {
        "vs", 0x12345678, -1, "2d", "R8G8B8A8Unorm", 0, 1, 0, 1
    };
    struct s2a_input_identity in = {0};
    in.direct_abi = 5;
    in.translator_closure = "";
    in.stage_mask = 3;
    in.source_present = 3;
    in.source_code_bytes[0] = in.source_code_bytes[1] = 1;
    in.source_control_bytes[0] = in.source_control_bytes[1] = 1;
    in.gfx = &gfx;
    in.textures = &tex;
    in.texture_count = 1;
    // Empty strings have four zero length bytes. Other scalar fields are zero.
    // Header ends at 99; two source records occupy 80 bytes; graphics/facts
    // end at 430. The 17-byte texture record has target=1 and format=24.
    uint8_t graphics[447] = {
        [0]='S', [1]='2', [2]='A', [3]='I', [4]='1', [5]=3, [6]=5,
        [46]=3, [98]=3, [131]=1, [135]=1, [171]=1, [175]=1,
        [426]=1, [430]=0, [431]=0x78, [432]=0x56, [433]=0x34, [434]=0x12,
        [435]=255, [436]=255, [437]=255, [438]=255,
        [439]=1, [440]=24, [442]=2, [443]=1
    };
    uint8_t out[S2A_INPUT_IDENTITY_MAX];
    size_t n = 0;
    int rc = s2a_input_identity_encode(&in, out, sizeof out, &n);
    CHECK(rc == 0 && n == sizeof graphics && !memcmp(out, graphics, sizeof graphics),
          "vs canonical bytes unchanged");
    tex.stage = "fs";
    graphics[430] = 1;
    rc = s2a_input_identity_encode(&in, out, sizeof out, &n);
    CHECK(rc == 0 && n == sizeof graphics && !memcmp(out, graphics, sizeof graphics),
          "fs canonical bytes unchanged");

    // NVN CS is bit 5, but proof is packed in slot 0, not slot 5. One source
    // record removes 40 bytes. Texture stage index 2 is distinct from vs/fs.
    static const uint8_t compute[409] = {
        [0]='S', [1]='2', [2]='A', [3]='I', [4]='1', [5]=3, [6]=5,
        [46]=32, [98]=1, [131]=1, [135]=1,
        [386]=1, [390]=2, [391]=0x78, [392]=0x56, [393]=0x34, [394]=0x12,
        [395]=255, [396]=255, [397]=255, [398]=255,
        [399]=1, [400]=24, [402]=2, [403]=1, [407]=1, [408]=1
    };
    tex.stage = "cs";
    in.stage_mask = 32;
    in.source_present = 1;
    in.compute_has_unaligned_ssbo = 1;
    in.compute_unaligned_ssbo = 1;
    rc = s2a_input_identity_encode(&in, out, sizeof out, &n);
    CHECK(rc == 0 && n == sizeof compute && !memcmp(out, compute, sizeof compute),
          "cs canonical bytes and packed source proof");
    in.compute_unaligned_ssbo = 0;
    rc = s2a_input_identity_encode(&in, out, sizeof out, &n);
    CHECK(rc == 0 && n == sizeof compute && out[n-2] == 1 && out[n-1] == 0 &&
          !memcmp(out, compute, n-1), "aligned CS differs only in alignment value");
    in.compute_has_unaligned_ssbo = 0;
    CHECK(s2a_input_identity_encode(&in, out, sizeof out, &n) == -1, "unknown CS alignment rejected");
    in.compute_has_unaligned_ssbo = 1; in.compute_unaligned_ssbo = 2;
    CHECK(s2a_input_identity_encode(&in, out, sizeof out, &n) == -1, "invalid CS alignment rejected");
    in.compute_unaligned_ssbo = 1;
    static const char *bad[] = {NULL, "", "unknown", "CS", "cs ", "tcs"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        tex.stage = bad[i];
        n = 777;
        CHECK(s2a_input_identity_encode(&in, out, sizeof out, &n) == -1 && n == 777,
              "unknown stage rejected without publishing output length");
    }
    tex.stage = "cs";
    in.source_present = 32;
    CHECK(s2a_input_identity_encode(&in, out, sizeof out, &n) == -1,
          "unpacked cs proof rejected");
    in.source_present = 1;
    in.source_code_bytes[0] = 0;
    CHECK(s2a_input_identity_encode(&in, out, sizeof out, &n) == -1,
          "missing cs code length rejected");
    in.source_code_bytes[0] = 1;
    CHECK(s2a_input_identity_encode(&in, out, sizeof compute - 1, &n) == -1,
          "cs truncation rejected");
    tex.format = "unknown";
    CHECK(s2a_input_identity_encode(&in, out, sizeof out, &n) == -1,
          "cs unknown format rejected");
    printf("compute identity: %d/%d passed\n", checks - fails, checks);
    return fails != 0;
}
