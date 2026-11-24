// Included by the real LibPack producer fixture. The compiler boundary supplies
// a precompiled, type-correct shader; draw the program returned by resolve.
static int test_mrt(int compatible) {
    uint32_t fmts[] = {NVN_FORMAT_R11G11B10F, NVN_FORMAT_RGBA8UI, NVN_FORMAT_RGBA8UI, NVN_FORMAT_RG16UI};
    struct rp_shader_req q = {.prim=4, .rt_count=4};
    memcpy(q.rt_format, fmts, sizeof fmts);
    const struct rp_prog *program = NULL;
    int rc = rp_shader_resolve(&q, &program);
    CHECK(!rc && program && program->lib_sourced == compatible && compile_calls == !compatible,
          "MRT admission: compatible=%d rc=%d compiler=%d", compatible, rc, compile_calls);
    if (g_fail) return 1; // A withdrawn gate fails before submitting an invalid interface.
    nvn_gpu_pipe_desc pd = {.samples=1, .prim=4};
    memcpy(pd.cfmt, fmts, sizeof fmts);
    pd.st[0].sh=program->sh[0]; pd.st[1].sh=program->sh[1];
    pd.chmask.mask=0xffff; pd.poly.mode=NVN_POLYGON_MODE_FILL; pd.color.logic_op=3;
    nvn_gpu_err err={0}; nvn_gpu_pipe pipe=NULL;
    if (!OK(nvn_gpu_pipe_get(&pd,NVN_GPU_PIPE_SYNC,&pipe,&err))) return 2;
    nvn_gpu_tex tex[4]; nvn_gpu_view views[4];
    nvn_gpu_pass_desc pass={.ncolor=4,.w=4,.h=4,.layers=1};
    for (int i=0;i<4;i++) {
        tex[i]=make_tex(4,4,fmts[i],NVN_GPU_TEX_RENDER);
        views[i]=make_view(tex[i]);
        pass.color[i].view=views[i]; pass.color[i].load=NVN_GPU_LOAD_CLEAR; pass.color[i].store=NVN_GPU_STORE_STORE;
    }
    uint8_t initial[192]; memset(initial,0xee,sizeof initial);
    nvn_gpu_buf pixels=make_buf(sizeof initial,NVN_GPU_MEM_SHARED,initial);
    nvn_gpu_cmd cmd=begin_cmd();
    nvn_gpu_pass_begin(cmd,&pass); nvn_gpu_set_pipe(cmd,pipe);
    nvn_gpu_dyn dyn={.nvp=1,.has_sc=1};
    dyn.vp[0][2]=dyn.vp[0][3]=4; dyn.vp[0][5]=1; dyn.sc[2]=dyn.sc[3]=4;
    nvn_gpu_set_dyn(cmd,&dyn);
    nvn_gpu_draw_args args={.prim=4,.count=3,.instances=1}; nvn_gpu_draw(cmd,&args);
    nvn_gpu_pass_end(cmd);
    for (int i=1;i<4;i++) nvn_gpu_copy_tex_buf(cmd,tex[i],0,0,NULL,pixels,(i-1)*64,16,64);
    if (!OK(nvn_gpu_cmd_commit(cmd,1,on_done,NULL,&err)) || !OK(nvn_gpu_fence_wait(1,5000000000ull,&err))) return 2;
    const uint8_t expected[3][4]={{13,37,69,255},{21,58,95,121},{1,1,1,2}};
    for (int slot=0;slot<3;slot++) for (int pixel=0;pixel<16;pixel++)
        CHECK(!memcmp(buf_cpu(pixels)+slot*64+pixel*4,expected[slot],4), "MRT slot %d pixel %d",slot+1,pixel);
    nvn_gpu_buf_release(pixels,NULL,NULL);
    for (int i=0;i<4;i++) { nvn_gpu_view_release(views[i]); nvn_gpu_tex_release(tex[i]); }
    rp_shader_reset();
    CHECK(OK(nvn_gpu_close(5000000000ull,&err)),"close MRT session"); s2a_vk_device_destroy(&g_dev,1);
    printf("libpack MRT compatible=%d compiler=%d integer_pixels=48 failures=%d\n",compatible,compile_calls,g_fail);
    return g_fail!=0;
}
