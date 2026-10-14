/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: James Darnley
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe module demultiplexing urefs carrying several ubufs into a main
 * output and several output subpipes, using the additional ubuf features of
 * uref_sub.
 *
 * This is the inverse of upipe_uref_mux. The main output receives each
 * incoming carrier uref with only its primary ubuf, and the input flow
 * definition, both without their per-entry attributes. Each additional entry
 * is extracted (@ref uref_sub_extract) and sent, in entry order, to the output
 * subpipe of its flow ID ("sub[i].f.id"), without that flow ID. Entries
 * whose flow ID has no subpipe are dropped, and entries without a flow ID are
 * dropped with a warning.
 *
 * The flow definitions of the entries, one per flow ID and carrying it, are
 * announced with a split update event whenever they change, and are returned
 * by @ref upipe_split_iterate. An output subpipe is allocated with
 * @ref upipe_flow_alloc_sub and one of them, for instance by a uprobe_selflow
 * probe, and outputs the announced flow definition; there may be one per
 * announced flow ID. A subpipe is ended with a source end event when its flow
 * disappears or changes its "f.def", and its owner is then expected to
 * release it. Releasing the pipe ends all its subpipes and announces that
 * there are no flows anymore.
 */

#ifndef _UPIPE_MODULES_UPIPE_UREF_DEMUX_H_
/** @hidden */
#define _UPIPE_MODULES_UPIPE_UREF_DEMUX_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "upipe/upipe.h"

#define UPIPE_UREF_DEMUX_SIGNATURE     UBASE_FOURCC('u','r','d','x')
#define UPIPE_UREF_DEMUX_SUB_SIGNATURE UBASE_FOURCC('u','r','d','s')

/** @This returns the management structure for uref_demux pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_uref_demux_mgr_alloc(void);

#ifdef __cplusplus
}
#endif
#endif
