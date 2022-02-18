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
#include "upipe/ubuf_pic.h"
#include "upipe/ubuf_sound.h"
#include "upipe/ubuf_super.h"
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
};

UBASE_FROM_TO(ubuf_super, ubuf, ubuf, ubuf)

UBASE_FROM_TO(ubuf_super_mgr, ubuf_mgr, ubuf_mgr, mgr)
UBASE_FROM_TO(ubuf_super_mgr, urefcount, urefcount, refcount)

static struct ubuf *ubuf_super_alloc(struct ubuf_mgr *mgr, uint32_t signature,  va_list args)
{
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
        super->buf_b[i] = ubuf_block_alloc(ctx->mgr_b[i], SIZE);
        if (super->buf_b == NULL)
            is_null = true;
    }
    for (int i = 0; i < ctx->num_mgr_p; i++) {
        super->buf_p[i] = ubuf_pic_alloc(ctx->mgr_p[i], WIDTH, HEIGHT);
        if (super->buf_p == NULL)
            is_null = true;
    }
    for (int i = 0; i < ctx->num_mgr_s; i++) {
        super->buf_s[i] = ubuf_sound_alloc(ctx->mgr_s[i], SAMPLES);
        if (super->buf_s == NULL)
            is_null = true;
    }

    if (is_null) {
        ubuf_free(ubuf);
        return NULL;
    }

    return ubuf;
}

static int ubuf_super_control(struct ubuf *ubuf, int command, va_list args)
{
    return UBASE_ERR_UNHANDLED;
}

static void ubuf_super_free(struct ubuf *ubuf)
{
    return;
}

static int ubuf_super_mgr_control(struct ubuf_mgr *mgr, int command, va_list args)
{
    return UBASE_ERR_UNHANDLED;
}

static void ubuf_super_mgr_free(struct urefcount *urefcount)
{
    struct ubuf_super_mgr *ctx = ubuf_super_mgr_from_urefcount(urefcount);
    urefcount_clean(urefcount);
    free(ctx);
}

struct ubuf_mgr *ubuf_super_mgr_alloc(void)
{
    struct ubuf_super_mgr *ctx = malloc(sizeof(struct ubuf_super_mgr));
    if (unlikely(ctx == NULL))
        return NULL;

    ctx->mgr_b = ctx->mgr_p = ctx->mgr_s = NULL;
    ctx->num_mgr_b = ctx->num_mgr_p = ctx->num_mgr_s = 0;

    urefcount_init(ubuf_super_mgr_to_urefcount(ctx), ubuf_super_mgr_free);

    struct ubuf_mgr *mgr = ubuf_super_mgr_to_ubuf_mgr(ctx);
    mgr->signature = UBUF_SUPER_SIGNATURE;
    mgr->ubuf_alloc = ubuf_super_alloc;
    mgr->ubuf_control = ubuf_super_control;
    mgr->ubuf_free = ubuf_super_free;
    mgr->ubuf_mgr_control = ubuf_super_mgr_control;

    return mgr;
}
