// Physical subresources in a real render -> sample -> copy -> present chain.
static nvn_gpu_view frame_slice(nvn_gpu_tex image, unsigned mip, unsigned layer) {
    view_t d = {.set_target=1, .target=NVN_TEXTURE_TARGET_2D, .set_levels=1, .base_level=mip,
                .nlevels=1, .set_layers=1, .min_layer=layer, .nlayers=1};
    nvn_gpu_err e = {0};
    nvn_gpu_view view = NULL;
    CHECK(OK(nvn_gpu_view_create(image, &d, &view, &e)), "frame subresource view: %s", e.why);
    return view;
}

static void frame_clear_pass(nvn_gpu_cmd cmd, nvn_gpu_view view, unsigned size, float red) {
    nvn_gpu_pass_desc p = {.ncolor=1, .layers=1, .w=size, .h=size};
    p.color[0].view = view;
    p.color[0].load = NVN_GPU_LOAD_CLEAR;
    p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = red;
    p.color[0].clear.f[3] = 1;
    nvn_gpu_pass_begin(cmd, &p);
    nvn_gpu_pass_end(cmd);
}

static int test_resource_chain(const char *dir, const char *arm, const char *final,
                               nvn_gpu_surface surface, nvn_gpu_tex output, nvn_gpu_view output_view) {
    nvn_gpu_err e = {0};
    nvn_gpu_tex_desc td = {.usage=NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED};
    td.d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    td.d.format = NVN_FORMAT_RGBA8;
    td.d.width = td.d.height = 128;
    td.d.depth = td.d.levels = 2;
    nvn_gpu_tex source = NULL;
    if (!OK(nvn_gpu_tex_create(&td, &source, &e))) return 2;
    nvn_gpu_view canonical = frame_slice(source, 1, 1);
    nvn_gpu_view other_mip = frame_slice(source, 0, 1);
    nvn_gpu_view other_layer = frame_slice(source, 1, 0);
    if (!canonical || !other_mip || !other_layer) return 2;
    int wrong_mip = !strcmp(arm,"res-mip"), wrong_layer = !strcmp(arm,"res-layer");
    int overwrite = !strcmp(arm,"res-overwrite");
    int pool_one = !strcmp(arm, "res-pool-one");
    int dynamic = !strcmp(arm, "res-pool-dynamic");
    int pool_arm = !strcmp(arm, "res-pool") || pool_one || dynamic;
    nvn_gpu_view sampled = wrong_mip ? other_mip : (wrong_layer || pool_one || dynamic) ? other_layer : canonical;
    nvn_gpu_tex display_src = make_tex(64,64,NVN_FORMAT_RGBA8,NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view display_view = make_view(display_src);
    nvn_gpu_access_span src_span, out_span, display_span;
    if (!nvn_gpu_view_access_span(sampled,&src_span) || !nvn_gpu_view_access_span(output_view,&out_span) ||
        !nvn_gpu_view_access_span(display_view,&display_span)) return 2;
    nvn_gpu_layout_ent ent = {.set=0,.binding=0,.cls=NVN_GPU_RES_TEX_SAMP,.access=NVN_GPU_ACC_R,
                              .count=1,.stages=1u << NVN_GPU_SH_FS};
    nvn_gpu_layout_ent pool_entries[] = {
        {.binding=0, .cls=NVN_GPU_RES_TEX, .access=NVN_GPU_ACC_R, .count=3,
         .stages=1u << NVN_GPU_SH_FS, .src_kind=NVN_RB_TEXP},
        {.binding=1, .cls=NVN_GPU_RES_SAMP, .access=NVN_GPU_ACC_R, .count=1,
         .stages=1u << NVN_GPU_SH_FS},
    };
    nvn_gpu_layout layout = {.ent=pool_arm ? pool_entries : &ent, .nent=pool_arm ? 2u : 1u};
    if (dynamic) layout.push_bytes=4;
    int roi=!strncmp(arm,"res-roi",7);
    const char *frag = roi ? "frame_roi.frag" : dynamic ? "feedback_pool_dyn.frag" : pool_one ? "frame_pool_one.frag" : pool_arm ? "feedback_pool_phantom.frag" : "separate.frag";
    nvn_gpu_shader stages[3] = {0};
    void *vs = NULL, *fs = NULL;
    if (!load_shader_pair(dir, "replay_front_face.vert", frag, NULL, &layout,
                          &stages[0], &stages[1], &vs, &fs)) return 2;
    nvn_gpu_pipe_desc pd = {0};
    pd.st[0].sh=stages[0]; pd.st[1].sh=stages[1]; pd.cfmt[0]=NVN_FORMAT_RGBA8;
    pd.samples=1; pd.prim=4; pd.chmask.mask=15; pd.color.logic_op=3; pd.poly.mode=NVN_POLYGON_MODE_FILL;
    nvn_gpu_pipe pipe = NULL;
    samp_par sd = {0};
    nvn_gpu_samp sampler = NULL;
    if (!OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&e)) || !OK(nvn_gpu_samp_create(&sd,&sampler,&e))) return 2;
    nvn_gpu_cmd cmd = begin_cmd();
    frame_clear_pass(cmd, canonical, 64, 0.0f);
    if (overwrite) frame_clear_pass(cmd, canonical, 64, 1.0f);
    frame_clear_pass(cmd, other_mip, 128, 0.25f);
    frame_clear_pass(cmd, other_layer, 64, 0.5f);
    int uploaded=!strcmp(arm,"res-roi-upload");
    int other_upload=!strcmp(arm,"res-roi-upload-other");
    if (uploaded || other_upload) {
        const uint8_t pixel[4]={128,0,0,255};
        NVNcopyRegion center={32,32,0,1,1,1};
        nvn_gpu_buf upload=make_buf(sizeof pixel,NVN_GPU_MEM_SHARED,pixel);
        nvn_gpu_copy_buf_tex(cmd,upload,0,4,4,source,1,!other_upload,&center);
        nvn_gpu_buf_release(upload,NULL,NULL);
    }
    nvn_gpu_pass_desc p = {.ncolor=1,.layers=1,.w=64,.h=64};
    p.color[0].view=output_view; p.color[0].load=NVN_GPU_LOAD_CLEAR; p.color[0].store=NVN_GPU_STORE_STORE;
    nvn_gpu_pass_begin(cmd,&p);
    nvn_gpu_set_pipe(cmd,pipe);
    nvn_gpu_dyn dyn = {.nvp=1,.has_sc=1};
    dyn.vp[0][2]=dyn.vp[0][3]=64; dyn.sc[2]=dyn.sc[3]=64;
    nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_binding bind = {.cls=NVN_GPU_RES_TEX_SAMP};
    bind.u.ts.view=sampled; bind.u.ts.samp=sampler;
    nvn_gpu_pool_table pool = NULL;
    if (pool_arm) {
        // Constant arms fetch one known element. The other view and tail are
        // live and valid descriptors with a different mip/layer and pixel.
        nvn_gpu_pool_item items[] = {
            {.state=NVN_GPU_POOL_READY, .resource.view=canonical},
            {.state=NVN_GPU_POOL_READY, .resource.view=other_layer},
        };
        nvn_gpu_pool_desc desc = {.kind=NVN_GPU_POOL_IMAGE, .count=2, .items=items, .dummy.view=other_layer};
        if (!OK(nvn_gpu_pool_create(&desc,&pool,&e))) {
            CHECK(0, "resource fixture pool creation: %s", e.why);
            return 2;
        }
        nvn_gpu_binding sampler_binding = {.ent=1, .cls=NVN_GPU_RES_SAMP, .u.samp=sampler};
        nvn_gpu_bind(cmd,NVN_GPU_SH_FS,&sampler_binding,1);
        CHECK(OK(nvn_gpu_pool_bind(cmd,NVN_GPU_SH_FS,0,pool,&e)), "resource fixture pool bind: %s", e.why);
    } else {
        nvn_gpu_bind(cmd,NVN_GPU_SH_FS,&bind,1);
    }
    uint8_t request[32]={1},owner[32]={2};
    nvn_gpu_draw_observation observation={12,request,owner,stages,0,0};
    int ledger=roi || !strncmp(arm,"res-choice",10);
    int stale_choice=!strcmp(arm,"res-choice-stale");
    if (ledger) observation.choice=frame_test_choice(request,owner,stages[1],RP_ROUTE_FACT,RP_LIB_NONE);
    if (stale_choice) observation.choice=1; // real prior request, different owner and module
    nvn_gpu_draw_args draw={.prim=4,.count=3,.instances=1,.observation=&observation};
    if (dynamic) { uint32_t index=1; nvn_gpu_push(cmd,NVN_GPU_SH_FS,0,&index,sizeof index); }
    nvn_gpu_draw(cmd,&draw);
    nvn_gpu_pass_end(cmd);
    nvn_gpu_copy_tex(cmd,output,0,0,NULL,display_src,0,0,NULL);
    nvn_gpu_buf readback = make_buf(64*64*4,NVN_GPU_MEM_SHARED,NULL);
    nvn_gpu_copy_tex_buf(cmd,display_src,0,0,NULL,readback,0,64*4,64*64*4);
    CHECK(OK(nvn_gpu_cmd_commit(cmd,2,on_done,NULL,&e)), "resource draw submit");
    CHECK(frame_test_present(surface,display_view,12,41), "resource present");
    CHECK(OK(nvn_gpu_fence_wait(2,5000000000ull,&e)), "resource fence");
    unsigned pixel = buf_cpu(readback)[4*(32*64+32)];
    unsigned expected_red = uploaded ? 160 : roi ? 32 : (pool_one || dynamic) ? 192 : (wrong_mip || pool_arm) ? 64 : wrong_layer ? 128 : overwrite ? 255 : 0;
    CHECK(pixel == expected_red, "physical sample red=%u expected=%u",pixel,expected_red);

    FILE *f = fopen(final,"r");
    if (pool_arm)
        fprintf(stderr, "pool-frame arm=%s physical=%u expected=%u receipt=%s\n",
                arm,pixel,expected_red,frame_test_log);
    CHECK(f != NULL, "readable physical sample must have a present-to-writer record: %s", frame_test_log);
    char line[768], op[32];
    int sample_ok=0, present_ok=0, copy_read=0, copy_write=0, color_write=0, attachments=0;
    unsigned long long image, view;
    unsigned mip, layer, version;
    unsigned samples = 0, gaps = 0, schema = 0;
    int upload_gap=0, sampled_version=-1;
    unsigned choices=0, first_twice=0, draw_choice=0, roi_rows=0;
    while (f && fgets(line,sizeof line,f)) {
        if (!strncmp(line,"roi\t",4)) roi_rows++;
        if (!strncmp(line,"gap\tbuffer_upload\t",18)) upload_gap++;
        unsigned number, uses;
        if (sscanf(line,"choice\t%u\t%u",&number,&uses)==2) {
            choices++;
            if (number==1 && uses==2) first_twice++;
        }
        if (!strcmp(line,stale_choice ? "draw-choice\t0\t0\n" : "draw-choice\t0\t2\n")) draw_choice++;
        if (sscanf(line,"v=%u",&schema)==1) continue;
        if (!strcmp(line,"gap\tpool_index_unproven\t2\t3\t0\t1\t0\t0\n")) gaps++;
        if (sscanf(line,"res\t%31s\t%llu\t%u\t%u\t%u\t%llu",&op[0],&image,&mip,&layer,&version,&view)!=6) continue;
        if (!strcmp(op,"sample")) {
            sampled_version=(int)version;
            unsigned elem=UINT32_MAX;
            int decoded=sscanf(line,"res\tsample\t%*u\t%*u\t%*u\t%*u\t%*u\t%*u\t%*u\t%*u\t%*u\t%*u\t%*u\t%u",&elem);
            CHECK(decoded==1 && elem==(unsigned)pool_one, "descriptor array element %u, expected %d",elem,pool_one);
            samples++;
        }
        if (!strcmp(op,"clear") && image==src_span.storage && mip==src_span.level && layer==src_span.layer && view)
            attachments++;
        if (!strcmp(op,"sample") && image==src_span.storage && mip==src_span.level && layer==src_span.layer &&
            version==(uploaded ? 0u : (unsigned)(1+overwrite)) && view) sample_ok++;
        if (!strcmp(op,"color") && image==out_span.storage && !mip && !layer && version==2 && view) color_write++;
        if (!strcmp(op,"copy-read") && image==out_span.storage && !mip && !layer && version==2) copy_read++;
        if (!strcmp(op,"copy-write") && image==display_span.storage && !mip && !layer && version==1) copy_write++;
        if (!strcmp(op,"present") && image==display_span.storage && !mip && !layer && version==1) present_ok++;
    }
    if (f) fclose(f);
    const char *leaf = strrchr(final,'/');
    if (leaf) fprintf(stderr,"resource-file arm=%s file=%s\n",arm,leaf+1);
    if (uploaded || other_upload) CHECK(upload_gap==1 && sample_ok==1,"unknown partial write must invalidate the old sampled version (gaps=%d current=%d)",upload_gap,sample_ok);
    if (roi) CHECK(roi_rows>=10, "five requested regions need intermediate and final pixel payloads (got %u, GPU center=%u)",roi_rows,pixel);
    if (ledger) CHECK(choices==2 && first_twice==1 && draw_choice==1 && observation.choice==(stale_choice ? 1u : 2u),
                      "startup choices must survive nonce and module release; draw must join its selected FS (choices=%u first=%u join=%u id=%u)",
                      choices,first_twice,draw_choice,observation.choice);
    CHECK(schema==((ledger || roi) ? 5u : (dynamic || pool_one) ? 4u : 3u) && gaps==(unsigned)dynamic, "partial schema %u scoped pool gaps %u",schema,gaps);
    CHECK(samples==(unsigned)!dynamic, "one descriptor fetched, unused pool entries must not appear as samples (got %u)",samples);
    CHECK(attachments==1+overwrite && sample_ok==!dynamic, "actual descriptor must identify the sampled mip/layer and latest write version");
    CHECK(color_write==1 && copy_read==1 && copy_write==1 && present_ok==1,
          "present source must trace through copy to the sampled draw's color attachment");
    fprintf(stderr,"resource-frame arm=%s red=%u expected=%u mip=%u layer=%u version=%d failures=%d\n",
            arm,pixel,expected_red,src_span.level,src_span.layer,sampled_version,g_fail);
    nvn_gpu_buf_release(readback, NULL, NULL);
    if (pool) nvn_gpu_pool_release(pool);
    nvn_gpu_samp_release(sampler);
    for (unsigned i=0;i<2;i++) nvn_gpu_shader_release(stages[i]);
    free(vs); free(fs);
    nvn_gpu_view_release(display_view); nvn_gpu_tex_release(display_src);
    nvn_gpu_view_release(canonical); nvn_gpu_view_release(other_mip); nvn_gpu_view_release(other_layer);
    nvn_gpu_tex_release(source); nvn_gpu_view_release(output_view); nvn_gpu_tex_release(output);
    CHECK(OK(nvn_gpu_close(5000000000ull,&e)), "resource fixture close");
    return g_fail != 0;
}
