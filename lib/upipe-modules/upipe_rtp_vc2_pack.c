/*
 * Copyright (C) 2016-2017 Open Broadcast Systems Ltd
 *
 * Authors: Rafaël Carré
 *          James Darnley
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
 * @short Upipe module to split vc2 bitstream into RTP packets
 */

#include <stdlib.h>
#include <limits.h>

#include <upipe/upipe.h>
#include <upipe/uclock.h>
#include <upipe/uref_clock.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_uref_stream.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_flow_def.h>
#include <upipe/upipe_helper_ubuf_mgr.h>
#include <upipe/upipe_helper_uref_stream.h>
#include <upipe/upipe_helper_input.h>

#include <bitstream/ietf/rtp.h>
#include <libavutil/intreadwrite.h>

#include <upipe-modules/upipe_rtp_vc2_pack.h>

enum DiracParseCodes {
    DIRAC_PCODE_SEQ_HEADER      = 0x00,
    DIRAC_PCODE_END_SEQ         = 0x10,
    DIRAC_PCODE_AUX             = 0x20,
    DIRAC_PCODE_PAD             = 0x30,
    DIRAC_PCODE_PICTURE_HQ      = 0xE8,
    DIRAC_PCODE_PICTURE_FRAGMENT_HQ = 0xEC,
    DIRAC_PCODE_MAGIC           = 0x42424344,
};

struct dirac_parse_info {
    uint32_t magic_prefix;
    uint8_t parse_code;
    uint32_t next_offset;
    uint32_t prev_offset;
};

struct upipe_rtp_vc2_pack {
    /** refcount management structure */
    struct urefcount urefcount;

    /* output stuff */
    /** pipe acting as output */
    struct upipe *output;
    /** output flow definition packet */
    struct uref *flow_def;
    /** output state */
    enum upipe_helper_output_state output_state;
    /** list of output requests */
    struct uchain request_list;

    /** ubuf manager */
    struct ubuf_mgr *ubuf_mgr;
    /** flow format packet */
    struct uref *flow_format;
    /** ubuf manager request */
    struct urequest ubuf_mgr_request;

    /** next uref to be processed */
    struct uref *next_uref;
    /** original size of the next uref */
    size_t next_uref_size;
    /** urefs received after next uref */
    struct uchain urefs;

    /** temporary uref storage (used during urequest) */
    struct uchain input_urefs;
    /** nb urefs in storage */
    unsigned int nb_urefs;
    /** max urefs in storage */
    unsigned int max_urefs;
    /** list of blockers (used during urequest) */
    struct uchain blockers;

    /** public upipe structure */
    struct upipe upipe;

    uint64_t sequence_number;
};

/** @hidden */
static int upipe_rtp_vc2_pack_check(struct upipe *upipe, struct uref *flow_format);

/** @hidden */
static bool upipe_rtp_vc2_pack_handle(struct upipe *upipe, struct uref *uref,
                                  struct upump **upump_p);

UPIPE_HELPER_UPIPE(upipe_rtp_vc2_pack, upipe, UPIPE_RTP_VC2_PACK_SIGNATURE)
UPIPE_HELPER_UREFCOUNT(upipe_rtp_vc2_pack, urefcount, upipe_rtp_vc2_pack_free)
UPIPE_HELPER_VOID(upipe_rtp_vc2_pack)
UPIPE_HELPER_OUTPUT(upipe_rtp_vc2_pack, output, flow_def, output_state,
                    request_list)
UPIPE_HELPER_UBUF_MGR(upipe_rtp_vc2_pack, ubuf_mgr, flow_format, ubuf_mgr_request,
                      upipe_rtp_vc2_pack_check,
                      upipe_rtp_vc2_pack_register_output_request,
                      upipe_rtp_vc2_pack_unregister_output_request)
UPIPE_HELPER_UREF_STREAM(upipe_rtp_vc2_pack, next_uref, next_uref_size, urefs, NULL)
UPIPE_HELPER_INPUT(upipe_rtp_vc2_pack, input_urefs, nb_urefs, max_urefs, blockers,
        upipe_rtp_vc2_pack_handle)

static int upipe_rtp_vc2_pack_check(struct upipe *upipe, struct uref *flow_format)
{
    if (flow_format)
        upipe_rtp_vc2_pack_store_flow_def(upipe, flow_format);

    bool was_buffered = !upipe_rtp_vc2_pack_check_input(upipe);
    upipe_rtp_vc2_pack_output_input(upipe);
    upipe_rtp_vc2_pack_unblock_input(upipe);
    if (was_buffered && upipe_rtp_vc2_pack_check_input(upipe)) {
        /* All packets have been output, release again the pipe that has been
         * used in @ref upipe_rtp_vc2_pack_input. */
        upipe_release(upipe);
    }

    return UBASE_ERR_NONE;
}

static int upipe_rtp_vc2_pack_set_flow_def(struct upipe *upipe,
                                       struct uref *flow_def)
{
    struct upipe_rtp_vc2_pack *upipe_rtp_vc2_pack = upipe_rtp_vc2_pack_from_upipe(upipe);

    if (flow_def == NULL)
        return UBASE_ERR_INVALID;
    UBASE_RETURN(uref_flow_match_def(flow_def, "block.dirac.pic."))

    struct uref *flow_def_dup = uref_sibling_alloc(flow_def);
    if (unlikely(flow_def_dup == NULL)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return UBASE_ERR_ALLOC;
    }

    uref_flow_set_def(flow_def_dup, "block.rtp.vc2.");
    /* RTP clock rate */
    //uref_sound_flow_set_rate(flow_def_dup, upipe_rtp_pcm_pack->rate);

    upipe_rtp_vc2_pack_require_ubuf_mgr(upipe, flow_def_dup);

    return UBASE_ERR_NONE;
}

/** @internal @This provides a flow format suggestion.
 *
 * @param upipe description structure of the pipe
 * @param request description structure of the request
 * @return an error code
 */
static int upipe_rtp_vc2_pack_provide_flow_format(struct upipe *upipe,
                                          struct urequest *request)
{
    struct uref *flow = uref_dup(request->uref);
    UBASE_ALLOC_RETURN(flow);

    //uref_flow_set_def(flow, "block.dirac.pic.");

    return urequest_provide_flow_format(request, flow);
}

static int upipe_rtp_vc2_pack_control(struct upipe *upipe, int command,
                                  va_list args)
{
    switch (command) {
        case UPIPE_REGISTER_REQUEST: {
            struct urequest *request = va_arg(args, struct urequest *);
            if (request->type == UREQUEST_FLOW_FORMAT)
                return upipe_rtp_vc2_pack_provide_flow_format(upipe, request);
            if (request->type == UREQUEST_UBUF_MGR)
                return upipe_throw_provide_request(upipe, request);
            return upipe_rtp_vc2_pack_alloc_output_proxy(upipe, request);
        }
        case UPIPE_UNREGISTER_REQUEST: {
            struct urequest *request = va_arg(args, struct urequest *);
            if (request->type == UREQUEST_FLOW_FORMAT ||
                request->type == UREQUEST_UBUF_MGR)
                return UBASE_ERR_NONE;
            return upipe_rtp_vc2_pack_free_output_proxy(upipe, request);
        }
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow_def = va_arg(args, struct uref *);
            return upipe_rtp_vc2_pack_set_flow_def(upipe, flow_def);
        }
        case UPIPE_GET_FLOW_DEF:
        case UPIPE_GET_OUTPUT:
        case UPIPE_SET_OUTPUT:
            return upipe_rtp_vc2_pack_control_output(upipe, command, args);
        default:
            return UBASE_ERR_UNHANDLED;
    }
}

static void upipe_rtp_vc2_pack_free(struct upipe *upipe)
{
    upipe_rtp_vc2_pack_clean_ubuf_mgr(upipe);
    upipe_rtp_vc2_pack_clean_urefcount(upipe);
    upipe_rtp_vc2_pack_clean_uref_stream(upipe);
    upipe_rtp_vc2_pack_clean_output(upipe);
    upipe_rtp_vc2_pack_clean_input(upipe);
    upipe_rtp_vc2_pack_free_void(upipe);
}

static struct upipe *upipe_rtp_vc2_pack_alloc(struct upipe_mgr *mgr,
                                           struct uprobe *uprobe,
                                           uint32_t signature,
                                           va_list args)
{
    struct upipe *upipe =
        upipe_rtp_vc2_pack_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;
    struct upipe_rtp_vc2_pack *upipe_rtp_vc2_pack = upipe_rtp_vc2_pack_from_upipe(upipe);

    upipe_rtp_vc2_pack_init_urefcount(upipe);
    upipe_rtp_vc2_pack_init_input(upipe);
    upipe_rtp_vc2_pack_init_ubuf_mgr(upipe);
    upipe_rtp_vc2_pack_init_uref_stream(upipe);
    upipe_rtp_vc2_pack_init_output(upipe);

    return upipe;
}

static void upipe_rtp_vc2_pack_input(struct upipe *upipe, struct uref *uref,
                                  struct upump **upump_p)
{
    if (!upipe_rtp_vc2_pack_check_input(upipe)) {
        upipe_rtp_vc2_pack_hold_input(upipe, uref);
        upipe_rtp_vc2_pack_block_input(upipe, upump_p);
    } else if (!upipe_rtp_vc2_pack_handle(upipe, uref, upump_p)) {
        upipe_rtp_vc2_pack_hold_input(upipe, uref);
        upipe_rtp_vc2_pack_block_input(upipe, upump_p);
        /* Increment upipe refcount to avoid disappearing before all packets
         * have been sent. */
        upipe_use(upipe);
    }
}

#define MTU 1440 /* MTU from rtp pcm pack */

static int output_packet(struct upipe *upipe, struct uref *uref, struct upump **upump_p,
        struct ubuf *packet, uint8_t parse_code)
{
    struct upipe_rtp_vc2_pack *rtp_vc2_pack = upipe_rtp_vc2_pack_from_upipe(upipe);
    uint8_t *dst = NULL;
    int dst_size = -1;
    UBASE_RETURN(ubuf_block_write(packet, 0, &dst_size, &dst));

    memset(dst, 0, RTP_HEADER_SIZE);
    rtp_set_hdr(dst);

    /* sequence number */
    rtp_set_seqnum(dst, rtp_vc2_pack->sequence_number);
    /* extended sequence number */
    dst[RTP_HEADER_SIZE + 0] = (rtp_vc2_pack->sequence_number >> 16);
    dst[RTP_HEADER_SIZE + 1] = (rtp_vc2_pack->sequence_number >> 24);
    rtp_vc2_pack->sequence_number++;

    /* parse code */
    dst[RTP_HEADER_SIZE + 3] = parse_code;

    ubuf_block_unmap(packet, 0);
    struct uref *uref_packet = uref_fork(uref, packet);
    if (!uref_packet)
        return UBASE_ERR_ALLOC;

    /*TODO: get errors from output? */
    upipe_rtp_vc2_pack_output(upipe, uref_packet, upump_p);
    return UBASE_ERR_NONE;
}

static bool upipe_rtp_vc2_pack_handle(struct upipe *upipe, struct uref *uref,
                                  struct upump **upump_p)
{
    struct upipe_rtp_vc2_pack *rtp_vc2_pack = upipe_rtp_vc2_pack_from_upipe(upipe);
    if (!rtp_vc2_pack->ubuf_mgr)
        return false;

    const uint8_t *src = NULL;
    int src_size = -1;
    UBASE_RETURN(uref_block_read(uref, 0, &src_size, &src));

    //copy data from input to output

    /* Put sequence header into first RTP packet:
     * - remove parse info header
     *
     * Put transform parameters into second RTP packet:
     * - remove parse info header
     * - remove fragment header
     * - keep picture number
     * - keep slice prefix bytes
     * - keep slice scaler
     * - keep/create fragment length
     *
     * Put slices into subsequent RTP packets:
     * - remove parse info header
     * - remove fragment header
     * - keep picture number
     * - keep slice prefix bytes
     * - keep slice scaler
     * - keep/create fragment length
     * - keep/create slice x/y offsets
     */

    ptrdiff_t src_offset = 0;
    /* some loop over parse info blocks in the input buffer */
    do {
        if (AV_RN32(src + src_offset) != UBASE_FOURCC('B', 'B', 'C', 'D')) {
            upipe_err(upipe, "incorrect magic prefix");
            uref_free(uref);
            upipe_throw_fatal(upipe, UBASE_ERR_UNKNOWN);
            return true;
        }
        uint8_t parse_code = src[src_offset + 4];
        ptrdiff_t next_offset = AV_RB32(src + src_offset + 5);
        ptrdiff_t prev_offset = AV_RB32(src + src_offset + 9);
        if (!next_offset) {
            upipe_err(upipe, "next offset is 0, this is not currently supported");
            uref_free(uref);
            upipe_throw_fatal(upipe, UBASE_ERR_UNKNOWN);
            return true;
        }

        switch (parse_code) {
            case DIRAC_PCODE_SEQ_HEADER: {
                upipe_dbg(upipe, "found sequence header");
                /* TODO: remove parse info header */

                size_t packet_size = RTP_HEADER_SIZE
                                   + 4 /* ext seqnum, reserved byte, parse code */
                                   + next_offset;
                /* TODO: check size is less than INT_MAX */

                struct ubuf *packet = ubuf_block_alloc(rtp_vc2_pack->ubuf_mgr, packet_size);
                if (!packet) {
                    uref_free(uref);
                    upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
                    return true;
                }

                uint8_t *dst = NULL;
                int dst_size = -1;
                UBASE_RETURN(ubuf_block_write(packet, 0, &dst_size, &dst));
                /* TODO: check dst_size is equal to packet_size */

                memcpy(dst + RTP_HEADER_SIZE + 4, src + src_offset, next_offset);
                UBASE_RETURN(ubuf_block_unmap(packet, 0));
                UBASE_RETURN(output_packet(upipe, uref, upump_p, packet, parse_code));
            } break;

            case DIRAC_PCODE_AUX: {
                upipe_dbg(upipe, "found auxiliary data");
                /* TODO: remove parse info header, add data length */

                size_t packet_size = RTP_HEADER_SIZE
                                   + 4 /* ext seqnum, reserved byte, parse code */
                                   //+ 4 /* data length */
                                   + next_offset;
                /* TODO: check size is less than INT_MAX */

                struct ubuf *packet = ubuf_block_alloc(rtp_vc2_pack->ubuf_mgr, packet_size);
                if (!packet) {
                    uref_free(uref);
                    upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
                    return true;
                }

                uint8_t *dst = NULL;
                int dst_size = -1;
                UBASE_RETURN(ubuf_block_write(packet, 0, &dst_size, &dst));
                /* TODO: check dst_size is equal to packet_size */

                memcpy(dst + RTP_HEADER_SIZE + 4, src + src_offset, next_offset);
                UBASE_RETURN(ubuf_block_unmap(packet, 0));
                UBASE_RETURN(output_packet(upipe, uref, upump_p, packet, parse_code));
            } break;

            case DIRAC_PCODE_PICTURE_HQ: {
                upipe_dbg(upipe, "found HQ picture");
                upipe_err(upipe, "splitting HQ pictures into fragments and "
                        "packets is not implemented at this time, skipping picture");
            } break;

            case DIRAC_PCODE_PICTURE_FRAGMENT_HQ: {
                /* TODO: remove parse info header, remove fragment header,
                 * get slice prefix bytes, get slice size scaler */

                uint32_t picture_number = AV_RN32(src + src_offset + 13);
                //uint16_t fragment_data_length = AV_RB16(src + src_offset + 17);
                uint16_t fragment_slice_count = AV_RB16(src + src_offset + 19);

                /* TODO: check size is less than INT_MAX */
                size_t packet_size = RTP_HEADER_SIZE
                                + 4 /* ext seqnum, reserved byte, parse code */
                                + 4 /* picture number */
                                + 4 /* slice prefix bytes, slice size scaler */
                                + 4 /* fragment length, num slices */
                                + next_offset;
                if (fragment_slice_count)
                    packet_size += 4; /* slice x/y offset */

                struct ubuf *packet = ubuf_block_alloc(rtp_vc2_pack->ubuf_mgr, packet_size);
                if (!packet) {
                    uref_free(uref);
                    upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
                    return true;
                }

                uint8_t *dst = NULL;
                int dst_size = -1;
                UBASE_RETURN(ubuf_block_write(packet, 0, &dst_size, &dst));
                /* TODO: check dst_size is equal to packet_size */

                AV_WN32(dst + RTP_HEADER_SIZE + 4, picture_number);
                AV_WB16(dst + RTP_HEADER_SIZE + 8, 0); /* slice prefix bytes */
                AV_WB16(dst + RTP_HEADER_SIZE + 10, 0); /* slice size scaler */
                if (fragment_slice_count)
                    /* fragment data length, fragment slice count, slice x
                     * offset, slice y offset */
                    memcpy(dst + RTP_HEADER_SIZE + 12, src + src_offset + 17, 8);
                else
                    /* fragment data length, fragment slice count */
                    memcpy(dst + RTP_HEADER_SIZE + 12, src + src_offset + 17, 4);

                /* packet payload */
                /* TODO: remove headers and copy only coded data */
                if (fragment_slice_count)
                    memcpy(dst + RTP_HEADER_SIZE + 20, src + src_offset, next_offset);
                else
                    memcpy(dst + RTP_HEADER_SIZE + 16, src + src_offset, next_offset);

                UBASE_RETURN(ubuf_block_unmap(packet, 0));
                UBASE_RETURN(output_packet(upipe, uref, upump_p, packet, parse_code));
            } break;

            case DIRAC_PCODE_END_SEQ: {
                upipe_dbg(upipe, "found end sequence");
                size_t packet_size = RTP_HEADER_SIZE
                                   + 4; /* ext seqnum, reserved byte, parse code */

                struct ubuf *packet = ubuf_block_alloc(rtp_vc2_pack->ubuf_mgr, packet_size);
                if (!packet) {
                    uref_free(uref);
                    upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
                    return true;
                }

                UBASE_RETURN(ubuf_block_unmap(packet, 0));
                UBASE_RETURN(output_packet(upipe, uref, upump_p, packet, parse_code));
            } break;

#if 0
            case DIRAC_PCODE_PAD: {
                upipe_dbg(upipe, "found padding");
            } break;
#endif

            default:
                upipe_err(upipe, "unknown parse code");
                uref_free(uref);
                upipe_throw_fatal(upipe, UBASE_ERR_LOCAL);
                return true;
        }

        src_offset += next_offset;
    } while (src_offset < src_size);

    uref_block_unmap(uref, 0);
    uref_free(uref);

#if 0
    const size_t chunk_size = (MTU / 3 / upipe_rtp_pcm_pack->channels)
        * 3 * upipe_rtp_pcm_pack->channels;

    upipe_rtp_pcm_pack_append_uref_stream(upipe, uref);

    if (upipe_rtp_pcm_pack->next_uref_size + s < chunk_size)
        return true;

    uint64_t pts_prog = 0;
    uref_clock_get_pts_prog(upipe_rtp_pcm_pack->next_uref, &pts_prog);

    do {
        uref = upipe_rtp_pcm_pack_extract_uref_stream(upipe, chunk_size);
        uref_clock_set_pts_prog(uref, pts_prog);
        pts_prog += (chunk_size / 3 / upipe_rtp_pcm_pack->channels)
            * UCLOCK_FREQ / upipe_rtp_pcm_pack->rate;
        upipe_rtp_pcm_pack_output(upipe, uref, upump_p);
    } while (uref && upipe_rtp_pcm_pack->next_uref_size > chunk_size);
#endif

    return true;
}

static struct upipe_mgr upipe_rtp_vc2_pack_mgr = {
    .refcount = NULL,
    .signature = UPIPE_RTP_VC2_PACK_SIGNATURE,

    .upipe_alloc = upipe_rtp_vc2_pack_alloc,
    .upipe_input = upipe_rtp_vc2_pack_input,
    .upipe_control = upipe_rtp_vc2_pack_control,

    .upipe_mgr_control = NULL,
};

struct upipe_mgr *upipe_rtp_vc2_pack_mgr_alloc(void)
{
    return &upipe_rtp_vc2_pack_mgr;
}