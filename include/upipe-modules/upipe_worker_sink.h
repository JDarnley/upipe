/*
 * Copyright (C) 2014 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
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
 * @short Bin pipe wrapping a queue and a sink
 */

#ifndef _UPIPE_MODULES_UPIPE_WORKER_SINK_H_
/** @hidden */
#define _UPIPE_MODULES_UPIPE_WORKER_SINK_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/upipe.h>

#define UPIPE_WSINK_SIGNATURE UBASE_FOURCC('w','s','n','k')

/** @This returns the management structure for all wsink pipes.
 *
 * @param xfer_mgr manager to transfer pipes to the remote thread
 * @return pointer to manager
 */
struct upipe_mgr *upipe_wsink_mgr_alloc(struct upipe_mgr *xfer_mgr);

/** @This extends upipe_mgr_command with specific commands for wsink. */
enum upipe_wsink_mgr_command {
    UPIPE_WSINK_MGR_SENTINEL = UPIPE_MGR_CONTROL_LOCAL,

/** @hidden */
#define UPIPE_WSINK_MGR_GET_SET_MGR(name, NAME)                             \
    /** returns the current manager for name inner pipes                    \
     * (struct upipe_mgr **) */                                             \
    UPIPE_WSINK_MGR_GET_##NAME##_MGR,                                       \
    /** sets the manager for name inner pipes (struct upipe_mgr *) */       \
    UPIPE_WSINK_MGR_SET_##NAME##_MGR,

    UPIPE_WSINK_MGR_GET_SET_MGR(qsrc, QSRC)
    UPIPE_WSINK_MGR_GET_SET_MGR(qsink, QSINK)
#undef UPIPE_WSINK_MGR_GET_SET_MGR
};

/** @hidden */
#define UPIPE_WSINK_MGR_GET_SET_MGR2(name, NAME)                            \
/** @This returns the current manager for name inner pipes.                 \
 *                                                                          \
 * @param mgr pointer to manager                                            \
 * @param p filled in with the name manager                                 \
 * @return an error code                                                    \
 */                                                                         \
static inline int                                                           \
    upipe_wsink_mgr_get_##name##_mgr(struct upipe_mgr *mgr,                 \
                                     struct upipe_mgr *p)                   \
{                                                                           \
    return upipe_mgr_control(mgr, UPIPE_WSINK_MGR_GET_##NAME##_MGR,         \
                             UPIPE_WSINK_SIGNATURE, p);                     \
}                                                                           \
/** @This sets the manager for name inner pipes. This may only be called    \
 * before any pipe has been allocated.                                      \
 *                                                                          \
 * @param mgr pointer to manager                                            \
 * @param m pointer to name manager                                         \
 * @return an error code                                                    \
 */                                                                         \
static inline int                                                           \
    upipe_wsink_mgr_set_##name##_mgr(struct upipe_mgr *mgr,                 \
                                     struct upipe_mgr *m)                   \
{                                                                           \
    return upipe_mgr_control(mgr, UPIPE_WSINK_MGR_SET_##NAME##_MGR,         \
                             UPIPE_WSINK_SIGNATURE, m);                     \
}

UPIPE_WSINK_MGR_GET_SET_MGR2(qsrc, QSRC)
UPIPE_WSINK_MGR_GET_SET_MGR2(qsink, QSINK)
#undef UPIPE_WSINK_MGR_GET_SET_MGR2

/** @This allocates and initializes a worker sink pipe. It allows to
 * transfer an existing sink pipe to a remote upump_mgr, while setting up
 * a queue to send the packets to the sink in the remote upump_mgr.
 *
 * Please note that upipe_remote is not "used" so its refcount is not
 * incremented. For that reason it shouldn't be "released" afterwards. Only
 * release the wsink pipe.
 *
 * @param mgr management structure for queue source type
 * @param uprobe structure used to raise events
 * @param upipe_remote pipe to transfer to remote upump_mgr
 * @param uprobe_remote probe hierarchy to use on the remote thread (belongs to
 * the callee)
 * @param queue_length number of packets in the queue between main and
 * remote thread
 * @return pointer to allocated pipe, or NULL in case of failure
 */
static inline struct upipe *upipe_wsink_alloc(struct upipe_mgr *mgr,
                                              struct uprobe *uprobe,
                                              struct upipe *upipe_remote,
                                              struct uprobe *uprobe_remote,
                                              unsigned int queue_length)
{
    return upipe_alloc(mgr, uprobe, UPIPE_WSINK_SIGNATURE, upipe_remote,
                       uprobe_remote, queue_length);
}

#ifdef __cplusplus
}
#endif
#endif