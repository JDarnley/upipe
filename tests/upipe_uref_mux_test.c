/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: James Darnley
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short unit tests for uref_mux pipes
 */

#undef NDEBUG

#include "upipe/uprobe.h"
#include "upipe/uprobe_stdio.h"
#include "upipe/uprobe_prefix.h"
#include "upipe/umem.h"
#include "upipe/umem_alloc.h"
#include "upipe/udict.h"
#include "upipe/udict_inline.h"
#include "upipe/uref.h"
#include "upipe/uref_std.h"
#include "upipe/uref_flow.h"
#include "upipe/uref_block.h"
#include "upipe/uref_block_flow.h"
#include "upipe/uref_sub.h"
#include "upipe/ubuf.h"
#include "upipe/ubuf_block.h"
#include "upipe/ubuf_block_mem.h"
#include "upipe/upipe.h"
#include "upipe-ts/uref_ts_flow.h"
#include "upipe-modules/upipe_uref_mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define UDICT_POOL_DEPTH    0
#define UREF_POOL_DEPTH     0
#define UBUF_POOL_DEPTH     0
#define UPROBE_LOG_LEVEL    UPROBE_LOG_VERBOSE

/* PIDs used as per-input keys */
#define MAIN_PID    100
#define PID_A       200
#define PID_B       300
#define PID_D       400

/* distinct block sizes, used to tell which buffer landed at which entry */
#define MAIN_SIZE   188
#define SIZE_A      64
#define SIZE_A2     65
#define SIZE_B      128
#define SIZE_D      96

/** last output flow def seen through UPROBE_NEW_FLOW_DEF (not owned) */
static struct uref *last_flow_def = NULL;
/** number of output flow defs emitted */
static unsigned new_flow_def_count = 0;
/** last data uref received by the sink (owned) */
static struct uref *sink_uref = NULL;

/** definition of our uprobe */
static int catch(struct uprobe *uprobe, struct upipe *upipe,
                 int event, va_list args)
{
    switch (event) {
        case UPROBE_READY:
        case UPROBE_DEAD:
            break;
        case UPROBE_NEW_FLOW_DEF:
            last_flow_def = va_arg(args, struct uref *);
            new_flow_def_count++;
            break;
        default:
            assert(0);
            break;
    }
    return UBASE_ERR_NONE;
}

/** helper phony pipe */
static struct upipe *test_alloc(struct upipe_mgr *mgr, struct uprobe *uprobe,
                                uint32_t signature, va_list args)
{
    struct upipe *upipe = malloc(sizeof(struct upipe));
    assert(upipe != NULL);
    upipe_init(upipe, mgr, uprobe);
    return upipe;
}

/** helper phony pipe: capture the multiplexed output uref */
static void test_input(struct upipe *upipe, struct uref *uref,
                       struct upump **upump_p)
{
    assert(uref != NULL);
    uref_free(sink_uref);
    sink_uref = uref;
}

/** helper phony pipe */
static int test_control(struct upipe *upipe, int command, va_list args)
{
    switch (command) {
        case UPIPE_REGISTER_REQUEST:
        case UPIPE_UNREGISTER_REQUEST:
        case UPIPE_SET_FLOW_DEF:
            return UBASE_ERR_NONE;
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** helper phony pipe */
static void test_free(struct upipe *upipe)
{
    upipe_clean(upipe);
    free(upipe);
}

/** helper phony pipe manager */
static struct upipe_mgr test_mgr = {
    .upipe_alloc = test_alloc,
    .upipe_input = test_input,
    .upipe_control = test_control,
};

/** @This allocates a block uref of the given size. */
static struct uref *block_alloc(struct uref_mgr *uref_mgr,
                                struct ubuf_mgr *ubuf_mgr, size_t size)
{
    struct uref *uref = uref_block_alloc(uref_mgr, ubuf_mgr, size);
    assert(uref != NULL);
    return uref;
}

/** @This allocates a flow def carrying a TS PID. */
static struct uref *flow_def_alloc(struct uref_mgr *uref_mgr,
                                   const char *def, uint64_t pid)
{
    struct uref *flow_def = uref_block_flow_alloc_def(uref_mgr, def);
    assert(flow_def != NULL);
    ubase_assert(uref_ts_flow_set_pid(flow_def, pid));
    return flow_def;
}

int main(int argc, char *argv[])
{
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    struct uref_mgr *uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr,
                                                   0);
    assert(uref_mgr != NULL);
    struct ubuf_mgr *ubuf_mgr = ubuf_block_mem_mgr_alloc(UBUF_POOL_DEPTH,
                                                         UBUF_POOL_DEPTH,
                                                         umem_mgr, 0, 0, -1, 0);
    assert(ubuf_mgr != NULL);

    struct uprobe uprobe;
    uprobe_init(&uprobe, catch, NULL);
    struct uprobe *logger = uprobe_stdio_alloc(&uprobe, stdout,
                                               UPROBE_LOG_LEVEL);
    assert(logger != NULL);

    /* sink capturing the multiplexed output */
    struct upipe *sink = upipe_void_alloc(&test_mgr, uprobe_use(logger));
    assert(sink != NULL);

    /* the mux under test */
    struct upipe_mgr *upipe_uref_mux_mgr = upipe_uref_mux_mgr_alloc();
    assert(upipe_uref_mux_mgr != NULL);
    struct upipe *mux = upipe_void_alloc(upipe_uref_mux_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "mux"));
    assert(mux != NULL);
    ubase_assert(upipe_set_output(mux, sink));

    struct uref *uref;
    uint64_t id;
    const char *def;
    struct ubuf *ubuf;
    size_t size;

    /*
     * Flow definition aggregation (control path).
     */

    /* a main flow def without a PID must be rejected */
    uref = uref_block_flow_alloc_def(uref_mgr, "mpegts.");
    assert(uref != NULL);
    ubase_nassert(upipe_set_flow_def(mux, uref));
    uref_free(uref);
    assert(new_flow_def_count == 0);

    /* a main flow def with a PID is accepted and tags entry 0 */
    uref = flow_def_alloc(uref_mgr, "mpegts.", MAIN_PID);
    ubase_assert(upipe_set_flow_def(mux, uref));
    uref_free(uref);
    assert(new_flow_def_count == 1);
    assert(last_flow_def != NULL);
    ubase_assert(uref_sub_get_flow_id(last_flow_def, &id, 0));
    assert(id == MAIN_PID);
    ubase_nassert(uref_sub_get_flow_id(last_flow_def, &id, 1));

    /* input subpipe A */
    struct upipe *sub_a = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "A"));
    assert(sub_a != NULL);

    /* a subpipe flow def without a PID must be rejected */
    uref = uref_block_flow_alloc_def(uref_mgr, "a.");
    assert(uref != NULL);
    ubase_nassert(upipe_set_flow_def(sub_a, uref));
    uref_free(uref);
    assert(new_flow_def_count == 1);

    /* a proper flow def adds A as entry 1 of the output flow def */
    uref = flow_def_alloc(uref_mgr, "a.", PID_A);
    ubase_assert(upipe_set_flow_def(sub_a, uref));
    uref_free(uref);
    assert(new_flow_def_count == 2);
    ubase_assert(uref_sub_get_flow_id(last_flow_def, &id, 1));
    assert(id == PID_A);
    ubase_assert(uref_sub_get_def(last_flow_def, &def, 1));
    assert(!strcmp(def, "block.a."));

    /* input subpipe B, added as entry 2 */
    struct upipe *sub_b = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "B"));
    assert(sub_b != NULL);
    uref = flow_def_alloc(uref_mgr, "b.", PID_B);
    ubase_assert(upipe_set_flow_def(sub_b, uref));
    uref_free(uref);
    assert(new_flow_def_count == 3);
    ubase_assert(uref_sub_get_flow_id(last_flow_def, &id, 2));
    assert(id == PID_B);
    ubase_assert(uref_sub_get_def(last_flow_def, &def, 2));
    assert(!strcmp(def, "block.b."));

    /*
     * Multiplexing (data path).
     */

    /* both inputs contribute: output carries entry 0 plus both PIDs */
    upipe_input(sub_a, block_alloc(uref_mgr, ubuf_mgr, SIZE_A), NULL);
    upipe_input(sub_b, block_alloc(uref_mgr, ubuf_mgr, SIZE_B), NULL);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);

    assert(sink_uref != NULL);
    assert(uref_sub_count(sink_uref) == 3);

    ubuf = uref_sub_get(sink_uref, 0);
    assert(ubuf != NULL);
    ubase_assert(ubuf_block_size(ubuf, &size));
    assert(size == MAIN_SIZE);
    ubase_assert(uref_sub_get_flow_id(sink_uref, &id, 0));
    assert(id == MAIN_PID);

    ubuf = uref_sub_find_flow_id(sink_uref, PID_A, NULL);
    assert(ubuf != NULL);
    ubase_assert(ubuf_block_size(ubuf, &size));
    assert(size == SIZE_A);

    ubuf = uref_sub_find_flow_id(sink_uref, PID_B, NULL);
    assert(ubuf != NULL);
    ubase_assert(ubuf_block_size(ubuf, &size));
    assert(size == SIZE_B);

    uref_free(sink_uref);
    sink_uref = NULL;

    /* consume-once: nothing held, only the reference is output */
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(sink_uref != NULL);
    assert(uref_sub_count(sink_uref) == 1);
    assert(uref_sub_find_flow_id(sink_uref, PID_A, NULL) == NULL);
    assert(uref_sub_find_flow_id(sink_uref, PID_B, NULL) == NULL);
    uref_free(sink_uref);
    sink_uref = NULL;

    /* single hold: a second uref arriving before consumption is dropped,
     * the first (already held) one is kept */
    upipe_input(sub_a, block_alloc(uref_mgr, ubuf_mgr, SIZE_A), NULL);
    upipe_input(sub_a, block_alloc(uref_mgr, ubuf_mgr, SIZE_A2), NULL);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(uref_sub_count(sink_uref) == 2);
    ubuf = uref_sub_find_flow_id(sink_uref, PID_A, NULL);
    assert(ubuf != NULL);
    ubase_assert(ubuf_block_size(ubuf, &size));
    assert(size == SIZE_A);
    uref_free(sink_uref);
    sink_uref = NULL;

    /* data received before a flow def is dropped, not held */
    struct upipe *sub_d = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "D"));
    assert(sub_d != NULL);
    upipe_input(sub_d, block_alloc(uref_mgr, ubuf_mgr, SIZE_D), NULL);
    uref = flow_def_alloc(uref_mgr, "d.", PID_D);
    ubase_assert(upipe_set_flow_def(sub_d, uref));
    uref_free(uref);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(uref_sub_count(sink_uref) == 1);
    assert(uref_sub_find_flow_id(sink_uref, PID_D, NULL) == NULL);
    uref_free(sink_uref);
    sink_uref = NULL;
    upipe_release(sub_d);

    /* removing a contributing input re-emits the output flow def without it */
    unsigned before = new_flow_def_count;
    upipe_release(sub_b);
    assert(new_flow_def_count == before + 1);
    ubase_assert(uref_sub_get_flow_id(last_flow_def, &id, 1));
    assert(id == PID_A);
    ubase_nassert(uref_sub_get_flow_id(last_flow_def, &id, 2));

    upipe_release(sub_a);
    upipe_release(mux);
    upipe_mgr_release(upipe_uref_mux_mgr); // nop

    uref_free(sink_uref);
    test_free(sink);

    ubuf_mgr_release(ubuf_mgr);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);

    uprobe_release(logger);
    uprobe_clean(&uprobe);
    return 0;
}
