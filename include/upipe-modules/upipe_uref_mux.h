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
 * Each input is a subpipe; incoming urefs are merged (@ref uref_sub_merge)
 * into a single carrier uref that is emitted on the main pipe's output. The
 * inverse operation (splitting a carrier uref back into standalone urefs)
 * is left to a separate demux module.
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
