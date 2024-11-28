/*
* Copyright (c) 2023 NetInt
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

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "filters.h"
#include "formats.h"
#include "internal.h"
#if HAVE_IO_H
#include <io.h>
#endif
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"
#include "libswscale/swscale.h"
#include "ni_device_api.h"
#include "ni_util.h"
#include "nifilter.h"
#include "video.h"

#define NI_NUM_FRAMES_IN_QUEUE 8

#define NI_AI_PRE_KEEPALIVE_TIMEOUT 10

typedef struct _ni_ai_pre_network_layer {
   int32_t width;
   int32_t height;
   int32_t channel;
   int32_t classes;
   int32_t component;
   int32_t output_number;
   float *output;
} ni_ai_pre_network_layer_t;

typedef struct _ni_ai_pre_network {
   int32_t netw;
   int32_t neth;
   int32_t net_out_w;
   int32_t net_out_h;
   ni_network_data_t raw;
   ni_ai_pre_network_layer_t *layers;
} ni_ai_pre_network_t;

typedef struct AiContext {
   ni_session_context_t api_ctx;
   ni_session_data_io_t api_src_frame;
   ni_session_data_io_t api_dst_frame;
} AiContext;

typedef struct NetIntAiPreprocessContext {
   const AVClass *class;
   const char *nb_file;  /* path to network binary */
   int initialized;
   int devid;
   int out_width, out_height;

   AiContext *ai_ctx;

   AVBufferRef *out_frames_ref;

   ni_ai_pre_network_t network;
   int keep_alive_timeout; /* keep alive timeout setting */
   int ai_timeout;
   int channel_mode;
   int buffer_limit;
} NetIntAiPreprocessContext;

static int ni_ai_pre_query_formats(AVFilterContext *ctx) {
   AVFilterFormats *formats;

   static const enum AVPixelFormat pix_fmts[] = {
       AV_PIX_FMT_YUV420P,
       AV_PIX_FMT_YUVJ420P,
       AV_PIX_FMT_YUV420P10LE,
       AV_PIX_FMT_NI_QUAD,
       AV_PIX_FMT_NONE,
   };

   formats = ff_make_format_list(pix_fmts);
   if (!formats)
       return AVERROR(ENOMEM);

   return ff_set_common_formats(ctx, formats);
}

static void cleanup_ai_context(AVFilterContext *ctx, NetIntAiPreprocessContext *s) {
    ni_retcode_t retval;
    AiContext *ai_ctx = s->ai_ctx;

    if (ai_ctx) {
        ni_frame_buffer_free(&ai_ctx->api_src_frame.data.frame);
        ni_frame_buffer_free(&ai_ctx->api_dst_frame.data.frame);
        ni_packet_buffer_free(&ai_ctx->api_dst_frame.data.packet);
        //ni_packet_buffer_free(&ai_ctx->api_dst_pkt.data.packet);

        retval =
            ni_device_session_close(&ai_ctx->api_ctx, 1, NI_DEVICE_TYPE_AI);
        if (retval != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR,
                    "%s: failed to close ai session. retval %d\n", __func__,
                    retval);
        }
        if (ai_ctx->api_ctx.hw_action != NI_CODEC_HW_ENABLE)
        {
#ifdef _WIN32
            if (ai_ctx->api_ctx.device_handle != NI_INVALID_DEVICE_HANDLE)
            {
                ni_device_close(ai_ctx->api_ctx.device_handle);
            }
#elif __linux__
            if (ai_ctx->api_ctx.device_handle != NI_INVALID_DEVICE_HANDLE)
            {
                ni_device_close(ai_ctx->api_ctx.device_handle);
            }
            if (ai_ctx->api_ctx.blk_io_handle != NI_INVALID_DEVICE_HANDLE)
            {
                ni_device_close(ai_ctx->api_ctx.blk_io_handle);
            }
#endif
            ai_ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
            ai_ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
        }      
        ni_device_session_context_clear(&ai_ctx->api_ctx);
        av_free(ai_ctx);
        s->ai_ctx = NULL;
    }
}

static int init_ai_context(AVFilterContext *ctx, NetIntAiPreprocessContext *s,
                          AVFrame *frame) {
   ni_retcode_t retval;
   AiContext *ai_ctx;
   ni_ai_pre_network_t *network = &s->network;
   int hwframe = frame->format == AV_PIX_FMT_NI_QUAD ? 1 : 0;
   int ret;

#if HAVE_IO_H
   if ((s->nb_file == NULL) || (_access(s->nb_file, R_OK) != 0)) {
#else
   if ((s->nb_file == NULL) || (access(s->nb_file, R_OK) != 0)) {
#endif
       av_log(ctx, AV_LOG_ERROR, "invalid network binary path\n");
       return AVERROR(EINVAL);
   }

   ai_ctx = av_mallocz(sizeof(AiContext));
   if (!ai_ctx) {
       av_log(ctx, AV_LOG_ERROR, "failed to allocate ai context\n");
       return AVERROR(ENOMEM);
   }
   s->ai_ctx = ai_ctx;
   retval = ni_device_session_context_init(&ai_ctx->api_ctx);
    if (retval != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "ai session context init failure\n");
        return AVERROR(EIO);
    }

    AVHWFramesContext *pAVHFWCtx;
    AVNIDeviceContext *pAVNIDevCtx;
    AVHWFramesContext *out_frames_ctx;
    NIFramesContext *ni_ctx;
    int cardno;
    if (hwframe)
    {
        pAVHFWCtx   = (AVHWFramesContext *)frame->hw_frames_ctx->data;
        pAVNIDevCtx = (AVNIDeviceContext *)pAVHFWCtx->device_ctx->hwctx;
        cardno      = ni_get_cardno(frame);

        ai_ctx->api_ctx.device_handle = pAVNIDevCtx->cards[cardno];
        ai_ctx->api_ctx.blk_io_handle = pAVNIDevCtx->cards[cardno];
        ai_ctx->api_ctx.hw_action     = NI_CODEC_HW_ENABLE;
        ai_ctx->api_ctx.hw_id         = cardno;
    }
    else
        ai_ctx->api_ctx.hw_id     = s->devid;


   ai_ctx->api_ctx.device_type = NI_DEVICE_TYPE_AI;
   ai_ctx->api_ctx.keep_alive_timeout = s->keep_alive_timeout;

   retval = ni_device_session_open(&ai_ctx->api_ctx, NI_DEVICE_TYPE_AI);
   if (retval != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "failed to open ai session. retval %d\n",
               retval);
        ret = AVERROR(EIO);
        goto failed_out;
   }

   retval = ni_ai_config_network_binary(&ai_ctx->api_ctx, &network->raw,
                                        s->nb_file);
   if (retval != NI_RETCODE_SUCCESS) {
       av_log(ctx, AV_LOG_ERROR, "failed to configure ai session. retval %d\n",
              retval);
       ret = AVERROR(EIO);
       goto failed_out;
   }

    if (!hwframe)
    {
        return 0;
    }
   out_frames_ctx = (AVHWFramesContext *)s->out_frames_ref->data;
   ni_ctx = out_frames_ctx->internal->priv;
   ni_ctx->api_ctx.session_timestamp = ai_ctx->api_ctx.session_timestamp;

   // Create frame pool
   int format = ff_ni_ffmpeg_to_gc620_pix_fmt(pAVHFWCtx->sw_format);
   int options = NI_AI_FLAG_IO |  NI_AI_FLAG_PC;
   if (s->buffer_limit)
       options |= NI_AI_FLAG_LM;

   /* Allocate a pool of frames by the AI */
   retval = ni_device_alloc_frame(&ai_ctx->api_ctx,
                              FFALIGN(s->out_width,2),
                              FFALIGN(s->out_height,2),
                              format,
                              options,
                              0, // rec width
                              0, // rec height
                              0, // rec X pos
                              0, // rec Y pos
                              8, // rgba color/pool size
                              0, // frame index
                              NI_DEVICE_TYPE_AI);
   if(retval != NI_RETCODE_SUCCESS){
       av_log(ctx, AV_LOG_ERROR, "failed to create buffer pool\n");
       ret = AVERROR(ENOMEM);
       goto failed_out;
   }
   retval = ni_frame_buffer_alloc_hwenc(&ai_ctx->api_dst_frame.data.frame,
                                         FFALIGN(s->out_width,2), FFALIGN(s->out_height,2), 0);

   if (retval != NI_RETCODE_SUCCESS) {
       av_log(ctx, AV_LOG_ERROR, "failed to allocate ni dst frame\n");
       ret = AVERROR(ENOMEM);
       goto failed_out;
   }

   return 0;

failed_out:
   cleanup_ai_context(ctx, s);
   return ret;
}

static void ni_destroy_network(AVFilterContext *ctx,
                              ni_ai_pre_network_t *network) {
   if (network) {
       int i;

       if (network->layers) {
           for (i = 0; i < network->raw.output_num; i++) {
               if (network->layers[i].output) {
                   free(network->layers[i].output);
                   network->layers[i].output = NULL;
               }
           }

           free(network->layers);
           network->layers = NULL;
       }
   }
}

static int ni_create_network(AVFilterContext *ctx, ni_ai_pre_network_t *network) {
   int ret;
   int i;
   ni_network_data_t *ni_network = &network->raw;

   av_log(ctx, AV_LOG_VERBOSE, "network input number %d, output number %d\n",
          ni_network->input_num, ni_network->output_num);

   if (ni_network->input_num == 0 || ni_network->output_num == 0) {
       av_log(ctx, AV_LOG_ERROR, "invalid network layer\n");
       return AVERROR(EINVAL);
   }

   network->layers =
       malloc(sizeof(ni_ai_pre_network_layer_t) * ni_network->output_num);
   if (!network->layers) {
       av_log(ctx, AV_LOG_ERROR, "cannot allocate network layer memory\n");
       return AVERROR(ENOMEM);
   }
   memset(network->layers, 0,
          sizeof(ni_ai_pre_network_layer_t) * ni_network->output_num);

   for (i = 0; i < ni_network->output_num; i++) {
       network->layers[i].width     = ni_network->linfo.out_param[i].sizes[0];
       network->layers[i].height    = ni_network->linfo.out_param[i].sizes[1];
       network->layers[i].channel   = ni_network->linfo.out_param[i].sizes[2];
       network->layers[i].component = 3;
       network->layers[i].classes =
           (network->layers[i].channel / network->layers[i].component) -
           (4 + 1);
       network->layers[i].output_number =
           ni_ai_network_layer_dims(&ni_network->linfo.out_param[i]);
       av_assert0(network->layers[i].output_number ==
                  network->layers[i].width * network->layers[i].height *
                      network->layers[i].channel);

       network->layers[i].output =
           malloc(network->layers[i].output_number * sizeof(float));
       if (!network->layers[i].output) {
           av_log(ctx, AV_LOG_ERROR,
                  "failed to allocate network layer %d output buffer\n", i);
           ret = AVERROR(ENOMEM);
           goto out;
       }

       av_log(ctx, AV_LOG_DEBUG,
              "network layer %d: w %d, h %d, ch %d, co %d, cl %d\n", i,
              network->layers[i].width, network->layers[i].height,
              network->layers[i].channel, network->layers[i].component,
              network->layers[i].classes);
   }

   network->netw = ni_network->linfo.in_param[0].sizes[1];
   network->neth = ni_network->linfo.in_param[0].sizes[2];
   network->net_out_w = ni_network->linfo.out_param[0].sizes[1];
   network->net_out_h = ni_network->linfo.out_param[0].sizes[2];

   return 0;
out:
   ni_destroy_network(ctx, network);
   return ret;
}

static int ni_ai_pre_config_input(AVFilterContext *ctx, AVFrame *frame) {
   NetIntAiPreprocessContext *s = ctx->priv;
   int ret;

   if (s->initialized)
       return 0;

   ret = init_ai_context(ctx, s, frame);
   if (ret < 0) {
       av_log(ctx, AV_LOG_ERROR, "failed to initialize ai context\n");
       return ret;
   }

   ret = ni_create_network(ctx, &s->network);
   if (ret != 0) {
       goto fail_out;
   }

    if (s->channel_mode == 0) {
        if((s->network.netw != frame->width && s->network.neth != frame->height) &&
            s->network.netw != FFALIGN(frame->width, 128)){
            av_log(ctx, AV_LOG_ERROR, "Model not match input, "
                                        "model resolution=%dx%d, "
                                        "input resolution=%dx%d\n",
                    s->network.netw, s->network.neth,
                    frame->width, frame->height);
            ret = AVERROR(EINVAL);
            goto fail_out;
        }

        if((s->network.net_out_w != s->out_width && s->network.net_out_h != s->out_height) &&
            s->network.net_out_w != FFALIGN(s->out_width, 128)){
            av_log(ctx, AV_LOG_ERROR, "Model not match output, "
                                        "model resolution=%dx%d, "
                                        "input resolution=%dx%d\n",
                    s->network.net_out_w, s->network.net_out_h,
                    s->out_width, s->out_height);
            ret = AVERROR(EINVAL);
            goto fail_out;
        }
    }

   s->initialized = 1;
   return 0;

fail_out:
   cleanup_ai_context(ctx, s);

   ni_destroy_network(ctx, &s->network);

   return ret;
}

static av_cold int ni_ai_pre_init(AVFilterContext *ctx) {
   NetIntAiPreprocessContext *s = ctx->priv;
#if HAVE_IO_H
   if ((s->nb_file == NULL) || (_access(s->nb_file, R_OK) != 0)) {
#else
   if ((s->nb_file == NULL) || (access(s->nb_file, R_OK) != 0)) {
#endif
       av_log(ctx, AV_LOG_ERROR, "invalid network binary path\n");
       return AVERROR(EINVAL);
   }
   return 0;
}

static av_cold void ni_ai_pre_uninit(AVFilterContext *ctx) {
   NetIntAiPreprocessContext *s       = ctx->priv;
   ni_ai_pre_network_t *network = &s->network;

   cleanup_ai_context(ctx, s);

   ni_destroy_network(ctx, network);

   av_buffer_unref(&s->out_frames_ref);
   s->out_frames_ref = NULL;
}

static int ni_ai_pre_output_config_props(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    AVHWFramesContext *in_frames_ctx;
    AVHWFramesContext *out_frames_ctx;
    NetIntAiPreprocessContext *s = ctx->priv;
    int out_width, out_height;

    if ((inlink->hw_frames_ctx == NULL) && (inlink->format == AV_PIX_FMT_NI_QUAD)) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }

    if(s->out_width == -1 || s->out_height == -1){
        out_width = inlink->w;
        out_height = inlink->h;
        s->out_width = out_width;
        s->out_height = out_height;
    }else{
        out_width = s->out_width;
        out_height = s->out_height;
    }

    outlink->w = out_width;
    outlink->h = out_height;

    if (inlink->hw_frames_ctx == NULL) {
        av_log(ctx, AV_LOG_DEBUG, "sw frame\n");
        return 0;
    }

    in_frames_ctx = (AVHWFramesContext *)ctx->inputs[0]->hw_frames_ctx->data;

    if (in_frames_ctx->format != AV_PIX_FMT_NI_QUAD) {
        av_log(ctx, AV_LOG_ERROR, "sw frame not supported, format=%d\n", in_frames_ctx->format);
        return AVERROR(EINVAL);
    }
    if (in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
        in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) {
        av_log(ctx, AV_LOG_ERROR, "tile4x4 not supported\n");
        return AVERROR(EINVAL);
    }


    s->out_frames_ref = av_hwframe_ctx_alloc(in_frames_ctx->device_ref);
    if (!s->out_frames_ref)
        return AVERROR(ENOMEM);

    out_frames_ctx = (AVHWFramesContext *)s->out_frames_ref->data;

    out_frames_ctx->format            = AV_PIX_FMT_NI_QUAD;
    out_frames_ctx->width             = outlink->w;
    out_frames_ctx->height            = outlink->h;
    out_frames_ctx->sw_format         = in_frames_ctx->sw_format;
    out_frames_ctx->initial_pool_size = NI_AI_PREPROCESS_ID;

    av_buffer_unref(&ctx->outputs[0]->hw_frames_ctx);
    ctx->outputs[0]->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);

    if (!ctx->outputs[0]->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

static int av_to_niframe_copy(ni_frame_t *dst, const AVFrame *src, int nb_planes) {
    int dst_stride[4],src_height[4], hpad[4], vpad[4];
    int i, j, h;
    uint8_t *src_line, *dst_line, YUVsample, *sample, *dest;
    uint16_t lastidx;
    bool tenBit;
    uint32_t src_size = 0;

    switch (src->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        dst_stride[0] = FFALIGN(src->width, 128);
        dst_stride[1] = FFALIGN((src->width / 2), 128);
        dst_stride[2] = dst_stride[1];
        dst_stride[3] = 0;
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = FFMAX(dst_stride[1] - src->linesize[1], 0);
        hpad[2] = FFMAX(dst_stride[2] - src->linesize[2], 0);
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = FFALIGN(src->height, 2) / 2;
        src_height[2] = FFALIGN(src->height, 2) / 2;
        src_height[3] = 0;

        vpad[0] = FFALIGN(src_height[0], 2) - src_height[0];
        vpad[1] = FFALIGN(src_height[1], 2) - src_height[1];
        vpad[2] = FFALIGN(src_height[2], 2) - src_height[2];
        vpad[3] = 0;

        tenBit = false;
        break;
    case AV_PIX_FMT_YUV420P10LE:
        dst_stride[0] = FFALIGN(src->width * 2, 128);
        dst_stride[1] = FFALIGN(src->width, 128);
        dst_stride[2] = dst_stride[1];
        dst_stride[3] = 0;
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = FFMAX(dst_stride[1] - src->linesize[1], 0);
        hpad[2] = FFMAX(dst_stride[2] - src->linesize[2], 0);
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = FFALIGN(src->height, 2) / 2;
        src_height[2] = FFALIGN(src->height, 2) / 2;
        src_height[3] = 0;

        vpad[0] = FFALIGN(src_height[0], 2) - src_height[0];
        vpad[1] = FFALIGN(src_height[1], 2) - src_height[1];
        vpad[2] = FFALIGN(src_height[2], 2) - src_height[2];
        vpad[3] = 0;

        tenBit = true;
        break;
    case AV_PIX_FMT_NV12:
        dst_stride[0] = FFALIGN(src->width, 128);
        dst_stride[1] = dst_stride[0];
        dst_stride[2] = 0;
        dst_stride[3] = 0;
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = FFMAX(dst_stride[1] - src->linesize[1], 0);
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = FFALIGN(src->height, 2) / 2;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = FFALIGN(src_height[0], 2) - src_height[0];
        vpad[1] = FFALIGN(src_height[1], 2) - src_height[1];
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;
    case AV_PIX_FMT_NV16:
        dst_stride[0] = FFALIGN(src->width, 64);
        dst_stride[1] = dst_stride[0];
        dst_stride[2] = 0;
        dst_stride[3] = 0;
        hpad[0] = 0;
        hpad[1] = 0;
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = src->height;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = 0;
        vpad[1] = 0;
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;
    case AV_PIX_FMT_P010LE:
        dst_stride[0] = FFALIGN(src->width * 2, 128);
        dst_stride[1] = dst_stride[0];
        dst_stride[2] = 0;
        dst_stride[3] = 0;
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = FFMAX(dst_stride[1] - src->linesize[1], 0);
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = FFALIGN(src->height, 2) / 2;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = FFALIGN(src_height[0], 2) - src_height[0];
        vpad[1] = FFALIGN(src_height[1], 2) - src_height[1];
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = true;
        break;
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_BGR0:
        dst_stride[0] = FFALIGN(src->width, 16) * 4;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = 0;
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = 0;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = 0;
        vpad[1] = 0;
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;
    case AV_PIX_FMT_BGRP:
        dst_stride[0] = FFALIGN(src->width, 16) * 4;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = FFMAX(dst_stride[1] - src->linesize[1], 0);
        hpad[2] = FFMAX(dst_stride[2] - src->linesize[2], 0);
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = src->height;
        src_height[2] = src->height;
        src_height[3] = 0;

        vpad[0] = 0;
        vpad[1] = 0;
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;
    case AV_PIX_FMT_YUYV422:
    case AV_PIX_FMT_UYVY422:
        dst_stride[0] = FFALIGN(src->width, 16) * 2;
        dst_stride[1] = 0;
        dst_stride[2] = 0;
        dst_stride[3] = 0;
        hpad[0] = FFMAX(dst_stride[0] - src->linesize[0], 0);
        hpad[1] = 0;
        hpad[2] = 0;
        hpad[3] = 0;

        src_height[0] = src->height;
        src_height[1] = 0;
        src_height[2] = 0;
        src_height[3] = 0;

        vpad[0] = 0;
        vpad[1] = 0;
        vpad[2] = 0;
        vpad[3] = 0;

        tenBit = false;
        break;
    default:
        av_log(NULL, AV_LOG_ERROR, "Pixel format %s not supported\n",
               av_get_pix_fmt_name(src->format));
        return AVERROR(EINVAL);
    }

    for (i = 0; i < nb_planes; i++)
    {
        src_size +=  (dst_stride[i] + vpad[i]) * src_height[i];
    }
    if (dst->buffer_size < src_size)
    {
        av_log(NULL,AV_LOG_ERROR, "av_to_niframe_copy failed, "
                "please check the input/output frame reslotion "
                "and the module reslotion\n");
        return -1;
    }
    dst_line = dst->p_buffer;
    for (i = 0; i < nb_planes; i++) {
        src_line = src->data[i];
        for (h = 0; h < src_height[i]; h++) {
            memcpy(dst_line, src_line, FFMIN(src->linesize[i], dst_stride[i]));

            if (hpad[i]) {
                lastidx = src->linesize[i];

                if (tenBit) {
                    sample = &src_line[lastidx - 2];
                    dest   = &dst_line[lastidx];

                    /* two bytes per sample */
                    for (j = 0; j < hpad[i] / 2; j++) {
                        memcpy(dest, sample, 2);
                        dest += 2;
                    }
                } else {
                    YUVsample = dst_line[lastidx - 1];
                    memset(&dst_line[lastidx], YUVsample, hpad[i]);
                }
            }

            src_line += src->linesize[i];
            dst_line += dst_stride[i];
        }

        /* Extend the height by cloning the last line */
        src_line = dst_line - dst_stride[i];
        for (h = 0; h < vpad[i]; h++) {
            memcpy(dst_line, src_line, dst_stride[i]);
            dst_line += dst_stride[i];
        }
    }

    return 0;
}

static int ni_to_avframe_copy(AVFrame *dst, const ni_packet_t *src, int nb_planes) {
    int src_linesize[4], src_height[4];
    int i, h;
    uint8_t *src_line, *dst_line;
    uint32_t dst_size = 0;
    switch (dst->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        src_linesize[0] = FFALIGN(dst->width, 128);
        src_linesize[1] = FFALIGN(dst->width / 2, 128);
        src_linesize[2] = src_linesize[1];
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = FFALIGN(dst->height, 2) / 2;
        src_height[2] = src_height[1];
        src_height[3] = 0;
        break;
    case AV_PIX_FMT_YUV420P10LE:
        src_linesize[0] = FFALIGN(dst->width * 2, 128);
        src_linesize[1] = FFALIGN(dst->width, 128);
        src_linesize[2] = src_linesize[1];
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = FFALIGN(dst->height, 2) / 2;
        src_height[2] = src_height[1];
        src_height[3] = 0;
        break;
    case AV_PIX_FMT_NV12:
        src_linesize[0] = FFALIGN(dst->width, 128);
        src_linesize[1] = FFALIGN(dst->width, 128);
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = FFALIGN(dst->height, 2) / 2;
        src_height[2] = 0;
        src_height[3] = 0;
        break;
    case AV_PIX_FMT_NV16:
        src_linesize[0] = FFALIGN(dst->width, 64);
        src_linesize[1] = FFALIGN(dst->width, 64);
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = dst->height;
        src_height[2] = 0;
        src_height[3] = 0;
        break;
    case AV_PIX_FMT_YUYV422:
    case AV_PIX_FMT_UYVY422:
        src_linesize[0] = FFALIGN(dst->width, 16) * 2;
        src_linesize[1] = 0;
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = 0;
        src_height[2] = 0;
        src_height[3] = 0;
        break;
    case AV_PIX_FMT_P010LE:
        src_linesize[0] = FFALIGN(dst->width * 2, 128);
        src_linesize[1] = FFALIGN(dst->width * 2, 128);
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = FFALIGN(dst->height, 2) / 2;
        src_height[2] = 0;
        src_height[3] = 0;
        break;
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_ABGR:
    case AV_PIX_FMT_ARGB:
    case AV_PIX_FMT_BGR0:
        src_linesize[0] = FFALIGN(dst->width, 16) * 4;
        src_linesize[1] = 0;
        src_linesize[2] = 0;
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = 0;
        src_height[2] = 0;
        src_height[3] = 0;
        break;
    case AV_PIX_FMT_BGRP:
        src_linesize[0] = FFALIGN(dst->width, 32);
        src_linesize[1] = FFALIGN(dst->width, 32);
        src_linesize[2] = FFALIGN(dst->width, 32);
        src_linesize[3] = 0;

        src_height[0] = dst->height;
        src_height[1] = dst->height;
        src_height[2] = dst->height;
        src_height[3] = 0;
        break;
    default:
        av_log(NULL, AV_LOG_ERROR, "Unsupported pixel format %s\n",
               av_get_pix_fmt_name(dst->format));
        return AVERROR(EINVAL);
    }
    for (i = 0; i < nb_planes; i++)
    {
        dst_size += FFMIN(src_linesize[i], dst->linesize[i]) * src_height[i];
    }
    if (dst_size > src->data_len)
    {
        av_log(NULL,AV_LOG_ERROR, "ni_to_avframe_copy failed, "
                "please check the input/output frame reslotion "
                "and the module reslotion\n");
        return -1;
    }
    src_line = src->p_data;
    for (i = 0; i < nb_planes; i++) {
        dst_line = dst->data[i];

        for (h = 0; h < src_height[i]; h++) {
            memcpy(dst_line, src_line,
                   FFMIN(src_linesize[i], dst->linesize[i]));
            dst_line += FFMIN(src_linesize[i], dst->linesize[i]);
            src_line += src_linesize[i];
        }
    }

    return 0;
}

static int ni_ai_pre_filter_frame(AVFilterLink *link, AVFrame *in) {
    AVFilterContext *ctx = link->dst;
    AVHWFramesContext *in_frames_context = NULL; //= (AVHWFramesContext *) in->hw_frames_ctx->data;
    NetIntAiPreprocessContext *s  = ctx->priv;
    AVFrame *out         = NULL;
    ni_retcode_t retval;
    int ret;
    AiContext *ai_ctx;
    ni_ai_pre_network_t *network = &s->network;
    int nb_planes;

    if (in == NULL) {
        av_log(ctx, AV_LOG_WARNING, "in frame is null\n");
        return AVERROR(EINVAL);
    }
    int hwframe = in->format == AV_PIX_FMT_NI_QUAD ? 1 : 0;
    if (!s->initialized) {
        AVHWFramesContext *pAVHFWCtx;
        if (hwframe)
        {
            pAVHFWCtx = (AVHWFramesContext *) in->hw_frames_ctx->data;
        }
        ret = ni_ai_pre_config_input(ctx, in);
        if (ret) {
            av_log(ctx, AV_LOG_ERROR, "failed to config input\n");
            goto failed_out;
        }
        if (hwframe)
        {
            av_hwframe_ctx_init(s->out_frames_ref);
            ff_ni_clone_hwframe_ctx(pAVHFWCtx, (AVHWFramesContext *)s->out_frames_ref->data, &s->ai_ctx->api_ctx);
        }
    }

    ai_ctx = s->ai_ctx;
    out = av_frame_alloc();
    if (!out)
    {
        ret = AVERROR(ENOMEM);
        goto failed_out;
    }

    av_frame_copy_props(out, in);
    out->width = s->out_width;
    out->height = s->out_height;
    if (hwframe)
    {
        in_frames_context = (AVHWFramesContext *) in->hw_frames_ctx->data;
        out->format = AV_PIX_FMT_NI_QUAD;

        out->data[3] = av_malloc(sizeof(niFrameSurface1_t));
        if (!out->data[3])
        {
            av_log(ctx, AV_LOG_ERROR, "ni ai_pre filter av_alloc returned NULL\n");
            ret = AVERROR(ENOMEM);
            goto failed_out;
        }

        niFrameSurface1_t *frame_surface;
        frame_surface = (niFrameSurface1_t *)in->data[3];
        niFrameSurface1_t *frame_surface2;

        memcpy(out->data[3], frame_surface, sizeof(niFrameSurface1_t));
        av_log(ctx, AV_LOG_DEBUG, "input frame surface frameIdx %d\n",
            frame_surface->ui16FrameIdx);

        int64_t start_t = av_gettime();

        /* set output buffer */
        int ai_out_format = ff_ni_ffmpeg_to_gc620_pix_fmt(in_frames_context->sw_format);

#ifdef NI_MEASURE_LATENCY
        ff_ni_update_benchmark(NULL);
#endif

        niFrameSurface1_t dst_surface = {0};
        do{
            if (s->channel_mode) {
                retval = ni_device_alloc_dst_frame(&(ai_ctx->api_ctx), &dst_surface, NI_DEVICE_TYPE_AI);
            } else {
                retval = ni_device_alloc_frame(
                    &ai_ctx->api_ctx, FFALIGN(s->out_width, 2), FFALIGN(s->out_height,2),
                    ai_out_format, NI_AI_FLAG_IO, 0, 0,
                    0, 0, 0, -1, NI_DEVICE_TYPE_AI);
            }

            if (retval < NI_RETCODE_SUCCESS) {
                av_log(ctx, AV_LOG_ERROR, "failed to alloc hw output frame\n");
                ret = AVERROR(ENOMEM);
                goto failed_out;
            }

            if(av_gettime() - start_t > s->ai_timeout * 1000000){
                av_log(ctx, AV_LOG_ERROR, "alloc hw output timeout\n");
                ret = AVERROR(ENOMEM);
                goto failed_out;
            }
        }while(retval != NI_RETCODE_SUCCESS);

        if (s->channel_mode) {
            // copy input hw frame to dst hw frame
            ni_frameclone_desc_t frame_clone_desc = {0};
            frame_clone_desc.ui16DstIdx = dst_surface.ui16FrameIdx;
            frame_clone_desc.ui16SrcIdx = frame_surface->ui16FrameIdx;
            if (in_frames_context->sw_format == AV_PIX_FMT_YUV420P) {
                // only support yuv420p
                // offset Y size
                frame_clone_desc.ui32Offset = NI_VPU_ALIGN128(s->out_width) * NI_VPU_CEIL(s->out_height, 2);
                // copy U+V size
                frame_clone_desc.ui32Size = NI_VPU_ALIGN128(s->out_width / 2) * NI_VPU_CEIL(s->out_height, 2);
                retval = ni_device_clone_hwframe(&ai_ctx->api_ctx, &frame_clone_desc);
                if (retval != NI_RETCODE_SUCCESS) {
                    av_log(ctx, AV_LOG_ERROR, "failed to clone hw input frame\n");
                    ret = AVERROR(ENOMEM);
                    goto failed_out;
                }
            } else {
                av_log(ctx, AV_LOG_ERROR, "Error: support yuv420p only, current fmt %d\n",
                    in_frames_context->sw_format);
                ret = AVERROR(EINVAL);
                goto failed_out;
            }
        }

        /* set input buffer */
        retval = ni_device_alloc_frame(&ai_ctx->api_ctx, 0, 0, 0, 0, 0, 0, 0, 0,
                                    frame_surface->ui32nodeAddress,
                                    frame_surface->ui16FrameIdx,
                                    NI_DEVICE_TYPE_AI);
        if (retval != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "failed to alloc hw input frame\n");
            ret = AVERROR(ENOMEM);
            goto failed_out;
        }

        /* Set the new frame index */
        start_t = av_gettime();
        do{
            retval = ni_device_session_read_hwdesc(
                &ai_ctx->api_ctx, &s->ai_ctx->api_dst_frame, NI_DEVICE_TYPE_AI);

            if(retval < NI_RETCODE_SUCCESS){
                av_log(ctx, AV_LOG_ERROR, "failed to read hwdesc,ret=%d\n", ret);
                ret = AVERROR(EINVAL);
                goto failed_out;
            }
            if(av_gettime() - start_t > s->ai_timeout * 1000000){
                av_log(ctx, AV_LOG_ERROR, "alloc hw output timeout\n");
                ret = AVERROR(ENOMEM);
                goto failed_out;
            }
        } while(retval != NI_RETCODE_SUCCESS);

#ifdef NI_MEASURE_LATENCY
        ff_ni_update_benchmark("ni_quadra_ai_pre");
#endif

        frame_surface2 = (niFrameSurface1_t *) ai_ctx->api_dst_frame.data.frame.p_data[3];
        frame_surface = (niFrameSurface1_t *) out->data[3];

        av_log(ctx, AV_LOG_DEBUG,"ai pre process, idx=%d\n", frame_surface2->ui16FrameIdx);

        frame_surface->ui16FrameIdx = frame_surface2->ui16FrameIdx;
        frame_surface->ui16session_ID = frame_surface2->ui16session_ID;
        frame_surface->device_handle = frame_surface2->device_handle;
        frame_surface->output_idx = frame_surface2->output_idx;
        frame_surface->src_cpu = frame_surface2->src_cpu;
        frame_surface->ui32nodeAddress = 0;
        frame_surface->dma_buf_fd = 0;
        ff_ni_set_bit_depth_and_encoding_type(&frame_surface->bit_depth,
                                            &frame_surface->encoding_type,
                                            in_frames_context->sw_format);
        frame_surface->ui16width = out->width;
        frame_surface->ui16height = out->height;

        out->buf[0] = av_buffer_create(out->data[3],
                                    sizeof(niFrameSurface1_t),
                                    ff_ni_frame_free,
                                    NULL,
                                    0);
        if (!out->buf[0])
        {
            av_log(ctx, AV_LOG_ERROR, "ni ai_pre filter av_buffer_create returned NULL\n");
            ret = AVERROR(ENOMEM);
            av_log(NULL, AV_LOG_DEBUG, "Recycle trace ui16FrameIdx = [%d] DevHandle %d\n",
                    frame_surface->ui16FrameIdx, frame_surface->device_handle);
            retval = ni_hwframe_buffer_recycle(frame_surface, frame_surface->device_handle);
            if (retval != NI_RETCODE_SUCCESS)
            {
                av_log(NULL, AV_LOG_ERROR, "ERROR Failed to recycle trace ui16FrameIdx = [%d] DevHandle %d\n",
                        frame_surface->ui16FrameIdx, frame_surface->device_handle);
            }
            goto failed_out;
        }

        /* Reference the new hw frames context */
        out->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);
    }
    else
    {
        out->format = in->format;
        if (av_frame_get_buffer(out, 32) < 0)
        {
            av_log(ctx, AV_LOG_ERROR, "Could not allocate the AVFrame buffers\n");
            ret = AVERROR(ENOMEM);
            goto failed_out;
        }

        int64_t start_t = av_gettime();
        retval = ni_ai_frame_buffer_alloc(&ai_ctx->api_src_frame.data.frame,
                                          &network->raw);
        if (retval != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "cannot allocate ai frame\n");
            ret = AVERROR(ENOMEM);
            goto failed_out;
        }
        nb_planes = av_pix_fmt_count_planes(in->format);
        if (s->channel_mode)
        {
            if (in->format != AV_PIX_FMT_YUV420P && in->format != AV_PIX_FMT_YUVJ420P)
            {
                av_log(ctx, AV_LOG_ERROR, "Error: support yuv420p and yuvj420p only, current fmt %d\n",
                        in->format);
                ret = AVERROR(EINVAL);
                goto failed_out;
            }
            nb_planes = 1; // only copy Y data
        }
        retval = av_to_niframe_copy(&ai_ctx->api_src_frame.data.frame, in, nb_planes);
        if (retval < 0)
        {
            av_log(ctx, AV_LOG_ERROR, "ai_pre cannot copy frame\n");
            ret = AVERROR(EIO);
            goto failed_out;
        }

#ifdef NI_MEASURE_LATENCY
        ff_ni_update_benchmark(NULL);
#endif

        /* write frame */
        do {
            retval = ni_device_session_write(
                &ai_ctx->api_ctx, &ai_ctx->api_src_frame, NI_DEVICE_TYPE_AI);
            if (retval < 0) {
                av_log(ctx, AV_LOG_ERROR,
                       "failed to write ai session: retval %d\n", retval);
                ret = AVERROR(EIO);
                goto failed_out;
            }

            if(av_gettime() - start_t > s->ai_timeout * 1000000){
                av_log(ctx, AV_LOG_ERROR, "write sw frame to AI timeout\n");
                ret = AVERROR(ENOMEM);
                goto failed_out;
            }
        } while (retval == 0);
        retval = ni_ai_packet_buffer_alloc(&ai_ctx->api_dst_frame.data.packet,
                                        &network->raw);
        if (retval != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "failed to allocate ni packet\n");
            ret = AVERROR(ENOMEM);
            goto failed_out;
        }

        start_t = av_gettime();
        do {
            retval = ni_device_session_read(&ai_ctx->api_ctx, &ai_ctx->api_dst_frame, NI_DEVICE_TYPE_AI);
            if (retval < 0) {
                av_log(NULL,AV_LOG_ERROR,"read AI data retval %d\n", retval);
                ret = AVERROR(EIO);
                goto failed_out;
            } else if (retval > 0) {
                if(av_gettime() - start_t > s->ai_timeout * 1000000){
                    av_log(ctx, AV_LOG_ERROR, "read sw frame from AI timeout\n");
                    ret = AVERROR(ENOMEM);
                    goto failed_out;
                }
            }
        } while (retval == 0);
#ifdef NI_MEASURE_LATENCY
        ff_ni_update_benchmark("ni_quadra_ai_pre");
#endif
        nb_planes = av_pix_fmt_count_planes(out->format);
        if (s->channel_mode)
        {
            if (out->format != AV_PIX_FMT_YUV420P && out->format != AV_PIX_FMT_YUVJ420P)
            {
                av_log(ctx, AV_LOG_ERROR, "Error: support yuv420p and yuvj420p only, current fmt %d\n",
                        out->format);
                ret = AVERROR(EINVAL);
                goto failed_out;
            }
            nb_planes = 1; // only copy Y data
        }
        retval = ni_to_avframe_copy(out, &ai_ctx->api_dst_frame.data.packet, nb_planes);
        if (retval < 0)
        {
            av_log(ctx, AV_LOG_ERROR, "ai_pre cannot copy ai frame to avframe\n");
            ret = AVERROR(EIO);
            goto failed_out;
        }
        if (s->channel_mode)
        {
            // copy U/V data from the input sw frame
            memcpy(out->data[1], in->data[1], in->height * in->linesize[1] / 2);
            memcpy(out->data[2], in->data[2], in->height * in->linesize[2] / 2);
        }
    }

    av_frame_free(&in);
    return ff_filter_frame(link->dst->outputs[0], out);

failed_out:
    if (out)
        av_frame_free(&out);

    av_frame_free(&in);
    return ret;
}

#if IS_FFMPEG_61_AND_ABOVE
static int activate(AVFilterContext *ctx)
{
    AVFilterLink  *inlink = ctx->inputs[0];
    AVFilterLink  *outlink = ctx->outputs[0];
    AVFrame *frame = NULL;
    int ret = 0;
    NetIntAiPreprocessContext *s = inlink->dst->priv;

    // Forward the status on output link to input link, if the status is set, discard all queued frames
    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    if (ff_inlink_check_available_frame(inlink))
    {
        ret = ff_inlink_consume_frame(inlink, &frame);
        if (ret < 0)
            return ret;

        return ni_ai_pre_filter_frame(inlink, frame);
    }

    // We did not get a frame from input link, check its status
    FF_FILTER_FORWARD_STATUS(inlink, outlink);

    // We have no frames yet from input link and no EOF, so request some.
    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}
#endif

#define OFFSET(x) offsetof(NetIntAiPreprocessContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption ni_ai_pre_options[] = {{"nb", "path to network binary file",
                                          OFFSET(nb_file), AV_OPT_TYPE_STRING,
                                          .flags = FLAGS},
                                         {"devid",
                                          "device to operate in swframe mode",
                                          OFFSET(devid),
                                          AV_OPT_TYPE_INT,
                                          {.i64 = 0},
                                          -1,
                                          INT_MAX,
                                          .flags = FLAGS,
                                          "range"},

                                         {"keep_alive_timeout",
                                          "Specify a custom session keep alive timeout in seconds.",
                                          OFFSET(keep_alive_timeout),
                                          AV_OPT_TYPE_INT,
                                          {.i64 = NI_AI_PRE_KEEPALIVE_TIMEOUT},
                                          NI_MIN_KEEP_ALIVE_TIMEOUT,
                                          NI_MAX_KEEP_ALIVE_TIMEOUT,
                                          FLAGS,
                                          "keep_alive_timeout"},

                                         {"mode",
                                          "Specify the processing channel of the network, 0: YUV channels, 1: Y channel only",
                                          OFFSET(channel_mode),
                                          AV_OPT_TYPE_BOOL,
                                          {.i64 = 0},
                                          0,
                                          1},

                                         {"buffer_limit",
                                          "Whether to limit output buffering count, 0: no, 1: yes",
                                          OFFSET(buffer_limit),
                                          AV_OPT_TYPE_BOOL,
                                          {.i64 = 0},
                                          0,
                                          1},

                                             {"timeout",
                                               "Specify a custom timeout in seconds.",
                                               OFFSET(ai_timeout),
                                               AV_OPT_TYPE_INT,
                                               {.i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT},
                                               NI_MIN_KEEP_ALIVE_TIMEOUT,
                                               NI_MAX_KEEP_ALIVE_TIMEOUT,
                                               FLAGS,
                                               "keep_alive_timeout"},
                                             {"width",
                                               "Specify the output frame width.",
                                               OFFSET(out_width),
                                               AV_OPT_TYPE_INT,
                                               {.i64 = -1},
                                               -1,
                                               8192,
                                               FLAGS,
                                               "width"},

                                              {"height",
                                               "Specify the output frame height.",
                                               OFFSET(out_height),
                                               AV_OPT_TYPE_INT,
                                               {.i64 = -1},
                                               -1,
                                               8192,
                                               FLAGS,
                                               "height"},
                                         {NULL}};

static const AVClass ni_ai_pre_class = {
   .class_name = "ni_ai_pre",
   .item_name  = av_default_item_name,
   .option     = ni_ai_pre_options,
   .version    = LIBAVUTIL_VERSION_INT,
   .category   = AV_CLASS_CATEGORY_FILTER,
   //    .child_class_next = child_class_next,
};

static const AVFilterPad avfilter_vf_ai_pre_inputs[] = {
   {
       .name         = "default",
       .type         = AVMEDIA_TYPE_VIDEO,
       .filter_frame = ni_ai_pre_filter_frame,
   },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
   {NULL}
#endif
};

static const AVFilterPad avfilter_vf_ai_pre_outputs[] = {
   {
       .name         = "default",
       .type         = AVMEDIA_TYPE_VIDEO,
       .config_props = ni_ai_pre_output_config_props,
   },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
   {NULL}
#endif
};

AVFilter ff_vf_ai_pre_ni_quadra = {
   .name           = "ni_quadra_ai_pre",
   .description    = NULL_IF_CONFIG_SMALL("NETINT Quadra video ai preprocess v" NI_XCODER_REVISION),
   .init           = ni_ai_pre_init,
   .uninit         = ni_ai_pre_uninit,
#if IS_FFMPEG_61_AND_ABOVE
   .activate      = activate,
#endif
   .priv_size      = sizeof(NetIntAiPreprocessContext),
   .priv_class     = &ni_ai_pre_class,
   .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
   FILTER_INPUTS(avfilter_vf_ai_pre_inputs),
   FILTER_OUTPUTS(avfilter_vf_ai_pre_outputs),
   FILTER_QUERY_FUNC(ni_ai_pre_query_formats),
#else
   .inputs         = avfilter_vf_ai_pre_inputs,
   .outputs        = avfilter_vf_ai_pre_outputs,
   .query_formats  = ni_ai_pre_query_formats,
#endif
};
