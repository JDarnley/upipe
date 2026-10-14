/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: James Darnley
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short unit tests for uref_demux pipes
 *
 * The inputs are built by hand: per-entry attributes are set by name and the
 * additional ubufs are chained with uref_sub_attach_ubuf, without
 * uref_sub_merge, so that the format expected by the demux is checked
 * independently of the way upipe_uref_mux builds it. The outputs are checked
 * with plain accessors for the same reason.
 *
 * It covers the flow definitions announced to allocate subpipes, the
 * allocation rules, the updates and ends of subpipes when the input flow
 * definition changes, the routing of the entries and the cleaning of the
 * main output, the warnings for dropped entries, the release of the demux,
 * subpipes allocated both by uprobe_selflow and by hand, and flow IDs at both
 * ends of their range.
 */

#undef NDEBUG

#include "upipe/uprobe.h"
#include "upipe/ulog.h"
#include "upipe/uprobe_stdio.h"
#include "upipe/uprobe_prefix.h"
#include "upipe/uprobe_select_flows.h"
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
#include "upipe/urequest.h"
#include "upipe/upipe.h"
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

/* flow IDs of the main flow and of the entries: the entry of ID_NODEF has
 * no f.def, and ID_OTHER is never announced */
#define ID_MAIN    256
#define ID_SOUND   200
#define ID_PIC     300
#define ID_DATA    400
#define ID_NODEF   500
#define ID_OTHER   999

/** maximum number of subpipes the test tracks over its whole run */
#define MAX_SUBS    32

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
    /** number of urefs received */
    unsigned count;
    /** public upipe structure */
    struct upipe upipe;
};

/** set when a uref_mgr request reaches the sink */
static bool request_forwarded = false;
/** pipe released by the next sink receiving a uref */
static struct upipe *release_on_input = NULL;

static void sink_free(struct urefcount *urefcount)
{
    struct sink *sink = container_of(urefcount, struct sink, urefcount);
    uref_free(sink->flow_def);
    uref_free(sink->uref);
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
    uref_free(sink->uref);
    sink->uref = uref;
    sink->count++;
    /* as a pipeline after a subpipe might, release a pipe while the demux
     * is still sending the entries of the same carrier */
    if (release_on_input != NULL) {
        struct upipe *release = release_on_input;
        release_on_input = NULL;
        upipe_release(release);
    }
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
        case UPIPE_REGISTER_REQUEST: {
            /* answer uref manager requests, to check that those registered
             * on the demux reach its output */
            struct urequest *urequest = va_arg(args, struct urequest *);
            if (urequest->type != UREQUEST_UREF_MGR)
                return UBASE_ERR_NONE;
            request_forwarded = true;
            return urequest_provide_uref_mgr(urequest, uref_mgr_use(uref_mgr));
        }
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

/** sequence number of the events, to check their order */
static unsigned event_seq = 0;

/** demux under test */
static struct upipe *demux_pipe = NULL;
/** number of split update events thrown by the demux */
static unsigned split_updates = 0;
/** sequence number of the last split update event */
static unsigned split_update_seq = 0;
/** number of new flow def events thrown by the demux (main output) */
static unsigned main_new_flow_defs = 0;
/** true once the demux is dead */
static bool demux_dead = false;
/** number of warnings logged by the demux */
static unsigned main_warnings = 0;

/** state of an output subpipe */
struct sub_state {
    /** subpipe, or NULL once dead */
    struct upipe *upipe;
    /** sink attached when the subpipe needed an output */
    struct sink *sink;
    /** flow ID of its flow def, valid once has_flow_id is set */
    uint64_t flow_id;
    /** true once the subpipe has a flow def */
    bool has_flow_id;
    /** true if the test owns the subpipe and releases it on source end */
    bool release_on_end;
    /** numbers of events */
    unsigned ready, new_flow_defs, need_outputs, source_ends, dead;
    /** sequence number of the source end event */
    unsigned source_end_seq;
};

static struct sub_state subs[MAX_SUBS];
static unsigned nb_subs = 0;

/** returns the state of a subpipe */
static struct sub_state *sub_state(struct upipe *upipe)
{
    for (unsigned i = 0; i < nb_subs; i++)
        if (subs[i].upipe == upipe)
            return &subs[i];
    return NULL;
}

/** returns the state of the subpipe currently outputting a flow ID */
static struct sub_state *live_sub(uint64_t flow_id)
{
    struct sub_state *found = NULL;
    for (unsigned i = 0; i < nb_subs; i++)
        if (subs[i].upipe != NULL && subs[i].has_flow_id &&
            subs[i].flow_id == flow_id && !subs[i].source_ends) {
            assert(found == NULL);
            found = &subs[i];
        }
    return found;
}

/** last probe of the chains: no errors are expected */
static int catch_base(struct uprobe *uprobe, struct upipe *upipe,
                      int event, va_list args)
{
    assert(event != UPROBE_FATAL && event != UPROBE_ERROR);
    return UBASE_ERR_NONE;
}

/** probe of the demux under test, after uprobe_selflow in the chain of the
 * first one: records the split updates, new flow defs, warnings and death of
 * the demux */
static int catch_main(struct uprobe *uprobe, struct upipe *upipe,
                      int event, va_list args)
{
    event_seq++;
    if (upipe == NULL || upipe != demux_pipe)
        return uprobe_throw_next(uprobe, upipe, event, args);

    switch (event) {
        case UPROBE_SPLIT_UPDATE:
            split_updates++;
            split_update_seq = event_seq;
            return UBASE_ERR_NONE;
        case UPROBE_NEW_FLOW_DEF:
            main_new_flow_defs++;
            return UBASE_ERR_NONE;
        case UPROBE_DEAD:
            demux_dead = true;
            return UBASE_ERR_NONE;
        case UPROBE_LOG: {
            va_list args_copy;
            va_copy(args_copy, args);
            struct ulog *ulog = va_arg(args_copy, struct ulog *);
            if (ulog->level == UPROBE_LOG_WARNING)
                main_warnings++;
            va_end(args_copy);
            return uprobe_throw_next(uprobe, upipe, event, args);
        }
        default:
            return uprobe_throw_next(uprobe, upipe, event, args);
    }
}

/** probe of the output subpipes, whoever allocates them: keeps a state for
 * each subpipe, attaches a sink when one needs an output, and releases those
 * the test owns when they are ended */
static int catch_sub(struct uprobe *uprobe, struct upipe *upipe,
                     int event, va_list args)
{
    event_seq++;
    /* the state is created when the subpipe is ready, before its allocation
     * returns, so it also covers the subpipes which are then rejected */
    if (event == UPROBE_READY) {
        assert(sub_state(upipe) == NULL);
        assert(nb_subs < MAX_SUBS);
        struct sub_state *state = &subs[nb_subs++];
        memset(state, 0, sizeof(*state));
        state->upipe = upipe;
        state->ready++;
        return UBASE_ERR_NONE;
    }

    struct sub_state *state = sub_state(upipe);
    if (state == NULL)
        return uprobe_throw_next(uprobe, upipe, event, args);

    switch (event) {
        case UPROBE_DEAD:
            state->dead++;
            state->upipe = NULL;
            state->sink = NULL;
            return UBASE_ERR_NONE;
        case UPROBE_NEW_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            state->new_flow_defs++;
            ubase_assert(uref_flow_get_id(flow_def, &state->flow_id));
            state->has_flow_id = true;
            return UBASE_ERR_NONE;
        }
        case UPROBE_NEED_OUTPUT: {
            state->need_outputs++;
            struct upipe *sink = upipe_void_alloc(&sink_mgr,
                                                  uprobe_use(logger));
            assert(sink != NULL);
            ubase_assert(upipe_set_output(upipe, sink));
            state->sink = container_of(sink, struct sink, upipe);
            upipe_release(sink);
            return UBASE_ERR_NONE;
        }
        case UPROBE_SOURCE_END:
            state->source_ends++;
            state->source_end_seq = event_seq;
            if (state->release_on_end)
                upipe_release(upipe);
            return UBASE_ERR_NONE;
        default:
            return uprobe_throw_next(uprobe, upipe, event, args);
    }
}

static struct uprobe main_probe;
static struct uprobe sub_probe;

/** set when the uref_mgr request is provided */
static bool uref_mgr_provided = false;

/** receives the uref manager provided for the request registered on the
 * demux */
static int provide_uref_mgr(struct urequest *urequest, va_list args)
{
    struct uref_mgr *mgr = va_arg(args, struct uref_mgr *);
    assert(mgr == uref_mgr);
    uref_mgr_release(mgr);
    uref_mgr_provided = true;
    return UBASE_ERR_NONE;
}

/*
 * Inputs, built by hand
 */

/** sets a per-entry string attribute by name */
static void set_entry_string(struct uref *uref, uint8_t index,
                             const char *name, const char *value)
{
    char attr[64];
    snprintf(attr, sizeof(attr), "sub[%" PRIu8 "].%s", index, name);
    ubase_assert(uref_attr_set_string(uref, value, UDICT_TYPE_STRING, attr));
}

/** sets a per-entry unsigned attribute by name */
static void set_entry_unsigned(struct uref *uref, uint8_t index,
                               const char *name, uint64_t value)
{
    char attr[64];
    snprintf(attr, sizeof(attr), "sub[%" PRIu8 "].%s", index, name);
    ubase_assert(uref_attr_set_unsigned(uref, value, UDICT_TYPE_UNSIGNED,
                                        attr));
}

/** allocates the flow def of the main flow, tagged as entry 0 */
static struct uref *main_flow_def(void)
{
    struct uref *flow_def = uref_block_flow_alloc_def(uref_mgr, "main.");
    assert(flow_def != NULL);
    ubase_assert(uref_flow_set_id(flow_def, ID_MAIN));
    ubase_assert(uref_flow_set_name(flow_def, "main"));
    set_entry_unsigned(flow_def, 0, "f.id", ID_MAIN);
    return flow_def;
}

/** adds the flow def of an entry without flow ID to a carrier flow def, as
 * upipe_uref_mux would merge it: an f.def unless def is NULL, and an optional
 * name */
static void add_untagged_entry_flow_def(struct uref *flow_def, uint8_t index,
                                        const char *def, const char *name)
{
    if (def != NULL)
        set_entry_string(flow_def, index, "f.def", def);
    if (name != NULL)
        set_entry_string(flow_def, index, "f.name", name);
}

/** adds the flow def of an entry to a carrier flow def, as upipe_uref_mux
 * would merge it, with its flow ID */
static void add_entry_flow_def(struct uref *flow_def, uint8_t index,
                               uint64_t flow_id, const char *def,
                               const char *name)
{
    add_untagged_entry_flow_def(flow_def, index, def, name);
    set_entry_unsigned(flow_def, index, "f.id", flow_id);
}

/** builds a carrier flow def with entries for ID_SOUND and ID_PIC (unless
 * their f.def is NULL) and ID_DATA */
static struct uref *build_flow_def(const char *sound_def,
                                   const char *sound_name,
                                   const char *pic_def)
{
    struct uref *flow_def = main_flow_def();
    uint8_t index = 1;
    if (sound_def != NULL)
        add_entry_flow_def(flow_def, index++, ID_SOUND, sound_def,
                           sound_name);
    if (pic_def != NULL)
        add_entry_flow_def(flow_def, index++, ID_PIC, pic_def, NULL);
    add_entry_flow_def(flow_def, index++, ID_DATA, "block.data.", NULL);
    return flow_def;
}

/** allocates a block ubuf filled with a byte */
static struct ubuf *block_ubuf(char c, int size)
{
    struct ubuf *ubuf = ubuf_block_alloc(ubuf_mgr, size);
    assert(ubuf != NULL);
    uint8_t *buffer;
    int wsize = -1;
    ubase_assert(ubuf_block_write(ubuf, 0, &wsize, &buffer));
    assert(wsize == size);
    memset(buffer, c, size);
    ubase_assert(ubuf_block_unmap(ubuf, 0));
    return ubuf;
}

/** allocates a carrier uref with its primary ubuf, a date, a flag and an
 * attribute, tagged as entry 0 */
static struct uref *carrier(char c, int size, uint64_t pts)
{
    struct uref *uref = uref_alloc(uref_mgr);
    assert(uref != NULL);
    uref_attach_ubuf(uref, block_ubuf(c, size));
    uref_clock_set_pts_prog(uref, pts);
    uref_flow_set_random(uref);
    ubase_assert(uref_attr_set_unsigned(uref, pts, UDICT_TYPE_UNSIGNED,
                                        "x.main"));
    set_entry_unsigned(uref, 0, "f.id", ID_MAIN);
    return uref;
}

/** appends an entry without flow ID to a carrier uref, with its byte as an
 * attribute; entries must be added in index order, and no flags, dates or
 * delays are kept for them */
static void add_untagged_entry(struct uref *uref, uint8_t index, char c,
                               int size)
{
    assert(uref_sub_attach_ubuf(uref, block_ubuf(c, size)) == index);
    set_entry_unsigned(uref, index, "x.tag", (uint8_t)c);
}

/** appends an entry to a carrier uref, as add_untagged_entry, with its flow
 * ID */
static void add_entry(struct uref *uref, uint8_t index, uint64_t flow_id,
                      char c, int size)
{
    add_untagged_entry(uref, index, c, size);
    set_entry_unsigned(uref, index, "f.id", flow_id);
}

/*
 * Checks
 */

/** checks that a uref has neither additional ubufs nor per-entry
 * attributes */
static void check_no_entries(struct uref *uref)
{
    assert(ulist_empty(&uref->sub_ubufs));
    if (uref->udict == NULL)
        return;
    const char *name = NULL;
    enum udict_type type = UDICT_TYPE_END;
    for ( ; ; ) {
        udict_iterate(uref->udict, &name, &type);
        if (type == UDICT_TYPE_END)
            break;
        assert(name == NULL || strncmp(name, "sub[", 4));
    }
}

/** deletes the per-entry attributes of a uref */
static void strip_entries(struct uref *uref)
{
    const char *name = NULL;
    enum udict_type type = UDICT_TYPE_END;
    for ( ; ; ) {
        udict_iterate(uref->udict, &name, &type);
        if (type == UDICT_TYPE_END)
            break;
        if (name != NULL && !strncmp(name, "sub[", 4)) {
            char copy[64];
            snprintf(copy, sizeof(copy), "%s", name);
            ubase_assert(uref_attr_delete(uref, type, copy));
            /* deleting invalidates the iteration, so restart it */
            name = NULL;
            type = UDICT_TYPE_END;
        }
    }
}

/** checks that a uref carries a block of a byte, and that it may be written,
 * which requires its ubuf not to be shared */
static void check_block(struct uref *uref, char c, int size)
{
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
}

/** checks the last uref received by the sink of a subpipe for an entry */
static void check_entry(struct sub_state *state, unsigned count, char c,
                        int size, uint64_t pts)
{
    assert(state->sink != NULL && state->sink->count == count);
    struct uref *uref = state->sink->uref;
    check_block(uref, c, size);
    check_no_entries(uref);
    uint64_t v;
    ubase_assert(uref_attr_get_unsigned(uref, &v, UDICT_TYPE_UNSIGNED,
                                        "x.tag"));
    assert(v == (uint8_t)c);
    /* entries were added without kept flags, dates and delays, so they get
     * those of the carrier */
    ubase_assert(uref_clock_get_pts_prog(uref, &v));
    assert(v == pts);
    ubase_assert(uref_flow_get_random(uref));
    /* the flow ID only identified the entry in the carrier, and the
     * attributes of the main flow aren't carried */
    ubase_nassert(uref_flow_get_id(uref, &v));
    ubase_nassert(uref_attr_get_unsigned(uref, &v, UDICT_TYPE_UNSIGNED,
                                         "x.main"));
}

/** returns the flow def announced for a flow ID */
static struct uref *announced(struct upipe *demux, uint64_t flow_id)
{
    struct uref *flow_def = NULL;
    while (ubase_check(upipe_split_iterate(demux, &flow_def)) &&
           flow_def != NULL) {
        uint64_t id;
        if (ubase_check(uref_flow_get_id(flow_def, &id)) && id == flow_id)
            return flow_def;
    }
    return NULL;
}

/** returns the number of announced flow defs */
static unsigned nb_announced(struct upipe *demux)
{
    unsigned nb = 0;
    struct uref *flow_def = NULL;
    while (ubase_check(upipe_split_iterate(demux, &flow_def)) &&
           flow_def != NULL)
        nb++;
    return nb;
}

/** checks a flow def announced for a flow ID */
static void check_announced(struct upipe *demux, uint64_t flow_id,
                            const char *def, const char *name)
{
    struct uref *flow_def = announced(demux, flow_id);
    assert(flow_def != NULL);
    check_no_entries(flow_def);
    const char *s;
    ubase_assert(uref_flow_get_def(flow_def, &s));
    assert(!strcmp(s, def));
    uint64_t v;
    ubase_assert(uref_flow_get_id(flow_def, &v));
    assert(v == flow_id);
    if (name != NULL) {
        ubase_assert(uref_flow_get_name(flow_def, &s));
        assert(!strcmp(s, name));
    } else
        ubase_nassert(uref_flow_get_name(flow_def, &s));
}

/** allocates an output subpipe as a user would, without uprobe_selflow */
static struct upipe *alloc_sub(struct upipe *demux, struct uref *flow_def,
                               const char *name)
{
    return upipe_flow_alloc_sub(demux,
            uprobe_pfx_alloc(uprobe_use(&sub_probe), UPROBE_LOG_LEVEL, name),
            flow_def);
}

/** sets a flow def on the demux, and frees it */
static void set_flow_def(struct upipe *demux, struct uref *flow_def)
{
    ubase_assert(upipe_set_flow_def(demux, flow_def));
    uref_free(flow_def);
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
    uprobe_init(&main_probe, catch_main, uprobe_use(logger));
    uprobe_init(&sub_probe, catch_sub, uprobe_use(logger));

    struct upipe *sink = upipe_void_alloc(&sink_mgr, uprobe_use(logger));
    assert(sink != NULL);
    struct sink *main_sink = container_of(sink, struct sink, upipe);

    /* uprobe_selflow allocates the subpipes of sound flows, the test
     * allocates the others */
    struct uprobe *selflow = uprobe_selflow_alloc(uprobe_use(&main_probe),
                                                  uprobe_use(&sub_probe),
                                                  UPROBE_SELFLOW_SOUND,
                                                  "all");
    assert(selflow != NULL);
    struct upipe_mgr *upipe_uref_demux_mgr = upipe_uref_demux_mgr_alloc();
    assert(upipe_uref_demux_mgr != NULL);
    struct upipe *demux = upipe_void_alloc(upipe_uref_demux_mgr,
            uprobe_pfx_alloc(selflow, UPROBE_LOG_LEVEL, "demux"));
    assert(demux != NULL);
    demux_pipe = demux;
    ubase_assert(upipe_set_output(demux, sink));

    /* nothing is announced before a flow def, so no subpipe can be
     * allocated, and data is dropped */
    assert(nb_announced(demux) == 0);
    struct uref *flow_def = uref_block_flow_alloc_def(uref_mgr, "x.");
    assert(flow_def != NULL);
    ubase_assert(uref_flow_set_id(flow_def, ID_PIC));
    assert(alloc_sub(demux, flow_def, "too early") == NULL);
    uref_free(flow_def);
    upipe_input(demux, carrier('Z', 10, 0), NULL);
    assert(main_sink->count == 0);

    /* requests are forwarded to the main output */
    struct urequest request;
    urequest_init_uref_mgr(&request, provide_uref_mgr, NULL);
    ubase_assert(upipe_register_request(demux, &request));
    assert(request_forwarded && uref_mgr_provided);
    ubase_assert(upipe_unregister_request(demux, &request));
    urequest_clean(&request);

    /* a flow def with entries for ID_SOUND, ID_PIC and ID_DATA, and
     * entries without flow ID, without f.def and repeating a flow ID, which
     * are skipped */
    flow_def = build_flow_def("sound.s32.", "audio", "pic.");
    add_untagged_entry_flow_def(flow_def, 4, "block.noid.", NULL);
    add_entry_flow_def(flow_def, 5, ID_NODEF, NULL, NULL);
    add_entry_flow_def(flow_def, 6, ID_PIC, "block.dup.", NULL);
    ubase_assert(upipe_set_flow_def(demux, flow_def));

    /* the main output gets the flow def without the entries */
    assert(main_new_flow_defs == 1);
    struct uref *output_flow_def;
    ubase_assert(upipe_get_flow_def(demux, &output_flow_def));
    check_no_entries(output_flow_def);
    strip_entries(flow_def);
    assert(!udict_cmp(output_flow_def->udict, flow_def->udict));
    uref_free(flow_def);

    /* one flow def per valid entry is announced */
    assert(split_updates == 1);
    assert(nb_announced(demux) == 3);
    check_announced(demux, ID_SOUND, "sound.s32.", "audio");
    check_announced(demux, ID_PIC, "pic.", NULL);
    check_announced(demux, ID_DATA, "block.data.", NULL);
    assert(announced(demux, ID_NODEF) == NULL);

    /* uprobe_selflow allocated a subpipe for the sound flow */
    struct sub_state *sound = live_sub(ID_SOUND);
    assert(sound != NULL && sound->ready == 1 && sound->new_flow_defs == 1);

    /* the same entries again, without the invalid ones: nothing changes */
    set_flow_def(demux, build_flow_def("sound.s32.", "audio", "pic."));
    assert(split_updates == 1 && main_new_flow_defs == 1);
    assert(sound->new_flow_defs == 1);

    /* a subpipe needs a flow def with an announced flow ID which has no
     * subpipe yet */
    assert(upipe_void_alloc_sub(demux,
                uprobe_pfx_alloc(uprobe_use(&sub_probe), UPROBE_LOG_LEVEL,
                                 "void")) == NULL);
    flow_def = uref_block_flow_alloc_def(uref_mgr, "x.");
    assert(flow_def != NULL);
    assert(alloc_sub(demux, flow_def, "no flow ID") == NULL);
    ubase_assert(uref_flow_set_id(flow_def, ID_OTHER));
    assert(alloc_sub(demux, flow_def, "not announced") == NULL);
    ubase_assert(uref_flow_set_id(flow_def, ID_SOUND));
    assert(alloc_sub(demux, flow_def, "sound again") == NULL);

    /* the announced flow def is used, not the one passed */
    ubase_assert(uref_flow_set_id(flow_def, ID_PIC));
    struct upipe *pic = alloc_sub(demux, flow_def, "pic");
    assert(pic != NULL);
    uref_free(flow_def);
    struct sub_state *pic_state = sub_state(pic);
    assert(pic_state != NULL && pic_state->flow_id == ID_PIC);
    ubase_assert(upipe_get_flow_def(pic, &output_flow_def));
    assert(!udict_cmp(output_flow_def->udict,
                      announced(demux, ID_PIC)->udict));
    assert(alloc_sub(demux, announced(demux, ID_PIC), "pic again") == NULL);

    /* subpipe controls */
    struct upipe *super = NULL;
    ubase_assert(upipe_sub_get_super(pic, &super));
    assert(super == demux);
    unsigned nb = 0;
    struct upipe *iter = NULL;
    while (ubase_check(upipe_iterate_sub(demux, &iter)) && iter != NULL)
        nb++;
    assert(nb == 2);

    /* each entry goes to the subpipe of its flow ID, which then asks for an
     * output; entries without a subpipe are dropped, and so are those
     * without a flow ID, with a warning; the main output gets the same uref
     * with only its primary ubuf */
    struct uref *uref = carrier('M', 100, 1000);
    add_entry(uref, 1, ID_SOUND, 'A', 64);
    add_entry(uref, 2, ID_PIC, 'B', 32);
    add_entry(uref, 3, ID_DATA, 'C', 16);
    add_untagged_entry(uref, 4, 'D', 8);
    struct uref *main_uref = uref;
    unsigned warnings = main_warnings;
    upipe_input(demux, uref, NULL);
    assert(main_warnings == warnings + 1);
    assert(sound->need_outputs == 1 && pic_state->need_outputs == 1);
    check_entry(sound, 1, 'A', 64, 1000);
    check_entry(pic_state, 1, 'B', 32, 1000);
    assert(main_sink->count == 1 && main_sink->uref == main_uref);
    check_block(main_uref, 'M', 100);
    check_no_entries(main_uref);
    uint64_t v;
    ubase_assert(uref_attr_get_unsigned(main_uref, &v, UDICT_TYPE_UNSIGNED,
                                        "x.main"));
    assert(v == 1000);
    ubase_assert(uref_clock_get_pts_prog(main_uref, &v));
    assert(v == 1000);
    ubase_assert(uref_flow_get_random(main_uref));

    /* a uref without entries only goes to the main output */
    upipe_input(demux, carrier('N', 50, 2000), NULL);
    assert(main_sink->count == 2);
    check_block(main_sink->uref, 'N', 50);
    assert(sound->sink->count == 1 && pic_state->sink->count == 1);

    /* entries are routed by flow ID, whatever their index */
    uref = carrier('O', 50, 3000);
    add_entry(uref, 1, ID_PIC, 'E', 24);
    upipe_input(demux, uref, NULL);
    check_entry(pic_state, 2, 'E', 24, 3000);
    assert(sound->sink->count == 1);

    /* entries with the same flow ID are sent in entry order */
    uref = carrier('S', 50, 3500);
    add_entry(uref, 1, ID_PIC, 'X', 6);
    add_entry(uref, 2, ID_SOUND, 'W', 4);
    add_entry(uref, 3, ID_PIC, 'Y', 7);
    upipe_input(demux, uref, NULL);
    check_entry(pic_state, 4, 'Y', 7, 3500);
    check_entry(sound, 2, 'W', 4, 3500);

    /* an entry whose subpipe is released while the previous entries are
     * being sent is dropped with a warning: here the picture sink releases
     * the data subpipe when it receives the picture entry */
    struct upipe *gone = alloc_sub(demux, announced(demux, ID_DATA),
                                   "gone");
    assert(gone != NULL);
    struct sub_state *gone_state = sub_state(gone);
    release_on_input = gone;
    uref = carrier('T', 50, 3700);
    add_entry(uref, 1, ID_PIC, 'Z', 5);
    add_entry(uref, 2, ID_DATA, 'V', 3);
    warnings = main_warnings;
    upipe_input(demux, uref, NULL);
    assert(release_on_input == NULL && gone_state->dead == 1);
    assert(gone_state->need_outputs == 0);
    assert(main_warnings == warnings + 1);
    check_entry(pic_state, 5, 'Z', 5, 3700);

    /* an attribute change with the same f.def is applied in place: the
     * subpipe's new flow def reaches its sink with the next uref, and the
     * main output flow def doesn't change */
    set_flow_def(demux, build_flow_def("sound.s32.", "audio2", "pic."));
    assert(split_updates == 2 && main_new_flow_defs == 1);
    assert(sound->new_flow_defs == 2 && sound->source_ends == 0);
    uref = carrier('P', 50, 4000);
    add_entry(uref, 1, ID_SOUND, 'F', 12);
    upipe_input(demux, uref, NULL);
    check_entry(sound, 3, 'F', 12, 4000);
    const char *name;
    ubase_assert(uref_flow_get_name(sound->sink->flow_def, &name));
    assert(!strcmp(name, "audio2"));

    /* f.def changes end the subpipes: uprobe_selflow releases the sound
     * subpipe and allocates a new one, the test keeps the old picture
     * subpipe, which gets no more data, and allocates a new one */
    set_flow_def(demux, build_flow_def("sound.s16.", "audio2", "pic.other."));
    assert(sound->source_ends == 1 && sound->dead == 1);
    assert(pic_state->source_ends == 1 && pic_state->dead == 0);
    assert(split_updates == 3);
    struct sub_state *sound2 = live_sub(ID_SOUND);
    assert(sound2 != NULL && sound2 != sound && sound2->ready == 1);
    struct upipe *pic2 = alloc_sub(demux, announced(demux, ID_PIC),
                                   "pic 2");
    assert(pic2 != NULL);
    struct sub_state *pic2_state = sub_state(pic2);
    /* the test releases this one from its source end */
    pic2_state->release_on_end = true;

    uref = carrier('Q', 50, 5000);
    add_entry(uref, 1, ID_PIC, 'G', 20);
    add_entry(uref, 2, ID_SOUND, 'H', 10);
    upipe_input(demux, uref, NULL);
    check_entry(pic2_state, 1, 'G', 20, 5000);
    check_entry(sound2, 1, 'H', 10, 5000);
    assert(pic_state->sink->count == 5);
    upipe_release(pic);
    assert(pic_state->dead == 1);

    /* ID_SOUND disappears: uprobe_selflow's subpipe is ended and
     * released */
    set_flow_def(demux, build_flow_def(NULL, NULL, "pic.other."));
    assert(sound2->source_ends == 1 && sound2->dead == 1);
    assert(pic2_state->source_ends == 0);
    assert(split_updates == 4 && nb_announced(demux) == 2);

    /* a flow def without entries is accepted and ends the last subpipe,
     * which the test releases from its source end; the main output flow def
     * is unchanged */
    set_flow_def(demux, main_flow_def());
    assert(pic2_state->source_ends == 1 && pic2_state->dead == 1);
    assert(split_updates == 5 && nb_announced(demux) == 0);
    assert(main_new_flow_defs == 1);

    /* releasing the demux ends all its subpipes, then announces that there
     * are no flows anymore; the demux only dies with its last subpipe */
    set_flow_def(demux, build_flow_def("sound.s32.", "audio", "pic."));
    assert(split_updates == 6);
    struct sub_state *sound3 = live_sub(ID_SOUND);
    assert(sound3 != NULL);
    struct upipe *pic3 = alloc_sub(demux, announced(demux, ID_PIC),
                                   "pic 3");
    assert(pic3 != NULL);
    struct sub_state *pic3_state = sub_state(pic3);
    pic3_state->release_on_end = true;
    struct upipe *data = alloc_sub(demux, announced(demux, ID_DATA),
                                   "data");
    assert(data != NULL);
    struct sub_state *data_state = sub_state(data);

    upipe_release(demux);
    assert(sound3->source_ends == 1 && sound3->dead == 1);
    assert(pic3_state->source_ends == 1 && pic3_state->dead == 1);
    assert(data_state->source_ends == 1 && data_state->dead == 0);
    assert(split_updates == 7);
    assert(sound3->source_end_seq < split_update_seq);
    assert(pic3_state->source_end_seq < split_update_seq);
    assert(data_state->source_end_seq < split_update_seq);
    assert(!demux_dead);
    upipe_release(data);
    assert(data_state->dead == 1 && demux_dead);

    /* 0 and UINT64_MAX are flow IDs like any other: a second demux announces
     * them, allows one subpipe each and routes their entries */
    struct upipe *demux2 = upipe_void_alloc(upipe_uref_demux_mgr,
            uprobe_pfx_alloc(uprobe_use(&main_probe), UPROBE_LOG_LEVEL,
                             "demux 2"));
    assert(demux2 != NULL);
    demux_pipe = demux2;
    split_updates = 0;
    demux_dead = false;
    ubase_assert(upipe_set_output(demux2, sink));
    flow_def = main_flow_def();
    add_entry_flow_def(flow_def, 1, UINT64_MAX, "block.max.", NULL);
    add_entry_flow_def(flow_def, 2, 0, "block.zero.", NULL);
    set_flow_def(demux2, flow_def);
    assert(split_updates == 1 && nb_announced(demux2) == 2);
    check_announced(demux2, UINT64_MAX, "block.max.", NULL);
    check_announced(demux2, 0, "block.zero.", NULL);

    struct upipe *max = alloc_sub(demux2, announced(demux2, UINT64_MAX),
                                  "max");
    struct upipe *zero = alloc_sub(demux2, announced(demux2, 0), "zero");
    assert(max != NULL && zero != NULL);
    struct sub_state *max_state = sub_state(max);
    struct sub_state *zero_state = sub_state(zero);
    max_state->release_on_end = true;
    zero_state->release_on_end = true;
    assert(alloc_sub(demux2, announced(demux2, UINT64_MAX),
                     "max again") == NULL);
    assert(alloc_sub(demux2, announced(demux2, 0), "zero again") == NULL);

    uref = carrier('U', 50, 9000);
    add_entry(uref, 1, 0, 'K', 6);
    add_entry(uref, 2, UINT64_MAX, 'L', 7);
    upipe_input(demux2, uref, NULL);
    check_entry(zero_state, 1, 'K', 6, 9000);
    check_entry(max_state, 1, 'L', 7, 9000);

    upipe_release(demux2);
    assert(max_state->dead == 1 && zero_state->dead == 1 && demux_dead);

    /* every subpipe is dead */
    for (unsigned i = 0; i < nb_subs; i++)
        assert(subs[i].upipe == NULL && subs[i].dead == 1);

    upipe_release(sink);
    upipe_mgr_release(upipe_uref_demux_mgr); // nop

    ubuf_mgr_release(ubuf_mgr);
    uref_mgr_release(uref_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    uprobe_clean(&main_probe);
    uprobe_clean(&sub_probe);
    uprobe_release(logger);
    uprobe_clean(&uprobe);

    return 0;
}
