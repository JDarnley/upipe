/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: James Darnley
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe module multiplexing urefs from several input subpipes into a
 * single output, using the additional ubuf features of uref_sub.
 *
 * The main pipe's input is the reference flow: each uref it receives becomes
 * a carrier uref, emitted on the main pipe's output, into which the urefs
 * held by the input subpipes are merged (@ref uref_sub_merge) as additional
 * entries. All the urefs an input receives between two urefs of the main
 * input are carried by the next one, in arrival order, so an input may have
 * several entries in a carrier, for instance when its frames are shorter than
 * the main ones. The flow definitions of the inputs are likewise merged into
 * the output flow definition, which is otherwise that of the main input.
 *
 * Every flow, main or input, is identified by the flow ID ("f.id") of its
 * flow definition, which must be present and distinct: a flow definition
 * without a flow ID, or reusing the flow ID of another flow, is rejected.
 * Every entry, in the carriers and in the output flow definition, carries the
 * flow ID of its flow ("sub[i].f.id"); the urefs of an input are tagged
 * with it, replacing any flow ID of their own.
 *
 * An input holds at most UREF_SUB_MAX_ENTRIES - 1 urefs, as many as a carrier
 * can carry. Further urefs, urefs which don't fit in the carrier with those
 * of the inputs before it, and urefs still held when the input gets a new
 * flow definition or is released, are dropped with a warning.
 *
 * upipe_uref_demux performs the inverse operation.
 */

#ifndef _UPIPE_MODULES_UPIPE_UREF_MUX_H_
/** @hidden */
#define _UPIPE_MODULES_UPIPE_UREF_MUX_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "upipe/upipe.h"

#define UPIPE_UREF_MUX_SIGNATURE     UBASE_FOURCC('u','r','m','x')
#define UPIPE_UREF_MUX_SUB_SIGNATURE UBASE_FOURCC('u','r','m','s')

/** @This returns the management structure for uref_mux pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_uref_mux_mgr_alloc(void);

#ifdef __cplusplus
}
#endif
#endif
