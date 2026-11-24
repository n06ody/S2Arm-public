// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Copy metadata while the resolved shader is live. A choice can precede the
// capture trigger and the module can be retired long before the frame is sealed.
enum { FRAME_CHOICES = 4096, FRAME_CHOICE_SLOTS = FRAME_CHOICES * 2 };
struct frame_choice_key {
    int32_t cbuf, word, status;
    uint8_t request[32], owner[32], fs[32], keys[2][8];
    uint8_t source, route, refusal, stage, requested_class, declared_class;
};
struct frame_choice_record {
    struct frame_choice_key key;
    uint64_t uses, first, last;
};
struct frame_choices {
    uint16_t slots[FRAME_CHOICE_SLOTS];
    uint32_t count;
    uint64_t calls, dropped, unidentified;
    struct frame_choice_record records[FRAME_CHOICES];
};
static struct frame_choices *frame_choices;
int nvn_gpu_draw_choices_enabled(void) { return atomic_load(&frame_enabled)==3; }

uint32_t nvn_gpu_program_choice_note(const uint8_t request[32], const uint8_t owner[32],
                                     nvn_gpu_shader fs, int status, const struct rp_shader_choice *choice) {
    if (!nvn_gpu_draw_choices_enabled()) return 0;
    pthread_mutex_lock(&frame_mu);
    uint32_t number=0;
    struct frame_choices *t=frame_choices;
    if (!t || frame_capture.finished) goto done;
    uint64_t ordinal=++t->calls;
    if (!choice || !frame_digest(request)) { t->unidentified++; goto done; }
    struct frame_choice_key key;
    memset(&key,0,sizeof key);
    memcpy(key.request,request,32);
    if (owner) memcpy(key.owner,owner,32);
    if (fs) memcpy(key.fs,fs->spirv_digest,32);
    memcpy(key.keys,choice->keys,sizeof key.keys);
    key.status=status; key.source=choice->source; key.route=choice->route; key.refusal=choice->refusal;
    key.cbuf=key.word=-1; key.requested_class=key.declared_class=key.stage=UINT8_MAX;
    if (key.refusal==RP_LIB_NUMERIC_MISMATCH || key.refusal==RP_LIB_NUMERIC_UNPROVEN) {
        key.cbuf=choice->cbuf; key.word=choice->word; key.stage=choice->stage;
        key.requested_class=choice->requested_class; key.declared_class=choice->declared_class;
    }
    // The first SHA word only selects a bucket. Compare the full identity and
    // outcome so an alias or a later fallback cannot overwrite its predecessor.
    uint32_t bucket;
    memcpy(&bucket,request,sizeof bucket);
    bucket &= FRAME_CHOICE_SLOTS-1;
    for (;;) {
        number=t->slots[bucket];
        if (!number) {
            if (t->count==FRAME_CHOICES) { t->dropped++; goto done; }
            number=++t->count;
            t->slots[bucket]=(uint16_t)number;
            t->records[number-1].key=key;
            t->records[number-1].first=ordinal;
            break;
        }
        if (!memcmp(&t->records[number-1].key,&key,sizeof key)) break;
        bucket=(bucket+1)&(FRAME_CHOICE_SLOTS-1);
    }
    t->records[number-1].uses++;
    t->records[number-1].last=ordinal;
done:
    pthread_mutex_unlock(&frame_mu);
    return number;
}

static uint32_t frame_draw_choice(const nvn_gpu_draw_observation *o, nvn_gpu_shader fs) {
    if (!frame_choices || !o->choice || o->choice>frame_choices->count) return 0;
    const struct frame_choice_key *key=&frame_choices->records[o->choice-1].key;
    if (key->status || memcmp(key->request,o->request,32) || memcmp(key->owner,o->owner,32) ||
        (key->source==RP_SOURCE_LIBPACK)!=!!o->lib_sourced ||
        (fs ? memcmp(key->fs,fs->spirv_digest,32)!=0 : frame_digest(key->fs))) return 0;
    return o->choice;
}

static int frame_emit_choices(FILE *f) {
    if (!frame_choices) return 1;
    const struct frame_choices *t=frame_choices;
    if (fprintf(f,"choice-summary\t%llu\t%u\t%llu\t%llu\n",
                (unsigned long long)t->calls,t->count,(unsigned long long)t->dropped,
                (unsigned long long)t->unidentified)<0) return 0;
    for (unsigned i=0;i<t->count;i++) {
        const struct frame_choice_record *r=&t->records[i];
        const struct frame_choice_key *k=&r->key;
        char request[65],owner[65],fs[65],vs_key[17],fs_key[17];
        s2a_sha256_hex(k->request,request);
        s2a_sha256_hex(k->owner,owner);
        s2a_sha256_hex(k->fs,fs);
        for (unsigned j=0;j<8;j++) {
            snprintf(vs_key+2*j,3,"%02x",k->keys[0][j]);
            snprintf(fs_key+2*j,3,"%02x",k->keys[1][j]);
        }
        if (fprintf(f,"choice\t%u\t%llu\t%llu\t%llu\t%s\t%s\t%s\t%s\t%u\t%u\t%u\t%u\t%d\t%d\t%u\t%u\t%d\t%s\n",
                    i+1,(unsigned long long)r->uses,(unsigned long long)r->first,(unsigned long long)r->last,
                    request,owner,vs_key,fs_key,k->source,k->route,k->refusal,k->stage,k->cbuf,k->word,
                    k->requested_class,k->declared_class,k->status,frame_digest(k->fs)?fs:"-")<0) return 0;
    }
    for (unsigned i=0;i<frame_capture.count;i++)
        if (fprintf(f,"draw-choice\t%u\t%u\n",i,frame_capture.rows[i].choice)<0) return 0;
    return 1;
}
