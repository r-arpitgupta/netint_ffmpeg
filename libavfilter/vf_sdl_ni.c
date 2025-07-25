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

#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libswscale/swscale.h"
#include "version.h"
#include "avfilter.h"
#include "formats.h"
#if !(LIBAVFILTER_VERSION_MAJOR > 10 || (LIBAVFILTER_VERSION_MAJOR == 10 && LIBAVFILTER_VERSION_MINOR >= 4))
#include "internal.h"
#else
#include "filters.h"
#endif
#include <SDL.h>

typedef struct NetIntSdlContext {
    const AVClass *class;
    int quit;
    int width;
    int height;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Window *window;
} NetIntSdlContext;

static int query_formats(AVFilterContext *avctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAYF32,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);

    if (!fmts_list) {
        av_log(avctx, AV_LOG_ERROR, "could not create formats list\n");
        return AVERROR(ENOMEM);
    }

    return ff_set_common_formats(avctx, fmts_list);
}

static av_cold int init(AVFilterContext *avctx)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init SDL %s!", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *avctx)
{
    NetIntSdlContext *ctx = avctx->priv;

    if (!ctx->quit) {
        SDL_DestroyTexture(ctx->texture);
        SDL_DestroyRenderer(ctx->renderer);
        SDL_DestroyWindow(ctx->window);
        SDL_Quit();
    }
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    NetIntSdlContext     *ctx = avctx->priv;

    ctx->window = SDL_CreateWindow("FFmpeg SDL Filter", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                   inlink->w, inlink->h, SDL_WINDOW_RESIZABLE);
    if (!ctx->window) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create SDL window %s!", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    ctx->renderer = SDL_CreateRenderer(ctx->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ctx->renderer) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create SDL renderer %s!", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_IYUV,
                                     SDL_TEXTUREACCESS_STREAMING, inlink->w , inlink->h);
    if (!ctx->renderer) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create SDL texture %s!", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink  *outlink = avctx->outputs[0];
    NetIntSdlContext     *ctx = avctx->priv;
    SDL_Event event;

    if (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN:
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_q:
                    ctx->quit = 1;
                    break;
                default:
                    break;
                }
                break;
            case SDL_QUIT:
                ctx->quit = 1;
                break;
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_RESIZED:
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        ctx->width = event.window.data1;
                        ctx->height = event.window.data2;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }

    if (ctx->quit) {
        SDL_DestroyTexture(ctx->texture);
        SDL_DestroyRenderer(ctx->renderer);
        SDL_DestroyWindow(ctx->window);
        SDL_Quit();
    } else {
        SDL_UpdateYUVTexture(ctx->texture, NULL,
                             frame->data[0], frame->linesize[0],
                             frame->data[1], frame->linesize[1],
                             frame->data[2], frame->linesize[2]);
        SDL_RenderClear(ctx->renderer);
        SDL_RenderCopyEx(ctx->renderer, ctx->texture, NULL, NULL, 0, NULL, 0);
        SDL_RenderPresent(ctx->renderer);
    }

    return ff_filter_frame(outlink, frame);
}

#define OFFSET(x) offsetof(NetIntSdlContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
static const AVOption ni_sdl_options[] = {
    { NULL }
};

AVFILTER_DEFINE_CLASS(ni_sdl);

static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

static const AVFilterPad outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_sdl_ni_quadra = {
    .name          = "ni_quadra_sdl",
    .description   = NULL_IF_CONFIG_SMALL("Use SDL2.0 to display AVFrame."),
    .init          = init,
    .uninit        = uninit,

    .priv_size     = sizeof(NetIntSdlContext),
    .priv_class    = &ni_sdl_class,
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC(query_formats),
#else
    .inputs        = inputs,
    .outputs       = outputs,
    .query_formats = query_formats,
#endif
};
