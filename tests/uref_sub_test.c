/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: Kieran Kunhya
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short unit tests for urefs carrying multiple ubufs, and for flow
 * definitions carrying entries
 */

#undef NDEBUG

#include "upipe/umem.h"
#include "upipe/umem_alloc.h"
#include "upipe/udict.h"
#include "upipe/udict_inline.h"
#include "upipe/ubuf.h"
#include "upipe/ubuf_block.h"
#include "upipe/ubuf_block_mem.h"
#include "upipe/uref.h"
#include "upipe/uref_std.h"
#include "upipe/uref_flow.h"
#include "upipe/uref_block.h"
#include "upipe/uref_clock.h"
#include "upipe/uref_attr.h"
#include "upipe/uref_sub.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

#define UDICT_POOL_DEPTH 5
#define UREF_POOL_DEPTH 5
#define UBUF_POOL_DEPTH 5

/** @This allocates a block ubuf filled with the given octet. */
static struct ubuf *build_block(struct ubuf_mgr *mgr, uint8_t fill, int size)
{
    struct ubuf *ubuf = ubuf_block_alloc(mgr, size);
    assert(ubuf != NULL);
    int wsize = -1;
    uint8_t *w;
    ubase_assert(ubuf_block_write(ubuf, 0, &wsize, &w));
    assert(wsize == size);
    memset(w, fill, wsize);
    ubase_assert(ubuf_block_unmap(ubuf, 0));
    return ubuf;
}

/** @This checks that a block ubuf is filled with the given octet. */
static void check_block(struct ubuf *ubuf, uint8_t fill, int size)
{
    assert(ubuf != NULL);
    int rsize = -1;
    const uint8_t *r;
    ubase_assert(ubuf_block_read(ubuf, 0, &rsize, &r));
    assert(rsize == size);
    for (int i = 0; i < rsize; i++)
        assert(r[i] == fill);
    ubase_assert(ubuf_block_unmap(ubuf, 0));
}

int main(int argc, char **argv)
{
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    struct uref_mgr *mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr, 0);
    assert(mgr != NULL);
    struct ubuf_mgr *ubuf_mgr = ubuf_block_mem_mgr_alloc(UBUF_POOL_DEPTH,
                                                         UBUF_POOL_DEPTH,
                                                         umem_mgr, 0, 0, 0, 0);
    assert(ubuf_mgr != NULL);

    /* attach primary + two additional ubufs */
    struct uref *uref = uref_alloc(mgr);
    assert(uref != NULL);
    assert(uref_sub_count(uref) == 0);
    assert(uref_sub_get(uref, 0) == NULL);
    assert(uref_sub_get(uref, 1) == NULL);

    uref_attach_ubuf(uref, build_block(ubuf_mgr, 'V', 64));
    assert(uref_sub_count(uref) == 1);
    assert(uref_sub_get(uref, 0) == uref->ubuf);

    assert(uref_sub_attach_ubuf(uref, build_block(ubuf_mgr, 'A', 32)) == 1);
    assert(uref_sub_attach_ubuf(uref, build_block(ubuf_mgr, 'S', 16)) == 2);
    assert(uref_sub_count(uref) == 3);
    check_block(uref_sub_get(uref, 0), 'V', 64);
    check_block(uref_sub_get(uref, 1), 'A', 32);
    check_block(uref_sub_get(uref, 2), 'S', 16);
    assert(uref_sub_get(uref, 3) == NULL);

    /* per-entry attributes */
    ubase_assert(uref_sub_set_flow_id(uref, 0x100, 0));
    ubase_assert(uref_sub_set_flow_id(uref, 0x101, 1));
    ubase_assert(uref_sub_set_flow_id(uref, 0x102, 2));
    ubase_assert(uref_sub_set_def(uref, "sound.s32.", 1));

    uint8_t index = 0xff;
    ubase_assert(uref_sub_find_flow_id(uref, 0x101, &index));
    assert(index == 1);
    check_block(uref_sub_get(uref, index), 'A', 32);
    ubase_nassert(uref_sub_find_flow_id(uref, 0xdead, NULL));

    /* uref_dup duplicates the whole chain and its attributes */
    struct uref *dup = uref_dup(uref);
    assert(dup != NULL);
    assert(uref_sub_count(dup) == 3);
    assert(uref_sub_get(dup, 1) != uref_sub_get(uref, 1));
    check_block(uref_sub_get(dup, 0), 'V', 64);
    check_block(uref_sub_get(dup, 1), 'A', 32);
    check_block(uref_sub_get(dup, 2), 'S', 16);
    uint64_t v;
    ubase_assert(uref_sub_get_flow_id(dup, &v, 2));
    assert(v == 0x102);
    const char *def;
    ubase_assert(uref_sub_get_def(dup, &def, 1));
    assert(!strcmp(def, "sound.s32."));
    uref_free(dup);

    /* uref_fork replaces the primary ubuf and keeps the chain */
    struct uref *fork = uref_fork(uref, build_block(ubuf_mgr, 'W', 8));
    assert(fork != NULL);
    assert(uref_sub_count(fork) == 3);
    check_block(uref_sub_get(fork, 0), 'W', 8);
    ubase_assert(uref_sub_find_flow_id(fork, 0x101, &index));
    check_block(uref_sub_get(fork, index), 'A', 32);
    uref_free(fork);

    /* replace an entry in place */
    ubase_assert(uref_sub_replace_ubuf(uref, build_block(ubuf_mgr, 'B', 32),
                                       1));
    check_block(uref_sub_get(uref, 1), 'B', 32);
    assert(uref_sub_count(uref) == 3);
    ubase_nassert(uref_sub_replace_ubuf(uref, build_block(ubuf_mgr, 'X', 8),
                                        42));

    /* extract an entry as a standalone uref: per-entry attributes become
     * plain attributes again, visible to shorthand accessors, and as it was
     * attached without kept fields it gets the dates of the carrier */
    uref->date_prog = 42;
    struct uref *extracted = uref_sub_extract(uref, 1);
    assert(extracted != NULL);
    assert(uref_sub_count(extracted) == 1);
    check_block(extracted->ubuf, 'B', 32);
    assert(extracted->date_prog == 42);
    ubase_assert(uref_flow_get_id(extracted, &v));
    assert(v == 0x101);
    ubase_assert(uref_flow_get_def(extracted, &def));
    assert(!strcmp(def, "sound.s32."));
    uref_free(extracted);

    /* extract entry 0: the plain attributes of the carrier and its own
     * namespace, without other entries' */
    extracted = uref_sub_extract(uref, 0);
    assert(extracted != NULL);
    check_block(extracted->ubuf, 'V', 64);
    ubase_assert(uref_flow_get_id(extracted, &v));
    assert(v == 0x100);
    ubase_nassert(uref_sub_get_flow_id(extracted, &v, 1));
    uref_free(extracted);

    assert(uref_sub_extract(uref, 42) == NULL);

    /* merge a standalone uref as a new entry */
    struct uref *sound = uref_alloc(mgr);
    assert(sound != NULL);
    /* no ubuf, while the carrier has ubufs */
    ubase_nassert(uref_sub_merge(uref, sound, NULL));
    uref_attach_ubuf(sound, build_block(ubuf_mgr, 'M', 24));
    ubase_assert(uref_flow_set_id(sound, 0x200));
    ubase_assert(uref_flow_set_def(sound, "sound.s16."));
    uint8_t merge_index = 0;
    ubase_assert(uref_sub_merge(uref, sound, &merge_index));
    assert(merge_index == 3);
    assert(uref_sub_count(uref) == 4);
    ubase_assert(uref_sub_find_flow_id(uref, 0x200, &merge_index));
    assert(merge_index == 3);
    check_block(uref_sub_get(uref, merge_index), 'M', 24);
    ubase_assert(uref_sub_get_def(uref, &def, 3));
    assert(!strcmp(def, "sound.s16."));

    /* detach an additional entry: its attributes are deleted and following
     * entries' attributes are renumbered */
    struct ubuf *detached = uref_sub_detach_ubuf(uref, 1);
    check_block(detached, 'B', 32);
    ubuf_free(detached);
    assert(uref_sub_count(uref) == 3);
    check_block(uref_sub_get(uref, 1), 'S', 16);
    ubase_assert(uref_sub_get_flow_id(uref, &v, 1));
    assert(v == 0x102);
    ubase_nassert(uref_sub_get_def(uref, &def, 1));
    ubase_assert(uref_sub_get_flow_id(uref, &v, 2));
    assert(v == 0x200);
    ubase_assert(uref_sub_get_def(uref, &def, 2));
    assert(!strcmp(def, "sound.s16."));
    ubase_nassert(uref_sub_get_flow_id(uref, &v, 3));
    ubase_nassert(uref_sub_find_flow_id(uref, 0x101, NULL));
    ubase_assert(uref_sub_find_flow_id(uref, 0x200, &index));
    check_block(uref_sub_get(uref, index), 'M', 24);
    assert(uref_sub_detach_ubuf(uref, 3) == NULL);

    /* legacy detach only touches the primary ubuf */
    detached = uref_detach_ubuf(uref);
    check_block(detached, 'V', 64);
    ubuf_free(detached);
    assert(uref_sub_get(uref, 0) == NULL);
    check_block(uref_sub_get(uref, 1), 'S', 16);

    /* uref_free releases the remaining chain (checked by refcounts on
     * manager release below) */
    uref_free(uref);

    /* a uref without ubufs (here a flow definition) carries entries made
     * only of attributes, with the same functions */
    struct uref *flow_def = uref_alloc_control(mgr);
    assert(flow_def != NULL);
    ubase_assert(uref_flow_set_def(flow_def, "block.mpegts."));
    assert(uref_sub_count(flow_def) == 0);
    ubase_assert(uref_sub_set_flow_id(flow_def, 0x100, 0));
    assert(uref_sub_count(flow_def) == 1);

    struct uref *entry = uref_alloc_control(mgr);
    assert(entry != NULL);
    ubase_assert(uref_flow_set_def(entry, "sound.s32."));
    ubase_assert(uref_flow_set_id(entry, 0x101));
    ubase_assert(uref_flow_set_name(entry, "audio"));
    ubase_assert(uref_flow_set_language(entry, "eng", 0));
    ubase_assert(uref_sub_merge(flow_def, entry, &merge_index));
    assert(merge_index == 1);

    entry = uref_alloc_control(mgr);
    assert(entry != NULL);
    ubase_assert(uref_flow_set_def(entry, "pic.sub."));
    ubase_assert(uref_flow_set_id(entry, 0x102));
    ubase_assert(uref_sub_merge(flow_def, entry, &merge_index));
    assert(merge_index == 2);
    assert(uref_sub_count(flow_def) == 3);
    assert(uref_sub_get(flow_def, 1) == NULL);
    ubase_assert(uref_sub_get_def(flow_def, &def, 1));
    assert(!strcmp(def, "sound.s32."));
    ubase_assert(uref_sub_find_flow_id(flow_def, 0x102, &index));
    assert(index == 2);
    ubase_assert(uref_sub_find_flow_id(flow_def, 0x100, &index));
    assert(index == 0);
    ubase_nassert(uref_sub_find_flow_id(flow_def, 0xdead, NULL));

    /* an entry with a ubuf can't follow entries without ubufs */
    entry = uref_alloc(mgr);
    assert(entry != NULL);
    uref_attach_ubuf(entry, build_block(ubuf_mgr, 'X', 8));
    ubase_nassert(uref_sub_merge(flow_def, entry, NULL));
    uref_free(entry);
    /* an entry without ubuf must have attributes */
    entry = uref_alloc_control(mgr);
    assert(entry != NULL);
    ubase_nassert(uref_sub_merge(flow_def, entry, NULL));
    uref_free(entry);
    assert(uref_sub_count(flow_def) == 3);

    /* extracting an entry restores its plain attributes and shorthands */
    extracted = uref_sub_extract(flow_def, 1);
    assert(extracted != NULL);
    assert(extracted->ubuf == NULL);
    assert(uref_sub_count(extracted) == 0);
    ubase_assert(uref_flow_get_def(extracted, &def));
    assert(!strcmp(def, "sound.s32."));
    ubase_assert(uref_flow_get_id(extracted, &v));
    assert(v == 0x101);
    ubase_assert(uref_flow_get_name(extracted, &def));
    assert(!strcmp(def, "audio"));
    ubase_assert(uref_flow_get_language(extracted, &def, 0));
    assert(!strcmp(def, "eng"));
    uref_free(extracted);

    extracted = uref_sub_extract(flow_def, 0);
    assert(extracted != NULL);
    ubase_assert(uref_flow_get_def(extracted, &def));
    assert(!strcmp(def, "block.mpegts."));
    ubase_assert(uref_flow_get_id(extracted, &v));
    assert(v == 0x100);
    ubase_nassert(uref_sub_get_def(extracted, &def, 1));
    uref_free(extracted);

    assert(uref_sub_extract(flow_def, 3) == NULL);
    uref_free(flow_def);

    /* each entry keeps its own flags, dates and delays, which are fields of
     * the uref rather than attributes */
    uref = uref_alloc(mgr);
    assert(uref != NULL);
    uref_attach_ubuf(uref, build_block(ubuf_mgr, 'P', 16));
    uref_clock_set_pts_prog(uref, 1000);
    uref_clock_set_dts_pts_delay(uref, 10);
    uref_flow_set_random(uref);
    struct uref *carrier_fields = uref_dup(uref);
    assert(carrier_fields != NULL);

    struct uref *own = uref_alloc(mgr);
    assert(own != NULL);
    uref_attach_ubuf(own, build_block(ubuf_mgr, 'Q', 8));
    uref_clock_set_cr_sys(own, 2001);
    uref_clock_set_pts_prog(own, 2002);
    uref_clock_set_dts_orig(own, 2003);
    uref_clock_set_dts_pts_delay(own, 2004);
    uref_clock_set_cr_dts_delay(own, 2005);
    uref_clock_set_rap_cr_delay(own, 2006);
    uref_flow_set_discontinuity(own);
    uref_block_set_start(own);
    struct uref *own_fields = uref_dup(own);
    assert(own_fields != NULL);
    ubase_assert(uref_sub_merge(uref, own, &merge_index));
    assert(merge_index == 1);

    /* the carrier's own fields are unchanged */
    assert(uref->flags == carrier_fields->flags);
    assert(uref->date_sys == carrier_fields->date_sys);
    assert(uref->date_prog == carrier_fields->date_prog);
    assert(uref->dts_pts_delay == carrier_fields->dts_pts_delay);

    extracted = uref_sub_extract(uref, 1);
    assert(extracted != NULL);
    check_block(extracted->ubuf, 'Q', 8);
    assert(extracted->flags == own_fields->flags);
    assert(extracted->date_sys == own_fields->date_sys);
    assert(extracted->date_prog == own_fields->date_prog);
    assert(extracted->date_orig == own_fields->date_orig);
    assert(extracted->dts_pts_delay == own_fields->dts_pts_delay);
    assert(extracted->cr_dts_delay == own_fields->cr_dts_delay);
    assert(extracted->rap_cr_delay == own_fields->rap_cr_delay);
    /* they don't show up as attributes */
    ubase_nassert(uref_attr_get_unsigned(extracted, &v, UDICT_TYPE_UNSIGNED,
                                         "uref.flags"));
    uref_free(extracted);

    /* an entry attached without them gets those of the carrier */
    assert(uref_sub_attach_ubuf(uref, build_block(ubuf_mgr, 'R', 4)) == 2);
    extracted = uref_sub_extract(uref, 2);
    assert(extracted != NULL);
    assert(extracted->flags == uref->flags);
    assert(extracted->date_prog == uref->date_prog);
    assert(extracted->dts_pts_delay == uref->dts_pts_delay);
    uref_free(extracted);

    /* entry 0 has the carrier's */
    extracted = uref_sub_extract(uref, 0);
    assert(extracted != NULL);
    assert(extracted->flags == uref->flags);
    assert(extracted->date_prog == uref->date_prog);
    uref_free(extracted);

    /* detaching an entry deletes its kept fields along with its other
     * attributes; the entry renumbered in its place had none */
    ubuf_free(uref_sub_detach_ubuf(uref, 1));
    ubase_nassert(uref_attr_get_unsigned(uref, &v, UDICT_TYPE_UNSIGNED,
                                         "sub[1].uref.flags"));
    ubase_nassert(uref_attr_get_unsigned(uref, &v, UDICT_TYPE_UNSIGNED,
                                         "sub[2].uref.flags"));
    uref_free(uref);
    uref_free(own_fields);
    uref_free(carrier_fields);

    /* a uref holds at most UREF_SUB_MAX_ENTRIES entries, including entry 0 */
    uref = uref_alloc(mgr);
    assert(uref != NULL);
    uref_attach_ubuf(uref, build_block(ubuf_mgr, 'L', 4));
    for (unsigned i = 1; i < UREF_SUB_MAX_ENTRIES; i++) {
        entry = uref_alloc(mgr);
        assert(entry != NULL);
        uref_attach_ubuf(entry, build_block(ubuf_mgr, 'l', 4));
        ubase_assert(uref_sub_merge(uref, entry, &merge_index));
        assert(merge_index == i);
    }
    assert(uref_sub_count(uref) == UREF_SUB_MAX_ENTRIES);
    entry = uref_alloc(mgr);
    assert(entry != NULL);
    uref_attach_ubuf(entry, build_block(ubuf_mgr, 'x', 4));
    ubase_nassert(uref_sub_merge(uref, entry, NULL));
    uref_free(entry);
    assert(uref_sub_count(uref) == UREF_SUB_MAX_ENTRIES);
    uref_free(uref);

    ubuf_mgr_release(ubuf_mgr);
    uref_mgr_release(mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    return 0;
}
