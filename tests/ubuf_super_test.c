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
 * @short tests for ubuf_super
 */

#undef NDEBUG

#include "upipe/ubuf.h"
#include "upipe/ubuf_super.h"
#include "upipe/udict.h"
#include "upipe/udict_inline.h"
#include "upipe/umem.h"
#include "upipe/umem.h"
#include "upipe/umem_alloc.h"
#include "upipe/uref.h"
#include "upipe/uref_flow.h"
#include "upipe/uref_pic_flow.h"
#include "upipe/uref_sound_flow.h"
#include "upipe/uref_std.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

#define UBUF_POOL_DEPTH 1
#define UDICT_POOL_DEPTH 1
#define UREF_POOL_DEPTH 1

int main(int argc, char **argv)
{
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH, umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    struct uref_mgr *uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr, 0);
    assert(uref_mgr != NULL);

    struct ubuf_mgr *mgr = ubuf_super_mgr_alloc(UBUF_POOL_DEPTH, UBUF_POOL_DEPTH, umem_mgr);
    assert(mgr != NULL);

    struct uref *flow_def = uref_alloc(uref_mgr);
    assert(flow_def != NULL);

    ubase_nassert(ubuf_super_mgr_add_sub_flow(mgr, flow_def)); /* Should fail because no def is set */

    ubase_assert(uref_flow_set_def(flow_def, "error"));
    ubase_nassert(ubuf_super_mgr_add_sub_flow(mgr, flow_def)); /* Should fail because it is an unknown def. */

    ubase_assert(uref_flow_set_def(flow_def, "block."));
    ubase_assert(ubuf_super_mgr_add_sub_flow(mgr, flow_def));

    ubase_assert(uref_flow_set_def(flow_def, "pic."));
    ubase_assert(uref_pic_flow_set_macropixel(flow_def, 1));
    ubase_assert(uref_pic_flow_add_plane(flow_def, 1,1,1, "test"));
    ubase_assert(ubuf_super_mgr_add_sub_flow(mgr, flow_def));
    uref_free(flow_def);

    flow_def = uref_alloc(uref_mgr);
    assert(flow_def != NULL);
    ubase_assert(uref_flow_set_def(flow_def, "sound."));
    ubase_assert(uref_sound_flow_set_sample_size(flow_def, 1));
    ubase_assert(uref_sound_flow_add_plane(flow_def, "test"));
    ubase_assert(ubuf_super_mgr_add_sub_flow(mgr, flow_def));
    uref_free(flow_def);

    ubuf_mgr_release(mgr);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    return 0;
}
