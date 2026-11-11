// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "nvn/nvn_vk_compute_lookup.h"
#include "direct/offline_library.h"
#include "direct/libpack_vk.h"
#include "direct/program_cache.h"
#include "direct/compile_worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int mode, offline, disk, compile, stores, changed;
// 宿主强符号桩：lookup.c 的截断诊断走弱缝 __android_log_print，macOS 未定义
// 弱外部须强符号补位（无桩 = 静默不丢，与 Android liblog 同合同）。
int __android_log_print(int prio,const char *tag,const char *fmt,...) {
    (void)prio;(void)tag;(void)fmt;return 0;
}
static const char *good, *bad;
static struct s2a_direct_compile_context pc;
static int read_bytes(const char *path, uint8_t **b, size_t *n) {
    FILE *f=fopen(path,"rb"); if(!f)return -1;
    fseek(f,0,SEEK_END);*n=(size_t)ftell(f);rewind(f);*b=malloc(*n);
    int ok=*b && fread(*b,1,*n,f)==*n;fclose(f);return ok?0:-1;
}
#ifndef W2_NO_PROVIDER
int s2a_direct_provider_compile_context(struct s2a_direct_compile_context *out) {
    *out=pc;if(mode==8 || changed)out->generation++;return 0;
}
#endif
int s2a_offline_library_fetch(const char *root,const struct s2a_offline_request *r,
    struct s2a_offline_program *out,char *why,size_t cap) {
    (void)root;offline++;if(!r->profile_declared){snprintf(why,cap,"profile undeclared");return 1;}
    if(mode==29 || mode==1 || mode==2 || mode==7 || mode==11 || mode==12 || mode==17||mode==19) {
        if(mode==7)changed=1;
        return read_bytes(mode==2||mode==11||mode==12||mode==17||mode==19?bad:good,&out->bytes,&out->len);
    }
    snprintf(why,cap,"offline missing");return 1;
}
int s2a_program_cache_load(const char *root,const uint8_t id[32],uint8_t **b,size_t *n,
    uint8_t sha[32],char *why,size_t cap) {
    (void)root;(void)id;(void)sha;disk++;
    if(mode==30 || mode==2 || mode==3 || mode==4 || mode==11 || mode==12 || mode==41)
        return read_bytes(mode==4?bad:good,b,n);
    snprintf(why,cap,"private missing");return -1;
}
int s2a_compile_worker_run(uint64_t s,uint64_t g,const char *j,size_t n,const uint8_t id[32],int deadline,
    int(*pause)(void*),void(*resume)(void*),void *ctx,uint8_t **b,size_t *len,char *why,size_t cap) {
    (void)s;(void)g;(void)j;(void)n;(void)id;(void)deadline;(void)pause;(void)resume;(void)ctx;compile++;
    if(mode==5){snprintf(why,cap,"cancelled");return -1;}
    if(mode==6)changed=1;
    return read_bytes(mode==10||mode==13||mode==18||(mode>=31&&mode!=34&&mode!=38&&mode!=39&&mode!=42)?bad:good,b,len);
}
int s2a_program_cache_store_async(const char *root,const uint8_t id[32],const uint8_t *b,size_t n) {
    (void)root;(void)id;(void)b;(void)n;stores++;return mode==9?-1:0;
}
static int libpack_calls;
// #1434 第二笔 LibPack 桩：mode40 hit 回 good、41 miss、42 reject——
// 插序验收（离线库 miss 后、私有缓存前）由 libpack_calls/offline/disk 计数对照读出。
int s2a_libpack_compute_v1(const struct s2a_libpack *lp,const uint8_t cs_key[8],
    int unaligned,uint32_t sreq,uint32_t abi,const char *ph,const char *tc,
    uint8_t **j,size_t *jl,char *why,size_t cap) {
    (void)lp;(void)cs_key;(void)unaligned;(void)sreq;(void)abi;(void)ph;(void)tc;
    libpack_calls++;
    if(mode==41){snprintf(why,cap,"absent");return S2A_LIBPACK_MISS;}
    if(mode==42){snprintf(why,cap,"container broke");return S2A_LIBPACK_REJECT;}
    return read_bytes(good,j,jl)?S2A_LIBPACK_REJECT:S2A_LIBPACK_HIT;
}
#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL mode%d line%d: %s\n",mode,__LINE__,#c);return 1;}}while(0)
int main(int argc,char **argv){
    if(argc!=4)return 2;mode=atoi(argv[1]);good=argv[2];bad=argv[3];
    pc=(struct s2a_direct_compile_context){.session=11,.generation=22,.direct_abi=1,
        .translator_closure="closure",.profile_hash="sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",.offline_profile_available=1};
    const char *request="{\"requestId\":\"rid-lookup\",\"program\":{\"computeState\":{\"hasUnalignedStorageBuffer\":false}}}";
    if(mode==20)request="{}";
    // mode39：REQUEST 合法但**顶层缺 requestId**——fresh 结果无法回显，必须拒；
    // 这是"requestId 缺失时 fresh 结果必须被拒"的阳性负控（不是靠坏信封凑出来的红）。
    if(mode==39)request="{\"program\":{\"computeState\":{\"hasUnalignedStorageBuffer\":false}}}";
    if(mode==21)request="{\"requestId\":\"rid-lookup\",\"program\":{\"computeState\":{\"hasUnalignedStorageBuffer\":true}}}";
    if(mode==22)request="{\"program\":{\"computeState\":{\"hasUnalignedStorageBuffer\":0}}}";
    if(mode==23)request="{\"program\":{\"computeState\":{\"hasUnalignedStorageBuffer\":null}}}";
    if(mode==24)request="{\"program\":{\"computeState\":{\"hasUnalignedStorageBuffer\":false,\"hasUnalignedStorageBuffer\":true}}}";
    struct nvk_compute_request r={.session=11,.generation=22,.json=(char*)request,.json_bytes=strlen(request),.canonical_bytes=1};
    static uint8_t want_c1[4]={9,8,7,6};
    r.source.c1=want_c1;r.source.c1_bytes=4;
    r.profile.storage_alignment=16;snprintf(r.profile.hash,sizeof r.profile.hash,"%s",pc.profile_hash);
    // Producer-contract modes feed the real CLI envelope through the fresh path;
    // request/context fields come from the emitted request+profile, never literals.
    if(mode>=34&&mode<=38){
        const char *req=getenv("W2_REQ"),*clo=getenv("W2_CLOSURE"),*pro=getenv("W2_PROFILE"),
                   *al=getenv("W2_ALIGN"),*ws=getenv("W2_S"),*wg=getenv("W2_G");
        CHECK(req&&clo&&pro&&al&&ws&&wg);
        uint8_t *rb=NULL;size_t rn=0;CHECK(!read_bytes(req,&rb,&rn));
        r.json=(char*)rb;r.json_bytes=rn;
        pc.session=r.session=strtoull(ws,NULL,10);
        pc.generation=r.generation=strtoull(wg,NULL,10);
        pc.translator_closure=clo;pc.profile_hash=pro;
        snprintf(r.profile.hash,sizeof r.profile.hash,"%s",pro);
        r.profile.storage_alignment=(uint32_t)strtoul(al,NULL,10);
    }
    if(mode==25)r.profile.storage_alignment=0;
    if(mode==26)r.profile.hash[7]='b';
    struct nvk_compute_lookup_options o={.revision_root="/revision",.private_cache_root="/private",.diag_open=1};
    if(mode==38)o.diag_open=0;   // 调试门关：accept 照过、结果封套不落盘（正式包行为）
    struct nvk_compute_lookup_result result;char why[192]={0};
    if(mode==14)pc.offline_profile_available=0;
    if(mode==15)r.generation++;
    if(mode==16)o.revision_root=o.private_cache_root=NULL;
    if(mode>=28&&mode<=30)o.no_live_compile=1;
    // LibPack 门：options 双非空才查；profile_json 是 supportLayout.requiredSize 真值源
    if(mode>=40&&mode<=42){
        static const uint8_t k8[8]={0x40,0x0a,0x08,0xfc,0xa2,0x96,0x79,0xef};
        o.libpack=(const struct s2a_libpack *)(const void *)0x1;o.libpack_key=k8;
        pc.profile_json="{\"supportLayout\":{\"requiredSize\":2608}}";
        pc.profile_bytes=strlen(pc.profile_json);
    }
    int rc=nvk_compute_lookup(&r,&pc,&o,&result,why,sizeof why);
    if(mode==28) {
        CHECK(rc!=0 && !result.bytes && !compile && !stores);
        CHECK(offline==1 && disk==1);
        CHECK(strstr(why,"compute miss: live compilation disabled"));
        CHECK(strstr(result.offline_reason,"offline missing"));
        CHECK(strstr(result.private_reason,"private missing"));
    } else if(mode==29 || mode==30) {
        CHECK(!rc && result.bytes && !compile && !stores);
        CHECK(result.origin==(mode==29?1:2)); free(result.bytes);
    } else if(mode>=40&&mode<=42) {
        // #1434 第二笔插序验收：离线库 miss→LibPack→私有/现编。
        // 40 hit：origin=OFFLINE、私有缓存与现编都没碰过、不写回——「首遇现编 0」。
        // 41 miss：落到私有缓存命中。42 reject：落到现编并写回。三态不混。
        CHECK(libpack_calls==1&&offline==1);
        if(mode==40){CHECK(!rc&&result.origin==1&&!disk&&!compile&&!stores);
            CHECK(!result.private_reason[0]);}   // 私有缓存没进过
        if(mode==41){CHECK(!rc&&result.origin==2&&disk==1&&!compile&&!stores);
            CHECK(strstr(result.offline_reason,"libpack:"));}   // 追加在离线库原因后
        if(mode==42){CHECK(!rc&&result.origin==3&&compile==1&&stores==1);
            CHECK(strstr(result.offline_reason,"libpack: container broke"));}
        free(result.bytes);
    } else if(mode==5||mode==6||mode==7||mode==8||mode==10||mode==13||mode==15||mode==18||mode==20||(mode>=22&&mode!=34&&mode!=38)){
        CHECK(rc!=0);CHECK(!result.bytes);CHECK(!stores);
        if(mode==8||mode==15||mode==20||(mode>=22&&mode<=30))CHECK(!offline&&!disk&&!compile);
        if(mode==27)CHECK(strstr(why,"provider context unavailable"));
        if(mode==7)CHECK(!compile);if(mode==5)CHECK(strstr(why,"cancelled"));
        if(mode==31||mode==36)CHECK(strstr(why,"transport missing"));
        if(mode==32||mode==35||mode==37||mode==39) {
            CHECK(strstr(why,"transport mismatch")&&strstr(why,"requestId"));
            // got/want 诊断必须能定位漂移项（不能只剩一句 mismatch）。
            CHECK(strstr(why,"got=")&&strstr(why,"want="));
        }
        // 回显拒绝必须发生在“现编成功之后”，否则证明不了回显检查本身生效；
        // 离线/私有缓存各 miss 一次（两个计数器是访问次数，不是命中数）。
        if(mode==39)CHECK(compile==1&&offline==1&&disk==1);
    }else{
        if(!(!rc&&result.bytes&&result.length))fprintf(stderr,"mode%d rc=%d why=%s\n",mode,rc,why);
        CHECK(!rc&&result.bytes&&result.length);
        CHECK(result.profile.storage_alignment==16&&!strcmp(result.profile.hash,pc.profile_hash));
        // lookup 结果按 header 合同透传 c1（与 main 一致；本线 compute build 也走 source->c1，
        // 两条路径同源）。c1 的字节级覆盖另由 pipeline/integration 用例验证。
        CHECK(result.c1==want_c1&&result.c1_bytes==4);
        if(mode==1||mode==17){CHECK(result.origin==1);CHECK(!disk&&!compile&&!stores);}
        else if(mode==2||mode==3||mode==11||mode==12){CHECK(result.origin==2);CHECK(!compile&&!stores);if(mode!=3)CHECK(result.offline_reason[0]);}
        else{CHECK(result.origin==3);CHECK(compile==1);CHECK(stores==(mode==16?0:1));}
        if(mode==4)CHECK(result.private_reason[0]);
        CHECK(result.cache_store_failed==(mode==9));
        free(result.bytes);
    }
    printf("case%d PASS offline=%d private=%d compile=%d store=%d\n",mode,offline,disk,compile,stores);return 0;
}
