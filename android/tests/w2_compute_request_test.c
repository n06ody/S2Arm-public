// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn/nvn_vk_compute_request.h"
#include "direct/sha256.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static const char profile_json[] = "{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";
static const char profile_hash[] = "sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static int failed, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fprintf(stderr,"FAIL: %s\n",msg); failed++; } } while (0)
static void wr(uint8_t *p, unsigned v) { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
static void keep(uint8_t *ctl, const uint8_t *code, uint8_t key[8]) {
    uint8_t digest[32]; s2a_sha256 h; s2a_sha256_init(&h);
    s2a_sha256_update(&h,ctl,0x760);s2a_sha256_update(&h,code,16);s2a_sha256_final(&h,digest);
    memcpy(key,digest,8);CHECK(!s2a_source_keep(key,5,code,16,ctl,0x760),"source fixture");
}
int main(void) {
    uint8_t ctl[0x760]={0}, code[16]={1,2,3,4}, identity[32];
    wr(ctl+0x20,0x760);wr(ctl+0x748,8);wr(ctl+0x74c,2);wr(ctl+0x750,1);
    wr(ctl+0x754,17);wr(ctl+0x758,32);wr(ctl+0x700,4);wr(ctl+0x704,8);
    struct s2a_record_dispatch_state s={0};s.stages=32;s.ubo_use[5]=3;
    s.tex_pool_seen=s.samp_pool_seen=1;s.tex_pool_n=7;s.samp_pool_n=9;
    keep(ctl,code,s.keys[0]);
    struct s2a_direct_compile_context c={.session=4,.generation=6,.direct_abi=1,
        .profile_json=profile_json,.profile_hash=profile_hash,.translator_closure="closure-test"};
    c.profile_bytes=strlen(c.profile_json);
    struct nvk_compute_texture_facts tex={.complete=1};
    struct nvk_compute_request r;char why[128]; int known=1, unaligned=1;
#define PREP() nvk_compute_request_prepare(&s,&c,&tex,known,unaligned,"quote\"\\\n",&r,why,sizeof why)
#define BAD(msg) do { CHECK(PREP()!=0,msg); CHECK(!r.source.ref&&!r.json,"failed output released"); } while(0)
    CHECK(!PREP(),"positive request");
    CHECK(r.profile.storage_alignment==16&&!strcmp(r.profile.hash,profile_hash),"profile facts exact");
    CHECK(r.source.ref && r.local_memory_bytes==32 && r.cs.shared_bytes==32,"control facts exact");
    CHECK(r.cs.local[0]==8 && r.cs.local[1]==2 && r.cs.local[2]==1,"local size exact");
    memcpy(identity,r.identity,32);
    puts(r.json);printf("canonical=");for(size_t i=0;i<r.canonical_bytes;i++)printf("%02x",r.canonical[i]);puts("");
    nvk_compute_request_release(&r);nvk_compute_request_release(&r);
    // 计算源指令流偏移（与 graphics/tools/direct/dsh/Dsh.cs:149-155 同一口径）：
    //   · 79 56 34 12（计算 magic）头 + 0x30..0x100 零填充 → 指令从 +0x100 起；
    //   · 78 56 34 12（图形 magic）出现在计算源里 → 明确拒绝；
    //   · 79 头但 0x30..0x100 不是零填充 → 明确拒绝（不猜偏移）；
    //   · 无头源：指令从 +0 起，不剥。
    {
        uint8_t hcode[0x100 + 16];
        memset(hcode, 0, sizeof hcode);
        hcode[0] = 0x79; hcode[1] = 0x56; hcode[2] = 0x34; hcode[3] = 0x12;
        hcode[0x100] = 1; hcode[0x101] = 2; hcode[0x102] = 3; hcode[0x103] = 4;
        uint8_t hdigest[32]; s2a_sha256 hh; s2a_sha256_init(&hh);
        s2a_sha256_update(&hh, ctl, 0x760); s2a_sha256_update(&hh, hcode, sizeof hcode);
        s2a_sha256_final(&hh, hdigest); memcpy(s.keys[0], hdigest, 8);
        CHECK(!s2a_source_keep(s.keys[0], 5, hcode, sizeof hcode, ctl, 0x760), "compute header source fixture");
        CHECK(!PREP(), "compute header source request");
        CHECK(strstr(r.json, "\"code\":\"AQIDBAAAAAAAAAAAAAAAAA==\"") != NULL,
              "0x12345679 compute header strips 0x100 bytes (instructions at +0x100)");
        CHECK(strstr(r.json, "eVY0Eg") == NULL, "compute header bytes never reach the compiler");
        nvk_compute_request_release(&r);
        // 79 头但 0x30..0x100 里有一个非零字节：布局不明 → 拒绝，不猜偏移。
        hcode[0x60] = 0xAA;
        s2a_sha256_init(&hh); s2a_sha256_update(&hh, ctl, 0x760);
        s2a_sha256_update(&hh, hcode, sizeof hcode); s2a_sha256_final(&hh, hdigest);
        memcpy(s.keys[0], hdigest, 8);
        CHECK(!s2a_source_keep(s.keys[0], 5, hcode, sizeof hcode, ctl, 0x760), "dirty padding fixture");
        BAD("compute 0x30..0x100 padding with data is rejected");
        hcode[0x60] = 0;
        // 图形 magic 出现在计算源里：拒绝。
        hcode[0] = 0x78;
        s2a_sha256_init(&hh); s2a_sha256_update(&hh, ctl, 0x760);
        s2a_sha256_update(&hh, hcode, sizeof hcode); s2a_sha256_final(&hh, hdigest);
        memcpy(s.keys[0], hdigest, 8);
        CHECK(!s2a_source_keep(s.keys[0], 5, hcode, sizeof hcode, ctl, 0x760), "graphics magic fixture");
        BAD("graphics code-area header in a compute source is rejected");
        // 无头源：不剥，原样交给编译器。
        keep(ctl, code, s.keys[0]);
    }
    c.session=42;c.generation=63;CHECK(!PREP(),"new transport");
    CHECK(!memcmp(identity,r.identity,32),"transport excluded from content identity");nvk_compute_request_release(&r);
    known=0;BAD("unknown alignment rejected");known=1;
    unaligned=2;BAD("invalid alignment rejected");unaligned=0;
    CHECK(!PREP(),"aligned request");
    CHECK(memcmp(identity,r.identity,32)!=0,"alignment changes identity");
    CHECK(strstr(r.json,"\"hasUnalignedStorageBuffer\":false")!=NULL,"aligned request fact");
    nvk_compute_request_release(&r);unaligned=1;
    s.local_memory_seen=1;s.local_memory_bytes=0;BAD("conflicting local memory");
    s.local_memory_bytes=32;CHECK(!PREP(),"matching observed local memory");nvk_compute_request_release(&r);s.local_memory_seen=0;
    s.stages=3;BAD("graphics rejected");s.stages=32;
    s.ubo_use[5]=(1u<<19);BAD("cbuf beyond 18 rejected");
    // constantBufferUse 是 cbuf 号掩码：compute 的用户 uniform 槽 0..15 = cbuf 3..18，全部可表达。
    // 旧 5 位窗口（cbuf 0..4）保持原语义；出现窗口外的位时落 v4 身份（旧缓存/旧产物仍按 v3 可读）。
    {
        uint8_t window_id[32], wide_id[32];
        s.ubo_use[5]=0x18;   // cbuf 3,4
        CHECK(!PREP(),"old-window cbuf mask accepted");
        memcpy(window_id,r.identity,32);
        CHECK(strstr(r.json,"\"constantBufferUse\":24")!=NULL,"old window serialized");
        nvk_compute_request_release(&r);
        s.ubo_use[5]=0x18|(1u<<5);   // + cbuf 5
        CHECK(!PREP(),"cbuf 5 accepted");
        CHECK(strstr(r.json,"\"constantBufferUse\":56")!=NULL,"cbuf 5 serialized in the same canonical mask");
        memcpy(wide_id,r.identity,32);
        CHECK(memcmp(wide_id,window_id,32)!=0,"cbuf 5 changes identity");
        nvk_compute_request_release(&r);
        CHECK(!PREP(),"same wide input twice");
        CHECK(!memcmp(wide_id,r.identity,32),"wide input identity is byte-stable");
        nvk_compute_request_release(&r);
        const uint32_t boundaries[] = { 1u<<5, 1u<<6, 1u<<7, 1u<<18 };
        for (unsigned i=0;i<sizeof boundaries/sizeof boundaries[0];i++) {
            s.ubo_use[5]=0x18|boundaries[i];
            CHECK(!PREP(),"cbuf 5/6/7/18 boundary accepted");
            nvk_compute_request_release(&r);
        }
        s.ubo_use[5]=0x18|(1u<<19);BAD("cbuf beyond 18 rejected (width insufficient)");
        s.ubo_use[5]=0x18;
        s.ubo_use_overflow=1;BAD("record-proven unexpressible mask rejected");s.ubo_use_overflow=0;
        s.ubo_use[5]=3;
    }
    s.ubo_use_overflow=1;BAD("ubo overflow seen");s.ubo_use_overflow=0;
    s.tex_pool_seen=0;BAD("unknown pool");s.tex_pool_seen=1;
    s.tex_pool_n=0;BAD("zero pool");s.tex_pool_n=7;
    tex.complete=0;BAD("unknown texture facts");tex.complete=1;
    tex.count=S2A_COMPILE_TEXTURE_MAX+1;BAD("texture limit");tex.count=0;
    c.profile_json="{}junk";c.profile_bytes=6;BAD("malformed profile");
    c.profile_json=profile_json;c.profile_hash=profile_hash;c.profile_bytes=strlen(c.profile_json);
    c.profile_hash="sha256-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";BAD("provider profile mismatch");c.profile_hash=profile_hash;
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":null}}";c.profile_bytes=strlen(c.profile_json);BAD("invalid profile A");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":0}}";c.profile_bytes=strlen(c.profile_json);BAD("invalid profile A");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":3}}";c.profile_bytes=strlen(c.profile_json);BAD("invalid profile A");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":65537}}";c.profile_bytes=strlen(c.profile_json);BAD("invalid profile A");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":\"16\"}}";c.profile_bytes=strlen(c.profile_json);BAD("invalid profile A");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":true}}";c.profile_bytes=strlen(c.profile_json);BAD("invalid profile A");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":[]}}";c.profile_bytes=strlen(c.profile_json);BAD("invalid profile A");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":{}}}";c.profile_bytes=strlen(c.profile_json);BAD("invalid profile A");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{}}";c.profile_bytes=strlen(c.profile_json);BAD("missing profile A");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":null}";c.profile_bytes=strlen(c.profile_json);BAD("missing profile A");
    c.profile_json="{}";c.profile_bytes=strlen(c.profile_json);BAD("missing profile A");
    c.profile_json=profile_json;c.profile_bytes=strlen(c.profile_json);
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\\u0000junk\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";c.profile_bytes=strlen(c.profile_json);BAD("embedded NUL profile field");
    c.profile_json="{\"schema\":\"s2a-direct-profile-v1\",\"profileHash\":\"sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\u0000junk\",\"capabilities\":{\"storageBufferOffsetAlignment\":16}}";c.profile_bytes=strlen(c.profile_json);BAD("embedded NUL profile field");
    c.profile_json=profile_json;c.profile_bytes=strlen(c.profile_json);
    struct s2a_compile_texture_src items[2]={{"cs",3,-1,"2d","R8G8B8A8Unorm",0,1,0,1}};
    tex.items=items;tex.count=1;
    CHECK(!PREP(),"real CS texture fact");
    CHECK(r.canonical[r.canonical_bytes-19]==2,"CS texture canonical stage");
    CHECK(strstr(r.json,"\"samplerType\":\"2d\"")!=NULL,"texture serialized");
    uint8_t known_format_identity[32]; memcpy(known_format_identity,r.identity,32);
    nvk_compute_request_release(&r);
    items[0].stage="fs";BAD("foreign-stage texture");items[0].stage="cs";
    // 0e14af20b: Unknown is a supported sampling-only fact, not an invalid enum.
    // A shader that actually queries its image format is rejected by the compiler.
    items[0].format="Unknown";
    CHECK(!PREP(),"sampling-only Unknown format accepted");
    CHECK(r.source.ref && r.json,"Unknown request owns pinned source and JSON");
    CHECK(r.json && strstr(r.json,"\"format\":\"Unknown\""),"Unknown format serialized exactly");
    CHECK(memcmp(known_format_identity,r.identity,32)!=0,"Unknown differs from known format identity");
    nvk_compute_request_release(&r);
    CHECK(!r.source.ref && !r.json && !r.json_bytes,"Unknown output released explicitly");
    items[0].format="NotAFormat";BAD("invalid texture format rejected");
    items[0].format="R8G8B8A8Unorm";
    items[1]=items[0];tex.count=2;BAD("duplicate texture query");tex.count=0;tex.items=NULL;
    wr(ctl+0x758,0x100001);keep(ctl,code,s.keys[0]);BAD("local memory out of range");
    wr(ctl+0x758,64);keep(ctl,code,s.keys[0]);CHECK(!PREP(),"changed local memory");
    CHECK(memcmp(identity,r.identity,32)!=0,"local memory changes identity");
    CHECK(r.local_memory_bytes==64,"changed local memory exact");nvk_compute_request_release(&r);
    wr(ctl+0x748,0);keep(ctl,code,s.keys[0]);BAD("zero local size");
    wr(ctl+0x748,8);keep(ctl,code,s.keys[0]);CHECK(!PREP(),"pin before clear");
    s2a_source_clear();CHECK(r.source.control[0x758]==64 && r.source.code[0]==1,"pin survives source clear");
    uint32_t entries,bytes,dropped;s2a_source_stats(&entries,&bytes,&dropped);
    CHECK(bytes>0,"pinned bytes retained");nvk_compute_request_release(&r);s2a_source_stats(&entries,&bytes,&dropped);
    CHECK(bytes==0,"last release reclaims source");BAD("missing source");
    fprintf(stderr,"%d checks, %d failures\n",checks,failed);return failed?1:0;
}
