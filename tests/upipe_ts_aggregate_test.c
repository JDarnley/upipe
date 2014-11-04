/*
 * Copyright (C) 2013-2014 OpenHeadend S.A.R.L.
 *
 * Authors: Benjamin Cohen
 *          Christophe Massiot
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
 * @short unit tests for TS aggregate module
 */

#undef NDEBUG

#include <upipe/uclock.h>
#include <upipe/uprobe.h>
#include <upipe/uprobe_stdio.h>
#include <upipe/uprobe_prefix.h>
#include <upipe/uprobe_ubuf_mem.h>
#include <upipe/umem.h>
#include <upipe/umem_alloc.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/ubuf.h>
#include <upipe/ubuf_block.h>
#include <upipe/ubuf_block_mem.h>
#include <upipe/uref.h>
#include <upipe/uref_flow.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_block.h>
#include <upipe/uref_clock.h>
#include <upipe/uref_std.h>
#include <upipe/upipe.h>
#include <upipe-ts/upipe_ts_aggregate.h>
#include <upipe-ts/upipe_ts_mux.h>

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>

#include <bitstream/mpeg/ts.h>

#define UDICT_POOL_DEPTH 0
#define UREF_POOL_DEPTH 0
#define UBUF_POOL_DEPTH 0
#define UPROBE_LOG_LEVEL UPROBE_LOG_VERBOSE

#define TS_PER_PACKET 7
#define PACKETS_NUM 45

static unsigned int nb_packets = 0;
static unsigned int nb_padding = 0;

/** definition of our uprobe */
static int catch(struct uprobe *uprobe, struct upipe *upipe,
                 int event, va_list args)
{
    switch (event) {
        default:
            assert(0);
            break;
        case UPROBE_READY:
        case UPROBE_DEAD:
        case UPROBE_NEW_FLOW_DEF:
            break;
    }
    return UBASE_ERR_NONE;
}

/** helper phony pipe to test upipe_ts_agg */
static struct upipe *aggregate_test_alloc(struct upipe_mgr *mgr,
                                          struct uprobe *uprobe,
                                          uint32_t signature, va_list args)
{
    struct upipe *upipe = malloc(sizeof(struct upipe));
    assert(upipe != NULL);
    upipe_init(upipe, mgr, uprobe);
    return upipe;
}

/** helper phony pipe to test upipe_ts_agg */
static void aggregate_test_input(struct upipe *upipe, struct uref *uref,
                          struct upump **upump_p)
{
    assert(uref != NULL);
    const uint8_t *buffer;
    size_t size = 0;
    int pos = 0, len = -1;
    ubase_assert(uref_block_size(uref, &size));
    upipe_dbg_va(upipe, "received packet of size %zu", size);

    while (size > 0) {
        ubase_assert(uref_block_read(uref, pos, &len, &buffer));
        assert(ts_validate(buffer));
        bool padding = ts_get_pid(buffer) == 8191;
        uref_block_unmap(uref, 0);
        size -= len;
        pos += len;
        if (padding)
            nb_padding--;
        else
            nb_packets--;
    }
    uref_free(uref);
    upipe_dbg_va(upipe, "nb_packets %u padding %u", nb_packets, nb_padding);
}

/** helper phony pipe to test upipe_ts_agg */
static void aggregate_test_free(struct upipe *upipe)
{
    upipe_clean(upipe);
    free(upipe);
}

/** helper phony pipe to test upipe_ts_agg */
static struct upipe_mgr aggregate_test_mgr = {
    .refcount = NULL,
    .upipe_alloc = aggregate_test_alloc,
    .upipe_input = aggregate_test_input,
    .upipe_control = NULL
};

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
                                                         umem_mgr, -1, 0);
    assert(ubuf_mgr != NULL);
    struct uprobe uprobe;
    uprobe_init(&uprobe, catch, NULL);
    struct uprobe *logger = uprobe_stdio_alloc(&uprobe, stdout,
                                               UPROBE_LOG_LEVEL);
    assert(logger != NULL);
    logger = uprobe_ubuf_mem_alloc(logger, umem_mgr, UBUF_POOL_DEPTH,
                                   UBUF_POOL_DEPTH);
    assert(logger != NULL);

    /* flow def */
    struct uref *uref;
    uref = uref_block_flow_alloc_def(uref_mgr, "mpegts.");
    assert(uref != NULL);

    struct upipe *upipe_sink = upipe_void_alloc(&aggregate_test_mgr,
                                                uprobe_use(logger));
    assert(upipe_sink != NULL);

    struct upipe_mgr *upipe_ts_agg_mgr = upipe_ts_agg_mgr_alloc();
    assert(upipe_ts_agg_mgr != NULL);
    struct upipe *upipe_ts_agg = upipe_void_alloc(upipe_ts_agg_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL,
                             "aggregate"));
    assert(upipe_ts_agg != NULL);
    ubase_assert(upipe_set_flow_def(upipe_ts_agg, uref));
    ubase_assert(upipe_set_output(upipe_ts_agg, upipe_sink));
    uref_free(uref);

    uint8_t *buffer;
    int size, i;

    /* valid TS packets */
    nb_packets = PACKETS_NUM;
    nb_padding = ((PACKETS_NUM + TS_PER_PACKET - 1) / TS_PER_PACKET) * TS_PER_PACKET - PACKETS_NUM;
    for (i = 0; i < PACKETS_NUM; i++) {
        uref = uref_block_alloc(uref_mgr, ubuf_mgr, TS_SIZE);
        size = -1;
        uref_block_write(uref, 0, &size, &buffer);
        assert(size == TS_SIZE);
        ts_pad(buffer);
        ts_set_pid(buffer, 8190);
        uref_block_unmap(uref, 0);
        upipe_input(upipe_ts_agg, uref, NULL);
    }

    /* flush */
    upipe_release(upipe_ts_agg);

    printf("nb_packets: %u padding: %u\n", nb_packets, nb_padding);
    assert(!nb_packets);
    assert(!nb_padding);

    uref = uref_block_flow_alloc_def(uref_mgr, "mpegts.");
    assert(uref != NULL);

    upipe_ts_agg = upipe_void_alloc(upipe_ts_agg_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL,
                             "aggregate"));
    assert(upipe_ts_agg != NULL);
    ubase_assert(upipe_set_flow_def(upipe_ts_agg, uref));
    ubase_assert(upipe_set_output(upipe_ts_agg, upipe_sink));
    uref_free(uref);
    ubase_assert(upipe_ts_mux_set_mode(upipe_ts_agg, UPIPE_TS_MUX_MODE_CBR));
    ubase_assert(upipe_ts_mux_set_octetrate(upipe_ts_agg, TS_SIZE * TS_PER_PACKET * 10));

    /* valid TS packets */
    nb_packets = PACKETS_NUM;
    nb_padding = (PACKETS_NUM - 1) * (TS_PER_PACKET - 1) - 1;
    for (i = 0; i < PACKETS_NUM; i++) {
        uref = uref_block_alloc(uref_mgr, ubuf_mgr, TS_SIZE);
        size = -1;
        uref_block_write(uref, 0, &size, &buffer);
        assert(size == TS_SIZE);
        ts_pad(buffer);
        ts_set_pid(buffer, 8190);
        uref_block_unmap(uref, 0);
        uref_clock_set_dts_sys(uref, (UCLOCK_FREQ / 10) + (UCLOCK_FREQ / 10) * i);
        uref_clock_set_dts_prog(uref, (UCLOCK_FREQ / 10) + (UCLOCK_FREQ / 10) * i);
        upipe_input(upipe_ts_agg, uref, NULL);
    }

    /* flush */
    upipe_release(upipe_ts_agg);

    printf("nb_packets: %u %u\n", nb_packets, nb_padding);
    assert(!nb_packets);
    assert(!nb_padding);

    uref = uref_block_flow_alloc_def(uref_mgr, "mpegts.");
    assert(uref != NULL);

    upipe_ts_agg = upipe_void_alloc(upipe_ts_agg_mgr,
            uprobe_pfx_alloc(uprobe_use(logger), UPROBE_LOG_LEVEL,
                             "aggregate"));
    assert(upipe_ts_agg != NULL);
    ubase_assert(upipe_set_flow_def(upipe_ts_agg, uref));
    ubase_assert(upipe_set_output(upipe_ts_agg, upipe_sink));
    uref_free(uref);
    ubase_assert(upipe_ts_mux_set_mode(upipe_ts_agg, UPIPE_TS_MUX_MODE_CBR));
    ubase_assert(upipe_ts_mux_set_octetrate(upipe_ts_agg, TS_SIZE * TS_PER_PACKET * 10));

    /* valid TS packets */
    nb_packets = PACKETS_NUM;
    nb_padding = ((PACKETS_NUM + TS_PER_PACKET - 1) / TS_PER_PACKET) * TS_PER_PACKET - PACKETS_NUM;
    for (i = 0; i < PACKETS_NUM; i++) {
        uref = uref_block_alloc(uref_mgr, ubuf_mgr, TS_SIZE);
        size = -1;
        uref_block_write(uref, 0, &size, &buffer);
        assert(size == TS_SIZE);
        ts_pad(buffer);
        ts_set_pid(buffer, 8190);
        uref_block_unmap(uref, 0);
        uref_clock_set_dts_sys(uref, (UCLOCK_FREQ / 10) + (UCLOCK_FREQ / 10) * i / TS_PER_PACKET);
        uref_clock_set_dts_prog(uref, (UCLOCK_FREQ / 10) + (UCLOCK_FREQ / 10) * i / TS_PER_PACKET);
        uref_clock_set_cr_dts_delay(uref, UCLOCK_FREQ / 10);
        upipe_input(upipe_ts_agg, uref, NULL);
    }

    /* flush */
    upipe_release(upipe_ts_agg);

    printf("nb_packets: %u padding:%u\n", nb_packets, nb_padding);
    assert(!nb_packets);
    assert(!nb_padding);

    /* release everything */
    upipe_mgr_release(upipe_ts_agg_mgr); // nop

    aggregate_test_free(upipe_sink);

    uref_mgr_release(uref_mgr);
    ubuf_mgr_release(ubuf_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    uprobe_release(logger);
    uprobe_clean(&uprobe);

    return 0;
}
