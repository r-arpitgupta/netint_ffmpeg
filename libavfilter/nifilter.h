/*
 * XCoder Filter Lib Wrapper
 *
 * Copyright (c) 2020 NetInt
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

/**
 * @file
 * XCoder codec lib wrapper.
 */

#ifndef AVFILTER_NIFILTER_H
#define AVFILTER_NIFILTER_H

#include "version.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_ni_quad.h"

#include <ni_device_api.h>

#define IS_FFMPEG_342_AND_ABOVE                                                \
    ((LIBAVFILTER_VERSION_MAJOR > 6) ||                                        \
     (LIBAVFILTER_VERSION_MAJOR == 6 && LIBAVFILTER_VERSION_MINOR >= 107))

#define IS_FFMPEG_43_AND_ABOVE                                                \
    ((LIBAVFILTER_VERSION_MAJOR > 7) ||                                        \
     (LIBAVFILTER_VERSION_MAJOR == 7 && LIBAVFILTER_VERSION_MINOR >= 85))

#define IS_FFMPEG_61_AND_ABOVE                                                \
    ((LIBAVFILTER_VERSION_MAJOR > 9) ||                                        \
     (LIBAVFILTER_VERSION_MAJOR == 9 && LIBAVFILTER_VERSION_MINOR >= 12))

#define IS_FFMPEG_70_AND_ABOVE                                                \
    ((LIBAVFILTER_VERSION_MAJOR > 10) ||                                      \
     (LIBAVFILTER_VERSION_MAJOR == 10 && LIBAVFILTER_VERSION_MINOR >= 1))

#define IS_FFMPEG_71_AND_ABOVE                                                \
    ((LIBAVFILTER_VERSION_MAJOR > 10) ||                                      \
     (LIBAVFILTER_VERSION_MAJOR == 10 && LIBAVFILTER_VERSION_MINOR >= 4))

#define DEFAULT_NI_FILTER_POOL_SIZE     4

#define NI_FILT_OPTION_KEEPALIVE                                                           \
    { "keep_alive_timeout", "Specify a custom session keep alive timeout in seconds.",     \
      OFFSET(keep_alive_timeout), AV_OPT_TYPE_INT, {.i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT}, \
      NI_MIN_KEEP_ALIVE_TIMEOUT, NI_MAX_KEEP_ALIVE_TIMEOUT, FLAGS }

#define NI_FILT_OPTION_KEEPALIVE10                                                         \
    { "keep_alive_timeout", "Specify a custom session keep alive timeout in seconds.",     \
      OFFSET(keep_alive_timeout), AV_OPT_TYPE_INT, {.i64 = 10}, NI_MIN_KEEP_ALIVE_TIMEOUT, \
      NI_MAX_KEEP_ALIVE_TIMEOUT, FLAGS }

#define NI_FILT_OPTION_BUFFER_LIMIT                                                     \
    { "buffer_limit", "Limit output buffering", OFFSET(buffer_limit), AV_OPT_TYPE_BOOL, \
      {.i64 = 0}, 0, 1, FLAGS }

#define NI_FILT_OPTION_IS_P2P                                                              \
    { "is_p2p", "enable p2p transfer", OFFSET(is_p2p), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, \
      FLAGS}

#define NI_FILT_OPTION_AUTO_SKIP                                                            \
    { "auto_skip", "skip processing when output would be same as input", OFFSET(auto_skip), \
      AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS}

void ff_ni_update_benchmark(const char *fmt, ...);
int ff_ni_ffmpeg_to_gc620_pix_fmt(enum AVPixelFormat pix_fmt);
int ff_ni_ffmpeg_to_libxcoder_pix_fmt(enum AVPixelFormat pix_fmt);
int ff_ni_copy_device_to_host_frame(AVFrame *dst, const ni_frame_t *src, int pix_fmt);
int ff_ni_copy_host_to_device_frame(ni_frame_t *dst, const AVFrame *src, int pix_fmt);
int ff_ni_build_frame_pool(ni_session_context_t *ctx,int width,int height, enum AVPixelFormat out_format, int pool_size, int buffer_limit);
void ff_ni_frame_free(void *opaque, uint8_t *data);
void ff_ni_set_bit_depth_and_encoding_type(int8_t *p_bit_depth,
                                           int8_t *p_enc_type,
                                           enum AVPixelFormat pix_fmt);

#endif
