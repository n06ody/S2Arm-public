// Compare identical VS, VS+FS and CS identities with the pre-GS encoder.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "direct/input_identity.h"
#include "direct/texture_facts.h"
static int bad;
#define CHECK(c, what) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", what); bad++; } } while (0)
int main(int argc, char **argv) {
    struct s2a_compile_graphics_src gfx = {0}; gfx.topology = "Triangles";
    for (int i = 0; i < 8; i++) gfx.fragment_output_types[i] = "Float";
    for (int i = 0; i < 32; i++) gfx.attribute_types[i] = "Float";
    struct s2a_input_identity in = {0}; in.gfx = &gfx; in.translator_closure = "identity-compat"; in.direct_abi = 1;
    struct s2a_compile_texture_src tex[S2A_COMPILE_TEXTURE_MAX] = {0};
    for (int i = 0; i < 96; i++) { tex[i].stage = i < 32 ? "vs" : i < 64 ? "fs" : "gs"; tex[i].handle = 8 + 2 * (i % 32); tex[i].cbuf_slot = -1; tex[i].sampler_type = "2d"; tex[i].format = "Unknown"; tex[i].normalized = 1; tex[i].array_length = 1; }
    in.textures = tex; in.texture_array_length = 32; in.sampler_array_length = 32;
    for (int i = 0; i < 3; i++) { in.keys[i][0] = (uint8_t)(i + 1); in.source_sha[i][0] = (uint8_t)(i + 9); in.source_code_bytes[i] = 112; in.source_control_bytes[i] = 1888; }
    uint8_t a[S2A_INPUT_IDENTITY_MAX], b[S2A_INPUT_IDENTITY_MAX]; size_t na, nb;
    const unsigned masks[] = {1, 3, 32};
    for (int i = 0; i < 3; i++) {
        in.stage_mask = masks[i]; in.source_present = masks[i] == 3 ? 3 : 1;
        in.texture_count = masks[i] == 3 ? 64 : 32;
        for (int j = 0; j < in.texture_count; j++) tex[j].stage = masks[i] == 32 ? "cs" : j < 32 ? "vs" : "fs";
        in.compute_has_unaligned_ssbo = masks[i] == 32;
        CHECK(argc == 4, "three pre-GS identity fixtures supplied");
        if (argc != 4) return 1;
        FILE *f = fopen(argv[i + 1], "rb"); CHECK(f != NULL, "pre-GS identity fixture opens"); if (!f) return 1;
        na = fread(a, 1, sizeof a, f); int extra = fgetc(f); fclose(f);
        nb = 0; int y = s2a_input_identity_encode(&in, b, sizeof b, &nb);
        CHECK(extra == EOF && y == 0 && na == nb && !memcmp(a,b,na), "old vs new bytes for VS, VS+FS or CS");

    }
    in.stage_mask = 7; in.source_present = 7; in.texture_count = 96;
    for (int i = 0; i < 96; i++) tex[i].stage = i < 32 ? "vs" : i < 64 ? "fs" : "gs";
    na = 0; CHECK(!s2a_input_identity_encode(&in, a, sizeof a, &na), "96 texture facts fit");
    if (na >= 96 * 17) for (int i = 0; i < 96; i++) CHECK(a[na - 96 * 17 + i * 17] == (i < 32 ? 0 : i < 64 ? 1 : 3), "stage tags preserve VS FS and distinct GS");
    tex[95].format = "R8G8B8A8Unorm"; nb = 0;
    CHECK(!s2a_input_identity_encode(&in, b, sizeof b, &nb) && (na != nb || memcmp(a,b,na)), "last GS fact changes identity");
    for (int i = 0; i < S2A_COMPILE_TEXTURE_MAX; i++) {
        int st = i / 160, slot = i % 160;
        tex[i].stage = st == 0 ? "vs" : st == 1 ? "fs" : "gs";
        tex[i].handle = slot < 32 ? 8 + 2 * slot : 90 + 2 * (slot - 32);
        tex[i].cbuf_slot = slot < 32 ? -1 : 2; tex[i].separate = slot >= 32;
        tex[i].sampler_type = "2d"; tex[i].format = "Unknown"; tex[i].normalized = 1; tex[i].array_length = 1;
    }
    in.texture_count = 96; na = 0;
    CHECK(!s2a_input_identity_encode(&in, a, sizeof a, &na), "96 facts at combined/separate boundary fit");
    in.texture_count = 97; nb = 0;
    CHECK(!s2a_input_identity_encode(&in, b, sizeof b, &nb) && nb == na + 17 && memcmp(a, b, na), "97th fact is encoded without truncation");
    in.texture_count = S2A_COMPILE_TEXTURE_MAX; na = 0;
    CHECK(!s2a_input_identity_encode(&in, a, sizeof a, &na), "480 texture facts fit");
    if (na >= S2A_COMPILE_TEXTURE_MAX * 17) for (int i = 0; i < S2A_COMPILE_TEXTURE_MAX; i++) {
        size_t off = na - S2A_COMPILE_TEXTURE_MAX * 17 + i * 17;
        CHECK(a[off] == (i / 160 == 0 ? 0 : i / 160 == 1 ? 1 : 3), "480 facts retain stage tags");
        CHECK(!!(a[off + 12] & 4) == (i % 160 >= 32), "480 facts retain separate flag");
    }
    in.texture_count = S2A_COMPILE_TEXTURE_MAX + 1; CHECK(s2a_input_identity_encode(&in, b, sizeof b, &nb) != 0, "481 facts explicitly rejected");
    {
        // The stable identity index is appended, not inserted before existing targets.
        uint8_t idx = 255;
        CHECK(!s2a_nvn_texture_target_index("2d", &idx) && idx == 1, "existing 2d identity index stays 1");
        CHECK(!s2a_nvn_texture_target_index("buffer", &idx) && idx == 10, "buffer identity index is appended at 10");
        in.stage_mask = 1; in.source_present = 1; in.texture_count = 1;
        tex[0] = (struct s2a_compile_texture_src){"vs", 90, 2, "buffer", "R32Uint", 1, 1, 1, 1};
        na = 0;
        CHECK(!s2a_input_identity_encode(&in, a, sizeof a, &na), "buffer texture fact has a canonical identity");
        if (na >= 17) CHECK(a[na - 17 + 9] == 10 && a[na - 17 + 12] == 7, "buffer identity retains target and flags");
        tex[0].sampler_type = "2d"; tex[0].is_buffer = 0;
        nb = 0;
        CHECK(!s2a_input_identity_encode(&in, b, sizeof b, &nb), "2d control identity encodes");
        if (na && nb == na) CHECK(memcmp(a, b, na) != 0, "buffer identity cannot collide with 2d");
    }
    if (!bad) puts("identity: baseline VS/VS+FS/CS byte-identical; 96, 97 and 480 facts, 481 rejection passed");
    return bad != 0;
}
