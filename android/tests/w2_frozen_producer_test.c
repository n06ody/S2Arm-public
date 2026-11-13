// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
/* Original synthetic source, registry metadata and facts. No GPU/game input. */
#include "direct/compile_export_w2.h"
#include "direct/source_store.h"
#include "direct/sha256.h"
#include "nvn/nvn_vk_compile_facts.h"
#include "nvn.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int enabled = 1, changed_lookup;
static uint64_t reggen = 17;
static struct nvk_w2_raw_registration registration = {1,0,1,0x25,0xA,1,0,0,0};
int s2a_compile_export_open(void) { return enabled; }
void s2a_source_log_line(const char *line) { (void)line; }
uint32_t nvk_attr_pair_canon(uint32_t x) { return x; }
int s2a_nvn_window_origin_mode = 1, s2a_nvn_depth_mode = 1;
uint64_t s2a_vk_adapter_registry_generation(void) { return reggen; }
int s2a_vk_adapter_w2_capture_registered(const int *ids, size_t count,
    struct nvk_w2_raw_registration *out, uint64_t *generation) {
    for (size_t i=0;i<count;i++) { memset(&out[i],0,sizeof out[i]); if (ids[i]==4) out[i]=registration; }
    *generation=reggen; return 0;
}
static int lookup(int id, struct nvk_tex_desc *out, void *ctx) {
    (void)ctx;
    if(id!=4 || !registration.present) return -1;
    out->nvn_format=registration.format;
    out->nvn_target=registration.target;
    out->layers=registration.layers;
    if(changed_lookup) { out->layers++; reggen++; }
    return 0;
}
static void hash_source(const uint8_t *code, size_t cn, const uint8_t *ctl,size_t tn,uint8_t key[8]) {
    s2a_sha256 c; uint8_t sha[32]; s2a_sha256_init(&c);
    s2a_sha256_update(&c,ctl,tn);s2a_sha256_update(&c,code,cn);s2a_sha256_final(&c,sha);memcpy(key,sha,8);
}
static struct s2a_compile_export_w2_key threaded_key;
static int threaded_result;
static void *other_thread(void *arg) {
    (void)arg;struct s2a_w2_query_receipt receipt;
    threaded_result=s2a_compile_export_w2_diagnose(&threaded_key,&receipt);return NULL;
}
int main(int argc, char **argv) {
    const char *mode=argc>1?argv[1]:"original";
    int partial=!strcmp(mode,"partial_view"), fullview=!strcmp(mode,"full_view");
    int zero=!strcmp(mode,"zero_layers"), missing=!strcmp(mode,"missing_raw"), epoch=!strcmp(mode,"source_epoch");
    int no_return=!strcmp(mode,"no_return"), wrong_key=!strcmp(mode,"wrong_key"), stale=!strcmp(mode,"stale_return");
    int closed=!strcmp(mode,"closed"), packed=!strcmp(mode,"packed"), cbuf=!strcmp(mode,"cbuf_fallback");
    int replaced=!strcmp(mode,"replaced_same_source"), binding=!strcmp(mode,"binding"), replay=!strcmp(mode,"claim_replay");
    if(partial||fullview) {registration.view_present=1;registration.view_complete=fullview;
        registration.view_format=2;registration.view_target=3;registration.view_layers=4;}
    if(zero) registration.layers=0;
    if(missing) registration.present=0;
    if(closed) enabled=0;
    int tess=argc>1 && !strcmp(argv[1],"tess_derived");
    int stripped=argc>1 && !strcmp(argv[1],"stripped");
    int mismatch=argc>1 && !strcmp(argv[1],"allocation");
    int mutate=argc>1 && !strcmp(argv[1],"identity");
    int race=argc>1 && !strcmp(argv[1],"race");
    int stopped=argc>1 && !strcmp(argv[1],"stop");
    uint8_t code[2][128],ctl[2][S2A_SOURCE_CONTROL_BYTES],keys[2][8];
    struct s2a_source_entry_view views[2];
    for (int i=0;i<2;i++) {
        memset(code[i],0x31+i,sizeof code[i]); memset(ctl[i],0x41+i,sizeof ctl[i]);
        memset(ctl[i]+0x700,0,8);
        if(tess) memset(ctl[i]+0x744,0,4);
        if(stripped) {code[i][0]=0x78;code[i][1]=0x56;code[i][2]=0x34;code[i][3]=0x12;}
        hash_source(code[i],sizeof code[i],ctl[i],sizeof ctl[i],keys[i]);
        CHECK(s2a_source_keep(keys[i],(uint32_t)i,code[i],sizeof code[i],ctl[i],sizeof ctl[i])==0);
        CHECK(s2a_source_acquire(keys[i],&views[i])==1);
    }
    struct s2a_record_draw_state state={0};state.stages=3;state.prim=4;state.tex_pool_base=4;
    state.tex_pool_n=8;state.samp_pool_n=8;memcpy(state.keys,keys,sizeof keys);
    struct s2a_direct_compile_context pc={0};pc.session=31;pc.generation=7;pc.direct_abi=5;
    pc.profile_json="{\"needsFragmentOutputSpecialization\":true}";pc.profile_bytes=strlen(pc.profile_json);
    char closure[]="synthetic-w2-producer";pc.translator_closure=closure;
    struct nvk_env env;nvk_env_fill(&pc,&env);
    struct s2a_compile_graphics_src gfx={0};gfx.topology="Triangles";
    struct nvk_texture_set tex;
    nvk_w2_diagnostic_init();
    changed_lookup=race;nvk_texture_facts_fill(&state,lookup,NULL,&tex);changed_lookup=0;
    CHECK(tex.count==(missing?64:0)); /* BUFFER target is genuinely unsupported by existing converter. */
    uint8_t canonical[S2A_INPUT_IDENTITY_MAX],sha[32];size_t canonical_bytes=0;
    int collection_stop=!strcmp(mode,"collection_stop"), identity_error=!strcmp(mode,"identity_error");
    if(collection_stop) {nvk_w2_diagnostic_cancel();nvk_w2_diagnostic_resume();}
    if(identity_error) {struct nvk_env absent=env;absent.have_profile=0;
        CHECK(nvk_input_identity(&state,&pc,&absent,&gfx,tex.items,tex.count,NULL,canonical,&canonical_bytes,sha)!=0);}
    if(binding) state.tex_pool_base=5;
    CHECK(!nvk_input_identity(&state,&pc,&env,&gfx,tex.items,tex.count,NULL,canonical,&canonical_bytes,sha));
    struct s2a_compile_request_input in={0};in.session=pc.session;in.generation=pc.generation;
    in.profile_json=pc.profile_json;in.profile_bytes=pc.profile_bytes;in.direct_abi=pc.direct_abi;
    in.translator_closure=pc.translator_closure;in.stage_count=2;in.gfx=gfx;
    in.texture_count=tex.count;memcpy(in.textures,tex.items,sizeof tex.items);
    in.texture_array_length=state.tex_pool_n;in.sampler_array_length=state.samp_pool_n;
    for(int i=0;i<2;i++) {
        in.stages[i].stage=i?"fs":"vs";in.stages[i].code=views[i].code+(stripped?0x30:0);
        in.stages[i].code_bytes=views[i].code_bytes-(stripped?0x30:0);
        in.stages[i].control=views[i].control;in.stages[i].control_bytes=views[i].control_bytes;
        in.stages[i].c1=views[i].c1;in.stages[i].c1_bytes=views[i].c1_bytes;
    }
    if (tess) {
        struct s2a_source_entry_view v=views[0];
#include "w2_tess_source.inc"
        CHECK(!gfx.has_tess_mode && in.gfx.has_tess_mode==1);
        struct s2a_input_identity actual={0};actual.direct_abi=in.direct_abi;
        actual.translator_closure=in.translator_closure;memcpy(actual.profile_sha,env.profile_sha,32);
        actual.stage_mask=state.stages;memcpy(actual.keys,state.keys,sizeof actual.keys);
        actual.source_present=3;
        for(int i=0;i<2;i++) {memcpy(actual.source_sha[i],views[i].content_sha,32);
            actual.source_code_bytes[i]=views[i].code_bytes;actual.source_control_bytes[i]=views[i].control_bytes;}
        actual.texture_array_length=in.texture_array_length;actual.sampler_array_length=in.sampler_array_length;
        actual.gfx=&in.gfx;actual.textures=in.textures;actual.texture_count=in.texture_count;
        uint8_t bytes[S2A_INPUT_IDENTITY_MAX];size_t n=0;
        CHECK(!s2a_input_identity_encode(&actual,bytes,sizeof bytes,&n));
        CHECK(n==canonical_bytes && memcmp(bytes,canonical,n));
        size_t first=0;while(first<n&&bytes[first]==canonical[first])first++;
        printf("real draw tess branch: original has=%d request has=%d identity first difference=%zu, %u->%u\n",
            gfx.has_tess_mode,in.gfx.has_tess_mode,first,canonical[first],bytes[first]);
    }
    uint8_t other[128];if(mismatch) {memcpy(other,in.stages[0].code,in.stages[0].code_bytes);in.stages[0].code=other;}
    if(mutate) in.gfx.rt_count=1;
    if(epoch) {uint8_t extra[128],k[8];memset(extra,0x59,sizeof extra);hash_source(extra,sizeof extra,ctl[0],sizeof ctl[0],k);
        CHECK(!s2a_source_keep(k,0,extra,sizeof extra,ctl[0],sizeof ctl[0]));}
    if(replaced) CHECK(!s2a_source_keep(keys[0],0,code[0],sizeof code[0],ctl[0],sizeof ctl[0]));
    struct nvk_w2_capsule *capsule=NULL;
    int claimed=nvk_w2_capsule_claim(&in,&capsule);
    if(mismatch||mutate||race||epoch||closed||replaced||binding||collection_stop||identity_error) {CHECK(claimed!=0 && !capsule);puts("PASS precise mismatch rejects");return 0;}
    CHECK(claimed==0 && capsule);
    if(replay) {struct nvk_w2_capsule *again=NULL;CHECK(nvk_w2_capsule_claim(&in,&again)!=0&&!again);}
    memset(closure,'x',sizeof closure-1); // original borrowed strings can change after claim

    struct s2a_w2_query q={S2A_W2_QUERY_NORMALIZED,{1,8,-1}};
    if(cbuf) q.coordinate.cbuf_slot=2;
    if(packed) q.coordinate.handle=0x100008;
    if(missing) q.method=S2A_W2_QUERY_FORMAT;
    static const uint8_t error[]="{\"schema\":\"s2a-direct-error-v1\",\"code\":\"MISSING_FACT\",\"message\":\"QueryTextureCoordNormalized has no source (stage=fs,handle=8,cbufSlot=-1)\"}";
    /* Registry changes after capture cannot replace the owned original raw data. */
    registration.format=0x1;registration.target=1;registration.layers=9;reggen++;
    if(stopped) nvk_w2_diagnostic_cancel();
    int published=nvk_w2_capsule_publish(capsule,&q,error,sizeof error-1);
    if(stopped) {CHECK(published!=0);nvk_w2_capsule_release(capsule);puts("PASS cancelled capsule rejects");return 0;}
    CHECK(published==0);
    if(!no_return) nvk_w2_capsule_returned(capsule);
    CHECK(nvk_w2_capsule_publish(capsule,&q,error,sizeof error-1)!=0);
    nvk_w2_capsule_release(capsule);
    struct s2a_compile_export_w2_key key={.session=pc.session,.generation=pc.generation};memcpy(key.input_sha,sha,32);
    struct s2a_w2_query_receipt receipt;
    threaded_key=key;pthread_t th;CHECK(!pthread_create(&th,NULL,other_thread,NULL));CHECK(!pthread_join(th,NULL));
    CHECK(threaded_result==S2A_W2_QUERY_NO_SOURCE);
    if(stale) nvk_w2_capsule_returned(NULL);
    if(wrong_key) {key.input_sha[31]^=1;CHECK(s2a_compile_export_w2_diagnose(&key,&receipt)==S2A_W2_QUERY_NO_SOURCE);key.input_sha[31]^=1;}
    int observed=s2a_compile_export_w2_diagnose(&key,&receipt);
    if(partial||zero||missing||no_return||wrong_key||stale||packed) {
        CHECK(observed==S2A_W2_QUERY_NO_SOURCE && !receipt.observed);
        puts("PASS unsafe or unbound source rejected by real provider");return 0;
    }
    CHECK(observed==S2A_W2_QUERY_OBSERVED);
    CHECK(receipt.observed && receipt.descriptor.format==0x25 && receipt.descriptor.target==0xA && receipt.descriptor.layers==1);
    CHECK(receipt.view_present==fullview);
    if(fullview) CHECK(receipt.view.present==S2A_W2_RAW_COMPLETE&&receipt.view.format==2&&receipt.view.target==3&&receipt.view.layers==4);
    else CHECK(!receipt.view.present);
    CHECK(receipt.original_identity_bytes==canonical_bytes && !memcmp(receipt.original_identity,canonical,canonical_bytes));
    CHECK(s2a_compile_export_w2_diagnose(&key,&receipt)==S2A_W2_QUERY_NO_SOURCE && !receipt.observed);
    nvk_w2_capsule_returned(NULL);
    for(int i=0;i<2;i++) s2a_source_release(views[i].ref);
    nvk_w2_diagnostic_cancel();
    puts("PASS frozen original metadata, full identity, and legal source allocation");return 0;
}
