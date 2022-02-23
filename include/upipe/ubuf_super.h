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

#ifndef _UPIPE_UBUF_SUPER_H_
/** @hidden */
#define _UPIPE_UBUF_SUPER_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/ubase.h>
#include <upipe/ubuf.h>
#include <upipe/uref.h>

#define UBUF_SUPER_SIGNATURE UBASE_FOURCC('s','u','p','a')

static inline struct ubuf *ubuf_super_alloc(struct ubuf_mgr *mgr, unsigned sizes[],
        unsigned widths[], unsigned heights[], unsigned samples[])
{
    return ubuf_alloc(mgr, UBUF_SUPER_SIGNATURE, sizes, widths, heights, samples);
}

struct ubuf_mgr *ubuf_super_mgr_alloc(uint16_t ubuf_pool_depth, uint16_t shared_pool_depth,
        struct umem_mgr *umem_mgr);

enum ubuf_super_mgr_command {
    UBUF_SUPER_MGR_ADD_SUB_FLOW = UBUF_MGR_CONTROL_LOCAL,
};

static inline int ubuf_super_mgr_add_sub_flow(struct ubuf_mgr *mgr,
        struct uref *flow)
{
    return ubuf_mgr_control(mgr, UBUF_SUPER_MGR_ADD_SUB_FLOW, UBUF_SUPER_SIGNATURE,
            flow);
}

#ifdef __cplusplus
}
#endif
#endif
