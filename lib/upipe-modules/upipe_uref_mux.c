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
 * This is currently a skeleton: it sets up the main pipe and its input
 * subpipes and wires request/flow-def propagation, but does not yet perform
 * the actual multiplexing (see @ref uref_sub_merge).
 */

#include "upipe/ubase.h"
#include "upipe/ulist.h"
#include "upipe/uref.h"
#include "upipe/ubuf.h"
#include "upipe/uref_flow.h"
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

/** @internal @This is the private context of a uref_mux pipe. */
struct upipe_uref_mux {
    /** refcount management structure */
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

    /** public upipe structure */
    struct upipe upipe;
};

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

    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This receives data on an input subpipe.
 *
 * @param upipe description structure of the subpipe
 * @param uref uref structure
 * @param upump_p reference to pump that generated the buffer
 */
static void upipe_uref_mux_sub_input(struct upipe *upipe, struct uref *uref,
                                     struct upump **upump_p)
{
    /* TODO: merge this uref into a carrier uref (uref_sub_merge) and, once
     * all inputs have contributed, output the carrier on the main pipe. For
     * now the skeleton simply drops the input. */
    upipe_verbose(upipe, "dropping input uref (mux not implemented yet)");
    uref_free(uref);
}

/** @internal @This sets the input flow definition of a subpipe.
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

    struct uref *dup = uref_dup(flow_def);
    if (unlikely(dup == NULL))
        return UBASE_ERR_ALLOC;
    uref_free(sub->flow_def);
    sub->flow_def = dup;
    return UBASE_ERR_NONE;
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

/** @internal @This frees an input subpipe.
 *
 * @param upipe description structure of the subpipe
 */
static void upipe_uref_mux_sub_free(struct upipe *upipe)
{
    struct upipe_uref_mux_sub *sub = upipe_uref_mux_sub_from_upipe(upipe);
    upipe_throw_dead(upipe);
    uref_free(sub->flow_def);
    upipe_uref_mux_sub_clean_sub(upipe);
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

    upipe_uref_mux_init_urefcount(upipe);
    upipe_uref_mux_init_output(upipe);
    upipe_uref_mux_init_sub_subs(upipe);
    upipe_uref_mux_init_sub_mgr(upipe);

    upipe_throw_ready(upipe);
    return upipe;
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
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

/** @internal @This frees a uref_mux pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_uref_mux_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);

    upipe_uref_mux_clean_sub_subs(upipe);
    upipe_uref_mux_clean_output(upipe);
    upipe_uref_mux_clean_urefcount(upipe);
    upipe_uref_mux_free_void(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_uref_mux_mgr = {
    .refcount = NULL,
    .signature = UPIPE_UREF_MUX_SIGNATURE,

    .upipe_alloc = upipe_uref_mux_alloc,
    .upipe_input = NULL,
    .upipe_control = upipe_uref_mux_control,

    .upipe_mgr_control = NULL
};

/** @This returns the management structure for uref_mux pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_uref_mux_mgr_alloc(void)
{
    return &upipe_uref_mux_mgr;
}
