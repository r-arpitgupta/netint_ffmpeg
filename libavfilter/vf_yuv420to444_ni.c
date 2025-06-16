/*
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
 * yuv420 to yuv444
 */

#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/timestamp.h"
#include "avfilter.h"
#include "version.h"
#if !(LIBAVFILTER_VERSION_MAJOR > 10 || (LIBAVFILTER_VERSION_MAJOR == 10 && LIBAVFILTER_VERSION_MINOR >= 4))
#include "internal.h"
#endif
#include "drawutils.h"
#include "formats.h"
#include "framesync.h"
#include "video.h"


#define IS_FFMPEG_342_AND_ABOVE                                                \
    ((LIBAVFILTER_VERSION_MAJOR > 6) ||                                        \
     (LIBAVFILTER_VERSION_MAJOR == 6 && LIBAVFILTER_VERSION_MINOR >= 107))

typedef struct NetIntYUV420to444Context {
    const AVClass *class;
    FFFrameSync fs;
    int mode;
#if !IS_FFMPEG_342_AND_ABOVE
    int opt_repeatlast;
    int opt_shortest;
    int opt_eof_action;
#endif
} NetIntYUV420to444Context;

static int do_blend(FFFrameSync *fs);

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    int ret;
    enum AVPixelFormat input_pix_fmt = AV_PIX_FMT_YUV420P;
    enum AVPixelFormat output_pix_fmt = AV_PIX_FMT_YUV444P;

    if (ctx->inputs[0]) {
        formats = NULL;
        if ((ret = ff_add_format(&formats, input_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->inputs[0]->outcfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->inputs[0]->out_formats)) < 0)
#endif
            return ret;
    }
    if (ctx->inputs[1]) {
        formats = NULL;
        if ((ret = ff_add_format(&formats, input_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->inputs[1]->outcfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->inputs[1]->out_formats)) < 0)
#endif
            return ret;
    }
    if (ctx->outputs[0]) {
        formats = NULL;

        if ((ret = ff_add_format(&formats, output_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->outputs[0]->incfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->outputs[0]->in_formats)) < 0)
#endif
            return ret;
    }

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    NetIntYUV420to444Context *s = ctx->priv;

    s->fs.on_event = do_blend;
    s->fs.opaque = s;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    NetIntYUV420to444Context *s = ctx->priv;

    ff_framesync_uninit(&s->fs);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    NetIntYUV420to444Context *s = ctx->priv;
    int i, ret;

    ret = ff_framesync_init(&s->fs, ctx, ctx->nb_inputs);
    if (ret < 0)
        return ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        FFFrameSyncIn *in = &s->fs.in[i];
        in->before    = EXT_STOP;
        in->after     = EXT_INFINITY;
        in->sync      = i ? 1 : 2;
        in->time_base = ctx->inputs[i]->time_base;
    }

    outlink->w = ctx->inputs[0]->w;
    outlink->h = ctx->inputs[0]->h;
    outlink->format = AV_PIX_FMT_YUV444P;
    outlink->time_base = ctx->inputs[0]->time_base;
    av_log(ctx, AV_LOG_INFO, "output w:%d h:%d fmt:%s\n",
           outlink->w, outlink->h, av_get_pix_fmt_name(outlink->format));

#if !IS_FFMPEG_342_AND_ABOVE
    if (!s->opt_repeatlast || s->opt_eof_action == EOF_ACTION_PASS) {
        s->opt_repeatlast = 0;
        s->opt_eof_action = EOF_ACTION_PASS;
    }
    if (s->opt_shortest || s->opt_eof_action == EOF_ACTION_ENDALL) {
        s->opt_shortest = 1;
        s->opt_eof_action = EOF_ACTION_ENDALL;
    }
    if (!s->opt_repeatlast) {
        for (i = 1; i < s->fs.nb_in; i++) {
            s->fs.in[i].after = EXT_NULL;
            s->fs.in[i].sync  = 0;
        }
    }
    if (s->opt_shortest) {
        for (i = 0; i < s->fs.nb_in; i++)
            s->fs.in[i].after = EXT_STOP;
    }
#endif

    return ff_framesync_configure(&s->fs);
}

static int do_blend(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    NetIntYUV420to444Context *s = ctx->priv;
    AVFrame *mainpic, *second, *out;
    int uv_420_linesize, uv_444_linesize;
    int i, j;

    ff_framesync_get_frame(fs, 0, &mainpic, 0);
    ff_framesync_get_frame(fs, 1, &second, 0);

    mainpic->pts =
        av_rescale_q(fs->pts, fs->time_base, ctx->outputs[0]->time_base);
    {
        //allocate a new buffer, data is null
        out = ff_get_video_buffer(ctx->outputs[0], ctx->outputs[0]->w, ctx->outputs[0]->h);
        if (!out) {
            return AVERROR(ENOMEM);
        }

        av_frame_copy_props(out, mainpic);
        out->format = ctx->outputs[0]->format;

        uv_420_linesize = mainpic->linesize[1];
        uv_444_linesize = out->linesize[1];

        //y component
        for (i = 0; i < out->height; i++) {
            memcpy(out->data[0] + i * out->linesize[0],
                   mainpic->data[0] + i * mainpic->linesize[0],
                   out->linesize[0]);
        }

        if (s->mode == 0) {
            //u component
            for (i = 0; i < out->height; i++) {
                memcpy(out->data[1] + i * out->linesize[0],
                       second->data[0] + i * second->linesize[0],
                       out->linesize[0]);
            }

            //v component
            for (i = 0; i < out->height / 2; i++) {
                for (j = 0; j < out->width / 2; j++) {
                    memcpy(out->data[2] + (2 * i * uv_444_linesize) + 2 * j,
                           mainpic->data[1] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + 2 * (i * uv_444_linesize) +
                           (2 * j + 1),
                           mainpic->data[2] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + ((2 * i + 1) * uv_444_linesize) +
                           2 * j,
                           second->data[1] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + ((2 * i + 1) * uv_444_linesize) +
                           (2 * j + 1),
                           second->data[2] + i * uv_420_linesize + j,
                           sizeof(char));
                }
            }
        } else if (s->mode == 1) {
            // uv component
            for (i = 0; i < out->height / 2; i++) {
                for (j = 0; j < out->width / 2; j++) {
                    memcpy(out->data[1] + (2 * i * uv_444_linesize) + 2 * j,
                           mainpic->data[1] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[1] + (2 * i * uv_444_linesize) +
                           (2 * j + 1),
                           second->data[1] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[1] + ((2 * i + 1) * uv_444_linesize) +
                           2 * j,
                           second->data[0] + 2 * i * uv_444_linesize +
                           2 * j,
                           sizeof(char) * 2);

                    memcpy(out->data[2] + (2 * i * uv_444_linesize) + 2 * j,
                           mainpic->data[2] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + 2 * (i * uv_444_linesize) +
                           (2 * j + 1),
                           second->data[2] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + ((2 * i + 1) * uv_444_linesize) +
                           2 * j,
                           second->data[0] + (2 * i + 1) * uv_444_linesize +
                           2 * j,
                           sizeof(char) * 2);
                }
            }
        }
    }

    return ff_filter_frame(ctx->outputs[0], out);
}

#if !IS_FFMPEG_342_AND_ABOVE
static int filter_frame(AVFilterLink *inlink, AVFrame *inpicref)
{
    NetIntYUV420to444Context *s = inlink->dst->priv;
    av_log(inlink->dst, AV_LOG_DEBUG, "Incoming frame (time:%s) from link #%d\n", av_ts2timestr(inpicref->pts, &inlink->time_base), FF_INLINK_IDX(inlink));
    return ff_framesync_filter_frame(&s->fs, inlink, inpicref);
}

static int request_frame(AVFilterLink *outlink)
{
    NetIntYUV420to444Context *s = outlink->src->priv;
    return ff_framesync_request_frame(&s->fs, outlink);
}
#else
static int activate(AVFilterContext *ctx)
{
    NetIntYUV420to444Context *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}
#endif

#define OFFSET(x) offsetof(NetIntYUV420to444Context, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption ni_420to444_options[] = {
    { "mode", "mode used by input yuv444to420 filter", OFFSET(mode), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, FLAGS, "mode" },
        { "better_psnr",       "better PSNR after encoding and recombination", 0, AV_OPT_TYPE_CONST, {.i64 = 0}, 0, 0, FLAGS, "mode" },
        { "visually_coherent", "output0 will be visually coherent as yuv420",  0, AV_OPT_TYPE_CONST, {.i64 = 1}, 0, 0, FLAGS, "mode" },
    { NULL }
};

#if IS_FFMPEG_342_AND_ABOVE
// NOLINTNEXTLINE(clang-diagnostic-deprecated-declarations)
FRAMESYNC_DEFINE_CLASS(ni_420to444, NetIntYUV420to444Context, fs);
#else
AVFILTER_DEFINE_CLASS(ni_420to444);
#endif

static const AVFilterPad inputs[] = {
    {
        .name          = "input0",
        .type          = AVMEDIA_TYPE_VIDEO,
#if !IS_FFMPEG_342_AND_ABOVE
        .filter_frame  = filter_frame,
#endif
    },
    {
        .name          = "input1",
        .type          = AVMEDIA_TYPE_VIDEO,
#if !IS_FFMPEG_342_AND_ABOVE
        .filter_frame  = filter_frame,
#endif
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

static const AVFilterPad outputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .config_props   = config_output,
#if !IS_FFMPEG_342_AND_ABOVE
        .request_frame  = request_frame,
#endif
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_yuv420to444_ni_quadra = {
    .name          = "ni_quadra_yuv420to444",
    .description   = NULL_IF_CONFIG_SMALL("NETINT Quadra YUV420 to YUV444."),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(NetIntYUV420to444Context),
    .priv_class    = &ni_420to444_class,
#if IS_FFMPEG_342_AND_ABOVE
    .preinit       = ni_420to444_framesync_preinit,
    .activate      = activate,
#endif
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC(query_formats),
#else
    .inputs        = inputs,
    .outputs       = outputs,
    .query_formats = query_formats,
#endif
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL |
                     AVFILTER_FLAG_SLICE_THREADS,
};
