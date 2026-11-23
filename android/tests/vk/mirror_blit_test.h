// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Original pixels; real rp_op_copy_t2t and Vulkan transfers. Resource lookup is
// a fixture mapping, not a test of the production texture registry/allocator.
static const tex_t *mirror_objects[2];
static nvn_gpu_tex mirror_images[2];
static nvn_gpu_view mirror_views[2];
static unsigned mirror_view_count;
nvn_gpu_tex rp_tex_resolve(const tex_t *t, nvn_gpu_cmd c) {
    (void)c;
    for (unsigned i=0;i<2;i++) if(t==mirror_objects[i]) return mirror_images[i];
    return NULL;
}
nvn_gpu_view rp_view_for(const tex_t *t, const view_t *v, nvn_gpu_cmd c) {
    nvn_gpu_tex image=rp_tex_resolve(t,c); REQUIRE(image && mirror_view_count<2);
    nvn_gpu_view result=NULL; GPU(nvn_gpu_view_create(image,v,&result,&err));
    mirror_views[mirror_view_count++]=result; return result;
}
// 夹具只用非 BC 图像，原格式就是当前格式。
nvn_gpu_tex rp_tex_resolve_plain(const tex_t *t, nvn_gpu_cmd c) { return rp_tex_resolve(t, c); }
nvn_gpu_view rp_view_for_plain(const tex_t *t, const view_t *v, nvn_gpu_cmd c) { return rp_view_for(t, v, c); }
static unsigned mirror_component(int x, int y, unsigned component) {
    return component==0 ? (unsigned)x&1u : component==1 ? (unsigned)y&1u :
        component==2 ? (unsigned)(x/2+y/2)&1u : 1u;
}
static void run_color_mirror(struct s2a_vk_device *device) {
    nvn_gpu_open_desc od={0};od.host_device=device;od.app_label="original-color-mirror";
    GPU(nvn_gpu_open(&od,&err));unsigned cases=0;
    for(unsigned axes=1;axes<=3;axes++)for(unsigned scaled=0;scaled<2;scaled++)
    for(unsigned packed=0;packed<2;packed++)for(unsigned sub=0;sub<2;sub++)for(unsigned linear=0;linear<2;linear++) {
        nvn_gpu_tex_desc desc={0};desc.d.target=sub?NVN_TEXTURE_TARGET_2D_ARRAY:NVN_TEXTURE_TARGET_2D;
        desc.d.width=desc.d.height=sub?24:12;desc.d.depth=sub?3:1;desc.d.levels=sub?2:1;desc.d.samples=1;
        desc.d.format=packed?NVN_FORMAT_R11G11B10F:NVN_FORMAT_RGBA8;desc.usage=NVN_GPU_TEX_SAMPLED|NVN_GPU_TEX_RENDER;
        tex_t source={0},destination={0};source.d=desc.d;source.serial=1000+cases*2;
        GPU(nvn_gpu_tex_create(&desc,&mirror_images[0],&err));desc.d.format=NVN_FORMAT_RGBA8;
        destination.d=desc.d;destination.serial=source.serial+1;GPU(nvn_gpu_tex_create(&desc,&mirror_images[1],&err));
        mirror_objects[0]=&source;mirror_objects[1]=&destination;mirror_view_count=0;
        uint32_t input[144],initial[144],spare[144];
        for(int y=0;y<12;y++)for(int x=0;x<12;x++) {
            unsigned r=mirror_component(x,y,0),g=mirror_component(x,y,1),b=mirror_component(x,y,2);
            input[y*12+x]=packed ? (r*0x3c0u)|((g*0x3c0u)<<11)|((b*0x1e0u)<<22) : r*255u|(g*255u<<8)|(b*255u<<16)|0xff000000u;
            initial[y*12+x]=0x44332211u;spare[y*12+x]=0x59432d7bu;
        }
        uint8_t sentinel[1216];memset(sentinel,0xa5,sizeof sentinel);nvn_gpu_buf output=buffer(sizeof sentinel,sentinel);
        nvn_gpu_cmd command=NULL;GPU(nvn_gpu_cmd_begin(NULL,&command,&err));
        NVNcopyRegion full={0,0,0,12,12,1};unsigned sl=sub?2:0,dl=sub?1:0;
        nvn_gpu_tex_upload(command,mirror_images[0],sub,sl,&full,input,48,sizeof input);
        nvn_gpu_tex_upload(command,mirror_images[1],sub,dl,&full,initial,48,sizeof initial);
        if(sub)nvn_gpu_tex_upload(command,mirror_images[1],sub,0,&full,spare,48,sizeof spare);
        p_copy_t2t op={0};op.src=&source;op.dst=&destination;
        op.sr=(NVNcopyRegion){2,3,sub?1:0,4,4,1};op.dr=(NVNcopyRegion){1,1,sub?1:0,scaled?8:4,scaled?8:4,1};
        op.has_sv=op.has_dv=1;op.sv.set_levels=op.dv.set_levels=1;op.sv.base_level=op.dv.base_level=(int)sub;
        op.sv.set_layers=op.dv.set_layers=1;op.sv.min_layer=sub?1:0;op.dv.min_layer=0;
        op.flags=(axes&1?NVN_COPY_FLAGS_MIRROR_X_BIT:0)|(axes&2?NVN_COPY_FLAGS_MIRROR_Y_BIT:0)|(linear?NVN_COPY_FLAGS_LINEAR_FILTER_BIT:0);
        nvn_unimpl_reset();rp_op_copy_t2t(command,&op,&op);
        nvn_gpu_copy_tex_buf(command,mirror_images[1],sub,dl,&full,output,0,48,576);
        if(sub)nvn_gpu_copy_tex_buf(command,mirror_images[1],sub,0,&full,output,608,48,576);
        GPU(nvn_gpu_cmd_commit(command,1+cases,done,NULL,&err));GPU(nvn_gpu_fence_wait(1+cases,5000000000ull,&err));
        const uint8_t *actual=nvn_gpu_buf_cpu(output);REQUIRE(actual);
        for(unsigned y=0;y<12;y++)for(unsigned x=0;x<12;x++)for(unsigned component=0;component<4;component++) {
            int want=(int)(initial[y*12+x]>>(8*component))&255;
            int inside=x>=1&&x<1+(unsigned)op.dr.width&&y>=1&&y<1+(unsigned)op.dr.height;
            if(inside) {
                float u=(x-1+.5f)*4/op.dr.width,v=(y-1+.5f)*4/op.dr.height;
                float sx=2+(axes&1?4-u:u),sy=3+(axes&2?4-v:v);
                if(!linear)want=(int)mirror_component((int)floorf(sx),(int)floorf(sy),component)*255;
                else {
                    sx-=.5f;sy-=.5f;int ix=(int)floorf(sx),iy=(int)floorf(sy);float tx=sx-ix,ty=sy-iy;
                    float value=0;
                    for(int dy=0;dy<2;dy++)for(int dx=0;dx<2;dx++)value+=mirror_component(ix+dx,iy+dy,component)*(dx?tx:1-tx)*(dy?ty:1-ty);
                    want=(int)lroundf(value*255);
                }
            }
            int delta=(int)actual[(y*12+x)*4+component]-want;
            int tolerance=inside&&linear&&component<3;
            if(delta < -tolerance || delta > tolerance) {
                fprintf(stderr,"mirror pixel case=%u axes=%u scale=%u packed=%u sub=%u linear=%u xy=%u,%u component=%u want=%d actual=%u\n",cases,axes,scaled,packed,sub,linear,x,y,component,want,actual[(y*12+x)*4+component]);
                REQUIRE(0);
            }
        }
        if(sub)REQUIRE(!memcmp(actual+608,spare,sizeof spare));
        for(unsigned j=576;j<608;j++)REQUIRE(actual[j]==0xa5);
        for(unsigned j=1184;j<1216;j++)REQUIRE(actual[j]==0xa5);
        REQUIRE(nvn_unimpl_kinds()==0);
        for(unsigned j=0;j<mirror_view_count;j++)nvn_gpu_view_release(mirror_views[j]);
        nvn_gpu_tex_release(mirror_images[0]);nvn_gpu_tex_release(mirror_images[1]);nvn_gpu_buf_release(output,NULL,NULL);cases++;
    }
    GPU(nvn_gpu_close(5000000000ull,&err));
    for(unsigned bad=0;bad<7;bad++) {
        GPU(nvn_gpu_open(&od,&err));
        nvn_gpu_tex_desc desc={0};desc.d.target=NVN_TEXTURE_TARGET_2D;desc.d.format=NVN_FORMAT_RGBA8;
        desc.d.width=desc.d.height=12;desc.d.depth=desc.d.levels=desc.d.samples=1;desc.usage=NVN_GPU_TEX_RENDER|NVN_GPU_TEX_SAMPLED;
        nvn_gpu_tex images[2]={0};nvn_gpu_view views[2]={0};
        for(unsigned j=0;j<2;j++){GPU(nvn_gpu_tex_create(&desc,&images[j],&err));GPU(nvn_gpu_view_create(images[j],NULL,&views[j],&err));}
        NVNcopyRegion from={4,3,0,-4,4,1},to={0,0,0,4,4,1};
        if(bad==0)from.xoffset=2;
        if(bad==1)from.width=INT32_MIN;
        if(bad==2)from.height=0;
        if(bad==3)to.width=0;
        if(bad==4){from.xoffset=13;from.width=-1;}
        if(bad==5){from.width=4;from.yoffset=2;from.height=-4;}
        nvn_gpu_cmd command=NULL;GPU(nvn_gpu_cmd_begin(NULL,&command,&err));nvn_gpu_blit(command,views[0],&from,views[bad==6?0:1],&to,0);
        REQUIRE(nvn_gpu_cmd_commit(command,1,done,NULL,&err).code==(bad==6?NVN_GPU_E_UNSUPPORTED:NVN_GPU_E_ARG));
        for(unsigned j=0;j<2;j++){nvn_gpu_view_release(views[j]);nvn_gpu_tex_release(images[j]);}
        GPU(nvn_gpu_close(5000000000ull,&err));
    }
    printf("color mirror: %u cases, 6912 destination pixels, 3456 spare-layer pixels, 3072 sentinel bytes, 6 invalid regions rejected, 1 same-image mirror rejected\n",cases);
}
