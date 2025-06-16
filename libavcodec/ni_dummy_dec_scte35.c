/*
 * XCoder SCTE-35 Dummy Decoder
 * Copyright (c) 2025 NetInt
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
 * SCTE-35 Dummy Decoder
 */

#include "avcodec.h"
#include "codec_internal.h"

#include "libavutil/opt.h"

typedef struct scte_35_ctx {
    AVClass *class;
    uint64_t pts_adjustment;
} scte_35_ctx;

static int scte35_dummy_decoder_init(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_INFO, "scte35_dummy_decoder_init.\n");
    return 0;
}

static int scte35_dummy_decoder_close(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_INFO, "scte35_dummy_decoder_close.\n");
    return 0;
}

static int scte35_dummy_decode(AVCodecContext *avctx, AVFrame *frame,
                               int *got_frame, AVPacket *avpkt)
{
    av_log(avctx, AV_LOG_INFO, "scte35_dummy_decode.\n");
    return 0;
}

static const AVOption dec_options[] = {
    {NULL}
};

static const AVClass scte35_xcoderdec_dummy_class = {
  .class_name = "scte35_xcoder_dummy_dec",
  .item_name = av_default_item_name,
  .option = dec_options,
  .version = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_scte35_ni_dummy_decoder = {
    .p.name         = "scte35_ni_dummy_dec",
    CODEC_LONG_NAME("SCTE-35 NETINT dummy decoder"),
    .p.type         = AVMEDIA_TYPE_DATA,
    .p.id           = AV_CODEC_ID_SCTE_35,
    .priv_data_size = sizeof(scte_35_ctx),
    .init           = scte35_dummy_decoder_init,
    .close          = scte35_dummy_decoder_close,
    FF_CODEC_DECODE_CB(scte35_dummy_decode),
    .p.priv_class   = &scte35_xcoderdec_dummy_class,
    .p.capabilities = AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_DELAY,
};
