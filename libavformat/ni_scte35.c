/*
 * Copyright (c) 2024 NetInt
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <string.h>

#include "inttypes.h"

#include "libavutil/base64.h"
#include "libavutil/bprint.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"

#include "ni_scte35.h"

#define BASE64_MAX_CHARS 256 /* Adjust as needed. */

const AVRational scte35_timebase = {1, 90000};

ni_scte35_decoder *ff_alloc_ni_scte35_decoder(void) {
    ni_scte35_decoder *p;

    p = (ni_scte35_decoder *)av_mallocz(sizeof(ni_scte35_decoder));
    if (!p) {
        return NULL;
    }

    if (pthread_mutex_init(&p->lock, NULL)) {
        av_free(p);
        return NULL;
    }

    return p;
}

void ff_free_ni_scte35_decoder(ni_scte35_decoder *d) {
    struct ni_scte35_queue_node *node, *temp;

    if (!d) {
        return;
    }

    pthread_mutex_lock(&d->lock);

    node = d->queue_head;
    while (node && node->next) {
        temp = node;
        node = node->next;
        av_free(temp);
    }
    if (node) {
        av_free(node);
        d->queue_head = NULL;
    }

    pthread_mutex_unlock(&d->lock);
    pthread_mutex_destroy(&d->lock);

    av_free(d);
}

static int cue_in_to_ext_x_scte35(const AVPacket *pkt, char *tag) {
    char base64[BASE64_MAX_CHARS];
    char fmt[] = "#EXT-X-SCTE35:TYPE=0x35,CUE-IN=YES,CUE=\"%s\"\n";

    if (!av_base64_encode(base64, BASE64_MAX_CHARS, pkt->data, pkt->size)) {
        av_log(NULL, AV_LOG_ERROR, "%s: base64 encoding failed\n", __func__);
        return AVERROR_EXIT;
    }

    return sprintf(tag, fmt, base64) < 0 ? AVERROR_EXIT : 0;
}

static int cue_out_to_ext_x_scte35(const AVPacket *pkt, char *tag) {
    char base64[BASE64_MAX_CHARS];
    char fmt[] = "#EXT-X-SCTE35:TYPE=0x34,CUE-OUT=YES,CUE=\"%s\"\n";

    if (!av_base64_encode(base64, BASE64_MAX_CHARS, pkt->data, pkt->size)) {
        av_log(NULL, AV_LOG_ERROR, "%s: base64 encoding failed\n", __func__);
        return AVERROR_EXIT;
    }

    return sprintf(tag, fmt, base64) < 0 ? AVERROR_EXIT : 0;
}

// added is set to 1 if pts_time is added to queue else 0
static int try_enqueue(ni_scte35_decoder *d, const uint64_t pts_time, const char *tag, int *added) {
    int ret = 0;
    struct ni_scte35_queue_node *node;

    *added = 0;

    pthread_mutex_lock(&d->lock);

    if (d->queue_head) {
        node = d->queue_head;
        while (node->next) {
            if (node->pts == pts_time) {
                // Duplicate detected. Do not add to queue.
                goto unlock;
            }
            node = node->next;
        }
        if (node->pts == pts_time) {
            // Duplicate detected. Do not add to queue.
            goto unlock;
        }
        node->next = (struct ni_scte35_queue_node *)av_mallocz(sizeof(struct ni_scte35_queue_node));
        if (!node->next) {
            ret = AVERROR(ENOMEM);
            goto unlock;
        }
        node->next->pts = pts_time;
        strcpy(node->next->tag, tag);
        *added = 1;
    } else {
        d->queue_head = (struct ni_scte35_queue_node *)av_mallocz(sizeof(struct ni_scte35_queue_node));
        if (!d->queue_head) {
            ret = AVERROR(ENOMEM);
            goto unlock;
        }
        d->queue_head->pts = pts_time;
        strcpy(d->queue_head->tag, tag);
        *added = 1;
    }

unlock:
    pthread_mutex_unlock(&d->lock);
    return ret;
}

static int splice_time(uint8_t *data, uint64_t *pts_time) {
    int i;

    if (((*data >> 7) & 0x1) == 0) { /* time_specified_flag */
        av_log(NULL, AV_LOG_ERROR, "%s: Expected time_specified_flag to be set\n", __func__);
        return 0;
    }

    *pts_time = *data & 0x1; /* time_specified_flag + reserved + pts_time:0 */
    for (i = 0; i < 4; i++) {
        data++;
        *pts_time = *data | (*pts_time << 8);
    }

    av_log(NULL, AV_LOG_VERBOSE, "%s: pts_time is %" PRIu64 "\n", __func__, *pts_time);
    return 1;
}

static int splice_insert_program_splice_flag_set_splice_immediate_flag_clear(ni_scte35_decoder *d,
                                                                             const AVPacket *pkt,
                                                                             uint8_t *data,
                                                                             uint64_t *pts_time,
                                                                             const uint64_t pts_adjustment,
                                                                             int *added,
                                                                             int tag_fn(const AVPacket *pkt, char *s)) {
    char tag[NI_HLS_TAG_MAX_CHARS];
    int ret;

    av_log(NULL, AV_LOG_TRACE, "%s: program_splice_flag 1 splice_immediate_flag 0\n", __func__);

    if (!splice_time(data, pts_time)) {
        return AVERROR_INVALIDDATA;
    }

    *pts_time += pts_adjustment;
    av_log(NULL,
           AV_LOG_VERBOSE,
           "%s: pts_adjustment %" PRIu64 " adjusted pts_time %" PRIu64 "\n",
           __func__,
           pts_adjustment,
           *pts_time);

    ret = tag_fn(pkt, tag);
    if (ret) {
        return ret;
    }

    return try_enqueue(d, *pts_time, tag, added);
}

static int break_duration(ni_scte35_decoder *d, const AVPacket *pkt, uint8_t *data, uint64_t *pts_time) {
    char tag[NI_HLS_TAG_MAX_CHARS];
    int i, ret = 0;
    uint64_t duration;

    if (((*data >> 7) & 0x1) == 0) { /* auto_return */
        return 0;
    }

    duration = *data & 0x1; /* auto_return + reserved + duration:0 */
    for (i = 0; i < 4; i++) {
        data++;
        duration = *data | (duration << 8);
    }

    av_log(NULL, AV_LOG_VERBOSE, "%s: duration %" PRIu64 "\n", __func__, duration);

    pthread_mutex_lock(&d->lock);
    d->ignore_next_cue_in = 1;
    pthread_mutex_unlock(&d->lock);

    ret = cue_in_to_ext_x_scte35(pkt, tag);
    if (ret) {
        return ret;
    }

    return try_enqueue(d, *pts_time + duration, tag, &i); // don't care whether added or not
}

static int splice_insert(ni_scte35_decoder *d, const AVPacket *pkt, uint8_t *data, const uint64_t pts_adjustment) {
    uint64_t pts_time;
    int out_of_network_indicator_set;
    int program_splice_flag_set;
    int duration_flag_set;
    int splice_immediate_flag_set;
    int ignore_next_cue_in = 0;
    int added, ret = 0;

    av_log(NULL, AV_LOG_TRACE, "%s\n", __func__);

    data += 4;

    if (((*data >> 7) & 0x1) == 1) { /* splice_event_cancel_indicator */
        return ret;
    }

    av_log(NULL, AV_LOG_VERBOSE, "%s: splice_event_cancel_indicator 0\n", __func__);

    data++;
    out_of_network_indicator_set = (((*data >> 7) & 0x1) == 1);
    program_splice_flag_set = (((*data >> 6) & 0x1) == 1);
    duration_flag_set = (((*data >> 5) & 0x1) == 1);
    splice_immediate_flag_set = (((*data >> 4) & 0x1) == 1);

    av_log(NULL, AV_LOG_VERBOSE, "%s: out_of_network_indicator_set %d\n", __func__, out_of_network_indicator_set);
    if (out_of_network_indicator_set) {
        if (program_splice_flag_set && !splice_immediate_flag_set) {
            data++;
            ret = splice_insert_program_splice_flag_set_splice_immediate_flag_clear(d,
                                                                                    pkt,
                                                                                    data,
                                                                                    &pts_time,
                                                                                    pts_adjustment,
                                                                                    &added,
                                                                                    cue_out_to_ext_x_scte35);
            if (ret < 0 || !added) {
                return ret;
            }
            pthread_mutex_lock(&d->lock);
            if (d->ignore_next_cue_in) {
                d->ignore_next_cue_in = 0;
            }
            pthread_mutex_unlock(&d->lock);
            if (duration_flag_set) {
                data += 5;
                ret = break_duration(d, pkt, data, &pts_time);
            }
        }
    } else {
        if (program_splice_flag_set && !splice_immediate_flag_set) {
            pthread_mutex_lock(&d->lock);
            if (d->ignore_next_cue_in) {
                ignore_next_cue_in = 1;
                d->ignore_next_cue_in = 0;
            }
            pthread_mutex_unlock(&d->lock);
            if (!ignore_next_cue_in) {
                data++;
                ret = splice_insert_program_splice_flag_set_splice_immediate_flag_clear(d,
                                                                                        pkt,
                                                                                        data,
                                                                                        &pts_time,
                                                                                        pts_adjustment,
                                                                                        &added,
                                                                                        cue_in_to_ext_x_scte35);
            }
        }
    }

    return ret;
}

static void dump_hex(uint8_t *data, const int size) {
    int i;
    AVBPrint b;

    av_bprint_init(&b, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&b, "%s: 0x", __func__);
    for (i = 0; i < size; i++) {
        av_bprintf(&b, "%02x", *(data + i));
    }
    av_log(NULL, AV_LOG_DEBUG, "%s\n", b.str);
    av_bprint_finalize(&b, NULL);
}

int decode_scte35(ni_scte35_decoder *d, const AVPacket *pkt) {
    int i, ret = 0;
    uint8_t *data;
    uint64_t pts_adjustment = 0;

    if (!pkt)
        return 0;

    data = pkt->data;
    if (*data != 0xFC) /* table_id */
        return AVERROR_INVALIDDATA;

    dump_hex(data, pkt->size);

    data += 4;
    pts_adjustment = *data & 0x1; /* encrypted_packet + encrypted_algorithm + pts_adjustment:0 */
    for (i = 0; i < 4; i++) {
        data++;
        pts_adjustment = *data | (pts_adjustment << 8);
    }

    data += 5;
    switch (*data) { /* splice_command_type */
    case 0x05:
        data++;
        ret = splice_insert(d, pkt, data, pts_adjustment);
        break;
    default:
        break;
    }

    return ret;
}

int is_scte35_keyframe(ni_scte35_decoder *d, const int64_t pts, const AVRational *pts_timebase) {
    int ret = 0;
    struct ni_scte35_queue_node *node;

    pthread_mutex_lock(&d->lock);

    if (!d->queue_head) {
        goto unlock;
    }

    if (av_compare_ts(pts, *pts_timebase, d->queue_head->pts, scte35_timebase) >= 0) {
        av_log(NULL,
               AV_LOG_DEBUG,
               "%s: in pts %" PRId64 " tb %d/%d v.s. head pts %" PRId64" tb %d/%d\n",
               __func__,
               pts,
               pts_timebase->num,
               pts_timebase->den,
               d->queue_head->pts,
               scte35_timebase.num,
               scte35_timebase.den);

        node = d->queue_head;
        // Remove node from queue.
        d->queue_head = d->queue_head->next;
        av_free(node);

        ret = 1;
    }

unlock:
    pthread_mutex_unlock(&d->lock);
    return ret;
}

int is_at_splice_point(ni_scte35_decoder *d, const int64_t pts, const AVRational *pts_timebase) {
    int ret = 0;

    pthread_mutex_lock(&d->lock);

    if (!d->queue_head) {
        goto unlock;
    }

    if (av_compare_ts(pts, *pts_timebase, d->queue_head->pts, scte35_timebase) >= 0) {
        av_log(NULL,
               AV_LOG_VERBOSE,
               "%s: in pts %" PRId64 " tb %d/%d v.s. head pts %" PRId64" tb %d/%d\n",
               __func__,
               pts,
               pts_timebase->num,
               pts_timebase->den,
               d->queue_head->pts,
               scte35_timebase.num,
               scte35_timebase.den);

        ret = 1;
    }

unlock:
    pthread_mutex_unlock(&d->lock);
    return ret;
}

void try_get_scte35_tag(ni_scte35_decoder *d, const int64_t pts, const AVRational *pts_timebase, char *s) {
    struct ni_scte35_queue_node *node;

    pthread_mutex_lock(&d->lock);

    if (!d->queue_head) {
        goto unlock;
    }

    node = d->queue_head;
    if (av_compare_ts(pts, *pts_timebase, node->pts, scte35_timebase) >= 0) {
        av_log(NULL,
               AV_LOG_DEBUG,
               "%s: in pts %" PRId64 " tb %d/%d v.s. head pts %" PRId64" tb %d/%d: tag %s\n",
               __func__,
               pts,
               pts_timebase->num,
               pts_timebase->den,
               node->pts,
               scte35_timebase.num,
               scte35_timebase.den,
               node->tag);

        strcpy(s, node->tag);

        // Remove node from queue.
        d->queue_head = d->queue_head->next;
        av_free(node);
    }

unlock:
    pthread_mutex_unlock(&d->lock);
}
