#include "nvn_vk_frame_pool_index.h"
// Physical color resource observations. Command buffers own their recording.
// Versions are assigned in successful queue submission order under frame_mu.
enum { FR_EVENTS = 16384, FR_KEYS = 4096, FR_DESCRIPTORS = 512 };
enum fr_op { FR_CLEAR, FR_SAMPLE, FR_COLOR, FR_COPY_READ, FR_COPY_WRITE, FR_PRESENT, FR_POOL_UNKNOWN, FR_WRITE_GAP, FR_SAMPLE_GAP };
static const char *const fr_names[] = {"clear","sample","color","copy-read","copy-write","present"};
struct fr_image { uint64_t image, view; uint32_t mip, layer, format; };
struct fr_event {
    struct fr_image sub;
    uint64_t signal;
    const char *reason;
    uint32_t op, version, pass, draw, stage, set, binding, elem, roi;
};
struct fr_descriptor { VkDescriptorSet set; uint32_t binding, elem; VkImageView view; };
struct fr_cmd {
    struct fr_event *events;
    uint32_t n, cap, nd;
    const char *missing;
    struct fr_descriptor descriptors[FR_DESCRIPTORS];
    VkDescriptorSet bound[4];
    struct fr_image colors[8];
};
struct fr_version { uint64_t image; uint32_t mip, layer, value, epoch; };
struct fr_frame {
    struct fr_event *events;
    uint32_t n, cap;
    unsigned partial, epoch;
    struct fr_version keys[FR_KEYS];
};
static uint64_t access_id(uint64_t *slot);
static int fr_option;  // session option, read only under frame_mu
static struct fr_frame *fr;
static void fr_reset(void) {
    if (fr) free(fr->events);
    free(fr);
    fr = NULL;
}

static void fr_dispose(struct fr_cmd *r) {
    if (r) free(r->events);
    free(r);
}
static int fr_reserve(struct fr_event **events, uint32_t *cap, uint32_t n) {
    if (n > (atomic_load(&frame_enabled)==3 ? 4u*FR_EVENTS : FR_EVENTS)) return 0;
    if (n <= *cap) return 1;
    uint32_t next = *cap ? *cap : 32;
    while (next < n) next *= 2;
    struct fr_event *p = realloc(*events, next * sizeof *p);
    if (!p) return 0;
    *events = p; *cap = next;
    return 1;
}

static struct fr_image fr_sub(nvn_gpu_tex t, unsigned mip, unsigned layer) {
    return (struct fr_image){.image=access_id(&t->access_id),.mip=mip,.layer=layer};
}
static struct fr_event *fr_add(nvn_gpu_cmd c, enum fr_op op, struct fr_image sub, uint32_t draw) {
    struct fr_cmd *r=c->resources;
    if (r->missing) return NULL;
    if (!fr_reserve(&r->events,&r->cap,r->n+1)) { r->missing="resource_limit"; return NULL; }
    struct fr_event *e=&r->events[r->n++];
    *e=(struct fr_event){.op=op,.sub=sub,.draw=draw,.pass=(op==FR_CLEAR || op==FR_COLOR || op==FR_SAMPLE || op==FR_POOL_UNKNOWN) ? c->observation_pass-1 : UINT32_MAX};
    return e;
}

// Only successful queue submissions publish an event. A known destination
// invalidates that slice; a write without a physical destination invalidates
// all prior versions. A missing read only affects its draw's provenance.
static void fr_write_gap(nvn_gpu_cmd c, const char *reason, struct fr_image dest) {
    if (!c || !c->resources) return;
    if (atomic_load(&frame_enabled)!=3) {
        if (!c->resources->missing) c->resources->missing=reason;
        return;
    }
    struct fr_event *e=fr_add(c,FR_WRITE_GAP,dest,UINT32_MAX);
    if (e) {
        e->reason=reason;
        e->pass=c->in_pass && c->observation_pass ? c->observation_pass-1 : UINT32_MAX;
    }
}
static void fr_gap(nvn_gpu_cmd c, const char *reason) {
    fr_write_gap(c,reason,(struct fr_image){0});
}
static void fr_read_gap(nvn_gpu_cmd c, const char *reason, unsigned draw, unsigned stage,
                        const nvn_gpu_layout_ent *entry) {
    if (atomic_load(&frame_enabled)!=3) { fr_gap(c,reason); return; }
    struct fr_event *e=fr_add(c,FR_SAMPLE_GAP,(struct fr_image){0},draw);
    if (e) {
        e->reason=reason; e->pass=c->observation_pass-1;
        e->stage=stage; e->set=entry->set; e->binding=entry->binding;
    }
}

// The handles are the exact framebuffer attachment array passed to pc_fb.
// Keep separate lifetime-stable IDs for normal and derived one-layer views.
static void fr_pass(nvn_gpu_cmd c, const nvn_gpu_pass_desc *d, const VkAttachmentReference *col,
                    const VkAttachmentDescription *att, const VkImageView *views) {
    struct fr_cmd *r=c->resources;
    memset(r->colors,0,sizeof r->colors);
    for (unsigned i=0;i<d->ncolor && i<8;i++) {
        nvn_gpu_view v=d->color[i].view;
        if (!v) continue;
        unsigned a=col[i].attachment;
        if (a>=9 || d->layers>1 || v->tex->target==NVN_TEXTURE_TARGET_3D ||
            att[a].storeOp!=VK_ATTACHMENT_STORE_OP_STORE || v->tex->aspect!=VK_IMAGE_ASPECT_COLOR_BIT ||
            !views[a] || (views[a]!=v->view && views[a]!=v->att)) { fr_gap(c,"attachment_view"); continue; }
        struct fr_image sub=fr_sub(v->tex,v->base_level,v->base_layer);
        sub.view=access_id(views[a]==v->view ? &v->resource_view_id : &v->resource_att_id);
        sub.format=att[a].format;
        r->colors[i]=sub;
        if (att[a].loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR) {
            if (d->w != (v->tex->w>>v->base_level ? v->tex->w>>v->base_level : 1) ||
                d->h != (v->tex->h>>v->base_level ? v->tex->h>>v->base_level : 1)) {
                fr_write_gap(c,"partial_attachment_clear",sub); continue;
            }
            struct fr_event *e=fr_add(c,FR_CLEAR,sub,UINT32_MAX);
            if (e) e->binding=i;
        } else if (att[a].loadOp!=VK_ATTACHMENT_LOAD_OP_LOAD) fr_write_gap(c,"attachment_discard",sub);
    }
}

static struct fr_descriptor *fr_slot(struct fr_cmd *r, VkDescriptorSet set, uint32_t binding, uint32_t elem) {
    for (unsigned i=0;i<r->nd;i++) {
        struct fr_descriptor *d=&r->descriptors[i];
        if (d->set==set && d->binding==binding && d->elem==elem) return d;
    }
    return NULL;
}
static void fr_descriptor_write(nvn_gpu_cmd c, const VkWriteDescriptorSet *w, uint32_t offset) {
    struct fr_cmd *r=c->resources;
    uint32_t elem=w->dstArrayElement+offset;
    struct fr_descriptor *d=fr_slot(r,w->dstSet,w->dstBinding,elem);
    if (!d) {
        if (r->nd==FR_DESCRIPTORS) { if (atomic_load(&frame_enabled)!=3) fr_gap(c,"descriptor_limit"); return; }
        d=&r->descriptors[r->nd++];
    }
    *d=(struct fr_descriptor){w->dstSet,w->dstBinding,elem,w->pImageInfo[offset].imageView};
}
// Record the view handles actually written, including feedback patches. Record
// only proven indices of a bulk pool write, while updating any older observation.
static void fr_writes(nvn_gpu_cmd c, uint32_t n, const VkWriteDescriptorSet *writes) {
    struct fr_cmd *r=c->resources;
    for (uint32_t i=0;i<n;i++) {
        const VkWriteDescriptorSet *w=&writes[i];
        if (w->descriptorType!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
            w->descriptorType!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        if (!w->pImageInfo) { fr_gap(c,"descriptor_view"); return; }
        if (w->descriptorCount==1) { fr_descriptor_write(c,w,0); continue; }
        for (unsigned j=0;j<r->nd;j++) {
            struct fr_descriptor *d=&r->descriptors[j];
            uint32_t off=d->elem-w->dstArrayElement;
            if (d->set==w->dstSet && d->binding==w->dstBinding && off<w->descriptorCount)
                d->view=w->pImageInfo[off].imageView;
        }
        if (!c->pipe) continue;
        for (unsigned st=0;st<5;st++) {
            nvn_gpu_shader sh=c->pipe->key.st[st].sh;
            if (!sh || !sh->frame_pool_indices) continue;
            for (unsigned j=0;j<sh->nent;j++) {
                const nvn_gpu_layout_ent *e=&sh->ents[j];
                uint32_t elem=sh->frame_pool_indices[j];
                if (e->set>=4 || w->dstSet!=c->desc_sets[e->set] || w->dstBinding!=e->binding ||
                    elem==UINT32_MAX || elem-w->dstArrayElement>=w->descriptorCount) continue;
                fr_descriptor_write(c,w,elem-w->dstArrayElement);
            }
        }
    }
}
static void fr_draw(nvn_gpu_cmd c, nvn_gpu_pipe p, uint32_t draw, int discard) {
    struct fr_cmd *r=c->resources;
    for (unsigned st=0;st<5;st++) {
        for (unsigned i=0;i<p->stage_nent[st];i++) {
            const nvn_gpu_layout_ent *e=&p->stage_ents[st][i];
            if (e->cls==NVN_GPU_RES_IMAGE) { fr_gap(c,"storage_image"); continue; }
            if (e->cls!=NVN_GPU_RES_TEX && e->cls!=NVN_GPU_RES_TEX_SAMP) continue;
            if (e->set>=4 || i>=64) { fr_read_gap(c,"descriptor_range",draw,st,e); continue; }
            uint32_t elem=0;
            nvn_gpu_view v=NULL;
            nvn_gpu_pool_table pool=c->pool_binds[st][i];
            if (pool) {
                nvn_gpu_shader sh=p->key.st[st].sh;
                if (!sh || !sh->frame_pool_indices || i>=sh->nent ||
                    (elem=sh->frame_pool_indices[i])>pool->count) {
                    struct fr_event *gap=fr_add(c,FR_POOL_UNKNOWN,(struct fr_image){0},draw);
                    if (gap) { gap->stage=st; gap->set=e->set; gap->binding=e->binding; }
                    continue;
                }
                v=pool->objects[elem];
            } else {
                if (e->count!=1) { fr_read_gap(c,"descriptor_array",draw,st,e); continue; }
                for (unsigned j=0;j<c->nbinds[st];j++) {
                    const nvn_gpu_binding *b=&c->binds[st][j].b;
                    if (b->ent==i && !b->elem) v=e->cls==NVN_GPU_RES_TEX ? b->u.view : b->u.ts.view;
                }
            }
            struct fr_descriptor *d=fr_slot(r,r->bound[e->set],e->binding,elem);
            if (!d || !v || !v->tex || !d->view || d->view!=v->view || v->aspect!=VK_IMAGE_ASPECT_COLOR_BIT ||
                v->levels!=1 || v->layers!=1 || v->type!=VK_IMAGE_VIEW_TYPE_2D) { fr_read_gap(c,"sample_view",draw,st,e); continue; }
            struct fr_image sub=fr_sub(v->tex,v->base_level,v->base_layer);
            sub.view=access_id(&v->resource_view_id);
            sub.format=v->format;
            struct fr_event *event=fr_add(c,FR_SAMPLE,sub,draw);
            if (event) { event->stage=st; event->set=e->set; event->binding=e->binding; event->elem=elem; }
        }
    }
    if (!discard && !c->draw_pipe.discard) for (unsigned i=0;i<8;i++) {
        if (!r->colors[i].image || !(p->key.chmask.mask >> (4*i) & 15)) continue;
        struct fr_event *e=fr_add(c,FR_COLOR,r->colors[i],draw);
        if (e) e->binding=i;
    }
}
static void fr_version(struct fr_event *e) {
    struct fr_image s=e->sub;
    unsigned ix=(unsigned)(s.image*2654435761u+s.mip*257u+s.layer) % FR_KEYS;
    for (unsigned probe=0;probe<FR_KEYS;probe++,ix=(ix+1)%FR_KEYS) {
        struct fr_version *k=&fr->keys[ix];
        if (k->image && (k->image!=s.image || k->mip!=s.mip || k->layer!=s.layer)) continue;
        if (!k->image) {
            *k=(struct fr_version){.image=s.image, .mip=s.mip, .layer=s.layer, .value=0, .epoch=fr->epoch};
        }
        if (e->op==FR_WRITE_GAP) { k->epoch=UINT32_MAX; return; }
        int known=k->value && k->epoch==fr->epoch;
        if (!known && e->op==FR_COLOR && !frame_choices) frame_reject("prior_attachment_contents");
        if (e->op==FR_CLEAR || e->op==FR_COPY_WRITE || e->op==FR_COLOR) {
            k->value++;
            k->epoch=fr->epoch;
            known=1;
        }
        e->version=known ? k->value : 0;
        if (!e->version && !frame_choices) frame_reject("prior_frame_source");
        return;
    }
    frame_reject("subresource_limit");
}
#include "backend_vulkan_frame_roi.inc"

static void fr_commit(nvn_gpu_cmd c, uint64_t signal) {
    if (!fr || !frame_capture.rows || frame_capture.finished) return;
    struct fr_cmd *r=c->resources;
    if (!r || c->observation_ticket!=frame_capture.previous_ticket) { frame_reject("resource_segment"); return; }
    if (r->missing) { frame_reject(r->missing); return; }
    if (!fr_reserve(&fr->events,&fr->cap,fr->n+r->n)) { frame_reject("resource_limit"); return; }
    for (unsigned i=0;i<r->n;i++) {
        struct fr_event *e=&fr->events[fr->n++];
        *e=r->events[i]; e->signal=signal;
        if (e->op==FR_POOL_UNKNOWN || e->op==FR_SAMPLE_GAP) fr->partial=1;
        else if (e->op==FR_WRITE_GAP && !e->sub.image) fr->epoch++;
        else fr_version(e);
        if (e->op==FR_SAMPLE && e->elem) fr->partial=1; // indexed observations use the extended schema
        roi_committed(e);
    }
}
static int fr_emit(FILE *f) {
    if (!fr) return 1;
    for (unsigned i=0;i<fr->n;i++) {
        const struct fr_event *e=&fr->events[i];
        if (e->op==FR_POOL_UNKNOWN || e->op==FR_SAMPLE_GAP || e->op==FR_WRITE_GAP) {
            const char *reason=e->op==FR_POOL_UNKNOWN ? "pool_index_unproven" : e->reason;
            if (fprintf(f,"gap\t%s\t%llu\t%u\t%u\t%u\t%u\t%u",reason,
                        (unsigned long long)e->signal,e->pass,e->draw,e->stage,e->set,e->binding)<0) return 0;
            if (e->op==FR_WRITE_GAP && e->sub.image &&
                fprintf(f,"\t%llu\t%u\t%u",(unsigned long long)e->sub.image,e->sub.mip,e->sub.layer)<0) return 0;
            if (fputc('\n',f)==EOF) return 0;
            continue;
        }
        if (fprintf(f,"res\t%s\t%llu\t%u\t%u\t%u\t%llu\t%llu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n",fr_names[e->op],
                    (unsigned long long)e->sub.image,e->sub.mip,e->sub.layer,e->version,(unsigned long long)e->sub.view,
                    (unsigned long long)e->signal,e->pass,e->draw,e->stage,e->set,e->binding,e->elem,e->sub.format)<0) return 0;
    }
    return 1;
}
