#include <dlfcn.h>

#include "config.h"
#include "buffer.h"
#include "common.h"
#include "hwcontext.h"
#include "hwcontext_internal.h"
//#include "hwcontext_omx.h"

typedef struct OMXDeviceContext {
    //AVOMXDeviceContext ctx;

    void *libmedia;
    //media_status_t (*create_surface)(ANativeWindow **surface);
} OMXDeviceContext;


static int omx_device_create(AVHWDeviceContext *ctx, const char *device,
                             AVDictionary *opts, int flags)
{
    //const AVDictionaryEntry *entry = NULL;
    //OMXDeviceContext *s = ctx->hwctx;
    //AVOMXDeviceContext *dev = &s->ctx;

    if (device && device[0]) {
        av_log(ctx, AV_LOG_ERROR, "Device selection unsupported.\n");
        return AVERROR_UNKNOWN;
    }


    return 0;
}

static int omx_device_init(AVHWDeviceContext *ctx)
{
    OMXDeviceContext *s = ctx->hwctx;
    //AVOMXDeviceContext *dev = (AVOMXDeviceContext *)s;

    s->libmedia = dlopen("libomxil-bellagio.so", RTLD_NOW);
    if (!s->libmedia)
        return AVERROR_UNKNOWN;

    return 0;
}

static void omx_device_uninit(AVHWDeviceContext *ctx)
{
    OMXDeviceContext *s = ctx->hwctx;
    //AVOMXDeviceContext *dev = ctx->hwctx;
    if (!s->libmedia)
        return;


    dlclose(s->libmedia);
    s->libmedia = NULL;
}

const HWContextType ff_hwcontext_type_OMX = {
    .type                 = AV_HWDEVICE_TYPE_OMX,
    .name                 = "openmax",

    .device_hwctx_size    = sizeof(OMXDeviceContext),

    .device_create        = omx_device_create,
    .device_init          = omx_device_init,
    .device_uninit        = omx_device_uninit,

    .pix_fmts = (const enum AVPixelFormat[])
    {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NONE
    },
};


