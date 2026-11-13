// Compare identical VS, VS+FS and CS identities with the pre-GS encoder.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "direct/input_identity.h"
static int bad;
#define CHECK(c, what) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", what); bad++; } } while (0)
int main(int argc, char **argv) {
    struct s2a_compile_graphics_src gfx = {0}; gfx.topology = "Triangles";
    for (int i = 0; i < 8; i++) gfx.fragment_output_types[i] = "Float";
    for (int i = 0; i < 32; i++) gfx.attribute_types[i] = "Float";
    struct s2a_input_identity in = {0}; in.gfx = &gfx; in.translator_closure = "identity-compat"; in.direct_abi = 1;
    struct s2a_compile_texture_src tex[96] = {0};
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
    in.texture_count = 97; CHECK(s2a_input_identity_encode(&in, b, sizeof b, &nb) != 0, "97 facts explicitly rejected");
    if (!bad) puts("identity: baseline VS/VS+FS/CS byte-identical; 96 facts and 97 rejection passed");
    return bad != 0;
}
