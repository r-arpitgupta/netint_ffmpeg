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

#ifndef AVFORMAT_NI_SCTE35_H
#define AVFORMAT_NI_SCTE35_H

#include "libavcodec/packet.h"

#include "libavutil/rational.h"
#include "libavutil/thread.h"

#define NI_HLS_TAG_MAX_CHARS 1024 /* Adjust as needed. */

struct ni_scte35_queue_node {
    uint64_t pts;
    char tag[NI_HLS_TAG_MAX_CHARS];
    struct ni_scte35_queue_node *next;
};

typedef struct {
    pthread_mutex_t lock;
    struct ni_scte35_queue_node *queue_head;
    int ignore_next_cue_in;
} ni_scte35_decoder;

ni_scte35_decoder *ff_alloc_ni_scte35_decoder(void);
void ff_free_ni_scte35_decoder(ni_scte35_decoder *d);

int decode_scte35(ni_scte35_decoder *d, const AVPacket *pkt);
int is_scte35_keyframe(ni_scte35_decoder *d, const int64_t pts, const AVRational *pts_timebase);
int is_at_splice_point(ni_scte35_decoder *d, const int64_t pts, const AVRational *pts_timebase);
void try_get_scte35_tag(ni_scte35_decoder *d, const int64_t pts, const AVRational *pts_timebase, char *s);

#endif /* AVFORMAT_NI_SCTE35_H */
