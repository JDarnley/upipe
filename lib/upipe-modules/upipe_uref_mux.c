/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: James Darnley
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe module multiplexing urefs from several input subpipes into a
 * single output.
 *
 * Each input subpipe holds the urefs it receives, in arrival order. Each uref
 * of the main input becomes a carrier: the urefs held by the inputs are
 * tagged with the flow ID of their input and merged into it
 * (@ref uref_sub_merge), input by input in creation order, and the carrier is
 * output. The output flow definition is built by merging the flow
 * definitions of the inputs, which carry their flow IDs, into that of the
 * main input. Requests from the inputs are forwarded to the output.
 *
 * The inputs hold the refcount of the main pipe, which therefore lives until
 * its upstream and all its inputs have released it.
 */

#include "upipe/ubase.h"
#include "upipe/ulist.h"
#include "upipe/uref.h"
#include "upipe/uref_flow.h"
#include "upipe/ubuf.h"
#include "upipe/uref_sub.h"
#include "upipe/upipe.h"
#include "upipe/upipe_helper_upipe.h"
#include "upipe/upipe_helper_urefcount.h"
#include "upipe/upipe_helper_void.h"
#include "upipe/upipe_helper_output.h"
#include "upipe/upipe_helper_subpipe.h"
#include "upipe-modules/upipe_uref_mux.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>

/** @internal @This is the private context of a uref_mux pipe. */
struct upipe_uref_mux {
    /** refcount management structure, held by the upstream pipe and by
     * every input subpipe */
    struct urefcount urefcount;

    /** output pipe */
    struct upipe *output;
    /** output flow definition packet */
    struct uref *flow_def;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** list of output requests */
    struct uchain request_list;

    /** list of input subpipes */
    struct uchain subs;
    /** manager to create input subpipes */
    struct upipe_mgr sub_mgr;

    /** input flow definition of the main pipe (the reference stream carried
     * as entry 0 of the output urefs) */
    struct uref *input_flow_def;
    /** flow ID of the main input, read from its flow definition */
    uint64_t flow_id;

    /** public upipe structure */
    struct upipe upipe;
};

/** @hidden */
static int upipe_uref_mux_build_flow_def(struct upipe *upipe);

UPIPE_HELPER_UPIPE(upipe_uref_mux, upipe, UPIPE_UREF_MUX_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_uref_mux, urefcount, upipe_uref_mux_free)
UPIPE_HELPER_VOID(upipe_uref_mux)
UPIPE_HELPER_OUTPUT(upipe_uref_mux, output, flow_def, output_state,
                    request_list)

/** @internal @This is the private context of an input of a uref_mux pipe. */
struct upipe_uref_mux_sub {
    /** refcount management structure */
    struct urefcount urefcount;
    /** structure for double-linked lists */
    struct uchain uchain;

    /** input flow definition packet */
    struct uref *flow_def;
    /** flow ID of the input, read from its flow definition */
    uint64_t flow_id;
    /** input urefs held until the main pipe consumes them, in arrival
     * order */
    struct uchain urefs;
    /** number of input urefs held, kept so that the list is never walked */
    unsigned nb_urefs;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_uref_mux_sub, upipe, UPIPE_UREF_MUX_SUB_SIGNATURE)
UPIPE_HELPER_UREFCOUNT(upipe_uref_mux_sub, urefcount, upipe_uref_mux_sub_free)
UPIPE_HELPER_VOID(upipe_uref_mux_sub)

UPIPE_HELPER_SUBPIPE(upipe_uref_mux, upipe_uref_mux_sub, sub, sub_mgr, subs,
                     uchain)

/*
 * Input subpipes
 */

/** @internal @This allocates an input subpipe of a uref_mux pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_uref_mux_sub_alloc(struct upipe_mgr *mgr,
                                              struct uprobe *uprobe,
                                              uint32_t signature,
                                              va_list args)
{
    struct upipe *upipe =
        upipe_uref_mux_sub_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    struct upipe_uref_mux_sub *sub = upipe_uref_mux_sub_from_upipe(upipe);
    upipe_uref_mux_sub_init_urefcount(upipe);
    upipe_uref_mux_sub_init_sub(upipe);
    sub->flow_def = NULL;
    sub->flow_id = 0;
    ulist_init(&sub->urefs);
    sub->nb_urefs = 0;

    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This drops the urefs held by an input subpipe, with a warning
 * for each of them.
 *
 * @param upipe description structure of the subpipe
 * @param reason reason for dropping them
 */
static void upipe_uref_mux_sub_drop(struct upipe *upipe, const char *reason)
{
    struct upipe_uref_mux_sub *sub = upipe_uref_mux_sub_from_upipe(upipe);
    struct uchain *uchain;
    while ((uchain = ulist_pop(&sub->urefs)) != NULL) {
        upipe_warn_va(upipe, "dropping input uref, %s", reason);
        uref_free(uref_from_uchain(uchain));
    }
    sub->nb_urefs = 0;
}

/** @internal @This receives data on an input subpipe. The urefs are held in
 * arrival order until the main pipe consumes them all with its next uref
 * (an input uref is never repeated nor duplicated). At most
 * UREF_SUB_MAX_ENTRIES - 1 urefs are held, as many as a uref can carry: the
 * following ones are dropped with a warning. Data received before a flow
 * definition has been set is dropped.
 *
 * @param upipe description structure of the subpipe
 * @param uref uref structure
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_uref_mux_sub_input(struct upipe *upipe, struct uref *uref,
                                     struct upump **upump_p)
{
    struct upipe_uref_mux_sub *sub = upipe_uref_mux_sub_from_upipe(upipe);

    if (unlikely(sub->flow_def == NULL)) {
        upipe_warn(upipe, "dropping input uref received before flow def");
        uref_free(uref);
        return;
    }
    if (unlikely(sub->nb_urefs >= UREF_SUB_MAX_ENTRIES - 1)) {
        upipe_warn(upipe, "dropping input uref, too many held");
        uref_free(uref);
        return;
    }
    ulist_add(&sub->urefs, uref_to_uchain(uref));
    sub->nb_urefs++;
}

/** @internal @This checks whether a flow ID is already used by another flow
 * of a uref_mux pipe: the main input, or an input subpipe.
 *
 * @param upipe description structure of the pipe
 * @param flow_id flow ID to check
 * @param sub input subpipe setting a flow definition, which is not compared
 * with itself, or NULL for the main input
 * @return true if another flow uses the flow ID
 */
static bool upipe_uref_mux_flow_id_used(struct upipe *upipe, uint64_t flow_id,
                                        struct upipe_uref_mux_sub *sub)
{
    struct upipe_uref_mux *upipe_uref_mux = upipe_uref_mux_from_upipe(upipe);
    if (sub != NULL && upipe_uref_mux->input_flow_def != NULL &&
        upipe_uref_mux->flow_id == flow_id)
        return true;

    struct uchain *uchain;
    ulist_foreach(&upipe_uref_mux->subs, uchain) {
        struct upipe_uref_mux_sub *other =
            upipe_uref_mux_sub_from_uchain(uchain);
        if (other != sub && other->flow_def != NULL &&
            other->flow_id == flow_id)
            return true;
    }
    return false;
}

/** @internal @This sets the input flow definition of a subpipe. The flow
 * definition must carry a flow ID, which is used as the key identifying this
 * input in the multiplexed output; a flow def without a flow ID, or with the
 * flow ID of the main input or of another input, is rejected. Once a flow def
 * is accepted, even one identical to the current one, the urefs held under
 * the previous one are dropped with a warning each, and the output flow
 * definition is rebuilt.
 *
 * @param upipe description structure of the subpipe
 * @param flow_def new input flow definition
 * @return an error code
 */
static int upipe_uref_mux_sub_set_flow_def(struct upipe *upipe,
                                           struct uref *flow_def)
{
    struct upipe_uref_mux_sub *sub = upipe_uref_mux_sub_from_upipe(upipe);
    if (flow_def == NULL)
        return UBASE_ERR_INVALID;

    uint64_t flow_id;
    if (unlikely(!ubase_check(uref_flow_get_id(flow_def, &flow_id)))) {
        upipe_warn(upipe, "input flow def has no flow ID");
        return UBASE_ERR_INVALID;
    }

    struct upipe_uref_mux *upipe_uref_mux =
        upipe_uref_mux_from_sub_mgr(upipe->mgr);
    if (unlikely(upipe_uref_mux_flow_id_used(
                    upipe_uref_mux_to_upipe(upipe_uref_mux), flow_id,
                    sub))) {
        upipe_warn_va(upipe, "flow ID %" PRIu64 " is already used", flow_id);
        return UBASE_ERR_INVALID;
    }

    struct uref *dup = uref_dup(flow_def);
    if (unlikely(dup == NULL))
        return UBASE_ERR_ALLOC;
    uref_free(sub->flow_def);
    sub->flow_def = dup;
    sub->flow_id = flow_id;
    upipe_uref_mux_sub_drop(upipe, "new flow def");

    /* the set of constituent flows changed, refresh the output flow def */
    return upipe_uref_mux_build_flow_def(
            upipe_uref_mux_to_upipe(upipe_uref_mux));
}

/** @internal @This processes control commands on an input subpipe.
 *
 * @param upipe description structure of the subpipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_uref_mux_sub_control(struct upipe *upipe,
                                      int command, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_uref_mux_sub_control_super(upipe, command,
                                                          args));

    switch (command) {
        /* the data of an input ends up on the output of the main pipe, so
         * that is where its requests go */
        case UPIPE_REGISTER_REQUEST: {
            struct upipe_uref_mux *upipe_uref_mux =
                upipe_uref_mux_from_sub_mgr(upipe->mgr);
            struct urequest *request = va_arg(args, struct urequest *);
            return upipe_uref_mux_alloc_output_proxy(
                    upipe_uref_mux_to_upipe(upipe_uref_mux), request);
        }
        case UPIPE_UNREGISTER_REQUEST: {
            struct upipe_uref_mux *upipe_uref_mux =
                upipe_uref_mux_from_sub_mgr(upipe->mgr);
            struct urequest *request = va_arg(args, struct urequest *);
            return upipe_uref_mux_free_output_proxy(
                    upipe_uref_mux_to_upipe(upipe_uref_mux), request);
        }
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_uref_mux_sub_set_flow_def(upipe, flow_def);
        }
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @internal @This frees an input subpipe. The input urefs it still holds
 * are dropped, with a single warning.
 *
 * @param upipe description structure of the subpipe
 */
static void upipe_uref_mux_sub_free(struct upipe *upipe)
{
    struct upipe_uref_mux_sub *sub = upipe_uref_mux_sub_from_upipe(upipe);
    struct upipe_uref_mux *upipe_uref_mux =
        upipe_uref_mux_from_sub_mgr(upipe->mgr);
    bool contributed = sub->flow_def != NULL;
    if (unlikely(sub->nb_urefs))
        upipe_warn_va(upipe, "dropping %u input uref(s) still held",
                      sub->nb_urefs);
    upipe_throw_dead(upipe);
    struct uchain *uchain, *uchain_tmp;
    ulist_delete_foreach(&sub->urefs, uchain, uchain_tmp) {
        ulist_delete(uchain);
        uref_free(uref_from_uchain(uchain));
    }
    uref_free(sub->flow_def);
    upipe_uref_mux_sub_clean_sub(upipe);

    /* removing a configured input changes the set of constituent flows; this
     * must be done before freeing the subpipe, which releases its reference
     * to the main pipe, possibly the last one */
    if (contributed)
        upipe_uref_mux_build_flow_def(
                upipe_uref_mux_to_upipe(upipe_uref_mux));

    upipe_uref_mux_sub_clean_urefcount(upipe);
    upipe_uref_mux_sub_free_void(upipe);
}

/** @internal @This initializes the input subpipe manager of a uref_mux pipe.
 *
 * @param upipe description structure of the (main) pipe
 */
static void upipe_uref_mux_init_sub_mgr(struct upipe *upipe)
{
    struct upipe_uref_mux *upipe_uref_mux = upipe_uref_mux_from_upipe(upipe);
    struct upipe_mgr *sub_mgr = &upipe_uref_mux->sub_mgr;
    /* every input holds the refcount of the main pipe */
    sub_mgr->refcount = upipe_uref_mux_to_urefcount(upipe_uref_mux);
    sub_mgr->signature = UPIPE_UREF_MUX_SUB_SIGNATURE;
    sub_mgr->upipe_alloc = upipe_uref_mux_sub_alloc;
    sub_mgr->upipe_input = upipe_uref_mux_sub_input;
    sub_mgr->upipe_control = upipe_uref_mux_sub_control;
}

/*
 * Main pipe
 */

/** @internal @This allocates a uref_mux pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_uref_mux_alloc(struct upipe_mgr *mgr,
                                          struct uprobe *uprobe,
                                          uint32_t signature, va_list args)
{
    struct upipe *upipe =
        upipe_uref_mux_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    struct upipe_uref_mux *upipe_uref_mux = upipe_uref_mux_from_upipe(upipe);
    upipe_uref_mux_init_urefcount(upipe);
    upipe_uref_mux_init_output(upipe);
    upipe_uref_mux_init_sub_subs(upipe);
    upipe_uref_mux_init_sub_mgr(upipe);
    upipe_uref_mux->input_flow_def = NULL;
    upipe_uref_mux->flow_id = 0;

    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This (re)builds the aggregated output flow definition from the
 * main pipe input flow def (entry 0, the reference stream) and the flow def
 * of each configured input subpipe (entries 1 and above). Each constituent
 * flow def is merged whole (@ref uref_sub_merge) under the per-entry
 * "sub[i]." namespace of the output flow def, where its flow ID identifies
 * the entry, so that a demux can extract it back (@ref uref_sub_extract).
 * This is called whenever the flow def of the main input or of an input
 * changes, and when a configured input is released.
 *
 * @param upipe description structure of the pipe
 * @return an error code
 */
static int upipe_uref_mux_build_flow_def(struct upipe *upipe)
{
    struct upipe_uref_mux *upipe_uref_mux = upipe_uref_mux_from_upipe(upipe);
    if (upipe_uref_mux->input_flow_def == NULL)
        /* nothing to base the output flow def on yet */
        return UBASE_ERR_NONE;

    struct uref *flow_def = uref_dup(upipe_uref_mux->input_flow_def);
    if (unlikely(flow_def == NULL)) {
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }

    /* entry 0: the reference stream carried by the main pipe input */
    if (unlikely(!ubase_check(uref_sub_set_flow_id(flow_def,
                                                   upipe_uref_mux->flow_id,
                                                   0))))
        upipe_warn(upipe, "unable to tag the main flow def with its flow ID");

    /* entries 1 and above: the flow def of each configured input subpipe,
     * identified by its flow ID */
    struct uchain *uchain;
    ulist_foreach(&upipe_uref_mux->subs, uchain) {
        struct upipe_uref_mux_sub *sub =
            upipe_uref_mux_sub_from_uchain(uchain);
        if (sub->flow_def == NULL)
            continue;

        struct uref *sub_flow_def = uref_dup(sub->flow_def);
        if (unlikely(sub_flow_def == NULL)) {
            uref_free(flow_def);
            upipe_throw_error(upipe, UBASE_ERR_ALLOC);
            return UBASE_ERR_ALLOC;
        }

        /* its flow ID, which identifies the entry, is merged with its other
         * attributes */
        int err = uref_sub_merge(flow_def, sub_flow_def, NULL);
        if (unlikely(!ubase_check(err))) {
            uref_free(sub_flow_def);
            uref_free(flow_def);
            upipe_throw_error(upipe, err);
            return err;
        }
    }

    upipe_uref_mux_store_flow_def(upipe, flow_def);
    return UBASE_ERR_NONE;
}

/** @internal @This sets the input flow definition of the main pipe, i.e.
 * the reference stream that drives the output and is carried as entry 0. Its
 * flow ID must not be used by an input.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new input flow definition
 * @return an error code
 */
static int upipe_uref_mux_set_flow_def(struct upipe *upipe,
                                       struct uref *flow_def)
{
    struct upipe_uref_mux *upipe_uref_mux = upipe_uref_mux_from_upipe(upipe);
    if (flow_def == NULL)
        return UBASE_ERR_INVALID;

    uint64_t flow_id;
    if (unlikely(!ubase_check(uref_flow_get_id(flow_def, &flow_id)))) {
        upipe_warn(upipe, "input flow def has no flow ID");
        return UBASE_ERR_INVALID;
    }
    if (unlikely(upipe_uref_mux_flow_id_used(upipe, flow_id, NULL))) {
        upipe_warn_va(upipe, "flow ID %" PRIu64 " is already used by an "
                      "input", flow_id);
        return UBASE_ERR_INVALID;
    }

    struct uref *dup = uref_dup(flow_def);
    if (unlikely(dup == NULL))
        return UBASE_ERR_ALLOC;
    uref_free(upipe_uref_mux->input_flow_def);
    upipe_uref_mux->input_flow_def = dup;
    upipe_uref_mux->flow_id = flow_id;

    return upipe_uref_mux_build_flow_def(upipe);
}

/** @internal @This receives a reference uref on the main pipe input. It
 * merges all the urefs held by each input subpipe into it (consuming them),
 * subpipe by subpipe and in arrival order, and outputs the resulting
 * multiplexed uref. The reference stream is carried as entry 0; each merged
 * input uref becomes an additional entry tagged with the flow ID of its
 * subpipe, so an input may have several entries. No uref is ever repeated: a
 * subpipe with nothing held simply contributes no entry to this output. A
 * uref has room for UREF_SUB_MAX_ENTRIES - 1 additional entries; input urefs
 * beyond that, and those which can't be tagged or merged, are dropped with a
 * warning from their subpipe.
 *
 * @param upipe description structure of the pipe
 * @param uref reference uref, used as the multiplex carrier
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_uref_mux_input(struct upipe *upipe, struct uref *uref,
                                 struct upump **upump_p)
{
    struct upipe_uref_mux *upipe_uref_mux = upipe_uref_mux_from_upipe(upipe);

    if (unlikely(upipe_uref_mux->input_flow_def == NULL)) {
        upipe_warn(upipe, "dropping uref received before flow def");
        uref_free(uref);
        return;
    }

    /* tag entry 0 (the reference stream) with its flow ID for uniform
     * lookup */
    if (unlikely(!ubase_check(uref_sub_set_flow_id(uref,
                                                   upipe_uref_mux->flow_id,
                                                   0))))
        upipe_warn(upipe, "unable to tag the main uref with its flow ID");

    /* merge the urefs held by each input subpipe, input by input in creation
     * order, and in arrival order; count tracks the entries of the carrier
     * from the index of each merge, rather than counting them again */
    unsigned count = uref_sub_count(uref);
    struct uchain *uchain;
    ulist_foreach(&upipe_uref_mux->subs, uchain) {
        struct upipe_uref_mux_sub *sub =
            upipe_uref_mux_sub_from_uchain(uchain);
        struct upipe *sub_upipe = upipe_uref_mux_sub_to_upipe(sub);
        struct uchain *uref_uchain;
        while (count < UREF_SUB_MAX_ENTRIES &&
               (uref_uchain = ulist_pop(&sub->urefs)) != NULL) {
            struct uref *sub_uref = uref_from_uchain(uref_uchain);
            /* tag the input uref with the flow ID of its input, which
             * merging carries into the per-entry attributes */
            if (unlikely(!ubase_check(uref_flow_set_id(sub_uref,
                                                       sub->flow_id)))) {
                upipe_warn(sub_upipe, "dropping input uref, unable to tag");
                uref_free(sub_uref);
                continue;
            }
            uint8_t index;
            if (unlikely(!ubase_check(uref_sub_merge(uref, sub_uref,
                                                     &index)))) {
                upipe_warn(sub_upipe, "dropping input uref, unable to merge");
                uref_free(sub_uref);
                continue;
            }
            count = index + 1;
        }
        /* whatever is still held didn't fit: drop it, which also resets the
         * number of urefs held (nothing happens if none is left) */
        upipe_uref_mux_sub_drop(sub_upipe, "no room left in the output");
    }

    upipe_uref_mux_output(upipe, uref, upump_p);
}

/** @internal @This processes control commands on a uref_mux pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_uref_mux_control(struct upipe *upipe,
                                  int command, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_uref_mux_control_output(upipe, command, args));
    UBASE_HANDLED_RETURN(upipe_uref_mux_control_subs(upipe, command, args));

    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_uref_mux_set_flow_def(upipe, flow_def);
        }
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @internal @This frees a uref_mux pipe, once its upstream and all its
 * inputs have released it.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_uref_mux_free(struct upipe *upipe)
{
    struct upipe_uref_mux *upipe_uref_mux = upipe_uref_mux_from_upipe(upipe);
    upipe_throw_dead(upipe);

    uref_free(upipe_uref_mux->input_flow_def);
    upipe_uref_mux_clean_sub_subs(upipe);
    upipe_uref_mux_clean_output(upipe);
    upipe_uref_mux_clean_urefcount(upipe);
    upipe_uref_mux_free_void(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_uref_mux_mgr = {
    .signature = UPIPE_UREF_MUX_SIGNATURE,
    .upipe_alloc = upipe_uref_mux_alloc,
    .upipe_input = upipe_uref_mux_input,
    .upipe_control = upipe_uref_mux_control,
};

/** @This returns the management structure for uref_mux pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_uref_mux_mgr_alloc(void)
{
    return &upipe_uref_mux_mgr;
}
