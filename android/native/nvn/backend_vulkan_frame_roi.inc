// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// The five reference boxes belong to the QA request. No screen or game identity
// is compiled in. The caller records the actual storage format and exact rectangle.
enum { ROI_BOXES=5, ROI_SIDE=4, ROI_SETS=384, ROI_BYTES=256*1024 };
struct roi_rect { unsigned x,y,w,h; };
struct roi_map {
    uint32_t swap_w,swap_h,src_w,src_h;
    int32_t sx,sy,sw,sh,dx,dy,dw,dh,flip,filter;
};
struct roi_sample {
    struct fr_image sub;
    uint32_t stage,format,width,height,slot,version,pass,draw;
    uint64_t submit;
    int committed;
    struct roi_rect box[ROI_BOXES];
    unsigned offset[ROI_BOXES],length[ROI_BOXES];
};
static struct roi_map roi_open_map;
static struct {
    struct roi_map mapping;
    struct roi_rect ref[ROI_BOXES];
    unsigned rw,rh,count,bytes,skips;
    const char *failure;
    int active,ready;
    nvn_gpu_buf buffer;
    struct roi_sample sample[ROI_SETS];
} *roi;
static void roi_forget(void) { free(roi); roi=NULL; }
static void roi_load(const struct frame_identity *id) {
    roi=calloc(1,sizeof *roi);
    if (!roi) { frame_reject("roi_allocation"); return; }
    char path[4096],leaf[192],line[256],prefix[192];
    snprintf(leaf,sizeof leaf,"%s-%s.roi",id->stem,id->nonce);
    if (!frame_path(path,leaf)) { roi->failure="plan_path"; return; }
    int fd=open(path,O_NOFOLLOW|O_NONBLOCK|O_RDONLY|O_CLOEXEC);
    struct stat st;
    if (fd<0) { roi->failure="plan_missing"; return; }
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>768) {
        close(fd); roi->failure="plan_stat"; return;
    }
    FILE *f=fdopen(fd,"r");
    if (!f) { close(fd); roi->failure="plan_read"; return; }
    snprintf(prefix,sizeof prefix,"v=1 pid=%ld birth=%llu session=%s nonce=%s reference=",id->pid,id->birth,id->session,id->nonce);
    int used=0, ok=fgets(line,sizeof line,f) && !strncmp(line,prefix,strlen(prefix)) &&
        sscanf(line+strlen(prefix),"%5ux%5u%n",&roi->rw,&roi->rh,&used)==2 &&
        !strcmp(line+strlen(prefix)+used,"\n") && roi->rw && roi->rh && roi->rw<=16384 && roi->rh<=16384;
    for (unsigned i=0;ok && i<ROI_BOXES;i++) {
        unsigned k;
        struct roi_rect *r=&roi->ref[i];
        ok=fgets(line,sizeof line,f) && sscanf(line,"%1u %5u %5u %5u %5u%n",&k,&r->x,&r->y,&r->w,&r->h,&used)==5 &&
            !strcmp(line+used,"\n") && k==i && r->w && r->h && r->x+r->w<=roi->rw && r->y+r->h<=roi->rh;
    }
    ok=ok && fgetc(f)==EOF && !ferror(f);
    fclose(f);
    if (!ok) { roi->failure="plan_identity_or_geometry"; return; }
    roi->mapping=roi_open_map;
    if (!roi->mapping.swap_w || roi->mapping.swap_w>16384 || roi->mapping.swap_h>16384 ||
        roi->mapping.src_w>16384 || roi->mapping.src_h>16384) { roi->failure="opening_transform"; return; }
    nvn_gpu_err err={0};
    if (!nvn_gpu_ok(nvn_gpu_buf_create(ROI_BYTES,NVN_GPU_MEM_SHARED,NULL,&roi->buffer,&err))) {
        roi->failure="readback_buffer"; return;
    }
    roi->active=1;
}
static unsigned roi_texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 0;
    }
}
// Map each axis independently from the QA reference via the installed WSI blit.
// Intermediate attachments use normalized source coordinates, with their local
// rectangle retained for cross-arm comparison. This does not infer shader UVs.
static int roi_axis(unsigned pos,unsigned len,unsigned ref,unsigned swap,int dst,int dlen,
                    int src,int slen,unsigned source,int flip,unsigned size,int final,
                    unsigned *start,unsigned *width) {
    int64_t lo=(int64_t)pos*swap,hi=(int64_t)(pos+len)*swap,den=ref;
    if (!final) {
        lo-=(int64_t)dst*ref; hi-=(int64_t)dst*ref;
        if (lo<0 || hi>(int64_t)dlen*ref) return 0;
        den=(int64_t)dlen*ref;
        if (flip) { int64_t old=lo; lo=den-hi; hi=den-old; }
        lo=(src*den+lo*slen)*size;
        hi=(src*den+hi*slen)*size;
        den*=source;
    }
    if (!den) return 0;
    unsigned l=(unsigned)(lo/den),h=(unsigned)((hi+den-1)/den);
    if (h>size || l>=h) return 0;
    *width=h-l>ROI_SIDE ? ROI_SIDE : h-l;
    *start=l+(h-l-*width)/2;
    return 1;
}
static unsigned roi_reserve(struct fr_event *e,unsigned final,unsigned format,unsigned w,unsigned h,unsigned slot) {
    if (!roi || !roi->active || !e) return 0;
    unsigned bpp=roi_texel_bytes(format);
    if (!bpp || !w || !h || w>16384 || h>16384 || roi->count>=ROI_SETS-(final ? 0u : 1u)) { roi->skips++; return 0; }
    struct roi_sample *s=&roi->sample[roi->count];
    unsigned bytes=roi->bytes;
    const struct roi_map *m=&roi->mapping;
    for (unsigned i=0;i<ROI_BOXES;i++) {
        struct roi_rect *b=&s->box[i],r=roi->ref[i];
        if (!roi_axis(r.x,r.w,roi->rw,m->swap_w,m->dx,m->dw,m->sx,m->sw,m->src_w,0,w,final,&b->x,&b->w) ||
            !roi_axis(r.y,r.h,roi->rh,m->swap_h,m->dy,m->dh,m->sy,m->sh,m->src_h,m->flip,h,final,&b->y,&b->h)) {
            roi->skips++; return 0;
        }
        bytes=(bytes+7u)&~7u;
        s->offset[i]=bytes; s->length[i]=b->w*b->h*bpp; bytes+=s->length[i];
    }
    if (bytes>ROI_BYTES-(final ? 0u : ROI_BOXES*ROI_SIDE*ROI_SIDE*8u)) { roi->skips++; return 0; }
    roi->bytes=bytes;
    s->sub=e->sub; s->stage=final; s->format=format; s->width=w; s->height=h; s->slot=slot;
    e->roi=++roi->count;
    return e->roi;
}
static void roi_committed(const struct fr_event *e) {
    if (!roi || !e->roi || e->roi>roi->count) return;
    struct roi_sample *s=&roi->sample[e->roi-1];
    s->committed=1; s->submit=e->signal; s->version=e->version; s->pass=e->pass; s->draw=e->draw;
}
static int roi_emit(FILE *f) {
    if (!roi) return 1;
    const struct roi_map *m=&roi->mapping;
    if (fprintf(f,"roi-status\t%s\t%u\t%u\t%u\t%u\t%u\n",roi->failure?roi->failure:roi->ready?"ready":"incomplete",
                roi->count,roi->bytes,roi->skips,roi->rw,roi->rh)<0) return 0;
    if (!roi->ready || roi->failure) return 1;
    if (fprintf(f,"roi-map\t%u\t%u\t%u\t%u\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
                m->swap_w,m->swap_h,m->src_w,m->src_h,m->sx,m->sy,m->sw,m->sh,m->dx,m->dy,m->dw,m->dh,m->flip,m->filter)<0) return 0;
    const uint8_t *cpu=nvn_gpu_buf_cpu(roi->buffer);
    if (!cpu) { errno = ENODATA; return 0; }
    for (unsigned k=0;k<roi->count;k++) {
        const struct roi_sample *s=&roi->sample[k];
        if (!s->committed) continue;
        for (unsigned i=0;i<ROI_BOXES;i++) {
            struct roi_rect b=s->box[i];
            char hex[ROI_SIDE*ROI_SIDE*8*2+1];
            static const char digits[]="0123456789abcdef";
            for (unsigned j=0;j<s->length[i];j++) {
                uint8_t v=cpu[s->offset[i]+j]; hex[2*j]=digits[v>>4]; hex[2*j+1]=digits[v&15];
            }
            hex[2*s->length[i]]=0;
            if (fprintf(f,"roi\t%u\t%s\t%llu\t%u\t%u\t%u\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%s\n",
                        i,s->stage?"swap":"attachment",(unsigned long long)s->sub.image,s->sub.mip,s->sub.layer,s->version,
                        (unsigned long long)s->submit,s->pass,s->draw,s->slot,s->format,s->width,s->height,b.x,b.y,b.w,b.h,hex)<0) return 0;
        }
    }
    return 1;
}
