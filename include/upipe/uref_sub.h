/*
 * Copyright (C) 2026 Open Broadcast Systems Ltd
 *
 * Authors: Kieran Kunhya
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe uref handling for multiple ubufs
 * This file defines the API to attach several ubufs to a single uref, so
 * that one uref may for instance carry video, audio and subtitles at the
 * same time.
 *
 * The primary ubuf (uref->ubuf) is entry 0 and remains fully usable by
 * legacy pipes, which only ever see the primary ubuf. Additional ubufs are
 * entries 1 and above, chained to the uref by their own uchain (which is
 * unused while a ubuf is in flight). Per-entry metadata is stored in the
 * uref attributes under indexed names ("sub[i].*"), so it is transparently
 * carried and duplicated by udict handling. This includes the flags, dates
 * and delays of each entry, which are fields of the uref structure rather
 * than attributes: @ref uref_sub_merge keeps them as "sub[i].uref.*"
 * attributes, from which @ref uref_sub_extract restores them. An entry
 * without them gets those of the carrier.
 *
 * @ref uref_free frees all entries, and @ref uref_dup and @ref uref_fork
 * duplicate the whole chain (ubuf_dup is a cheap reference increment), so
 * legacy pipes transparently pass additional entries through. Note however
 * that pipes deriving urefs through internal helpers (block splice/split,
 * picture field split) do not carry the chain.
 *
 * The intended model is that entries are merged into a carrier uref at one
 * pipe boundary (@ref uref_sub_merge) and extracted back into standalone
 * urefs at another (@ref uref_sub_extract); both round-trip all attributes
 * generically between plain names and the per-entry namespace, so pipes in
 * between never need per-entry attribute access.
 *
 * A uref without any ubuf, such as a flow definition, may also carry
 * entries: they then only consist of their per-entry attributes, so that for
 * instance the flow definitions of the constituent flows can be aggregated
 * into the flow definition of the carrier, with the same functions. Since an
 * additional ubuf's entry index is given by its position in the chain,
 * entries without ubufs can't be mixed with entries with ubufs: they only
 * exist in urefs without any ubuf.
 *
 * The index of an entry only means something within its uref: from one uref
 * to the next, a flow may have its entry at another index, or several
 * entries. Entries are therefore usually identified by a flow ID
 * ("sub[i].f.id", see @ref uref_sub_set_flow_id and
 * @ref uref_sub_find_flow_id). A uref holds at most
 * @ref UREF_SUB_MAX_ENTRIES entries.
 */

#ifndef _UPIPE_UREF_SUB_H_
/** @hidden */
#define _UPIPE_UREF_SUB_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "upipe/uref.h"
#include "upipe/uref_attr.h"
#include "upipe/ubuf.h"
#include "upipe/udict.h"
#include "upipe/ulist.h"

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @This is the maximum number of entries of a uref, including entry 0,
 * since entry indices are stored in a uint8_t: a uref carries at most
 * UREF_SUB_MAX_ENTRIES - 1 additional entries. */
#define UREF_SUB_MAX_ENTRIES UINT8_MAX

/** @internal @This is the maximum length of a per-entry attribute name
 * prefix, including the trailing zero. */
#define UREF_SUB_PREFIX_SIZE 16

/** @internal @This is the maximum length of the name of an attribute keeping
 * a uref field, after the per-entry prefix, including the trailing zero. */
#define UREF_SUB_FIELD_SIZE 24

/** @internal @This returns a uref field kept for each entry (flags, dates and
 * delays), by rank, along with the name of the attribute keeping it.
 *
 * @param uref pointer to uref structure
 * @param rank rank of the field, from 0
 * @param name_p filled in with the name of the attribute, after the per-entry
 * prefix
 * @return pointer to the field, or NULL after the last one
 */
static inline uint64_t *uref_sub_field(struct uref *uref, unsigned rank,
                                       const char **name_p)
{
    switch (rank) {
        case 0:
            *name_p = "uref.flags";
            return &uref->flags;
        case 1:
            *name_p = "uref.date_sys";
            return &uref->date_sys;
        case 2:
            *name_p = "uref.date_prog";
            return &uref->date_prog;
        case 3:
            *name_p = "uref.date_orig";
            return &uref->date_orig;
        case 4:
            *name_p = "uref.dts_pts_delay";
            return &uref->dts_pts_delay;
        case 5:
            *name_p = "uref.cr_dts_delay";
            return &uref->cr_dts_delay;
        case 6:
            *name_p = "uref.rap_cr_delay";
            return &uref->rap_cr_delay;
        default:
            return NULL;
    }
}

/** @internal @This returns the uref field kept by an attribute.
 *
 * @param uref pointer to uref structure
 * @param name name of the attribute, after the per-entry prefix
 * @return pointer to the field, or NULL if the attribute doesn't keep one
 */
static inline uint64_t *uref_sub_field_by_name(struct uref *uref,
                                               const char *name)
{
    const char *field_name;
    uint64_t *field;
    for (unsigned rank = 0;
         (field = uref_sub_field(uref, rank, &field_name)) != NULL; rank++)
        if (!strcmp(name, field_name))
            return field;
    return NULL;
}

UREF_ATTR_UNSIGNED_VA(sub, flow_id, "sub[%" PRIu8"].f.id", flow ID of an entry,
        uint8_t index, index)
UREF_ATTR_STRING_VA(sub, def, "sub[%" PRIu8"].f.def",
        flow definition of an entry, uint8_t index, index)

/** @This walks through the chain of additional ubufs of a uref (entries 1
 * and above). The list may not be altered during the walk.
 *
 * @param uref pointer to uref structure
 * @param uchain iterator, to be converted with @ref ubuf_from_uchain
 */
#define uref_sub_foreach(uref, uchain)                                      \
    ulist_foreach(&(uref)->sub_ubufs, uchain)

/** @internal @This parses the entry index of a per-entry attribute name.
 *
 * @param name attribute name (NULL for shorthands)
 * @param index_p filled in with the entry index
 * @return true if the name belongs to a per-entry namespace
 */
static inline bool uref_sub_name_index(const char *name, uint8_t *index_p)
{
    if (name == NULL || strncmp(name, "sub[", 4))
        return false;
    const char *p = name + 4;
    if (*p < '0' || *p > '9')
        return false;
    unsigned int index = 0;
    for ( ; *p >= '0' && *p <= '9'; p++) {
        index = index * 10 + (*p - '0');
        if (index > UINT8_MAX)
            return false;
    }
    if (p[0] != ']' || p[1] != '.')
        return false;
    *index_p = index;
    return true;
}

/** @This returns the number of entries of a uref, including entry 0. For a
 * uref carrying ubufs, this is one more than the number of additional ubufs,
 * entry 0 being the primary ubuf even if it is absent; this walks the chain.
 * For a uref without any ubuf (such as a flow definition), entries only
 * consist of attributes, so this is one more than the highest index found in
 * per-entry attributes; this scans all the attributes.
 *
 * @param uref pointer to uref structure
 * @return number of entries (0 if there is no entry at all)
 */
static inline uint8_t uref_sub_count(struct uref *uref)
{
    if (uref->ubuf != NULL || !ulist_empty(&uref->sub_ubufs))
        return ulist_depth(&uref->sub_ubufs) + 1;

    uint8_t count = 0;
    if (uref->udict != NULL) {
        const char *name = NULL;
        enum udict_type type = UDICT_TYPE_END;
        for ( ; ; ) {
            udict_iterate(uref->udict, &name, &type);
            if (unlikely(type == UDICT_TYPE_END))
                break;
            uint8_t index;
            if (uref_sub_name_index(name, &index) && index >= count &&
                index < UREF_SUB_MAX_ENTRIES)
                count = index + 1;
        }
    }
    return count;
}

/** @This returns the ubuf at the given entry of a uref, without transferring
 * ownership. Entry 0 is the primary ubuf.
 *
 * @param uref pointer to uref structure
 * @param index entry index
 * @return pointer to ubuf, or NULL if the entry does not exist or has no ubuf
 */
static inline struct ubuf *uref_sub_get(struct uref *uref, uint8_t index)
{
    if (index == 0)
        return uref->ubuf;
    struct uchain *uchain = ulist_at(&uref->sub_ubufs, index - 1);
    return uchain != NULL ? ubuf_from_uchain(uchain) : NULL;
}

/** @This attaches an additional ubuf to a given uref, appended at the end of
 * the chain. The ubuf pointer may no longer be used by the module
 * afterwards. The primary ubuf is not modified (use @ref uref_attach_ubuf
 * for entry 0), and no per-entry attribute is set (see @ref uref_sub_merge
 * to merge a whole uref). The uref must have fewer than
 * @ref UREF_SUB_MAX_ENTRIES entries, and no entries without ubufs.
 *
 * @param uref pointer to uref structure
 * @param ubuf pointer to ubuf structure to attach to uref
 * @return entry index given to the ubuf
 */
static inline uint8_t uref_sub_attach_ubuf(struct uref *uref,
                                           struct ubuf *ubuf)
{
    ulist_add(&uref->sub_ubufs, ubuf_to_uchain(ubuf));
    return ulist_depth(&uref->sub_ubufs);
}

/** @internal @This builds the attribute name prefix of an entry.
 *
 * @param buffer buffer of at least UREF_SUB_PREFIX_SIZE octets
 * @param index entry index
 * @return length of the prefix
 */
static inline size_t uref_sub_prefix(char *buffer, uint8_t index)
{
    return snprintf(buffer, UREF_SUB_PREFIX_SIZE, "sub[%" PRIu8 "].", index);
}

/** @internal @This copies the value of an attribute to another name and
 * type, possibly within the same udict. The value is staged in a temporary
 * buffer because setting an attribute may reallocate the underlying storage.
 *
 * @param dst_uref destination uref
 * @param dst_name destination attribute name (NULL if dst_type is a
 * shorthand)
 * @param dst_type destination attribute type
 * @param src_uref source uref (may be the same as dst_uref)
 * @param src_name source attribute name (NULL if src_type is a shorthand)
 * @param src_type source attribute type
 * @return an error code
 */
static inline int uref_sub_attr_transfer(struct uref *dst_uref,
                                         const char *dst_name,
                                         enum udict_type dst_type,
                                         struct uref *src_uref,
                                         const char *src_name,
                                         enum udict_type src_type)
{
    size_t size;
    const uint8_t *v;
    UBASE_RETURN(udict_get(src_uref->udict, src_name, src_type, &size, &v));
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (unlikely(size != 0 && tmp == NULL))
        return UBASE_ERR_ALLOC;
    if (size != 0)
        memcpy(tmp, v, size);

    if (dst_uref->udict == NULL) {
        dst_uref->udict = udict_alloc(dst_uref->mgr->udict_mgr, 0);
        if (unlikely(dst_uref->udict == NULL)) {
            free(tmp);
            return UBASE_ERR_ALLOC;
        }
    }
    uint8_t *new_v;
    int err = udict_set(dst_uref->udict, dst_name, dst_type, size, &new_v);
    if (likely(ubase_check(err)) && size != 0)
        memcpy(new_v, tmp, size);
    free(tmp);
    return err;
}

/** @internal @This checks whether a plain attribute name and base type
 * correspond to a shorthand type, so that extracted attributes remain
 * visible to shorthand accessors.
 *
 * @param uref pointer to uref structure (used to query the udict manager)
 * @param name plain attribute name
 * @param base_type base attribute type
 * @return the shorthand type, or base_type if there is none
 */
static inline enum udict_type uref_sub_shorthand(struct uref *uref,
                                                 const char *name,
                                                 enum udict_type base_type)
{
    for (int type = UDICT_TYPE_SHORTHAND + 1; ; type++) {
        const char *shorthand_name;
        enum udict_type shorthand_base;
        if (!ubase_check(udict_name(uref->udict, (enum udict_type)type,
                                    &shorthand_name, &shorthand_base)))
            return base_type;
        if (shorthand_base == base_type && !strcmp(shorthand_name, name))
            return (enum udict_type)type;
    }
}

/** @This deletes all per-entry attributes of the given entry.
 *
 * @param uref pointer to uref structure
 * @param index entry index
 * @return an error code
 */
static inline int uref_sub_delete_attrs(struct uref *uref, uint8_t index)
{
    if (uref->udict == NULL)
        return UBASE_ERR_NONE;
    char prefix[UREF_SUB_PREFIX_SIZE];
    size_t prefix_len = uref_sub_prefix(prefix, index);
    for ( ; ; ) {
        const char *name = NULL;
        enum udict_type type = UDICT_TYPE_END;
        for ( ; ; ) {
            udict_iterate(uref->udict, &name, &type);
            if (unlikely(type == UDICT_TYPE_END))
                return UBASE_ERR_NONE;
            if (name != NULL && !strncmp(name, prefix, prefix_len))
                break;
        }
        /* deletion invalidates the iteration, so restart it */
        UBASE_RETURN(udict_delete(uref->udict, type, name));
    }
}

/** @This renames all per-entry attributes of an entry to another index.
 * Existing attributes of the destination index are overwritten on name
 * collision, so the caller should make sure the destination namespace is
 * free.
 *
 * @param uref pointer to uref structure
 * @param from source entry index
 * @param to destination entry index
 * @return an error code
 */
static inline int uref_sub_rename_attrs(struct uref *uref, uint8_t from,
                                        uint8_t to)
{
    if (uref->udict == NULL || from == to)
        return UBASE_ERR_NONE;
    char from_prefix[UREF_SUB_PREFIX_SIZE], to_prefix[UREF_SUB_PREFIX_SIZE];
    size_t from_len = uref_sub_prefix(from_prefix, from);
    size_t to_len = uref_sub_prefix(to_prefix, to);
    for ( ; ; ) {
        const char *name = NULL;
        enum udict_type type = UDICT_TYPE_END;
        for ( ; ; ) {
            udict_iterate(uref->udict, &name, &type);
            if (unlikely(type == UDICT_TYPE_END))
                return UBASE_ERR_NONE;
            if (name != NULL && !strncmp(name, from_prefix, from_len))
                break;
        }
        /* copy both names as setting an attribute may reallocate the
         * storage the iterated name points into */
        size_t suffix_len = strlen(name + from_len);
        char *old_name = (char *)malloc(from_len + suffix_len + 1 +
                                        to_len + suffix_len + 1);
        if (unlikely(old_name == NULL))
            return UBASE_ERR_ALLOC;
        char *new_name = old_name + from_len + suffix_len + 1;
        memcpy(old_name, name, from_len + suffix_len + 1);
        memcpy(new_name, to_prefix, to_len);
        memcpy(new_name + to_len, name + from_len, suffix_len + 1);

        int err = uref_sub_attr_transfer(uref, new_name, type,
                                         uref, old_name, type);
        if (likely(ubase_check(err)))
            err = udict_delete(uref->udict, type, old_name);
        free(old_name);
        if (unlikely(!ubase_check(err)))
            return err;
        /* the transfer invalidates the iteration, so restart it */
    }
}

/** @This detaches the ubuf at the given entry from a uref. The returned
 * ubuf must be freed or re-attached at some point, otherwise it will leak.
 * Entry 0 detaches the primary ubuf (see @ref uref_detach_ubuf) and leaves
 * attributes untouched. For other entries, the entry's attributes, including
 * its kept flags, dates and delays, are deleted and the attributes of the
 * following entries are renumbered to match their new indices (best effort
 * in case of allocation failure). An entry without ubuf, as in a flow
 * definition, is left untouched: use @ref uref_sub_delete_attrs instead.
 *
 * @param uref pointer to uref structure
 * @param index entry index
 * @return pointer to detached ubuf, or NULL if the entry does not exist or
 * has no ubuf
 */
static inline struct ubuf *uref_sub_detach_ubuf(struct uref *uref,
                                                uint8_t index)
{
    if (index == 0)
        return uref_detach_ubuf(uref);
    struct uchain *uchain = ulist_at(&uref->sub_ubufs, index - 1);
    if (uchain == NULL)
        return NULL;
    ulist_delete(uchain);

    uref_sub_delete_attrs(uref, index);
    uint8_t last = ulist_depth(&uref->sub_ubufs) + 1;
    for (uint8_t i = index; i < last; i++)
        uref_sub_rename_attrs(uref, i + 1, i);
    return ubuf_from_uchain(uchain);
}

/** @This replaces the ubuf at the given entry of a uref, freeing the
 * previous one. The ubuf pointer may no longer be used by the module
 * afterwards, even in case of error. Per-entry attributes are left
 * untouched.
 *
 * @param uref pointer to uref structure
 * @param ubuf pointer to ubuf structure replacing the entry
 * @param index entry index
 * @return an error code
 */
static inline int uref_sub_replace_ubuf(struct uref *uref, struct ubuf *ubuf,
                                        uint8_t index)
{
    if (index == 0) {
        uref_attach_ubuf(uref, ubuf);
        return UBASE_ERR_NONE;
    }
    struct uchain *uchain = ulist_at(&uref->sub_ubufs, index - 1);
    if (uchain == NULL) {
        ubuf_free(ubuf);
        return UBASE_ERR_INVALID;
    }
    struct uchain *prev = uchain->prev;
    ulist_delete(uchain);
    ubuf_free(ubuf_from_uchain(uchain));
    ulist_insert(prev, prev->next, ubuf_to_uchain(ubuf));
    return UBASE_ERR_NONE;
}

/** @This finds the first entry carrying the given flow ID (as set with @ref
 * uref_sub_set_flow_id, including on entry 0); a flow may have several
 * entries in a uref. The ubuf of the entry, if it has one, may then be
 * retrieved with @ref uref_sub_get.
 *
 * @param uref pointer to uref structure
 * @param flow_id flow ID to look for
 * @param index_p filled in with the entry index, may be NULL
 * @return an error code (UBASE_ERR_INVALID if no entry carries this flow ID)
 */
static inline int uref_sub_find_flow_id(struct uref *uref, uint64_t flow_id,
                                        uint8_t *index_p)
{
    uint8_t count = uref_sub_count(uref);
    for (uint8_t i = 0; i < count; i++) {
        uint64_t v;
        if (ubase_check(uref_sub_get_flow_id(uref, &v, i)) && v == flow_id) {
            if (index_p != NULL)
                *index_p = i;
            return UBASE_ERR_NONE;
        }
    }
    return UBASE_ERR_INVALID;
}

/** @This extracts an entry of a uref as a standalone uref, without
 * modifying the source. The new uref references a duplicate of the entry's
 * ubuf if it has one (entries of urefs without ubufs, such as flow
 * definitions, only consist of attributes), gets the flags, dates and
 * delays kept for the entry by @ref uref_sub_merge, or else those of the
 * source uref, and its plain attributes are the per-entry attributes of the
 * entry, renamed from the "sub[i]." namespace and converted back to
 * shorthands where applicable.
 * For entry 0 the plain attributes of the source are also copied. Per-entry
 * attributes of other entries are not carried.
 *
 * @param uref pointer to uref structure
 * @param index entry index
 * @return extracted uref, or NULL if the entry does not exist or in case of
 * allocation failure
 */
static inline struct uref *uref_sub_extract(struct uref *uref, uint8_t index)
{
    if (index != 0 && index >= uref_sub_count(uref))
        return NULL;
    struct ubuf *ubuf = uref_sub_get(uref, index);
    /* an entry without ubuf (such as a flow definition) is a control uref */
    struct uref *new_uref = ubuf != NULL ? uref_sibling_alloc(uref) :
                                           uref_sibling_alloc_control(uref);
    if (unlikely(new_uref == NULL))
        return NULL;

    /* the fields of the carrier, unless the entry kept its own (below) */
    new_uref->flags = uref->flags;
    new_uref->date_sys = uref->date_sys;
    new_uref->date_prog = uref->date_prog;
    new_uref->date_orig = uref->date_orig;
    new_uref->dts_pts_delay = uref->dts_pts_delay;
    new_uref->cr_dts_delay = uref->cr_dts_delay;
    new_uref->rap_cr_delay = uref->rap_cr_delay;
    new_uref->priv = uref->priv;

    if (ubuf != NULL) {
        new_uref->ubuf = ubuf_dup(ubuf);
        if (unlikely(new_uref->ubuf == NULL))
            goto uref_sub_extract_err;
    }

    /* walk the attributes of the carrier: those of the entry's namespace
     * become plain attributes again, except the ones keeping its fields,
     * which are restored into them; for entry 0 the plain attributes of the
     * carrier are copied too; the other namespaces are skipped */
    if (uref->udict != NULL) {
        char prefix[UREF_SUB_PREFIX_SIZE];
        size_t prefix_len = uref_sub_prefix(prefix, index);
        const char *name = NULL;
        enum udict_type type = UDICT_TYPE_END;
        for ( ; ; ) {
            udict_iterate(uref->udict, &name, &type);
            if (unlikely(type == UDICT_TYPE_END))
                break;

            int err = UBASE_ERR_NONE;
            if (name != NULL && !strncmp(name, prefix, prefix_len)) {
                const char *plain = name + prefix_len;
                uint64_t *field = type != UDICT_TYPE_UNSIGNED ? NULL :
                    uref_sub_field_by_name(new_uref, plain);
                if (field != NULL)
                    /* a field kept for the entry, not an attribute */
                    err = uref_attr_get_unsigned(uref, field, type, name);
                else {
                    enum udict_type plain_type =
                        uref_sub_shorthand(uref, plain, type);
                    err = uref_sub_attr_transfer(new_uref,
                            plain_type > UDICT_TYPE_SHORTHAND ? NULL : plain,
                            plain_type, uref, name, type);
                }
            } else if (index == 0 &&
                       (name == NULL || strncmp(name, "sub[", 4)))
                err = uref_sub_attr_transfer(new_uref, name, type,
                                             uref, name, type);
            if (unlikely(!ubase_check(err)))
                goto uref_sub_extract_err;
        }
    }
    return new_uref;

uref_sub_extract_err:
    uref_free(new_uref);
    return NULL;
}

/** @This merges a standalone uref into a carrier uref as a new entry. On
 * success the merged uref is entirely consumed: its ubuf, if any, becomes
 * the new entry, its plain attributes (including shorthands) are copied into
 * the "sub[i]." namespace of the carrier, and its flags, dates and delays
 * are kept there too, for @ref uref_sub_extract to restore them.
 *
 * A merged uref without a ubuf (such as a flow definition) gives an entry
 * made only of its attributes, so it must have some, and the carrier must
 * not have any ubuf. Conversely, a merged uref with a ubuf can't follow
 * entries without ubufs. The merged uref must not itself carry additional
 * ubufs, and the carrier must have fewer than @ref UREF_SUB_MAX_ENTRIES
 * entries. In all these cases (and on allocation failure) an error is
 * returned and the merged uref remains owned by the caller.
 *
 * @param uref pointer to carrier uref structure
 * @param sub_uref uref to merge as a new entry
 * @param index_p filled in with the entry index, may be NULL
 * @return an error code
 */
static inline int uref_sub_merge(struct uref *uref, struct uref *sub_uref,
                                 uint8_t *index_p)
{
    if (unlikely(!ulist_empty(&sub_uref->sub_ubufs)))
        return UBASE_ERR_INVALID;

    /* TODO: ulist_depth() and uref_sub_count() walk the whole ubuf chain (or
     * scan all the attributes of a uref without ubufs) on every merge, so
     * merging n entries into a uref costs O(n^2); keeping the number of
     * entries alongside the chain would avoid it */
    size_t index;
    if (sub_uref->ubuf != NULL) {
        /* the ubuf is appended to the chain, and its position there gives
         * its entry index, so there must be no entries without ubufs */
        index = ulist_depth(&uref->sub_ubufs) + 1;
        if (unlikely(uref_sub_count(uref) > index))
            return UBASE_ERR_INVALID;
    } else {
        /* entries without ubufs only exist in urefs without any ubuf, and
         * take the next free index of their namespaces */
        if (unlikely(uref->ubuf != NULL || !ulist_empty(&uref->sub_ubufs)))
            return UBASE_ERR_INVALID;
        index = uref_sub_count(uref);
        if (index == 0)
            index = 1;
    }
    if (unlikely(index >= UREF_SUB_MAX_ENTRIES))
        /* no room left for another entry */
        return UBASE_ERR_INVALID;

    char prefix[UREF_SUB_PREFIX_SIZE];
    size_t prefix_len = uref_sub_prefix(prefix, index);

    /* copy every plain attribute of the merged uref into the namespace of
     * the entry, under its full name for shorthands; on failure, the
     * attributes copied so far are deleted again */
    bool merged = false;
    if (sub_uref->udict != NULL) {
        const char *name = NULL;
        enum udict_type type = UDICT_TYPE_END;
        for ( ; ; ) {
            udict_iterate(sub_uref->udict, &name, &type);
            if (unlikely(type == UDICT_TYPE_END))
                break;

            const char *plain = name;
            enum udict_type base = type;
            if (plain == NULL) {
                if (unlikely(!ubase_check(udict_name(sub_uref->udict, type,
                                                     &plain, &base)))) {
                    uref_sub_delete_attrs(uref, index);
                    return UBASE_ERR_INVALID;
                }
            } else if (!strncmp(plain, "sub[", 4))
                /* per-entry namespaces cannot be nested */
                continue;

            size_t plain_len = strlen(plain);
            char *new_name = (char *)malloc(prefix_len + plain_len + 1);
            if (unlikely(new_name == NULL)) {
                uref_sub_delete_attrs(uref, index);
                return UBASE_ERR_ALLOC;
            }
            memcpy(new_name, prefix, prefix_len);
            memcpy(new_name + prefix_len, plain, plain_len + 1);
            int err = uref_sub_attr_transfer(uref, new_name, base,
                                             sub_uref, name, type);
            free(new_name);
            if (unlikely(!ubase_check(err))) {
                uref_sub_delete_attrs(uref, index);
                return err;
            }
            merged = true;
        }
    }

    if (unlikely(sub_uref->ubuf == NULL && !merged))
        /* an entry with neither ubuf nor attributes would not exist */
        return UBASE_ERR_INVALID;

    /* the entry keeps its own flags, dates and delays */
    const char *field_name;
    uint64_t *field;
    for (unsigned rank = 0;
         (field = uref_sub_field(sub_uref, rank, &field_name)) != NULL;
         rank++) {
        char name[UREF_SUB_PREFIX_SIZE + UREF_SUB_FIELD_SIZE];
        snprintf(name, sizeof(name), "%s%s", prefix, field_name);
        int err = uref_attr_set_unsigned(uref, *field, UDICT_TYPE_UNSIGNED,
                                         name);
        if (unlikely(!ubase_check(err))) {
            uref_sub_delete_attrs(uref, index);
            return err;
        }
    }

    if (sub_uref->ubuf != NULL)
        uref_sub_attach_ubuf(uref, uref_detach_ubuf(sub_uref));
    uref_free(sub_uref);
    if (index_p != NULL)
        *index_p = index;
    return UBASE_ERR_NONE;
}

#ifdef __cplusplus
}
#endif
#endif
