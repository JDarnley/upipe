/*
 * Copyright (C) 2022 Open Broadcast Systems Ltd.
 *
 * Authors: James Darnley
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/** @file
 * @short something
 */

#include "upipe/ubase.h"
#include "upipe/ubuf.h"
#include "upipe/ubuf_block.h"
#include "upipe/ubuf_mem.h"
#include "upipe/ubuf_pic.h"
#include "upipe/ubuf_sound.h"
#include "upipe/ubuf_super.h"
#include "upipe/umem.h"
#include "upipe/uref_flow.h"
#include "upipe/urefcount.h"

struct ubuf_super {
    /* sub ubufs associated with this super ubuf */
    struct ubuf **buf_b, **buf_p, **buf_s;
    /* common public struct */
    struct ubuf ubuf;
};

struct ubuf_super_mgr {
    /** sub managers */
    struct ubuf_mgr **mgr_b, **mgr_p, **mgr_s;
    /** number of sub managers */
    uint8_t num_mgr_b, num_mgr_p, num_mgr_s;
    /** refcount management structure */
    struct urefcount refcount;
    /* common public struct */
    struct ubuf_mgr mgr;
    /** sub manager pool depths */
    uint16_t ubuf_pool_depth, shared_pool_depth;
    /** sub manager umem manager */
    struct umem_mgr *umem_mgr;
};

UBASE_FROM_TO(ubuf_super, ubuf, ubuf, ubuf)

UBASE_FROM_TO(ubuf_super_mgr, ubuf_mgr, ubuf_mgr, mgr)
UBASE_FROM_TO(ubuf_super_mgr, urefcount, urefcount, refcount)

static struct ubuf *super_alloc(struct ubuf_mgr *mgr, uint32_t signature,
        va_list args)
{
    if (unlikely(signature != UBUF_SUPER_SIGNATURE))
        return NULL;

    const int *sizes = va_arg(args, const int *);
    const int *widths = va_arg(args, const int *);
    const int *heights = va_arg(args, const int *);
    const int *samples = va_arg(args, const int *);

    struct ubuf_super_mgr *ctx = ubuf_super_mgr_from_ubuf_mgr(mgr);

    void *v = malloc(sizeof(struct ubuf_super)
            + sizeof(struct ubuf *) * ctx->num_mgr_b
            + sizeof(struct ubuf *) * ctx->num_mgr_p
            + sizeof(struct ubuf *) * ctx->num_mgr_s);
    if (unlikely(v == NULL))
        return NULL;

    struct ubuf_super *super = v;
    struct ubuf *ubuf = ubuf_super_to_ubuf(super);
    ubuf->mgr = mgr;

    v += sizeof(struct ubuf_super);
    super->buf_b = v;
    v += sizeof(struct ubuf **) * ctx->num_mgr_b;
    super->buf_p = v;
    v += sizeof(struct ubuf **) * ctx->num_mgr_p;
    super->buf_s = v;

    /* TODO: replace with goto and memset arrays. */
    bool is_null = false;
    for (int i = 0; i < ctx->num_mgr_b; i++) {
        super->buf_b[i] = ubuf_block_alloc(ctx->mgr_b[i], sizes[i]);
        if (super->buf_b == NULL)
            is_null = true;
    }
    for (int i = 0; i < ctx->num_mgr_p; i++) {
        super->buf_p[i] = ubuf_pic_alloc(ctx->mgr_p[i], widths[i], heights[i]);
        if (super->buf_p == NULL)
            is_null = true;
    }
    for (int i = 0; i < ctx->num_mgr_s; i++) {
        super->buf_s[i] = ubuf_sound_alloc(ctx->mgr_s[i], samples[i]);
        if (super->buf_s == NULL)
            is_null = true;
    }

    if (is_null) {
        ubuf_free(ubuf);
        return NULL;
    }

    return ubuf;
}

static int get_sub_ubuf(struct ubuf_super *super, int type, struct ubuf **sub, uint8_t which)
{
    struct ubuf *ubuf = ubuf_super_to_ubuf(super);
    struct ubuf_mgr *mgr = ubuf->mgr;
    struct ubuf_super_mgr *ctx = ubuf_super_mgr_from_ubuf_mgr(mgr);

    struct ubuf **array;
    uint8_t num;
    if (type == UBUF_SUPER_GET_BLK_UBUF) {
        array = super->buf_b;
        num = ctx->num_mgr_b;
    }
    if (type == UBUF_SUPER_GET_PIC_UBUF) {
        array = super->buf_p;
        num = ctx->num_mgr_p;
    }
    if (type == UBUF_SUPER_GET_SND_UBUF) {
        array = super->buf_s;
        num = ctx->num_mgr_s;
    }

    if (which >= num)
        return UBASE_ERR_INVALID;

    *sub = array[which];
    return UBASE_ERR_NONE;
}

static int ubuf_super_control(struct ubuf *ubuf, int command, va_list args)
{
    struct ubuf_super *super = ubuf_super_from_ubuf(ubuf);
    switch (command) {
    case UBUF_SUPER_GET_BLK_UBUF:
    case UBUF_SUPER_GET_PIC_UBUF:
    case UBUF_SUPER_GET_SND_UBUF:
        UBASE_SIGNATURE_CHECK(args, UBUF_SUPER_SIGNATURE)
        return get_sub_ubuf(super, command, va_arg(args, struct ubuf **), va_arg(args, int));
    }

    return UBASE_ERR_UNHANDLED;
}

static void ubuf_super_free(struct ubuf *ubuf)
{
    struct ubuf_mgr *mgr = ubuf->mgr;
    struct ubuf_super_mgr *ctx = ubuf_super_mgr_from_ubuf_mgr(mgr);
    struct ubuf_super *super = ubuf_super_from_ubuf(ubuf);
    for (int i = 0; i < ctx->num_mgr_b; i++) {
        ubuf_free(super->buf_b[i]);
    }
    for (int i = 0; i < ctx->num_mgr_p; i++) {
        ubuf_free(super->buf_p[i]);
    }
    for (int i = 0; i < ctx->num_mgr_s; i++) {
        ubuf_free(super->buf_s[i]);
    }
    free(super);
    return;
}

static int add_sub_flow(struct ubuf_super_mgr *ctx, struct uref *flow)
{
    const char *def;
    UBASE_RETURN(uref_flow_get_def(flow, &def));

    struct ubuf_mgr ***array = NULL;
    uint8_t *num = NULL;
    if (!ubase_ncmp(def, "block.")) {
        array = &ctx->mgr_b;
        num = &ctx->num_mgr_b;
    }
    else if (!ubase_ncmp(def, "pic.")) {
        array = &ctx->mgr_p;
        num = &ctx->num_mgr_p;
    }
    else if (!ubase_ncmp(def, "sound.")) {
        array = &ctx->mgr_s;
        num = &ctx->num_mgr_s;
    }
    else
        return UBASE_ERR_INVALID;

    if (unlikely(*num == UINT8_MAX))
        return UBASE_ERR_INVALID;

    struct ubuf_mgr *sub = ubuf_mem_mgr_alloc_from_flow_def(ctx->ubuf_pool_depth,
            ctx->shared_pool_depth, ctx->umem_mgr, flow);
    UBASE_ALLOC_RETURN(sub);

    struct ubuf_mgr **new_array = realloc(*array, (*num + 1) * sizeof(struct ubuf_mgr *));
    if (unlikely(new_array == NULL)) {
        ubuf_mgr_release(sub);
        return UBASE_ERR_ALLOC;
    }

    new_array[*num] = sub;
    *array = new_array;
    *num += 1;

    return UBASE_ERR_NONE;
}

static int ubuf_super_mgr_control(struct ubuf_mgr *mgr, int command, va_list args)
{
    struct ubuf_super_mgr *ctx = ubuf_super_mgr_from_ubuf_mgr(mgr);
    switch (command) {
    case UBUF_SUPER_MGR_ADD_SUB_FLOW:
        UBASE_SIGNATURE_CHECK(args, UBUF_SUPER_SIGNATURE)
        return add_sub_flow(ctx, va_arg(args, struct uref*));
    }

    return UBASE_ERR_UNHANDLED;
}

static void ubuf_super_mgr_free(struct urefcount *urefcount)
{
    struct ubuf_super_mgr *ctx = ubuf_super_mgr_from_urefcount(urefcount);
    for (int i = 0; i < ctx->num_mgr_b; i++) {
        ubuf_mgr_release(ctx->mgr_b[i]);
    }
    free(ctx->mgr_b);
    for (int i = 0; i < ctx->num_mgr_p; i++) {
        ubuf_mgr_release(ctx->mgr_p[i]);
    }
    free(ctx->mgr_p);
    for (int i = 0; i < ctx->num_mgr_s; i++) {
        ubuf_mgr_release(ctx->mgr_s[i]);
    }
    free(ctx->mgr_s);
    umem_mgr_release(ctx->umem_mgr);
    urefcount_clean(urefcount);
    free(ctx);
}

struct ubuf_mgr *ubuf_super_mgr_alloc(uint16_t ubuf_pool_depth, uint16_t shared_pool_depth,
        struct umem_mgr *umem_mgr)
{
    struct ubuf_super_mgr *ctx = malloc(sizeof(struct ubuf_super_mgr));
    if (unlikely(ctx == NULL))
        return NULL;

    ctx->mgr_b = ctx->mgr_p = ctx->mgr_s = NULL;
    ctx->num_mgr_b = ctx->num_mgr_p = ctx->num_mgr_s = 0;
    ctx->ubuf_pool_depth = ubuf_pool_depth;
    ctx->shared_pool_depth = shared_pool_depth;
    ctx->umem_mgr = umem_mgr_use(umem_mgr);

    urefcount_init(ubuf_super_mgr_to_urefcount(ctx), ubuf_super_mgr_free);

    struct ubuf_mgr *mgr = ubuf_super_mgr_to_ubuf_mgr(ctx);
    mgr->refcount = ubuf_super_mgr_to_urefcount(ctx);
    mgr->signature = UBUF_SUPER_SIGNATURE;
    mgr->ubuf_alloc = super_alloc;
    mgr->ubuf_control = ubuf_super_control;
    mgr->ubuf_free = ubuf_super_free;
    mgr->ubuf_mgr_control = ubuf_super_mgr_control;

    return mgr;
}
