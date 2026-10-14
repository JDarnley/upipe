/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: James Darnley
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short end-to-end tests for uref_mux and uref_demux pipes
 *
 * A uref_mux pipe feeds a uref_demux pipe, whose split probe allocates an
 * output subpipe for every announced flow. Every uref and flow def given to
 * the mux must come out of the demux with the same data, attributes, dates,
 * delays and flags: the main flow on the main output, and each input on the
 * subpipe of its flow ID, with the same flow def. The mux tags the urefs of
 * an input with its flow ID, which the demux removes, so the input urefs used
 * here have no flow ID of their own. Urefs the mux drops (beyond the limit,
 * or held when a flow def changes) must not come out at all.
 */

#undef NDEBUG

#include "upipe/uprobe.h"
#include "upipe/uprobe_stdio.h"
#include "upipe/uprobe_prefix.h"
#include "upipe/umem.h"
#include "upipe/umem_alloc.h"
#include "upipe/udict.h"
#include "upipe/udict_inline.h"
#include "upipe/ubuf.h"
#include "upipe/ubuf_block.h"
#include "upipe/ubuf_block_mem.h"
#include "upipe/uref.h"
#include "upipe/uref_std.h"
#include "upipe/uref_attr.h"
#include "upipe/uref_flow.h"
#include "upipe/uref_block.h"
#include "upipe/uref_block_flow.h"
#include "upipe/uref_clock.h"
#include "upipe/uref_sub.h"
#include "upipe/upipe.h"
#include "upipe-modules/upipe_uref_mux.h"
#include "upipe-modules/upipe_uref_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#define UDICT_POOL_DEPTH    0
#define UREF_POOL_DEPTH     0
#define UBUF_POOL_DEPTH     0
#define UPROBE_LOG_LEVEL    UPROBE_LOG_VERBOSE

#define ID_MAIN    256
#define ID_A       200
#define ID_B       300
#define ID_C       400

/* flags of the input urefs */
#define F_RANDOM    0x1
#define F_DISC      0x2
#define F_START     0x4
#define F_END       0x8
#define F_REF       0x10

#define MAX_SUBS    16

static struct uref_mgr *uref_mgr;
static struct ubuf_mgr *ubuf_mgr;
static struct uprobe *logger;

/*
 * Test sink
 */

/** test sink keeping the last flow def and uref it received */
struct sink {
    /** refcount management structure */
    struct urefcount urefcount;
    /** last flow def */
    struct uref *flow_def;
    /** last uref */
    struct uref *uref;
    /** uref received before the last one */
    struct uref *prev;
    /** number of urefs received */
    unsigned count;
    /** public upipe structure */
    struct upipe upipe;
};

static void sink_free(struct urefcount *urefcount)
{
    struct sink *sink = container_of(urefcount, struct sink, urefcount);
    uref_free(sink->flow_def);
    uref_free(sink->uref);
    uref_free(sink->prev);
    upipe_clean(&sink->upipe);
    free(sink);
}

static struct upipe *sink_alloc(struct upipe_mgr *mgr, struct uprobe *uprobe,
                                uint32_t signature, va_list args)
{
    struct sink *sink = calloc(1, sizeof(struct sink));
    assert(sink != NULL);
    upipe_init(&sink->upipe, mgr, uprobe);
    urefcount_init(&sink->urefcount, sink_free);
    sink->upipe.refcount = &sink->urefcount;
    return &sink->upipe;
}

static void sink_input(struct upipe *upipe, struct uref *uref,
                       struct upump **upump_p)
{
    struct sink *sink = container_of(upipe, struct sink, upipe);
    uref_free(sink->prev);
    sink->prev = sink->uref;
    sink->uref = uref;
    sink->count++;
}

static int sink_control(struct upipe *upipe, int command, va_list args)
{
    struct sink *sink = container_of(upipe, struct sink, upipe);
    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            uref_free(sink->flow_def);
            sink->flow_def = uref_dup(flow_def);
            assert(sink->flow_def != NULL);
            return UBASE_ERR_NONE;
        }
        case UPIPE_REGISTER_REQUEST:
        case UPIPE_UNREGISTER_REQUEST:
            return UBASE_ERR_NONE;
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

static struct upipe_mgr sink_mgr = {
    .refcount = NULL,
    .signature = 0,
    .upipe_alloc = sink_alloc,
    .upipe_input = sink_input,
    .upipe_control = sink_control,
    .upipe_mgr_control = NULL
};

/*
 * Events
 */

/** demux under test */
static struct upipe *demux_pipe = NULL;
/** number of split update events thrown by the demux */
static unsigned split_updates = 0;
/** true once the demux is dead */
static bool demux_dead = false;

/** state of an output subpipe of the demux */
struct sub_state {
    /** subpipe, or NULL once dead */
    struct upipe *upipe;
    /** sink attached when the subpipe needed an output */
    struct sink *sink;
    /** flow ID */
    uint64_t flow_id;
    /** numbers of events */
    unsigned new_flow_defs, source_ends, dead;
};

static struct sub_state subs[MAX_SUBS];
static unsigned nb_subs = 0;

static struct uprobe sub_probe;

/** returns the state of the subpipe currently outputting a flow ID */
static struct sub_state *live_sub(uint64_t flow_id)
{
    struct sub_state *found = NULL;
    for (unsigned i = 0; i < nb_subs; i++)
        if (subs[i].upipe != NULL && subs[i].flow_id == flow_id &&
            !subs[i].source_ends) {
            assert(found == NULL);
            found = &subs[i];
        }
    return found;
}

/** returns the state of a subpipe */
static struct sub_state *sub_state(struct upipe *upipe)
{
    for (unsigned i = 0; i < nb_subs; i++)
        if (subs[i].upipe == upipe)
            return &subs[i];
    return NULL;
}

/** last probe of the chains: no errors are expected */
static int catch_base(struct uprobe *uprobe, struct upipe *upipe,
                      int event, va_list args)
{
    assert(event != UPROBE_FATAL && event != UPROBE_ERROR);
    return UBASE_ERR_NONE;
}

/** probe of the demux, allocating a subpipe for every announced flow */
static int catch_demux(struct uprobe *uprobe, struct upipe *upipe,
                       int event, va_list args)
{
    if (upipe == NULL || upipe != demux_pipe)
        return uprobe_throw_next(uprobe, upipe, event, args);

    switch (event) {
        case UPROBE_SPLIT_UPDATE: {
            split_updates++;
            struct uref *flow_def = NULL;
            while (ubase_check(upipe_split_iterate(upipe, &flow_def)) &&
                   flow_def != NULL) {
                uint64_t flow_id;
                ubase_assert(uref_flow_get_id(flow_def, &flow_id));
                if (live_sub(flow_id) != NULL)
                    continue;

                assert(nb_subs < MAX_SUBS);
                struct sub_state *state = &subs[nb_subs++];
                memset(state, 0, sizeof(*state));
                state->flow_id = flow_id;
                state->upipe = upipe_flow_alloc_sub(upipe,
                        uprobe_pfx_alloc_va(uprobe_use(&sub_probe),
                                            UPROBE_LOG_LEVEL,
                                            "flow %" PRIu64, flow_id),
                        flow_def);
                assert(state->upipe != NULL);
            }
            return UBASE_ERR_NONE;
        }
        case UPROBE_DEAD:
            demux_dead = true;
            return UBASE_ERR_NONE;
        default:
            return uprobe_throw_next(uprobe, upipe, event, args);
    }
}

/** probe of the output subpipes, which the demux probe owns */
static int catch_sub(struct uprobe *uprobe, struct upipe *upipe,
                     int event, va_list args)
{
    struct sub_state *state = sub_state(upipe);
    switch (event) {
        case UPROBE_NEW_FLOW_DEF:
            /* thrown during the allocation, before the state knows the
             * subpipe */
            if (state != NULL)
                state->new_flow_defs++;
            return UBASE_ERR_NONE;
        case UPROBE_NEED_OUTPUT: {
            assert(state != NULL);
            struct upipe *sink = upipe_void_alloc(&sink_mgr,
                                                  uprobe_use(logger));
            assert(sink != NULL);
            ubase_assert(upipe_set_output(upipe, sink));
            state->sink = container_of(sink, struct sink, upipe);
            upipe_release(sink);
            return UBASE_ERR_NONE;
        }
        case UPROBE_SOURCE_END:
            assert(state != NULL);
            state->source_ends++;
            upipe_release(upipe);
            return UBASE_ERR_NONE;
        case UPROBE_DEAD:
            assert(state != NULL);
            state->dead++;
            state->upipe = NULL;
            state->sink = NULL;
            return UBASE_ERR_NONE;
        default:
            return uprobe_throw_next(uprobe, upipe, event, args);
    }
}

/*
 * Inputs
 */

/** allocates a uref with a block filled with a byte, and dates, delays,
 * flags and attributes of its own derived from a base value */
static struct uref *input_uref(char c, int size, uint64_t base,
                               unsigned flags)
{
    struct uref *uref = uref_block_alloc(uref_mgr, ubuf_mgr, size);
    assert(uref != NULL);
    uint8_t *buffer;
    int wsize = -1;
    ubase_assert(uref_block_write(uref, 0, &wsize, &buffer));
    assert(wsize == size);
    memset(buffer, c, size);
    ubase_assert(uref_block_unmap(uref, 0));

    uref_clock_set_cr_sys(uref, base + 1);
    uref_clock_set_pts_prog(uref, base + 2);
    uref_clock_set_dts_orig(uref, base + 3);
    uref_clock_set_dts_pts_delay(uref, base + 4);
    uref_clock_set_cr_dts_delay(uref, base + 5);
    uref_clock_set_rap_cr_delay(uref, base + 6);
    ubase_assert(uref_clock_set_duration(uref, base + 7));
    ubase_assert(uref_attr_set_unsigned(uref, base, UDICT_TYPE_UNSIGNED,
                                        "x.base"));
    ubase_assert(uref_attr_set_string(uref, "input", UDICT_TYPE_STRING,
                                      "x.name"));
    if (flags & F_RANDOM)
        uref_flow_set_random(uref);
    if (flags & F_DISC)
        uref_flow_set_discontinuity(uref);
    if (flags & F_START)
        uref_block_set_start(uref);
    if (flags & F_END)
        uref_block_set_end(uref);
    if (flags & F_REF)
        uref_clock_set_ref(uref);
    return uref;
}

/** returns a copy of a uref without its data, so that its ubuf isn't
 * shared */
static struct uref *snapshot(struct uref *uref)
{
    struct uref *copy = uref_dup(uref);
    assert(copy != NULL);
    ubuf_free(uref_detach_ubuf(copy));
    return copy;
}

/** allocates a block flow def with a flow ID, an optional name and a
 * language */
static struct uref *input_flow_def(const char *def, uint64_t flow_id,
                                   const char *name)
{
    struct uref *flow_def = uref_block_flow_alloc_def(uref_mgr, def);
    assert(flow_def != NULL);
    ubase_assert(uref_flow_set_id(flow_def, flow_id));
    if (name != NULL)
        ubase_assert(uref_flow_set_name(flow_def, name));
    ubase_assert(uref_flow_set_language(flow_def, "eng", 0));
    return flow_def;
}

/** sets the flow def of an input, and returns it, as the matching output
 * should get the same */
static struct uref *set_input_flow_def(struct upipe *input,
                                       struct uref *flow_def)
{
    ubase_assert(upipe_set_flow_def(input, flow_def));
    return flow_def;
}

/*
 * Checks
 */

/** checks that a uref has the data, attributes, dates, delays and flags of
 * an input uref, and that its ubuf isn't shared */
static void check_uref(struct uref *uref, struct uref *expected, char c,
                       int size)
{
    assert(uref != NULL);
    assert(ulist_empty(&uref->sub_ubufs));
    size_t total;
    ubase_assert(uref_block_size(uref, &total));
    assert(total == (size_t)size);
    uint8_t *buffer;
    int wsize = -1;
    ubase_assert(uref_block_write(uref, 0, &wsize, &buffer));
    assert(wsize == size);
    for (int i = 0; i < size; i++)
        assert(buffer[i] == (uint8_t)c);
    ubase_assert(uref_block_unmap(uref, 0));

    assert(uref->flags == expected->flags);
    assert(uref->date_sys == expected->date_sys);
    assert(uref->date_prog == expected->date_prog);
    assert(uref->date_orig == expected->date_orig);
    assert(uref->dts_pts_delay == expected->dts_pts_delay);
    assert(uref->cr_dts_delay == expected->cr_dts_delay);
    assert(uref->rap_cr_delay == expected->rap_cr_delay);
    assert(!udict_cmp(uref->udict, expected->udict));
}

/** checks the last uref received for a flow ID and the flow def of its
 * subpipe */
static struct sub_state *check_sub(uint64_t flow_id, unsigned count,
                                   struct uref *expected, char c, int size,
                                   struct uref *flow_def)
{
    struct sub_state *state = live_sub(flow_id);
    assert(state != NULL && state->sink != NULL);
    assert(state->sink->count == count);
    check_uref(state->sink->uref, expected, c, size);
    assert(state->sink->flow_def != NULL);
    assert(!udict_cmp(state->sink->flow_def->udict, flow_def->udict));
    return state;
}

/** sends an input uref, returning its snapshot */
static struct uref *send(struct upipe *input, struct uref *uref)
{
    struct uref *expected = snapshot(uref);
    upipe_input(input, uref, NULL);
    return expected;
}

int main(int argc, char **argv)
{
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr, 0);
    assert(uref_mgr != NULL);
    ubuf_mgr = ubuf_block_mem_mgr_alloc(UBUF_POOL_DEPTH, UBUF_POOL_DEPTH,
                                        umem_mgr, 0, 0, -1, 0);
    assert(ubuf_mgr != NULL);

    struct uprobe uprobe;
    uprobe_init(&uprobe, catch_base, NULL);
    logger = uprobe_stdio_alloc(&uprobe, stdout, UPROBE_LOG_LEVEL);
    assert(logger != NULL);
    struct uprobe demux_probe;
    uprobe_init(&demux_probe, catch_demux, uprobe_use(logger));
    uprobe_init(&sub_probe, catch_sub, uprobe_use(logger));

    struct upipe *sink = upipe_void_alloc(&sink_mgr, uprobe_use(logger));
    assert(sink != NULL);
    struct sink *main_sink = container_of(sink, struct sink, upipe);

    /* mux -> demux -> main sink; the mux holds the demux */
    struct upipe_mgr *upipe_uref_mux_mgr = upipe_uref_mux_mgr_alloc();
    struct upipe_mgr *upipe_uref_demux_mgr = upipe_uref_demux_mgr_alloc();
    assert(upipe_uref_mux_mgr != NULL && upipe_uref_demux_mgr != NULL);
    struct upipe *mux = upipe_void_alloc(upipe_uref_mux_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "mux"));
    assert(mux != NULL);
    struct upipe *demux = upipe_void_alloc_output(mux, upipe_uref_demux_mgr,
            uprobe_pfx_alloc(uprobe_use(&demux_probe), UPROBE_LOG_LEVEL,
                             "demux"));
    assert(demux != NULL);
    demux_pipe = demux;
    ubase_assert(upipe_set_output(demux, sink));
    upipe_release(demux);

    /* the main flow and two inputs */
    struct uref *main_flow_def = set_input_flow_def(mux,
            input_flow_def("main.", ID_MAIN, "main"));
    struct upipe *in_a = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "in A"));
    assert(in_a != NULL);
    struct uref *a_flow_def = set_input_flow_def(in_a,
            input_flow_def("s302m.sound.", ID_A, "audio"));
    struct upipe *in_b = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "in B"));
    assert(in_b != NULL);
    struct uref *b_flow_def = set_input_flow_def(in_b,
            input_flow_def("h264.pic.", ID_B, "video"));

    /* the flow def reaches the demux with the first carrier, so the inputs
     * have subpipes before their first data; everything comes back */
    struct uref *a1 = send(in_a, input_uref('a', 64, 1100, F_RANDOM));
    struct uref *b1 = send(in_b, input_uref('b', 32, 1200,
                                            F_RANDOM | F_DISC | F_REF));
    struct uref *m1 = send(mux, input_uref('m', 188, 1000, F_START | F_END));
    assert(split_updates == 1);
    struct sub_state *a = check_sub(ID_A, 1, a1, 'a', 64, a_flow_def);
    struct sub_state *b = check_sub(ID_B, 1, b1, 'b', 32, b_flow_def);
    assert(main_sink->count == 1);
    check_uref(main_sink->uref, m1, 'm', 188);
    assert(!udict_cmp(main_sink->flow_def->udict, main_flow_def->udict));
    uref_free(a1);
    uref_free(b1);
    uref_free(m1);

    /* entries are routed by flow ID: B is the first entry when A misses a
     * carrier */
    struct uref *b2 = send(in_b, input_uref('B', 40, 2200, F_START));
    struct uref *m2 = send(mux, input_uref('M', 188, 2000, 0));
    check_sub(ID_B, 2, b2, 'B', 40, b_flow_def);
    assert(a->sink->count == 1);
    assert(main_sink->count == 2);
    check_uref(main_sink->uref, m2, 'M', 188);
    uref_free(b2);
    uref_free(m2);

    /* every uref an input receives before the next main uref comes out of
     * its subpipe, in arrival order */
    struct uref *a3 = send(in_a, input_uref('c', 16, 3100, F_DISC));
    struct uref *a3b = send(in_a, input_uref('d', 18, 3150, 0));
    struct uref *m3 = send(mux, input_uref('n', 188, 3000, F_REF));
    check_sub(ID_A, 3, a3b, 'd', 18, a_flow_def);
    check_uref(a->sink->prev, a3, 'c', 16);
    check_uref(main_sink->uref, m3, 'n', 188);
    uref_free(a3);
    uref_free(a3b);
    uref_free(m3);

    /* an input added while running gets a subpipe before its first data */
    struct upipe *in_c = upipe_void_alloc_sub(mux,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL, "in C"));
    assert(in_c != NULL);
    struct uref *c_flow_def = set_input_flow_def(in_c,
            input_flow_def("scte35.", ID_C, NULL));
    struct uref *c4 = send(in_c, input_uref('e', 24, 4400, F_START | F_END));
    struct uref *m4 = send(mux, input_uref('o', 188, 4000, 0));
    assert(split_updates == 2);
    struct sub_state *c = check_sub(ID_C, 1, c4, 'e', 24, c_flow_def);
    assert(a->sink->count == 3 && b->sink->count == 2);
    uref_free(c4);
    uref_free(m4);

    /* a new flow def for A with the same f.def is applied in place; the
     * uref A holds under the previous one never comes out */
    upipe_input(in_a, input_uref('x', 64, 5050, F_RANDOM), NULL);
    uref_free(a_flow_def);
    a_flow_def = set_input_flow_def(in_a,
            input_flow_def("s302m.sound.", ID_A, "audio 2"));
    struct uref *a5 = send(in_a, input_uref('f', 64, 5100, F_RANDOM));
    struct uref *m5 = send(mux, input_uref('p', 188, 5000, 0));
    assert(split_updates == 3);
    assert(live_sub(ID_A) == a && a->source_ends == 0);
    assert(a->new_flow_defs == 1);
    check_sub(ID_A, 4, a5, 'f', 64, a_flow_def);
    uref_free(a5);
    uref_free(m5);

    /* a new f.def for B ends its subpipe, and a new one is allocated; the
     * uref B holds under the previous one doesn't reach it */
    upipe_input(in_b, input_uref('y', 48, 6150, F_RANDOM), NULL);
    uref_free(b_flow_def);
    b_flow_def = set_input_flow_def(in_b,
            input_flow_def("hevc.pic.", ID_B, "video"));
    struct uref *b6 = send(in_b, input_uref('g', 48, 6200, F_RANDOM));
    struct uref *m6 = send(mux, input_uref('q', 188, 6000, 0));
    assert(b->source_ends == 1 && b->dead == 1);
    assert(split_updates == 4);
    struct sub_state *b_new = check_sub(ID_B, 1, b6, 'g', 48, b_flow_def);
    assert(b_new != b);
    uref_free(b6);
    uref_free(m6);

    /* removing an input ends its subpipe when the next carrier comes */
    upipe_release(in_c);
    struct uref *m7 = send(mux, input_uref('r', 188, 7000, 0));
    assert(c->source_ends == 1 && c->dead == 1);
    assert(split_updates == 5);
    check_uref(main_sink->uref, m7, 'r', 188);
    uref_free(m7);

    /* a full carrier: A gets more urefs than a uref can carry, so only the
     * first UREF_SUB_MAX_ENTRIES - 1 come out, in order, and B's uref no
     * longer fits */
    unsigned a_count = a->sink->count, b_count = b_new->sink->count;
    struct uref *full_prev = NULL, *full_last = NULL;
    for (unsigned i = 0; i < UREF_SUB_MAX_ENTRIES + 10; i++) {
        struct uref *uref = input_uref('h', 8, 10000 + 10 * i, F_RANDOM);
        if (i == UREF_SUB_MAX_ENTRIES - 3)
            full_prev = snapshot(uref);
        else if (i == UREF_SUB_MAX_ENTRIES - 2)
            full_last = snapshot(uref);
        upipe_input(in_a, uref, NULL);
    }
    upipe_input(in_b, input_uref('i', 8, 20000, 0), NULL);
    struct uref *m8 = send(mux, input_uref('t', 188, 8000, 0));
    assert(a->sink->count == a_count + UREF_SUB_MAX_ENTRIES - 1);
    check_uref(a->sink->prev, full_prev, 'h', 8);
    check_uref(a->sink->uref, full_last, 'h', 8);
    assert(b_new->sink->count == b_count);
    check_uref(main_sink->uref, m8, 't', 188);
    uref_free(full_prev);
    uref_free(full_last);
    uref_free(m8);

    /* releasing the main pipe of the mux before its inputs: the inputs keep
     * the mux, and so the demux, alive; releasing the last one frees the
     * mux, which ends the demux subpipes, then a final split update is
     * thrown and the demux dies */
    upipe_release(mux);
    upipe_release(in_a);
    assert(a->source_ends == 0 && b_new->source_ends == 0 && !demux_dead);
    upipe_release(in_b);
    assert(a->source_ends == 1 && a->dead == 1);
    assert(b_new->source_ends == 1 && b_new->dead == 1);
    assert(split_updates == 6);
    assert(demux_dead);
    for (unsigned i = 0; i < nb_subs; i++)
        assert(subs[i].upipe == NULL && subs[i].dead == 1);

    uref_free(main_flow_def);
    uref_free(a_flow_def);
    uref_free(b_flow_def);
    uref_free(c_flow_def);
    upipe_release(sink);
    upipe_mgr_release(upipe_uref_mux_mgr); // nop
    upipe_mgr_release(upipe_uref_demux_mgr); // nop

    ubuf_mgr_release(ubuf_mgr);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    uprobe_clean(&demux_probe);
    uprobe_clean(&sub_probe);
    uprobe_release(logger);
    uprobe_clean(&uprobe);

    return 0;
}
