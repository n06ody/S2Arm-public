// VP9 representability audit (offline, host): decode an IVF stream with the pinned libvpx
// and apply the McVp9Backend gate semantics per frame, reporting the outcome distribution.
// Usage: vp9audit <file.ivf>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include "vpx/internal/vpx_codec_internal.h"
#include "vp9/common/vp9_onyxc_int.h"
#include "vp9/decoder/vp9_decoder.h"
#include "vp9/vp9_dx_iface.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: vp9audit <file.ivf>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    uint8_t hdr[32];
    if (fread(hdr, 1, 32, f) != 32 || memcmp(hdr, "DKIF", 4)) { fprintf(stderr, "not ivf\n"); return 1; }

    vpx_codec_ctx_t codec;
    if (vpx_codec_dec_init(&codec, vpx_codec_vp9_dx(), NULL, 0)) { fprintf(stderr, "init failed\n"); return 1; }
    struct vpx_codec_alg_priv *alg = (struct vpx_codec_alg_priv *)codec.priv;

    FRAME_CONTEXT *pre = malloc(sizeof(FRAME_CONTEXT) * FRAME_CONTEXTS);
    FRAME_CONTEXT defaults;
    int have_defaults = 0, frame = 0;
    int n_dec = 0, n_hidden = 0, n_header = 0, n_prev = 0, n_probs = 0, n_size = 0, n_exist = 0;
    int prev_w = 0, prev_h = 0;

    for (;;)
    {
        uint8_t fh[12];
        if (fread(fh, 1, 12, f) != 12) break;
        uint32_t sz = rd32(fh);
        uint8_t *buf = malloc(sz);
        if (fread(buf, 1, sz, f) != sz) { free(buf); break; }
        frame++;

        VP9_COMMON *cm = alg->pbi ? &alg->pbi->common : NULL;
        if (cm) memcpy(pre, cm->frame_contexts, sizeof(FRAME_CONTEXT) * FRAME_CONTEXTS);
        vpx_codec_err_t err = vpx_codec_decode(&codec, buf, sz, NULL, 0);
        if (err) { fprintf(stderr, "frame %d decode error\n", frame); free(buf); break; }
        free(buf);

        cm = &alg->pbi->common;
        if (cm->show_existing_frame) { n_exist++; continue; }

        int key = cm->frame_type == KEY_FRAME;
        int intra = key || cm->intra_only;
        int show = cm->intra_only ? 0 : 1;      // C# ShowFrame derivation (input.IntraOnly ? 0 : 1)

        if (key && !have_defaults)
        {
            defaults = cm->frame_contexts[0];
            have_defaults = 1;
        }
        int probs_ok = have_defaults && cm->frame_context_idx < FRAME_CONTEXTS &&
                       !memcmp(&pre[cm->frame_context_idx], &defaults, sizeof(FRAME_CONTEXT));

        const char *bucket;
        if (!show) { bucket = "hidden"; n_hidden++; }
        else if (!intra && (cm->tx_mode != 0 || cm->reference_mode != 0)) { bucket = "header"; n_header++; }
        else if (!intra && cm->use_prev_frame_mvs) { bucket = "prevMv"; n_prev++; }
        else if (!key && !probs_ok) { bucket = "probs"; n_probs++; }
        else if ((prev_w && (cm->width != prev_w || cm->height != prev_h)) && !key) { bucket = "size/key"; n_size++; }
        else { bucket = "Decoded"; n_dec++; }   // key frames decode with kf-init defaults (spec-forced)

        printf("f%04d %s key=%d intra_only=%d show=%d tx=%d ref=%d prevmv=%d errres=%d refresh_ctx=%d ctxidx=%u probs_ok=%d %ux%u -> %s\n",
               frame, key ? "K" : "I", key, cm->intra_only, cm->show_frame, cm->tx_mode, cm->reference_mode,
               cm->use_prev_frame_mvs, cm->error_resilient_mode, cm->refresh_frame_context, cm->frame_context_idx,
               probs_ok, cm->width, cm->height, bucket);
        prev_w = cm->width; prev_h = cm->height;
    }
    int n = frame - n_exist;
    printf("outcome: frames=%d (show_existing=%d) decoded=%d hidden=%d header=%d prevMv=%d probs=%d size/key=%d\n",
           frame, n_exist, n_dec, n_hidden, n_header, n_prev, n_probs, n_size);
    if (n) printf("decoded share: %d/%d = %.1f%%\n", n_dec, n, 100.0 * n_dec / n);
    return 0;
}
