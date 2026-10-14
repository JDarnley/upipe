/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: James Darnley
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe module demultiplexing urefs carrying several ubufs into a main
 * output and several output subpipes.
 *
 * The flow definitions of the entries are announced so that output subpipes
 * can be allocated, and each entry is extracted (@ref uref_sub_extract) and
 * sent to the output subpipe of its flow ID. The main output receives the
 * input and its flow definition without the additional entries, that is
 * without additional ubufs and per-entry attributes.
 *
 * Entries are matched to subpipes by their flow ID, which identifies their
 * flow in upipe_uref_mux, never by their index, which may differ from one
 * uref to the next. A new
 * input flow definition updates the subpipes in place when their "f.def" is
 * unchanged, and ends them otherwise, since the pipelines after them may rely
 * on the kind of data they carry.
 */

#include "upipe/ubase.h"
#include "upipe/ulist.h"
#include "upipe/uprobe.h"
#include "upipe/udict.h"
#include "upipe/uref.h"
#include "upipe/uref_flow.h"
#include "upipe/uref_sub.h"
#include "upipe/upipe.h"
#include "upipe/upipe_helper_upipe.h"
#include "upipe/upipe_helper_urefcount.h"
#include "upipe/upipe_helper_urefcount_real.h"
#include "upipe/upipe_helper_void.h"
#include "upipe/upipe_helper_flow.h"
#include "upipe/upipe_helper_output.h"
#include "upipe/upipe_helper_subpipe.h"
#include "upipe-modules/upipe_uref_demux.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

/** @internal @This is the private context of a uref_demux pipe. */
struct upipe_uref_demux {
    /** real refcount management structure */
    struct urefcount urefcount_real;
    /** refcount management structure exported to the public structure */
    struct urefcount urefcount;

    /** main output pipe (primary ubuf) */
    struct upipe *output;
    /** main output flow definition packet */
    struct uref *flow_def;
    /** main output state */
    enum upipe_helper_output_state output_state;
    /** list of main output requests */
    struct uchain request_list;

    /** flow definitions of the entries of the input, one per flow ID, as
     * announced and returned by split_iterate */
    struct uchain flow_defs;

    /** list of output subpipes */
    struct uchain subs;
    /** manager to create output subpipes */
    struct upipe_mgr sub_mgr;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_uref_demux, upipe, UPIPE_UREF_DEMUX_SIGNATURE)
UPIPE_HELPER_UREFCOUNT(upipe_uref_demux, urefcount, upipe_uref_demux_no_input)
UPIPE_HELPER_UREFCOUNT_REAL(upipe_uref_demux, urefcount_real,
                            upipe_uref_demux_free)
UPIPE_HELPER_VOID(upipe_uref_demux)
UPIPE_HELPER_OUTPUT(upipe_uref_demux, output, flow_def, output_state,
                    request_list)

/** @internal @This is the private context of an output subpipe of a
 * uref_demux pipe. */
struct upipe_uref_demux_sub {
    /** refcount management structure */
    struct urefcount urefcount;
    /** structure for double-linked lists */
    struct uchain uchain;

    /** flow ID of the entries output by this subpipe */
    uint64_t flow_id;
    /** true while the subpipe outputs no flow: until it has been keyed on a
     * flow ID, and once it has been ended (source end event); it then holds
     * no flow ID nor gets any data */
    bool ended;

    /** output pipe */
    struct upipe *output;
    /** output flow definition packet */
    struct uref *flow_def;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** list of output requests */
    struct uchain request_list;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_uref_demux_sub, upipe, UPIPE_UREF_DEMUX_SUB_SIGNATURE)
UPIPE_HELPER_UREFCOUNT(upipe_uref_demux_sub, urefcount,
                       upipe_uref_demux_sub_free)
UPIPE_HELPER_FLOW(upipe_uref_demux_sub, NULL)
UPIPE_HELPER_OUTPUT(upipe_uref_demux_sub, output, flow_def, output_state,
                    request_list)

UPIPE_HELPER_SUBPIPE(upipe_uref_demux, upipe_uref_demux_sub, sub, sub_mgr,
                     subs, uchain)

/*
 * Flow definitions of the entries
 */

/** @internal @This finds the flow definition of a flow ID in a list.
 *
 * @param flow_defs list of flow definitions
 * @param flow_id flow ID to look for
 * @return pointer to the flow definition, or NULL if it isn't in the list
 */
static struct uref *upipe_uref_demux_find_flow_def(struct uchain *flow_defs,
                                                   uint64_t flow_id)
{
    struct uchain *uchain;
    ulist_foreach(flow_defs, uchain) {
        struct uref *flow_def = uref_from_uchain(uchain);
        uint64_t id;
        if (ubase_check(uref_flow_get_id(flow_def, &id)) && id == flow_id)
            return flow_def;
    }
    return NULL;
}

/** @internal @This frees the flow definitions of a list.
 *
 * @param flow_defs list of flow definitions
 */
static void upipe_uref_demux_clean_flow_defs(struct uchain *flow_defs)
{
    struct uchain *uchain, *uchain_tmp;
    ulist_delete_foreach(flow_defs, uchain, uchain_tmp) {
        ulist_delete(uchain);
        uref_free(uref_from_uchain(uchain));
    }
}

/*
 * Output subpipes
 */

/** @internal @This allocates an output subpipe of a uref_demux pipe. The flow
 * definition must carry the flow ID of an entry announced by the pipe (see
 * @ref upipe_uref_demux_iterate) which has no output subpipe yet. The
 * announced flow definition is used on the output.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments (flow definition)
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_uref_demux_sub_alloc(struct upipe_mgr *mgr,
                                                struct uprobe *uprobe,
                                                uint32_t signature,
                                                va_list args)
{
    struct uref *flow_def;
    struct upipe *upipe = upipe_uref_demux_sub_alloc_flow(mgr, uprobe,
                                                          signature, args,
                                                          &flow_def);
    if (unlikely(upipe == NULL))
        return NULL;

    struct upipe_uref_demux_sub *sub = upipe_uref_demux_sub_from_upipe(upipe);
    upipe_uref_demux_sub_init_urefcount(upipe);
    upipe_uref_demux_sub_init_output(upipe);
    upipe_uref_demux_sub_init_sub(upipe);
    /* inactive until all the checks below have passed: any value is a valid
     * flow ID, so none can mean "not keyed yet" */
    sub->flow_id = 0;
    sub->ended = true;

    upipe_throw_ready(upipe);

    if (unlikely(flow_def == NULL)) {
        upipe_warn(upipe, "output subpipes must be allocated with a flow def");
        upipe_release(upipe);
        return NULL;
    }

    uint64_t flow_id;
    if (unlikely(!ubase_check(uref_flow_get_id(flow_def, &flow_id)))) {
        uref_free(flow_def);
        upipe_warn(upipe, "flow def has no flow ID");
        upipe_release(upipe);
        return NULL;
    }
    uref_free(flow_def);

    struct upipe_uref_demux *upipe_uref_demux =
        upipe_uref_demux_from_sub_mgr(mgr);
    struct uref *announced =
        upipe_uref_demux_find_flow_def(&upipe_uref_demux->flow_defs, flow_id);
    if (unlikely(announced == NULL)) {
        upipe_warn_va(upipe, "flow ID %" PRIu64 " is not announced", flow_id);
        upipe_release(upipe);
        return NULL;
    }

    /* one subpipe per flow ID, not counting ended ones (nor this one, which
     * is inactive) */
    struct uchain *uchain;
    ulist_foreach(&upipe_uref_demux->subs, uchain) {
        struct upipe_uref_demux_sub *other =
            upipe_uref_demux_sub_from_uchain(uchain);
        if (!other->ended && other->flow_id == flow_id) {
            upipe_warn_va(upipe, "flow ID %" PRIu64 " already has a subpipe",
                          flow_id);
            upipe_release(upipe);
            return NULL;
        }
    }

    struct uref *output_flow_def = uref_dup(announced);
    if (unlikely(output_flow_def == NULL)) {
        upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        upipe_release(upipe);
        return NULL;
    }
    sub->flow_id = flow_id;
    sub->ended = false;
    upipe_uref_demux_sub_store_flow_def(upipe, output_flow_def);
    return upipe;
}

/** @internal @This processes control commands on an output subpipe. A
 * subpipe has no input, so only its output may be controlled.
 *
 * @param upipe description structure of the subpipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_uref_demux_sub_control(struct upipe *upipe,
                                        int command, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_uref_demux_sub_control_super(upipe, command,
                                                            args));

    switch (command) {
        case UPIPE_GET_FLOW_DEF:
        case UPIPE_GET_OUTPUT:
        case UPIPE_SET_OUTPUT:
            return upipe_uref_demux_sub_control_output(upipe, command, args);
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @internal @This frees an output subpipe.
 *
 * @param upipe description structure of the subpipe
 */
static void upipe_uref_demux_sub_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);

    upipe_uref_demux_sub_clean_output(upipe);
    upipe_uref_demux_sub_clean_sub(upipe);
    upipe_uref_demux_sub_clean_urefcount(upipe);
    upipe_uref_demux_sub_free_flow(upipe);
}

/** @internal @This initializes the output subpipe manager of a uref_demux
 * pipe.
 *
 * @param upipe description structure of the (main) pipe
 */
static void upipe_uref_demux_init_sub_mgr(struct upipe *upipe)
{
    struct upipe_uref_demux *upipe_uref_demux =
        upipe_uref_demux_from_upipe(upipe);
    struct upipe_mgr *sub_mgr = &upipe_uref_demux->sub_mgr;
    /* the subpipes hold the real refcount: the pipe outlives its external
     * references until the last subpipe is released */
    sub_mgr->refcount = upipe_uref_demux_to_urefcount_real(upipe_uref_demux);
    sub_mgr->signature = UPIPE_UREF_DEMUX_SUB_SIGNATURE;
    sub_mgr->upipe_alloc = upipe_uref_demux_sub_alloc;
    sub_mgr->upipe_control = upipe_uref_demux_sub_control;
}

/*
 * Main pipe
 */

/** @internal @This allocates a uref_demux pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_uref_demux_alloc(struct upipe_mgr *mgr,
                                            struct uprobe *uprobe,
                                            uint32_t signature, va_list args)
{
    struct upipe *upipe =
        upipe_uref_demux_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;

    struct upipe_uref_demux *upipe_uref_demux =
        upipe_uref_demux_from_upipe(upipe);
    upipe_uref_demux_init_urefcount(upipe);
    upipe_uref_demux_init_urefcount_real(upipe);
    upipe_uref_demux_init_output(upipe);
    upipe_uref_demux_init_sub_subs(upipe);
    upipe_uref_demux_init_sub_mgr(upipe);
    ulist_init(&upipe_uref_demux->flow_defs);

    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This finds the output subpipe of a flow ID, ignoring ended
 * subpipes.
 *
 * @param upipe description structure of the pipe
 * @param flow_id flow ID
 * @return pointer to the subpipe, or NULL if the flow ID has no subpipe
 */
static struct upipe *upipe_uref_demux_find_sub(struct upipe *upipe,
                                               uint64_t flow_id)
{
    struct upipe_uref_demux *upipe_uref_demux =
        upipe_uref_demux_from_upipe(upipe);
    struct uchain *uchain;
    ulist_foreach(&upipe_uref_demux->subs, uchain) {
        struct upipe_uref_demux_sub *sub =
            upipe_uref_demux_sub_from_uchain(uchain);
        if (!sub->ended && sub->flow_id == flow_id)
            return upipe_uref_demux_sub_to_upipe(sub);
    }
    return NULL;
}

/** @internal @This receives a carrier uref on the main pipe input. Each
 * additional entry whose flow ID has an output subpipe is extracted
 * (@ref uref_sub_extract) and sent to it, in entry order, so that the
 * entries of a flow keep their order. Entries whose flow ID has no subpipe are
 * dropped silently, as nobody wants them; entries without a flow ID, or whose
 * subpipe goes away before they are sent, are dropped with a warning. All the
 * additional entries are removed from the uref, which is then sent to the
 * main output with only its primary ubuf.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_uref_demux_input(struct upipe *upipe, struct uref *uref,
                                   struct upump **upump_p)
{
    /* extract the entries whose flow ID has a subpipe, in entry order */
    struct uchain entries;
    ulist_init(&entries);
    uint8_t count = uref_sub_count(uref);
    for (uint8_t i = 1; i < count; i++) {
        uint64_t flow_id;
        if (unlikely(!ubase_check(uref_sub_get_flow_id(uref, &flow_id, i)))) {
            upipe_warn_va(upipe, "dropping entry %" PRIu8 ", no flow ID", i);
            continue;
        }
        if (upipe_uref_demux_find_sub(upipe, flow_id) == NULL)
            continue;

        struct uref *entry = uref_sub_extract(uref, i);
        if (unlikely(entry == NULL)) {
            upipe_throw_error(upipe, UBASE_ERR_ALLOC);
            continue;
        }
        ulist_add(&entries, uref_to_uchain(entry));
    }

    /* remove all the entries from the uref before outputting the extracted
     * ones, so that their ubufs aren't shared anymore, from the last one down
     * so that removing an entry never renumbers the remaining ones */
    for (uint8_t i = count; i > 1; i--) {
        struct ubuf *ubuf = uref_sub_detach_ubuf(uref, i - 1);
        if (ubuf != NULL)
            ubuf_free(ubuf);
        else
            uref_sub_delete_attrs(uref, i - 1);
    }
    uref_sub_delete_attrs(uref, 0);

    /* send the extracted entries in order, looking their subpipes up again,
     * as sending one may lead to another subpipe being released */
    struct uchain *uchain;
    while ((uchain = ulist_pop(&entries)) != NULL) {
        struct uref *entry = uref_from_uchain(uchain);
        uint64_t flow_id = 0;
        struct upipe *sub = NULL;
        if (likely(ubase_check(uref_flow_get_id(entry, &flow_id))))
            sub = upipe_uref_demux_find_sub(upipe, flow_id);
        /* the flow ID only identified the entry in the carrier */
        uref_flow_delete_id(entry);
        if (unlikely(sub == NULL)) {
            /* its subpipe went away while sending the previous ones */
            upipe_warn_va(upipe, "dropping entry for flow ID %" PRIu64
                          ", its subpipe is gone", flow_id);
            uref_free(entry);
            continue;
        }
        upipe_uref_demux_sub_output(sub, entry, upump_p);
    }

    upipe_uref_demux_output(upipe, uref, upump_p);
}

/** @internal @This builds the flow definitions of the entries of an input
 * flow definition, one per flow ID: each entry from 1 up is extracted
 * (@ref uref_sub_extract) and keyed by its flow ID, which identifies its flow
 * in upipe_uref_mux. The flow ID is kept, so that the announced flows can be
 * selected by it. Entries without a flow ID or a flow definition, or
 * repeating a flow ID, are skipped with a warning.
 *
 * @param upipe description structure of the pipe
 * @param flow_def input flow definition
 * @param flow_defs empty list filled in with the flow definitions
 * @return an error code
 */
static int upipe_uref_demux_build_flow_defs(struct upipe *upipe,
                                            struct uref *flow_def,
                                            struct uchain *flow_defs)
{
    uint8_t count = uref_sub_count(flow_def);
    for (uint8_t i = 1; i < count; i++) {
        uint64_t flow_id;
        if (unlikely(!ubase_check(uref_sub_get_flow_id(flow_def, &flow_id,
                                                       i)))) {
            upipe_warn_va(upipe, "entry %" PRIu8 " has no flow ID", i);
            continue;
        }
        if (unlikely(upipe_uref_demux_find_flow_def(flow_defs,
                                                    flow_id) != NULL)) {
            upipe_warn_va(upipe, "entry %" PRIu8 " repeats flow ID %" PRIu64,
                          i, flow_id);
            continue;
        }

        struct uref *entry = uref_sub_extract(flow_def, i);
        UBASE_ALLOC_RETURN(entry);
        const char *def;
        if (unlikely(!ubase_check(uref_flow_get_def(entry, &def)))) {
            upipe_warn_va(upipe, "entry %" PRIu8 " has no flow def", i);
            uref_free(entry);
            continue;
        }
        ulist_add(flow_defs, uref_to_uchain(entry));
    }
    return UBASE_ERR_NONE;
}

/** @internal @This checks whether two lists of flow definitions are the
 * same.
 *
 * @param flow_defs1 first list of flow definitions
 * @param flow_defs2 second list of flow definitions
 * @return true if both lists have the same flow definitions in the same order
 */
static bool upipe_uref_demux_flow_defs_equal(struct uchain *flow_defs1,
                                             struct uchain *flow_defs2)
{
    struct uchain *uchain1 = flow_defs1->next;
    struct uchain *uchain2 = flow_defs2->next;
    for ( ; uchain1 != flow_defs1 && uchain2 != flow_defs2;
         uchain1 = uchain1->next, uchain2 = uchain2->next) {
        struct udict *udict1 = uref_from_uchain(uchain1)->udict;
        struct udict *udict2 = uref_from_uchain(uchain2)->udict;
        if (udict1 == NULL || udict2 == NULL) {
            if (udict1 != udict2)
                return false;
        } else if (udict_cmp(udict1, udict2))
            return false;
    }
    return uchain1 == flow_defs1 && uchain2 == flow_defs2;
}

/** @internal @This updates the output subpipes after the flow definitions of
 * the entries changed. A subpipe whose flow ID is still announced with the
 * same "f.def" gets the new flow definition. Otherwise it is ended with a
 * source end event, since the pipeline after it may rely on the kind of data,
 * and its owner is expected to release it.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_uref_demux_update_subs(struct upipe *upipe)
{
    struct upipe_uref_demux *upipe_uref_demux =
        upipe_uref_demux_from_upipe(upipe);
    struct upipe_uref_demux_sub *sub = NULL;
    struct uchain *uchain;
    ulist_foreach(&upipe_uref_demux->subs, uchain) {
        if (sub != NULL)
            upipe_release(upipe_uref_demux_sub_to_upipe(sub));
        sub = upipe_uref_demux_sub_from_uchain(uchain);
        /* the owner may release the subpipe on source end, so keep it alive
         * until the iteration has moved on */
        upipe_use(upipe_uref_demux_sub_to_upipe(sub));
        if (sub->ended)
            continue;

        /* same flow ID with the same f.def: update the subpipe in place */
        struct uref *flow_def = upipe_uref_demux_find_flow_def(
                &upipe_uref_demux->flow_defs, sub->flow_id);
        const char *def, *old_def;
        if (flow_def != NULL && sub->flow_def != NULL &&
            ubase_check(uref_flow_get_def(flow_def, &def)) &&
            ubase_check(uref_flow_get_def(sub->flow_def, &old_def)) &&
            !strcmp(def, old_def)) {
            struct uref *flow_def_dup = uref_dup(flow_def);
            if (likely(flow_def_dup != NULL)) {
                upipe_uref_demux_sub_store_flow_def(
                        upipe_uref_demux_sub_to_upipe(sub), flow_def_dup);
                continue;
            }
            upipe_throw_error(upipe, UBASE_ERR_ALLOC);
        }

        /* flow gone, f.def changed or no memory: end the subpipe, which then
         * frees its flow ID for a new one */
        sub->ended = true;
        upipe_throw_source_end(upipe_uref_demux_sub_to_upipe(sub));
    }
    if (sub != NULL)
        upipe_release(upipe_uref_demux_sub_to_upipe(sub));
}

/** @internal @This iterates over the flow definitions of the entries, one per
 * flow ID, which may be used to allocate output subpipes.
 *
 * @param upipe description structure of the pipe
 * @param p filled in with the next flow definition (owned by the pipe), or
 * NULL at the end; initialize with NULL
 * @return an error code
 */
static int upipe_uref_demux_iterate(struct upipe *upipe, struct uref **p)
{
    struct upipe_uref_demux *upipe_uref_demux =
        upipe_uref_demux_from_upipe(upipe);
    assert(p != NULL);
    struct uchain *uchain = *p != NULL ? uref_to_uchain(*p) :
                                         &upipe_uref_demux->flow_defs;
    if (ulist_is_last(&upipe_uref_demux->flow_defs, uchain))
        *p = NULL;
    else
        *p = uref_from_uchain(uchain->next);
    return UBASE_ERR_NONE;
}

/** @internal @This sets the input flow definition of the main pipe. The main
 * output gets it without its per-entry attributes, the flow definitions of
 * its entries, one per flow ID, are rebuilt, the output subpipes are updated,
 * and a split update event is thrown if the flow definitions of the entries
 * changed.
 *
 * @param upipe description structure of the pipe
 * @param flow_def new input flow definition
 * @return an error code
 */
static int upipe_uref_demux_set_flow_def(struct upipe *upipe,
                                         struct uref *flow_def)
{
    struct upipe_uref_demux *upipe_uref_demux =
        upipe_uref_demux_from_upipe(upipe);
    if (flow_def == NULL)
        return UBASE_ERR_INVALID;

    /* build everything first, so that nothing changes on failure */
    struct uchain flow_defs;
    ulist_init(&flow_defs);
    int err = upipe_uref_demux_build_flow_defs(upipe, flow_def, &flow_defs);
    struct uref *flow_def_dup = ubase_check(err) ? uref_dup(flow_def) : NULL;
    if (unlikely(flow_def_dup == NULL)) {
        upipe_uref_demux_clean_flow_defs(&flow_defs);
        return ubase_check(err) ? UBASE_ERR_ALLOC : err;
    }

    /* the main output only carries the primary flow */
    uint8_t count = uref_sub_count(flow_def_dup);
    for (uint8_t i = 0; i < count; i++)
        uref_sub_delete_attrs(flow_def_dup, i);
    upipe_uref_demux_store_flow_def(upipe, flow_def_dup);

    /* replace the announced flow defs, noting whether they changed */
    bool changed = !upipe_uref_demux_flow_defs_equal(
            &upipe_uref_demux->flow_defs, &flow_defs);
    upipe_uref_demux_clean_flow_defs(&upipe_uref_demux->flow_defs);
    struct uchain *uchain, *uchain_tmp;
    ulist_delete_foreach(&flow_defs, uchain, uchain_tmp) {
        ulist_delete(uchain);
        ulist_add(&upipe_uref_demux->flow_defs, uchain);
    }

    /* end or update the subpipes before announcing the new flows, so that
     * probes reacting to the split update see the final state */
    upipe_uref_demux_update_subs(upipe);
    if (changed)
        upipe_split_throw_update(upipe);
    return UBASE_ERR_NONE;
}

/** @internal @This processes control commands on a uref_demux pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return an error code
 */
static int upipe_uref_demux_control(struct upipe *upipe,
                                    int command, va_list args)
{
    UBASE_HANDLED_RETURN(upipe_uref_demux_control_output(upipe, command,
                                                         args));
    UBASE_HANDLED_RETURN(upipe_uref_demux_control_subs(upipe, command, args));

    switch (command) {
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_uref_demux_set_flow_def(upipe, flow_def);
        }
        case UPIPE_SPLIT_ITERATE: {
            struct uref **p = va_arg(args, struct uref **);
            return upipe_uref_demux_iterate(upipe, p);
        }
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @internal @This frees a uref_demux pipe, once the last output subpipe
 * has been released.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_uref_demux_free(struct upipe *upipe)
{
    struct upipe_uref_demux *upipe_uref_demux =
        upipe_uref_demux_from_upipe(upipe);
    upipe_throw_dead(upipe);

    upipe_uref_demux_clean_flow_defs(&upipe_uref_demux->flow_defs);
    upipe_uref_demux_clean_sub_subs(upipe);
    upipe_uref_demux_clean_output(upipe);
    upipe_uref_demux_clean_urefcount_real(upipe);
    upipe_uref_demux_clean_urefcount(upipe);
    upipe_uref_demux_free_void(upipe);
}

/** @internal @This is called when there is no external reference to the
 * pipe anymore. It ends the output subpipes, which keep the pipe alive until
 * they are released, and announces that there are no flows anymore, so that
 * probes release the subpipes they allocated.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_uref_demux_no_input(struct upipe *upipe)
{
    struct upipe_uref_demux *upipe_uref_demux =
        upipe_uref_demux_from_upipe(upipe);
    upipe_uref_demux_throw_sub_subs(upipe, UPROBE_SOURCE_END);
    upipe_uref_demux_clean_flow_defs(&upipe_uref_demux->flow_defs);
    upipe_split_throw_update(upipe);
    upipe_uref_demux_release_urefcount_real(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_uref_demux_mgr = {
    .refcount = NULL,
    .signature = UPIPE_UREF_DEMUX_SIGNATURE,

    .upipe_alloc = upipe_uref_demux_alloc,
    .upipe_input = upipe_uref_demux_input,
    .upipe_control = upipe_uref_demux_control,

    .upipe_mgr_control = NULL
};

/** @This returns the management structure for uref_demux pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_uref_demux_mgr_alloc(void)
{
    return &upipe_uref_demux_mgr;
}
