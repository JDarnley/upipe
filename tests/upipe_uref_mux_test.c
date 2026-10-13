/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: James Darnley
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short unit tests for uref_mux pipes
 *
 * The output of the mux under test is checked directly, with the uref_sub
 * accessors: flow def aggregation, merging of the held urefs, the limits on
 * held urefs, the urefs dropped by a new flow def or a release, flow ID
 * uniqueness, including at both ends of their range, and the order in which
 * inputs and the main pipe are released.
 */

#undef NDEBUG

#include "upipe/uprobe.h"
#include "upipe/ulog.h"
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
#include "upipe-modules/upipe_uref_mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define UDICT_POOL_DEPTH    0
#define UREF_POOL_DEPTH     0
#define UBUF_POOL_DEPTH     0
#define UPROBE_LOG_LEVEL    UPROBE_LOG_VERBOSE

/* flow IDs used as per-input keys */
#define MAIN_ID    100
#define ID_A       200
#define ID_B       300
#define ID_D       400
#define ID_E       500
#define ID_F       600
#define ID_G       700

/* distinct block sizes, used to tell which buffer landed at which entry */
#define MAIN_SIZE   188
#define SIZE_A      64
#define SIZE_A2     65
#define SIZE_B      128
#define SIZE_D      96
#define SIZE_E      32

/** last output flow def seen through UPROBE_NEW_FLOW_DEF (not owned) */
static struct uref *last_flow_def = NULL;
/** number of output flow defs emitted */
static unsigned new_flow_def_count = 0;
/** last data uref received by the sink (owned) */
static struct uref *sink_uref = NULL;
/** mux under test */
static struct upipe *mux_pipe = NULL;
/** true once the mux under test has thrown its dead event */
static bool mux_dead = false;
/** number of output flow defs emitted when the mux died */
static unsigned mux_dead_flow_def_count = 0;

/** number of warnings logged by the inputs E and F */
static unsigned warnings_e = 0;
/** probe counting the warnings of the inputs E and F */
static struct uprobe warnings_probe;

/** counts warnings */
static int catch_warnings(struct uprobe *uprobe, struct upipe *upipe,
                          int event, va_list args)
{
    if (event == UPROBE_LOG) {
        va_list args_copy;
        va_copy(args_copy, args);
        struct ulog *ulog = va_arg(args_copy, struct ulog *);
        if (ulog->level == UPROBE_LOG_WARNING)
            warnings_e++;
        va_end(args_copy);
    }
    return uprobe_throw_next(uprobe, upipe, event, args);
}

/** probe of the mux and its inputs: records the output flow defs and the
 * death of the mux, and fails on any unexpected event */
static int catch(struct uprobe *uprobe, struct upipe *upipe,
                 int event, va_list args)
{
    /* nothing may come from the mux after its dead event */
    assert(upipe == NULL || upipe != mux_pipe || !mux_dead);

    switch (event) {
        case UPROBE_READY:
            break;
        case UPROBE_DEAD:
            if (upipe != NULL && upipe == mux_pipe) {
                mux_dead = true;
                mux_dead_flow_def_count = new_flow_def_count;
            }
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

/** @This allocates a flow def carrying a flow ID. */
static struct uref *flow_def_alloc(struct uref_mgr *uref_mgr,
                                   const char *def, uint64_t flow_id)
{
    struct uref *flow_def = uref_block_flow_alloc_def(uref_mgr, def);
    assert(flow_def != NULL);
    ubase_assert(uref_flow_set_id(flow_def, flow_id));
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
    mux_pipe = mux;
    ubase_assert(upipe_set_output(mux, sink));

    struct uref *uref;
    uint64_t id;
    const char *def;
    struct ubuf *ubuf;
    size_t size;
    uint8_t index;

    /*
     * Flow definition aggregation (control path).
     */

    /* a main flow def without a flow ID must be rejected */
    uref = uref_block_flow_alloc_def(uref_mgr, "main.");
    assert(uref != NULL);
    ubase_nassert(upipe_set_flow_def(mux, uref));
    uref_free(uref);
    assert(new_flow_def_count == 0);

    /* a main flow def with a flow ID is accepted and tags entry 0 */
    uref = flow_def_alloc(uref_mgr, "main.", MAIN_ID);
    ubase_assert(upipe_set_flow_def(mux, uref));
    uref_free(uref);
    assert(new_flow_def_count == 1);
    assert(last_flow_def != NULL);
    ubase_assert(uref_sub_get_flow_id(last_flow_def, &id, 0));
    assert(id == MAIN_ID);
    ubase_nassert(uref_sub_get_flow_id(last_flow_def, &id, 1));

    /* input subpipe A */
    struct upipe *sub_a = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "A"));
    assert(sub_a != NULL);

    /* a subpipe flow def without a flow ID must be rejected */
    uref = uref_block_flow_alloc_def(uref_mgr, "a.");
    assert(uref != NULL);
    ubase_nassert(upipe_set_flow_def(sub_a, uref));
    uref_free(uref);
    assert(new_flow_def_count == 1);

    /* a proper flow def adds A as entry 1 of the output flow def */
    uref = flow_def_alloc(uref_mgr, "a.", ID_A);
    ubase_assert(upipe_set_flow_def(sub_a, uref));
    uref_free(uref);
    assert(new_flow_def_count == 2);
    ubase_assert(uref_sub_get_flow_id(last_flow_def, &id, 1));
    assert(id == ID_A);
    ubase_assert(uref_sub_get_def(last_flow_def, &def, 1));
    assert(!strcmp(def, "block.a."));

    /* the whole flow def of A is carried and can be extracted back */
    uref = uref_sub_extract(last_flow_def, 1);
    assert(uref != NULL);
    ubase_assert(uref_flow_get_def(uref, &def));
    assert(!strcmp(def, "block.a."));
    ubase_assert(uref_flow_get_id(uref, &id));
    assert(id == ID_A);
    uref_free(uref);

    /* input subpipe B, added as entry 2 */
    struct upipe *sub_b = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "B"));
    assert(sub_b != NULL);
    uref = flow_def_alloc(uref_mgr, "b.", ID_B);
    ubase_assert(upipe_set_flow_def(sub_b, uref));
    uref_free(uref);
    assert(new_flow_def_count == 3);
    ubase_assert(uref_sub_get_flow_id(last_flow_def, &id, 2));
    assert(id == ID_B);
    ubase_assert(uref_sub_get_def(last_flow_def, &def, 2));
    assert(!strcmp(def, "block.b."));

    /*
     * Multiplexing (data path).
     */

    /* both inputs contribute: output carries entry 0 plus both flow IDs */
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
    assert(id == MAIN_ID);

    ubase_assert(uref_sub_find_flow_id(sink_uref, ID_A, &index));
    ubuf = uref_sub_get(sink_uref, index);
    assert(ubuf != NULL);
    ubase_assert(ubuf_block_size(ubuf, &size));
    assert(size == SIZE_A);

    ubase_assert(uref_sub_find_flow_id(sink_uref, ID_B, &index));
    ubuf = uref_sub_get(sink_uref, index);
    assert(ubuf != NULL);
    ubase_assert(ubuf_block_size(ubuf, &size));
    assert(size == SIZE_B);

    uref_free(sink_uref);
    sink_uref = NULL;

    /* consume-once: nothing held, only the reference is output */
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(sink_uref != NULL);
    assert(uref_sub_count(sink_uref) == 1);
    ubase_nassert(uref_sub_find_flow_id(sink_uref, ID_A, NULL));
    ubase_nassert(uref_sub_find_flow_id(sink_uref, ID_B, NULL));
    uref_free(sink_uref);
    sink_uref = NULL;

    /* every uref an input receives before the next main uref is carried,
     * in arrival order, as entries with the same flow ID */
    upipe_input(sub_a, block_alloc(uref_mgr, ubuf_mgr, SIZE_A), NULL);
    upipe_input(sub_a, block_alloc(uref_mgr, ubuf_mgr, SIZE_A2), NULL);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(uref_sub_count(sink_uref) == 3);
    ubase_assert(uref_sub_get_flow_id(sink_uref, &id, 1));
    assert(id == ID_A);
    ubase_assert(uref_sub_get_flow_id(sink_uref, &id, 2));
    assert(id == ID_A);
    ubuf = uref_sub_get(sink_uref, 1);
    assert(ubuf != NULL);
    ubase_assert(ubuf_block_size(ubuf, &size));
    assert(size == SIZE_A);
    ubuf = uref_sub_get(sink_uref, 2);
    assert(ubuf != NULL);
    ubase_assert(ubuf_block_size(ubuf, &size));
    assert(size == SIZE_A2);
    uref_free(sink_uref);
    sink_uref = NULL;

    /* an input holds at most UREF_SUB_MAX_ENTRIES - 1 urefs, as many as a
     * uref can carry, and drops the following ones with a warning */
    uprobe_init(&warnings_probe, catch_warnings, uprobe_use(logger));
    struct upipe *sub_e = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(&warnings_probe), UPROBE_LOG_LEVEL,
                             "E"));
    assert(sub_e != NULL);
    uref = flow_def_alloc(uref_mgr, "e.", ID_E);
    ubase_assert(upipe_set_flow_def(sub_e, uref));
    uref_free(uref);
    for (unsigned i = 0; i < UREF_SUB_MAX_ENTRIES + 10; i++)
        upipe_input(sub_e, block_alloc(uref_mgr, ubuf_mgr, SIZE_E), NULL);
    assert(warnings_e == 11);

    /* E's urefs fill the uref, so those of the inputs after it, here F,
     * don't fit and are dropped at the merge with a warning too */
    struct upipe *sub_f = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(&warnings_probe), UPROBE_LOG_LEVEL,
                             "F"));
    assert(sub_f != NULL);
    uref = flow_def_alloc(uref_mgr, "f.", ID_F);
    ubase_assert(upipe_set_flow_def(sub_f, uref));
    uref_free(uref);
    for (unsigned i = 0; i < 5; i++)
        upipe_input(sub_f, block_alloc(uref_mgr, ubuf_mgr, SIZE_E), NULL);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(uref_sub_count(sink_uref) == UREF_SUB_MAX_ENTRIES);
    ubase_nassert(uref_sub_find_flow_id(sink_uref, ID_F, NULL));
    assert(warnings_e == 16);
    uref_free(sink_uref);
    sink_uref = NULL;

    /* both inputs start again from an empty queue */
    upipe_input(sub_f, block_alloc(uref_mgr, ubuf_mgr, SIZE_E), NULL);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(uref_sub_count(sink_uref) == 2);
    ubase_assert(uref_sub_find_flow_id(sink_uref, ID_F, NULL));
    uref_free(sink_uref);
    sink_uref = NULL;
    for (unsigned i = 0; i < UREF_SUB_MAX_ENTRIES - 1; i++)
        upipe_input(sub_e, block_alloc(uref_mgr, ubuf_mgr, SIZE_E), NULL);
    assert(warnings_e == 16);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(uref_sub_count(sink_uref) == UREF_SUB_MAX_ENTRIES);
    uref_free(sink_uref);
    sink_uref = NULL;
    upipe_release(sub_f);

    /* a new flow def drops the urefs held under the previous one */
    upipe_input(sub_e, block_alloc(uref_mgr, ubuf_mgr, SIZE_E), NULL);
    upipe_input(sub_e, block_alloc(uref_mgr, ubuf_mgr, SIZE_E), NULL);
    uref = flow_def_alloc(uref_mgr, "e2.", ID_E);
    ubase_assert(upipe_set_flow_def(sub_e, uref));
    uref_free(uref);
    assert(warnings_e == 18);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(uref_sub_count(sink_uref) == 1);
    ubase_nassert(uref_sub_find_flow_id(sink_uref, ID_E, NULL));
    uref_free(sink_uref);
    sink_uref = NULL;

    /* releasing an input drops the urefs it still holds, with a single
     * warning */
    for (unsigned i = 0; i < 3; i++)
        upipe_input(sub_e, block_alloc(uref_mgr, ubuf_mgr, SIZE_E), NULL);
    upipe_release(sub_e);
    assert(warnings_e == 19);

    /* data received before a flow def is dropped, not held */
    struct upipe *sub_d = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "D"));
    assert(sub_d != NULL);
    upipe_input(sub_d, block_alloc(uref_mgr, ubuf_mgr, SIZE_D), NULL);
    uref = flow_def_alloc(uref_mgr, "d.", ID_D);
    ubase_assert(upipe_set_flow_def(sub_d, uref));
    uref_free(uref);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(uref_sub_count(sink_uref) == 1);
    ubase_nassert(uref_sub_find_flow_id(sink_uref, ID_D, NULL));
    uref_free(sink_uref);
    sink_uref = NULL;
    upipe_release(sub_d);

    /* every flow has its own flow ID: an input flow def with the flow ID of
     * another input or of the main input is rejected, and so is a main flow
     * def with the flow ID of an input */
    struct upipe *sub_g = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "G"));
    assert(sub_g != NULL);
    uref = flow_def_alloc(uref_mgr, "g.", ID_A);
    ubase_nassert(upipe_set_flow_def(sub_g, uref));
    uref_free(uref);
    uref = flow_def_alloc(uref_mgr, "g.", MAIN_ID);
    ubase_nassert(upipe_set_flow_def(sub_g, uref));
    uref_free(uref);
    uref = flow_def_alloc(uref_mgr, "g.", ID_G);
    ubase_assert(upipe_set_flow_def(sub_g, uref));
    uref_free(uref);
    /* an input may set a new flow def with its own flow ID */
    uref = flow_def_alloc(uref_mgr, "g2.", ID_G);
    ubase_assert(upipe_set_flow_def(sub_g, uref));
    uref_free(uref);
    uref = flow_def_alloc(uref_mgr, "main.", ID_G);
    ubase_nassert(upipe_set_flow_def(mux, uref));
    uref_free(uref);
    /* and so may the main input */
    uref = flow_def_alloc(uref_mgr, "main.", MAIN_ID);
    ubase_assert(upipe_set_flow_def(mux, uref));
    uref_free(uref);
    /* the flow ID of a released input may be used again */
    upipe_release(sub_g);
    sub_g = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "G2"));
    assert(sub_g != NULL);
    uref = flow_def_alloc(uref_mgr, "g.", ID_G);
    ubase_assert(upipe_set_flow_def(sub_g, uref));
    uref_free(uref);
    upipe_release(sub_g);

    /* 0 and UINT64_MAX are flow IDs like any other: they are unique, and the
     * urefs of their inputs are tagged with them */
    struct upipe *sub_zero = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "zero"));
    struct upipe *sub_max = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "max"));
    sub_g = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "G3"));
    assert(sub_zero != NULL && sub_max != NULL && sub_g != NULL);
    uref = flow_def_alloc(uref_mgr, "zero.", 0);
    ubase_assert(upipe_set_flow_def(sub_zero, uref));
    ubase_nassert(upipe_set_flow_def(sub_g, uref));
    uref_free(uref);
    uref = flow_def_alloc(uref_mgr, "max.", UINT64_MAX);
    ubase_assert(upipe_set_flow_def(sub_max, uref));
    ubase_nassert(upipe_set_flow_def(sub_g, uref));
    uref_free(uref);
    upipe_release(sub_g);
    upipe_input(sub_zero, block_alloc(uref_mgr, ubuf_mgr, SIZE_A), NULL);
    upipe_input(sub_max, block_alloc(uref_mgr, ubuf_mgr, SIZE_B), NULL);
    upipe_input(mux, block_alloc(uref_mgr, ubuf_mgr, MAIN_SIZE), NULL);
    assert(uref_sub_count(sink_uref) == 3);
    ubase_assert(uref_sub_find_flow_id(sink_uref, 0, &index));
    ubase_assert(ubuf_block_size(uref_sub_get(sink_uref, index), &size));
    assert(size == SIZE_A);
    ubase_assert(uref_sub_find_flow_id(sink_uref, UINT64_MAX, &index));
    ubase_assert(ubuf_block_size(uref_sub_get(sink_uref, index), &size));
    assert(size == SIZE_B);
    uref_free(sink_uref);
    sink_uref = NULL;
    upipe_release(sub_zero);
    upipe_release(sub_max);

    /* removing a contributing input re-emits the output flow def without it */
    unsigned before = new_flow_def_count;
    upipe_release(sub_b);
    assert(new_flow_def_count == before + 1);
    ubase_assert(uref_sub_get_flow_id(last_flow_def, &id, 1));
    assert(id == ID_A);
    ubase_nassert(uref_sub_get_flow_id(last_flow_def, &id, 2));

    /* releasing the main pipe before a contributing input: the input keeps
     * the main pipe alive, and its removal must still re-emit the output
     * flow def before the main pipe dies */
    upipe_release(mux);
    assert(!mux_dead);
    before = new_flow_def_count;
    upipe_release(sub_a);
    assert(mux_dead);
    assert(mux_dead_flow_def_count == before + 1);
    mux_pipe = NULL;
    upipe_mgr_release(upipe_uref_mux_mgr); // nop

    uref_free(sink_uref);
    test_free(sink);

    ubuf_mgr_release(ubuf_mgr);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);

    uprobe_clean(&warnings_probe);
    uprobe_release(logger);
    uprobe_clean(&uprobe);
    return 0;
}
