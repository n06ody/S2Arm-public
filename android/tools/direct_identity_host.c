// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Host fact transport only. Canonical encoding lives exclusively in the linked
// runtime/direct/input_identity.c. This tool does not infer runtime facts.
#include "direct/input_identity.h"
#include "direct/json.h"
#include "direct/sha256.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *why) { fprintf(stderr, "identity facts: %s\n", why); exit(1); }
static const s2a_json_field *field(const s2a_json_object *o, const char *key) {
    const s2a_json_field *f = s2a_json_get(o, key);
    if (!f) fail(key);
    return f;
}
static void object(const uint8_t *raw, size_t n, s2a_json_object *o) {
    char error[256];
    if (s2a_json_parse_object(raw, n, o, error, sizeof error)) fail(error);
}
static int integer(const s2a_json_field *f) {
    if (f->kind != S2A_JSON_NUMBER || floor(f->number) != f->number ||
        f->number < INT_MIN || f->number > INT_MAX) fail("int32 required");
    return (int)f->number;
}
static uint32_t uint32(const s2a_json_field *f) {
    if (f->kind != S2A_JSON_NUMBER || !f->is_unsigned || f->unsigned_value > UINT32_MAX)
        fail("uint32 required");
    return (uint32_t)f->unsigned_value;
}
static const char *string(const s2a_json_field *f) {
    if (f->kind == S2A_JSON_NULL) return NULL;
    if (f->kind != S2A_JSON_STRING || strlen(f->text) != f->text_bytes) fail("string required");
    return f->text;
}
static float real(const s2a_json_field *f) {
    if (f->kind != S2A_JSON_NUMBER || !isfinite((float)f->number)) fail("float32 required");
    return (float)f->number;
}
static void unhex(const s2a_json_field *f, uint8_t *out, size_t n) {
    const char *s = string(f);
    if (!s || strlen(s) != 2*n) fail("wrong hex length");
    for (size_t i=0; i<n; i++) {
        unsigned v=0;
        for (size_t k=0; k<2; k++) {
            char c=s[2*i+k];
            if (c>='0' && c<='9') v=v*16+(unsigned)(c-'0');
            else if(c>='a' && c<='f') v=v*16+(unsigned)(c-'a'+10);
            else fail("lowercase hex required");
        }
        out[i]=(uint8_t)v;
    }
}
static uint32_t array(const s2a_json_field *f, uint32_t max) {
    if (f->kind != S2A_JSON_ARRAY) fail("array required");
    uint32_t n=s2a_json_array_count(f);
    if (n>max) fail("array too long");
    return n;
}
static void item(const s2a_json_field *f, uint32_t i, s2a_json_object *o) {
    const uint8_t *raw; uint32_t n;
    if (s2a_json_array_span(f,i,&raw,&n)) fail("array item");
    // Parse scalar items using the same production JSON parser, not a second parser.
    if (n>4096) fail("array item too large");
    uint8_t wrapped[4104]; memcpy(wrapped,"{\"v\":",5);
    memcpy(wrapped+5,raw,n); wrapped[5+n]='}'; object(wrapped,n+6,o);
}
static void strings(const s2a_json_field *f, const char **out, unsigned n,
                    s2a_json_object *storage) {
    if (array(f,n)!=n) fail("wrong string array length");
    for(unsigned i=0;i<n;i++) { item(f,i,&storage[i]); out[i]=string(field(&storage[i],"v")); }
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--check-canonical")) {
        uint8_t bytes[S2A_INPUT_IDENTITY_MAX + 1];
        size_t len = fread(bytes, 1, sizeof bytes, stdin);
        static struct s2a_input_identity_decoded decoded;
        char reason[192];
        if (ferror(stdin) || len == sizeof bytes) fail("invalid canonical input length");
        if (s2a_input_identity_decode(bytes, len, &decoded, reason, sizeof reason)) fail(reason);
        return 0;
    }
    if (argc != 1) fail("usage: direct_identity_host [--check-canonical]");
    static uint8_t raw[65537]; size_t n=fread(raw,1,sizeof raw,stdin);
    if (ferror(stdin) || n==sizeof raw) fail("input read/size");
    static s2a_json_object root, gfx, stages[6], tex[64], strings_store[40], scalar;
    object(raw,n,&root);
    if(root.count != 13 && root.count != 15) fail("unexpected root fields");
    struct s2a_input_identity in={0}; struct s2a_compile_graphics_src g={0};
    struct s2a_compile_texture_src textures[64]={0};
    in.direct_abi=integer(field(&root,"direct_abi"));
    in.translator_closure=string(field(&root,"translator_closure"));
    unhex(field(&root,"profile_sha"),in.profile_sha,32);
    in.stage_mask=uint32(field(&root,"stage_mask"));
    in.requires_linked_varying_interface = s2a_direct_requires_linked_varying_interface(in.stage_mask);
    if(in.stage_mask==0x20) {
        if(root.count!=15) fail("missing compute alignment fields");
        in.compute_has_unaligned_ssbo=integer(field(&root,"compute_has_unaligned_ssbo"));
        in.compute_unaligned_ssbo=integer(field(&root,"compute_unaligned_ssbo"));
    } else if(root.count!=13) fail("unexpected graphics fields");
    unsigned present=uint32(field(&root,"source_present"));
    if(present>255) fail("source_present overflow");
    in.source_present=(uint8_t)present;
    const s2a_json_field *a=field(&root,"keys");
    if(array(a,6)!=6) fail("keys length");
    // The production encoder includes all six keys, even inactive slots.
    // Never silently zero caller facts which are part of the native identity.
    for(unsigned i=0;i<6;i++) { item(a,i,&scalar); unhex(field(&scalar,"v"),in.keys[i],8); }
    a=field(&root,"stages"); unsigned count=array(a,6);
    for(unsigned i=0;i<count;i++) {
        const uint8_t *r; uint32_t z;
        if(s2a_json_array_span(a,i,&r,&z)) fail("stage item");
        object(r,z,&stages[i]);
        if(stages[i].count != 3) fail("unexpected stage fields");
        unhex(field(&stages[i],"source_sha"),in.source_sha[i],32);
        in.source_code_bytes[i]=uint32(field(&stages[i],"code_bytes"));
        in.source_control_bytes[i]=uint32(field(&stages[i],"control_bytes"));
    }
    unsigned expected=0; for(unsigned m=in.stage_mask;m;m>>=1) expected+=m&1u;
    if(count!=expected) fail("stage count does not match mask");
    a=field(&root,"ubo_use"); if(array(a,6)!=6) fail("ubo length");
    for(unsigned i=0;i<6;i++) { item(a,i,&scalar); in.ubo_use[i]=uint32(field(&scalar,"v")); }
    in.texture_array_length=uint32(field(&root,"texture_array_length"));
    in.sampler_array_length=uint32(field(&root,"sampler_array_length"));
    in.transform_feedback_enabled=integer(field(&root,"transform_feedback_enabled"));
    const s2a_json_field *f=field(&root,"gfx"); object(f->raw,f->raw_bytes,&gfx);
    if(gfx.count != 35) fail("unexpected graphics fields");
    g.topology=string(field(&gfx,"topology"));
    g.alpha_test_compare=string(field(&gfx,"alpha_test_compare"));
    g.tess_mode=uint32(field(&gfx,"tess_mode"));
    g.point_size=real(field(&gfx,"point_size"));
    g.alpha_test_reference=real(field(&gfx,"alpha_test_reference"));
    strings(field(&gfx,"fragment_output_types"),g.fragment_output_types,8,strings_store);
    strings(field(&gfx,"attribute_types"),g.attribute_types,32,strings_store+8);
    g.rt_count=integer(field(&gfx,"rt_count"));
    g.has_tess_mode=integer(field(&gfx,"has_tess_mode"));
    g.constant_buffer_use_overflow=integer(field(&gfx,"constant_buffer_use_overflow"));
    g.has_alpha_to_coverage=integer(field(&gfx,"has_alpha_to_coverage"));
    g.alpha_to_coverage_enable=integer(field(&gfx,"alpha_to_coverage_enable"));
    g.alpha_to_coverage_dither=integer(field(&gfx,"alpha_to_coverage_dither"));
    g.has_alpha_test=integer(field(&gfx,"has_alpha_test"));
    g.has_blend=integer(field(&gfx,"has_blend"));
    g.dual_source_blend=integer(field(&gfx,"dual_source_blend"));
    g.has_depth_mode=integer(field(&gfx,"has_depth_mode"));
    g.depth_mode=integer(field(&gfx,"depth_mode"));
    g.has_y_negate=integer(field(&gfx,"has_y_negate"));
    g.y_negate=integer(field(&gfx,"y_negate"));
    g.has_origin=integer(field(&gfx,"has_origin"));
    g.origin_upper_left=integer(field(&gfx,"origin_upper_left"));
    g.has_halve_primitive_id=integer(field(&gfx,"has_halve_primitive_id"));
    g.halve_primitive_id=integer(field(&gfx,"halve_primitive_id"));
    g.has_cb_draw_params=integer(field(&gfx,"has_cb_draw_params"));
    g.cb_draw_params=integer(field(&gfx,"cb_draw_params"));
    g.has_point_size=integer(field(&gfx,"has_point_size"));
    g.program_point_size_enable=integer(field(&gfx,"program_point_size_enable"));
    g.has_alpha_test_reference=integer(field(&gfx,"has_alpha_test_reference"));
    g.has_unaligned_ssbo=integer(field(&gfx,"has_unaligned_ssbo"));
    g.unaligned_ssbo=integer(field(&gfx,"unaligned_ssbo"));
    g.has_geometry_shader=integer(field(&gfx,"has_geometry_shader"));
    g.host_supports_quads=integer(field(&gfx,"host_supports_quads"));
    g.early_z_force=integer(field(&gfx,"early_z_force"));
    g.viewport_transform_disable=integer(field(&gfx,"viewport_transform_disable"));
    in.gfx=&g; a=field(&root,"textures"); count=array(a,64);
    in.texture_count=(int)count; in.textures=textures;
    for(unsigned i=0;i<count;i++) {
        const uint8_t *r; uint32_t z;
        if(s2a_json_array_span(a,i,&r,&z)) fail("texture item");
        object(r,z,&tex[i]);
        if(tex[i].count != 9) fail("unexpected texture fields");
        struct s2a_compile_texture_src *t=&textures[i];
        t->stage=string(field(&tex[i],"stage"));
        t->sampler_type=string(field(&tex[i],"sampler_type"));
        t->format=string(field(&tex[i],"format"));
        t->handle=uint32(field(&tex[i],"handle"));
        t->array_length=uint32(field(&tex[i],"array_length"));
        t->cbuf_slot=integer(field(&tex[i],"cbuf_slot"));
        t->is_buffer=integer(field(&tex[i],"is_buffer"));
        t->normalized=integer(field(&tex[i],"normalized"));
        t->separate=integer(field(&tex[i],"separate"));
    }
    uint8_t canonical[S2A_INPUT_IDENTITY_MAX], digest[32]; size_t length=0;
    if(s2a_input_identity_encode(&in,canonical,sizeof canonical,&length)) fail("native encoder rejected facts");
    // Validate the exact production decoder/re-encoder, including v5 policy.
    // This is not a replacement for captured runtime/W2 cross-path fixtures.
    static struct s2a_input_identity_decoded decoded;
    uint8_t roundtrip[S2A_INPUT_IDENTITY_MAX]; size_t roundtrip_length = 0;
    char reason[192];
    if (s2a_input_identity_decode(canonical, length, &decoded, reason, sizeof reason))
        fail("native decoder rejected canonical identity");
    if (decoded.id.requires_linked_varying_interface != in.requires_linked_varying_interface ||
        s2a_input_identity_encode(&decoded.id, roundtrip, sizeof roundtrip, &roundtrip_length) ||
        roundtrip_length != length || memcmp(roundtrip, canonical, length))
        fail("native identity round-trip mismatch");
    s2a_sha256 h; s2a_sha256_init(&h); s2a_sha256_update(&h,canonical,length); s2a_sha256_final(&h,digest);
    char hex[65]; s2a_sha256_hex(digest,hex);
    printf("{\"identity\":\"%s\",\"canonical\":\"",hex);
    for(size_t i=0;i<length;i++) printf("%02x",canonical[i]);
    puts("\"}"); return ferror(stdout) ? 1 : 0;
}
