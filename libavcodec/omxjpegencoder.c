/* OMX Video encoder
* Copyright (C) 2011 Martin Storsjo
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

#include "config.h"

#if CONFIG_OMX_RPI
#define OMX_SKIP64BIT
#endif

#include <dlfcn.h>
#include <OMX_Core.h>
#include <OMX_Component.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "avcodec.h"
#include "h264.h"
#include "internal.h"
#include "codec_internal.h"
#include "encode.h"

#include <omxil/OMX_CsiExt.h>
// #include "vsi_vendor_ext.h"
extern int omx_load_count;
#define TIMEOUT_MS 1000
#ifdef OMX_SKIP64BIT
static OMX_TICKS to_omx_ticks(int64_t value)
{
    OMX_TICKS s;
    s.nLowPart  = value & 0xffffffff;
    s.nHighPart = value >> 32;
    return s;
}
static int64_t from_omx_ticks(OMX_TICKS value)
{
    return (((int64_t)value.nHighPart) << 32) | value.nLowPart;
}
#else
#define to_omx_ticks(x) (x)
#define from_omx_ticks(x) (x)
#endif

#define INIT_STRUCT(x) do {                                               \
        x.nSize = sizeof(x);                                              \
        x.nVersion = s->version;                                          \
    } while (0)
#define CHECK(x) do {                                                     \
        if (x != OMX_ErrorNone) {                                         \
            av_log(avctx, AV_LOG_ERROR,                                   \
                   "err %x (%d) on line %d\n", x, x, __LINE__);           \
            return AVERROR_UNKNOWN;                                       \
        }                                                                 \
    } while (0)

typedef struct OMXContext {
    void *lib;
    void *lib2;
    OMX_ERRORTYPE(*ptr_Init)(void);
    OMX_ERRORTYPE(*ptr_Deinit)(void);
    OMX_ERRORTYPE(*ptr_ComponentNameEnum)(OMX_STRING, OMX_U32, OMX_U32);
    OMX_ERRORTYPE(*ptr_GetHandle)(OMX_HANDLETYPE *, OMX_STRING, OMX_PTR, OMX_CALLBACKTYPE *);
    OMX_ERRORTYPE(*ptr_FreeHandle)(OMX_HANDLETYPE);
    OMX_ERRORTYPE(*ptr_GetComponentsOfRole)(OMX_STRING, OMX_U32 *, OMX_U8 **);
    OMX_ERRORTYPE(*ptr_GetRolesOfComponent)(OMX_STRING, OMX_U32 *, OMX_U8 **);
    void (*host_init)(void);
} OMXContext;

static av_cold void *dlsym_prefixed(void *handle, const char *symbol, const char *prefix)
{
    char buf[50];
    snprintf(buf, sizeof(buf), "%s%s", prefix ? prefix : "", symbol);
    return dlsym(handle, buf);
}

static av_cold int omx_try_load(OMXContext *s, void *logctx,
                                const char *libname, const char *prefix,
                                const char *libname2)
{
    if (libname2) {
        s->lib2 = dlopen(libname2, RTLD_NOW | RTLD_GLOBAL);
        if (!s->lib2) {
            av_log(logctx, AV_LOG_WARNING, "%s not found\n", libname2);
            return AVERROR_ENCODER_NOT_FOUND;
        }
        s->host_init = dlsym(s->lib2, "bcm_host_init");
        if (!s->host_init) {
            av_log(logctx, AV_LOG_WARNING, "bcm_host_init not found\n");
            dlclose(s->lib2);
            s->lib2 = NULL;
            return AVERROR_ENCODER_NOT_FOUND;
        }
    }
    s->lib = dlopen(libname, RTLD_NOW | RTLD_GLOBAL);
    if (!s->lib) {
        av_log(logctx, AV_LOG_WARNING, "%s not found\n", libname);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    s->ptr_Init                = dlsym_prefixed(s->lib, "OMX_Init", prefix);
    s->ptr_Deinit              = dlsym_prefixed(s->lib, "OMX_Deinit", prefix);
    s->ptr_ComponentNameEnum   = dlsym_prefixed(s->lib, "OMX_ComponentNameEnum", prefix);
    s->ptr_GetHandle           = dlsym_prefixed(s->lib, "OMX_GetHandle", prefix);
    s->ptr_FreeHandle          = dlsym_prefixed(s->lib, "OMX_FreeHandle", prefix);
    s->ptr_GetComponentsOfRole = dlsym_prefixed(s->lib, "OMX_GetComponentsOfRole", prefix);
    s->ptr_GetRolesOfComponent = dlsym_prefixed(s->lib, "OMX_GetRolesOfComponent", prefix);
    if (!s->ptr_Init || !s->ptr_Deinit || !s->ptr_ComponentNameEnum ||
        !s->ptr_GetHandle || !s->ptr_FreeHandle ||
        !s->ptr_GetComponentsOfRole || !s->ptr_GetRolesOfComponent) {
        av_log(logctx, AV_LOG_WARNING, "Not all functions found in %s\n", libname);
        dlclose(s->lib);
        s->lib = NULL;
        if (s->lib2)
            dlclose(s->lib2);
        s->lib2 = NULL;
        return AVERROR_ENCODER_NOT_FOUND;
    }
    return 0;
}

static av_cold OMXContext *omx_init(void *logctx, const char *libname, const char *prefix)
{
    static const char *const libnames[] = {
#if CONFIG_OMX_RPI
        "/opt/vc/lib/libopenmaxil.so", "/opt/vc/lib/libbcm_host.so",
#else
        "libomxil-bellagio.so.0", NULL,
        "libOMX_Core.so", NULL,
        "libOmxCore.so", NULL,
#endif
        NULL
    };
    const char *const *nameptr;
    int ret = AVERROR_ENCODER_NOT_FOUND;
    OMXContext *omx_context;
    OMX_ERRORTYPE error;
    omx_context = av_mallocz(sizeof(*omx_context));
    if (!omx_context)
        return NULL;
    if (libname) {
        ret = omx_try_load(omx_context, logctx, libname, prefix, NULL);
        if (ret < 0) {
            av_free(omx_context);
            return NULL;
        }
    } else {
        for (nameptr = libnames; *nameptr; nameptr += 2)
            if (!(ret = omx_try_load(omx_context, logctx, nameptr[0], prefix, nameptr[1])))
                break;
        if (!*nameptr) {
            av_free(omx_context);
            return NULL;
        }
    }

    if (omx_context->host_init)
        omx_context->host_init();
    av_log(NULL, AV_LOG_INFO, "OMX_count load %d\n", omx_load_count);
    if (omx_load_count == 0) {
        error = omx_context->ptr_Init();
        if (error != OMX_ErrorNone) {
            av_log(NULL, AV_LOG_WARNING, "OMX Init error\n");
        }
    }
    omx_load_count++;
    return omx_context;
}

static av_cold void omx_deinit(OMXContext *omx_context)
{
    if (!omx_context)
        return;
    omx_load_count--;
    av_log(NULL, AV_LOG_INFO, "OMX_count %d\n", omx_load_count);
    if (omx_load_count == 0) {
        omx_context->ptr_Deinit();
        dlclose(omx_context->lib);
    }
    av_free(omx_context);
}

typedef struct OMXCodecContext {
    const AVClass *class;
    char *libname;
    char *libprefix;
    OMXContext *omx_context;

    AVCodecContext *avctx;

    char component_name[OMX_MAX_STRINGNAME_SIZE];
    OMX_VERSIONTYPE version;
    OMX_HANDLETYPE handle;
    int in_port, out_port;
    OMX_COLOR_FORMATTYPE color_format;
    int stride, plane_size;

    int num_in_buffers, num_out_buffers;
    OMX_BUFFERHEADERTYPE **in_buffer_headers;
    OMX_BUFFERHEADERTYPE **out_buffer_headers;
    int num_free_in_buffers;
    OMX_BUFFERHEADERTYPE **free_in_buffers;
    int num_done_out_buffers;
    OMX_BUFFERHEADERTYPE **done_out_buffers;

    pthread_mutex_t input_mutex;
    pthread_cond_t input_cond;
    pthread_mutex_t output_mutex;
    pthread_cond_t output_cond;

    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    OMX_STATETYPE state;
    OMX_ERRORTYPE error;
    OMX_EVENTTYPE event;
    OMX_COMMANDTYPE cmd;

    int mutex_cond_inited;

    int eos_sent, got_eos;

    uint8_t *output_buf;
    int output_buf_size;

    int input_zerocopy;
    int QFactor;
    int maxpts;
    int stride_padding;
} OMXCodecContext;

static void say(OMXCodecContext *s, const char *message, ...)
{
    va_list args;
    size_t str_len;
    char str[1024];
    memset(str, 0, sizeof(str));
    va_start(args, message);
    vsnprintf(str, sizeof(str) - 1, message, args);
    va_end(args);
    str_len = strlen(str);
    if (str[str_len - 1] != '\n') {
        str[str_len] = '\n';
    }
    av_log(s->avctx, AV_LOG_INFO, "%s", str);
}

static const char *dump_compression_format(OMXCodecContext *s, OMX_VIDEO_CODINGTYPE c)
{
    char *f;
    switch (c) {
    case OMX_VIDEO_CodingUnused:
        return "not used";
    case OMX_VIDEO_CodingAutoDetect:
        return "autodetect";
    case OMX_VIDEO_CodingH263:
        return "H.263";
    case OMX_VIDEO_CodingMPEG4:
        return "MPEG4";
    case OMX_VIDEO_CodingWMV:
        return "Windows Media Video";
    case OMX_VIDEO_CodingRV:
        return "RealVideo";
    case OMX_VIDEO_CodingAVC:
        return "H.264/AVC";
    case OMX_VIDEO_CodingMJPEG:
        return "Motion JPEG";
    case OMX_IMAGE_CodingJPEG:
        return "JPEG";
//        case OMX_VIDEO_CodingTheora:     return "OGG Theora";

    default:
        f = calloc(32, sizeof(char));
        memset(f, 0, 32 * sizeof(char));
        if (f == NULL) {
            av_log(s->avctx, AV_LOG_ERROR, "Failed to allocate memory");
        }
        sprintf(f, "format type 0x%08x", c);
        return f;//maybe memleak
    }
}

static const char *dump_color_format(OMXCodecContext *s, OMX_COLOR_FORMATTYPE c)
{
    char *f;
    switch (c) {
    case OMX_COLOR_FormatUnused:
        return "OMX_COLOR_FormatUnused: not used";
    case OMX_COLOR_FormatMonochrome:
        return "OMX_COLOR_FormatMonochrome";
    case OMX_COLOR_Format8bitRGB332:
        return "OMX_COLOR_Format8bitRGB332";
    case OMX_COLOR_Format12bitRGB444:
        return "OMX_COLOR_Format12bitRGB444";
    case OMX_COLOR_Format16bitARGB4444:
        return "OMX_COLOR_Format16bitARGB4444";
    case OMX_COLOR_Format16bitARGB1555:
        return "OMX_COLOR_Format16bitARGB1555";
    case OMX_COLOR_Format16bitRGB565:
        return "OMX_COLOR_Format16bitRGB565";
    case OMX_COLOR_Format16bitBGR565:
        return "OMX_COLOR_Format16bitBGR565";
    case OMX_COLOR_Format18bitRGB666:
        return "OMX_COLOR_Format18bitRGB666";
    case OMX_COLOR_Format18bitARGB1665:
        return "OMX_COLOR_Format18bitARGB1665";
    case OMX_COLOR_Format19bitARGB1666:
        return "OMX_COLOR_Format19bitARGB1666";
    case OMX_COLOR_Format24bitRGB888:
        return "OMX_COLOR_Format24bitRGB888";
    case OMX_COLOR_Format24bitBGR888:
        return "OMX_COLOR_Format24bitBGR888";
    case OMX_COLOR_Format24bitARGB1887:
        return "OMX_COLOR_Format24bitARGB1887";
    case OMX_COLOR_Format25bitARGB1888:
        return "OMX_COLOR_Format25bitARGB1888";
    case OMX_COLOR_Format32bitBGRA8888:
        return "OMX_COLOR_Format32bitBGRA8888";
    case OMX_COLOR_Format32bitARGB8888:
        return "OMX_COLOR_Format32bitARGB8888";
    case OMX_COLOR_FormatYUV411Planar:
        return "OMX_COLOR_FormatYUV411Planar";
    case OMX_COLOR_FormatYUV411PackedPlanar:
        return "OMX_COLOR_FormatYUV411PackedPlanar: Planes fragmented when a frame is split in multiple buffers";
    case OMX_COLOR_FormatYUV420Planar:
        return "OMX_COLOR_FormatYUV420Planar: Planar YUV, 4:2:0 (I420)";
    case OMX_COLOR_FormatYUV420PackedPlanar:
        return "OMX_COLOR_FormatYUV420PackedPlanar: Planar YUV, 4:2:0 (I420), planes fragmented when a frame is split in multiple buffers";
    case OMX_COLOR_FormatYUV420SemiPlanar:
        return "OMX_COLOR_FormatYUV420SemiPlanar, Planar YUV, 4:2:0 (NV12), U and V planes interleaved with first U value";
    case OMX_COLOR_FormatYUV422Planar:
        return "OMX_COLOR_FormatYUV422Planar";
    case OMX_COLOR_FormatYUV422PackedPlanar:
        return "OMX_COLOR_FormatYUV422PackedPlanar: Planes fragmented when a frame is split in multiple buffers";
    case OMX_COLOR_FormatYUV422SemiPlanar:
        return "OMX_COLOR_FormatYUV422SemiPlanar";
    case OMX_COLOR_FormatYCbYCr:
        return "OMX_COLOR_FormatYCbYCr";
    case OMX_COLOR_FormatYCrYCb:
        return "OMX_COLOR_FormatYCrYCb";
    case OMX_COLOR_FormatCbYCrY:
        return "OMX_COLOR_FormatCbYCrY";
    case OMX_COLOR_FormatCrYCbY:
        return "OMX_COLOR_FormatCrYCbY";
    case OMX_COLOR_FormatYUV444Interleaved:
        return "OMX_COLOR_FormatYUV444Interleaved";
    case OMX_COLOR_FormatRawBayer8bit:
        return "OMX_COLOR_FormatRawBayer8bit";
    case OMX_COLOR_FormatRawBayer10bit:
        return "OMX_COLOR_FormatRawBayer10bit";
    case OMX_COLOR_FormatRawBayer8bitcompressed:
        return "OMX_COLOR_FormatRawBayer8bitcompressed";
    case OMX_COLOR_FormatL2:
        return "OMX_COLOR_FormatL2";
    case OMX_COLOR_FormatL4:
        return "OMX_COLOR_FormatL4";
    case OMX_COLOR_FormatL8:
        return "OMX_COLOR_FormatL8";
    case OMX_COLOR_FormatL16:
        return "OMX_COLOR_FormatL16";
    case OMX_COLOR_FormatL24:
        return "OMX_COLOR_FormatL24";
    case OMX_COLOR_FormatL32:
        return "OMX_COLOR_FormatL32";
    case OMX_COLOR_FormatYUV420PackedSemiPlanar:
        return "OMX_COLOR_FormatYUV420PackedSemiPlanar: Planar YUV, 4:2:0 (NV12), planes fragmented when a frame is split in multiple buffers, U and V planes interleaved with first U value";
    case OMX_COLOR_FormatYUV422PackedSemiPlanar:
        return "OMX_COLOR_FormatYUV422PackedSemiPlanar: Planes fragmented when a frame is split in multiple buffers";
    case OMX_COLOR_Format18BitBGR666:
        return "OMX_COLOR_Format18BitBGR666";
    case OMX_COLOR_Format24BitARGB6666:
        return "OMX_COLOR_Format24BitARGB6666";
    case OMX_COLOR_Format24BitABGR6666:
        return "OMX_COLOR_Format24BitABGR6666";
    default:
        f = calloc(32, sizeof(char));
        memset(f, 0, 32 * sizeof(char));
        if (f == NULL) {
            av_log(s->avctx, AV_LOG_ERROR, "Failed to allocate memory");
        }
        sprintf(f,  "format type 0x%08x", c);
        return f;
    }
}

static void dump_portdef(OMXCodecContext *s, OMX_PARAM_PORTDEFINITIONTYPE *portdef)
{
    OMX_VIDEO_PORTDEFINITIONTYPE *viddef = &portdef->format.video;
    OMX_IMAGE_PORTDEFINITIONTYPE *imgdef = &portdef->format.image;

    say(s, "Port %d is %s, %s, buffers wants:%d needs:%d, size:%d, pop:%d, aligned:%d",
        portdef->nPortIndex,
        (portdef->eDir ==  OMX_DirInput ? "input" : "output"),
        (portdef->bEnabled == OMX_TRUE ? "enabled" : "disabled"),
        portdef->nBufferCountActual,
        portdef->nBufferCountMin,
        portdef->nBufferSize,
        portdef->bPopulated,
        portdef->nBufferAlignment);


    switch (portdef->eDomain) {
    case OMX_PortDomainVideo:
        say(s, "Video type:\n"
            "\tWidth:\t\t%d\n"
            "\tHeight:\t\t%d\n"
            "\tStride:\t\t%d\n"
            "\tSliceHeight:\t%d\n"
            "\tBitrate:\t%d\n"
            "\tFramerate:\t%.02f\n"
            "\tError hiding:\t%s\n"
            "\tCodec:\t\t%s\n"
            "\tColor:\t\t%s\n",
            viddef->nFrameWidth,
            viddef->nFrameHeight,
            viddef->nStride,
            viddef->nSliceHeight,
            viddef->nBitrate,
            ((float)viddef->xFramerate / (float)65536),
            (viddef->bFlagErrorConcealment == OMX_TRUE ? "yes" : "no"),
            dump_compression_format(s, viddef->eCompressionFormat),
            dump_color_format(s, viddef->eColorFormat));
        break;
    case OMX_PortDomainImage:
        say(s, "Image type:\n"
            "\tWidth:\t\t%d\n"
            "\tHeight:\t\t%d\n"
            "\tStride:\t\t%d\n"
            "\tSliceHeight:\t%d\n"
            "\tError hiding:\t%s\n"
            "\tCodec:\t\t%s\n"
            "\tColor:\t\t%s\n",
            imgdef->nFrameWidth,
            imgdef->nFrameHeight,
            imgdef->nStride,
            imgdef->nSliceHeight,
            (imgdef->bFlagErrorConcealment == OMX_TRUE ? "yes" : "no"),
            dump_compression_format(s, imgdef->eCompressionFormat),
            dump_color_format(s, imgdef->eColorFormat));
        break;
    default:
        break;
    }
}


static void append_buffer(pthread_mutex_t *mutex, pthread_cond_t *cond,
                          int *array_size, OMX_BUFFERHEADERTYPE **array,
                          OMX_BUFFERHEADERTYPE *buffer)
{
    pthread_mutex_lock(mutex);
    array[(*array_size)++] = buffer;
    pthread_cond_broadcast(cond);
    pthread_mutex_unlock(mutex);
}

static OMX_BUFFERHEADERTYPE *get_buffer(pthread_mutex_t *mutex, pthread_cond_t *cond,
                                        int *array_size, OMX_BUFFERHEADERTYPE **array,
                                        int wait)
{
    OMX_BUFFERHEADERTYPE *buffer;
    pthread_mutex_lock(mutex);
    if (wait) {
        while (!*array_size)
            pthread_cond_wait(cond, mutex);
    }
    if (*array_size > 0) {
        buffer = array[0];
        (*array_size)--;
        memmove(&array[0], &array[1], (*array_size) * sizeof(OMX_BUFFERHEADERTYPE *));
    } else {
        buffer = NULL;
    }
    pthread_mutex_unlock(mutex);
    return buffer;

}

static int OnPortOutputChanged(OMXCodecContext *s)
{
    OMX_ERRORTYPE err;
    OMX_PARAM_PORTDEFINITIONTYPE portdef;
    av_log(s->avctx, AV_LOG_INFO, "OnEventOutputChanged\n");

    INIT_STRUCT(portdef);
    portdef.nPortIndex = s->out_port;
    OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);

    dump_portdef(s, &portdef);
    //s->stride = portdef.format.image.nStride;
    //s->plane_size = portdef.format.image.nSliceHeight;
    s->num_out_buffers = portdef.nBufferCountActual;
    err = OMX_SendCommand(s->handle, OMX_CommandPortEnable, s->out_port, NULL);
    s->out_buffer_headers = av_mallocz(s->num_out_buffers * sizeof(OMX_BUFFERHEADERTYPE *));
    for (int i = 0; i < s->num_out_buffers; i++) {
        err = OMX_AllocateBuffer(s->handle, &s->out_buffer_headers[i],  s->out_port,  s, portdef.nBufferSize);
    }

    for (int i = 0; i < s->num_out_buffers; i++) {
        err = OMX_FillThisBuffer(s->handle, s->out_buffer_headers[i]);
    }
    return 0 ;
}

static OMX_ERRORTYPE event_handler(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_EVENTTYPE event,
                                   OMX_U32 data1, OMX_U32 data2, OMX_PTR event_data)
{
    OMXCodecContext *s = app_data;
    // This uses casts in the printfs, since OMX_U32 actually is a typedef for
    // unsigned long in official header versions (but there are also modified
    // versions where it is something else).
    switch (event) {
    case OMX_EventError:
        pthread_mutex_lock(&s->state_mutex);
        av_log(s->avctx, AV_LOG_ERROR, "OMX error %"PRIx32"\n", (uint32_t) data1);
        s->error = data1;
        pthread_cond_broadcast(&s->state_cond);
        pthread_mutex_unlock(&s->state_mutex);
        break;
    case OMX_EventCmdComplete:
        if (data1 == OMX_CommandStateSet) {
            pthread_mutex_lock(&s->state_mutex);
            s->state = data2;
            av_log(s->avctx, AV_LOG_VERBOSE, "OMX state changed to %"PRIu32"\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else if (data1 == OMX_CommandPortDisable) {
            av_log(s->avctx, AV_LOG_VERBOSE, "OMX port %"PRIu32" disabled\n", (uint32_t) data2);
        } else if (data1 == OMX_CommandPortEnable) {
            av_log(s->avctx, AV_LOG_VERBOSE, "OMX port %"PRIu32" enabled\n", (uint32_t) data2);
        } else {
            av_log(s->avctx, AV_LOG_VERBOSE, "OMX command complete, command %"PRIu32", value %"PRIu32"\n",
                   (uint32_t) data1, (uint32_t) data2);
        }
        break;
    case OMX_EventPortSettingsChanged:
        av_log(s->avctx, AV_LOG_VERBOSE, "OMX port %"PRIu32" settings changed\n", (uint32_t) data1);
        break;
    default:
        av_log(s->avctx, AV_LOG_VERBOSE, "OMX event %d %"PRIx32" %"PRIx32"\n",
               event, (uint32_t) data1, (uint32_t) data2);
        break;
    }
    pthread_mutex_lock(&s->state_mutex);

    s->event = event;
    s->state = data2;
    s->cmd = data1;

    pthread_cond_broadcast(&s->state_cond);
    pthread_mutex_unlock(&s->state_mutex);

    return OMX_ErrorNone;
}

static OMX_ERRORTYPE empty_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data,
                                       OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCodecContext *s = app_data;
    if (s->input_zerocopy) {
        if (buffer->pAppPrivate) {
            if (buffer->pOutputPortPrivate)
                av_free(buffer->pAppPrivate);
            else
                av_frame_free((AVFrame **)&buffer->pAppPrivate);
            buffer->pAppPrivate = NULL;
        }
    }
    append_buffer(&s->input_mutex, &s->input_cond,
                  &s->num_free_in_buffers, s->free_in_buffers, buffer);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE fill_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data,
                                      OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCodecContext *s = app_data;
    append_buffer(&s->output_mutex, &s->output_cond,
                  &s->num_done_out_buffers, s->done_out_buffers, buffer);
    return OMX_ErrorNone;
}

static const OMX_CALLBACKTYPE callbacks = {
    event_handler,
    empty_buffer_done,
    fill_buffer_done
};

static av_cold int find_component(OMXContext *omx_context, void *logctx,
                                  const char *role, char *str, int str_size)
{
    OMX_U32 i, num = 0;
    char **components;
    int ret = 0;

#if CONFIG_OMX_RPI
    if (av_strstart(role, "video_encoder.", NULL)) {
        av_strlcpy(str, "OMX.broadcom.video_encode", str_size);
        return 0;
    }
#endif
    omx_context->ptr_GetComponentsOfRole((OMX_STRING) role, &num, NULL);
    if (!num) {
        av_log(logctx, AV_LOG_WARNING, "No component for role %s found\n", role);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    components = av_mallocz_array(num, sizeof(*components));
    if (!components)
        return AVERROR(ENOMEM);
    for (i = 0; i < num; i++) {
        components[i] = av_mallocz(OMX_MAX_STRINGNAME_SIZE);
        if (!components[i]) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }
    omx_context->ptr_GetComponentsOfRole((OMX_STRING) role, &num, (OMX_U8 **) components);
    av_strlcpy(str, components[0], str_size);
end:
    for (i = 0; i < num; i++)
        av_free(components[i]);
    av_free(components);
    return ret;
}

static av_cold int wait_for_state(OMXCodecContext *s, OMX_STATETYPE state)
{
    int ret = 0;
    pthread_mutex_lock(&s->state_mutex);
    while (s->state != state && s->error == OMX_ErrorNone)
        pthread_cond_wait(&s->state_cond, &s->state_mutex);
    if (s->error != OMX_ErrorNone)
        ret = AVERROR_ENCODER_NOT_FOUND;
    pthread_mutex_unlock(&s->state_mutex);
    return ret;
}

static int wait_for_event(AVCodecContext *avctx, OMX_EVENTTYPE event, OMX_COMMANDTYPE cmd, OMX_STATETYPE state,
                          int timeout)
{
    int ret = 0;
    OMXCodecContext *s = avctx->priv_data;
    pthread_mutex_lock(&s->state_mutex);
    while ((s->state != state ||  s->cmd != cmd  || s->event != event) && s->error == OMX_ErrorNone) {
        pthread_cond_wait(&s->state_cond, &s->state_mutex);
    }
    if (s->error != OMX_ErrorNone)
        ret = -1;
    pthread_mutex_unlock(&s->state_mutex);
    return ret;
}

static av_cold int omx_component_init(AVCodecContext *avctx, const char *role)
{
    OMXCodecContext *s = avctx->priv_data;
    OMX_PARAM_COMPONENTROLETYPE role_params = { 0 };
    OMX_PARAM_PORTDEFINITIONTYPE in_port_params = { 0 }, out_port_params = { 0 };

    OMX_PARAM_PORTDEFINITIONTYPE portdef;
    OMX_IMAGE_PARAM_PORTFORMATTYPE imagePortFormat;
    OMX_PORT_PARAM_TYPE port;
    OMX_IMAGE_PARAM_QFACTORTYPE qfactor;
    OMX_CSI_BUFFER_MODE_CONFIGTYPE bufferMode;

    OMX_ERRORTYPE err;
    int i;

    s->maxpts = 0;
    s->version.s.nVersionMajor = 1;
    s->version.s.nVersionMinor = 1;
    s->version.s.nRevision     = 2;
    err = s->omx_context->ptr_GetHandle(&s->handle, s->component_name, s, (OMX_CALLBACKTYPE *) &callbacks);
    if (err != OMX_ErrorNone) {
        av_log(s->avctx, AV_LOG_ERROR, "OMX_GetHandle(%s) failed: %x\n", s->component_name, err);
        return -1;
    }

    INIT_STRUCT(port);
    OMX_GetParameter(s->handle, OMX_IndexParamImageInit, &port);
    if (port.nPorts != 2) {
        return -1;
    }

    s->in_port = port.nStartPortNumber;
    s->out_port = port.nStartPortNumber + 1;

    err = OMX_SendCommand(s->handle, OMX_CommandPortDisable, s->in_port, NULL);
    CHECK(err);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandPortDisable, s->in_port, TIMEOUT_MS);
    err = OMX_SendCommand(s->handle, OMX_CommandPortDisable, s->out_port, NULL);
    CHECK(err);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandPortDisable, s->out_port, TIMEOUT_MS);


    memset((void *)&imagePortFormat, 0, sizeof(imagePortFormat));
    INIT_STRUCT(imagePortFormat);

    imagePortFormat.nPortIndex = s->in_port;
    imagePortFormat.eCompressionFormat = OMX_IMAGE_CodingUnused;

    OMX_SetParameter(s->handle, OMX_IndexParamImagePortFormat, &imagePortFormat);

    s->stride = (avctx->width / 64 * 64) < avctx->width ? ((avctx->width / 64 + 1) * 64) : avctx->width;
    s->stride_padding = s->stride - avctx->width;

    memset((void *)&portdef, 0, sizeof(portdef));
    INIT_STRUCT(portdef);
    portdef.nPortIndex =  s->in_port;

    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);
    CHECK(err);
    int iBufferCount = portdef.nBufferCountActual;
    int iBufferSize = portdef.nBufferSize;
    portdef.nBufferAlignment = 64;
    portdef.format.image.nFrameWidth = avctx->width;
    portdef.format.image.nFrameHeight = avctx->height;
    portdef.format.image.nStride = s->stride;
    portdef.format.image.nSliceHeight = 0;
    portdef.nBufferSize = s->stride * avctx->height * 3 / 2;
    portdef.format.image.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    portdef.eDomain = OMX_PortDomainImage;
    portdef.nBufferCountActual = 1 + 2;

    iBufferSize = portdef.nBufferSize;
    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);
    dump_portdef(s, &portdef);

    memset((void *)&portdef, 0, sizeof(portdef));
    INIT_STRUCT(portdef);
    portdef.nPortIndex =  s->out_port;

    OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);
    portdef.format.image.nFrameWidth = avctx->width;
    portdef.format.image.nFrameHeight = avctx->height;
    portdef.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;
    portdef.nBufferSize = avctx->width * avctx->height * 3 / 2;
    portdef.nBufferCountActual = 3;
    OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);
    CHECK(err);
    dump_portdef(s, &portdef);

    //OMX_IMAGE_PARAM_QFACTORTYPE qfactor;
    INIT_STRUCT(qfactor);
    qfactor.nPortIndex = s->out_port;
    err = OMX_GetParameter(s->handle, OMX_IndexParamQFactor, &qfactor);
    CHECK(err);
    qfactor.nPortIndex = s->out_port;
    qfactor.nQFactor = s->QFactor;
    err = OMX_SetParameter(s->handle, OMX_IndexParamQFactor, &qfactor);
    CHECK(err);

    if (s->input_zerocopy) {
        INIT_STRUCT(bufferMode);
        bufferMode.nPortIndex = s->in_port;
        bufferMode.eMode = OMX_CSI_BUFFER_MODE_DMA;
        err = OMX_SetParameter(s->handle, OMX_CSI_IndexParamBufferMode, &bufferMode);
        if (err != OMX_ErrorNone)
            av_log(avctx, AV_LOG_ERROR, "Unable to set DMA mode at port %d\n", s->in_port);
        else
            av_log(avctx, AV_LOG_INFO, "Set DMA mode at port %d\n", s->in_port);
    }

    OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandStateSet,  OMX_StateIdle, TIMEOUT_MS);

    s->num_in_buffers = iBufferCount = 1 + 2;
    s->in_buffer_headers = av_mallocz(iBufferCount * sizeof(OMX_BUFFERHEADERTYPE *));
    for (int i = 0; i < iBufferCount; i++) {
        OMX_AllocateBuffer(s->handle, &s->in_buffer_headers[i],  s->in_port,  s, iBufferSize);
    }
    s->num_free_in_buffers = 0;
    s->num_done_out_buffers = 0;
    s->free_in_buffers    = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE *) * s->num_in_buffers);
    s->done_out_buffers   = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE *) * s->num_out_buffers);
    if (!s->free_in_buffers || !s->done_out_buffers)
        return AVERROR(ENOMEM);
    for (i = 0; i < s->num_in_buffers; i++)
        s->free_in_buffers[s->num_free_in_buffers++] = s->in_buffer_headers[i];

    s->num_out_buffers = 3;
    s->out_buffer_headers = av_mallocz(s->num_out_buffers * sizeof(OMX_BUFFERHEADERTYPE *));
    for (int i = 0; i < s->num_out_buffers; i++) {
        OMX_AllocateBuffer(s->handle, &s->out_buffer_headers[i],  s->out_port,  s, iBufferSize);
    }

    OMX_SendCommand(s->handle, OMX_CommandPortEnable, s->in_port, NULL);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandPortEnable, s->in_port, TIMEOUT_MS);
    //OMX_SendCommand(s->handle,OMX_CommandPortEnable, s->out_port, NULL);
    //wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandPortEnable, s->out_port, TIMEOUT_MS);

    err = OMX_SendCommand(s->handle,  OMX_CommandStateSet, OMX_StateExecuting, NULL);
    CHECK(err);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateExecuting, TIMEOUT_MS);
    OMX_SendCommand(s->handle, OMX_CommandPortEnable, s->out_port, NULL);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandPortEnable, s->out_port, TIMEOUT_MS);
    for (int i = 0; i < s->num_out_buffers; i++) {
        err = OMX_FillThisBuffer(s->handle, s->out_buffer_headers[i]);
    }
    av_log(s->avctx, AV_LOG_INFO, "finish omx_component_init_encoder\n");
    return 0;
}

static av_cold void cleanup(OMXCodecContext *s)
{
    int i, executing;

    pthread_mutex_lock(&s->state_mutex);
    executing = s->state == OMX_StateExecuting;
    pthread_mutex_unlock(&s->state_mutex);
    if (executing || s->input_zerocopy) {
        OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
        wait_for_state(s, OMX_StateIdle);
        OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
        for (i = 0; i < s->num_in_buffers; i++) {
            OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->input_mutex, &s->input_cond,
                                           &s->num_free_in_buffers, s->free_in_buffers, 1);
            if (s->input_zerocopy)
                buffer->pBuffer = NULL;
            OMX_FreeBuffer(s->handle, s->in_port, buffer);
        }
        for (i = 0; i < s->num_out_buffers; i++) {
            OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                           &s->num_done_out_buffers, s->done_out_buffers, 1);
            OMX_FreeBuffer(s->handle, s->out_port, buffer);
        }
        wait_for_state(s, OMX_StateLoaded);
    }
    if (s->handle) {
        s->omx_context->ptr_FreeHandle(s->handle);
        s->handle = NULL;
    }

    omx_deinit(s->omx_context);
    s->omx_context = NULL;
    if (s->mutex_cond_inited) {
        pthread_cond_destroy(&s->state_cond);
        pthread_mutex_destroy(&s->state_mutex);
        pthread_cond_destroy(&s->input_cond);
        pthread_mutex_destroy(&s->input_mutex);
        pthread_cond_destroy(&s->output_cond);
        pthread_mutex_destroy(&s->output_mutex);
        s->mutex_cond_inited = 0;
    }

    av_freep(&s->in_buffer_headers);
    av_freep(&s->out_buffer_headers);
    av_freep(&s->free_in_buffers);
    av_freep(&s->done_out_buffers);
    av_freep(&s->output_buf);

}

static av_cold int omx_encode_init(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;
    int ret = AVERROR_ENCODER_NOT_FOUND;
    const char *role;
    OMX_BUFFERHEADERTYPE *buffer;
    OMX_ERRORTYPE err;

    s->omx_context = omx_init(avctx, s->libname, s->libprefix);
    if (!s->omx_context)
        return AVERROR_ENCODER_NOT_FOUND;

    pthread_mutex_init(&s->state_mutex, NULL);
    pthread_cond_init(&s->state_cond, NULL);
    pthread_mutex_init(&s->input_mutex, NULL);
    pthread_cond_init(&s->input_cond, NULL);
    pthread_mutex_init(&s->output_mutex, NULL);
    pthread_cond_init(&s->output_cond, NULL);
    s->mutex_cond_inited = 1;
    s->avctx = avctx;
    s->state = OMX_StateLoaded;
    s->error = OMX_ErrorNone;

    switch (avctx->codec->id) {
    case AV_CODEC_ID_MJPEG:
        role = "image_encoder.jpeg";
        break;
    default:
        return AVERROR(ENOSYS);
    }

    if ((ret = find_component(s->omx_context, avctx, role, s->component_name, sizeof(s->component_name))) < 0)
        goto fail;

    av_log(avctx, AV_LOG_INFO, "Using %s\n", s->component_name);

    if ((ret = omx_component_init(avctx, role)) < 0)
        goto fail;
#if 0
    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        while (1) {
            buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                &s->num_done_out_buffers, s->done_out_buffers, 1);
            if (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
                if ((ret = av_reallocp(&avctx->extradata,
                                       avctx->extradata_size + buffer->nFilledLen + AV_INPUT_BUFFER_PADDING_SIZE)) < 0) {
                    avctx->extradata_size = 0;
                    goto fail;
                }
                memcpy(avctx->extradata + avctx->extradata_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
                avctx->extradata_size += buffer->nFilledLen;
                memset(avctx->extradata + avctx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            }
            err = OMX_FillThisBuffer(s->handle, buffer);
            if (err != OMX_ErrorNone) {
                append_buffer(&s->output_mutex, &s->output_cond,
                              &s->num_done_out_buffers, s->done_out_buffers, buffer);
                av_log(avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
                ret = AVERROR_UNKNOWN;
                goto fail;
            }
            if (avctx->codec->id == AV_CODEC_ID_H264) {
                // For H.264, the extradata can be returned in two separate buffers
                // (the videocore encoder on raspberry pi does this);
                // therefore check that we have got both SPS and PPS before continuing.
                int nals[32] = { 0 };
                int i;
                for (i = 0; i + 4 < avctx->extradata_size; i++) {
                    if (!avctx->extradata[i + 0] &&
                        !avctx->extradata[i + 1] &&
                        !avctx->extradata[i + 2] &&
                        avctx->extradata[i + 3] == 1) {
                        nals[avctx->extradata[i + 4] & 0x1f]++;
                    }
                }
                if (nals[H264_NAL_SPS] && nals[H264_NAL_PPS])
                    break;
            } else {
                if (avctx->extradata_size > 0)
                    break;
            }
        }
    }
#endif

    return 0;
fail:
    return ret;
}

static int omx_line_copy(AVCodecContext *avctx, const AVFrame *frame, OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCodecContext *s = avctx->priv_data;
    memset(buffer->pBuffer, 0, s->stride * frame->height * 3 / 2);
    for (int i = 0; i < frame->height; i++) {
        memcpy(buffer->pBuffer + i * s->stride, frame->data[0] + i * frame->width, frame->width);
    }
    for (int i = 0; i < frame->height / 2; i++) {
        memcpy(buffer->pBuffer + s->stride * frame->height + i * s->stride, frame->data[1] + +i * frame->width, frame->width);
    }
    return 0;
}

static int omx_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *frame, int *got_packet)
{
    OMXCodecContext *s = avctx->priv_data;
    int ret = 0;
    OMX_BUFFERHEADERTYPE *buffer;
    OMX_ERRORTYPE err;
    AVFrame *local = NULL;
    int had_partial = 0;

    if (frame) {
        uint8_t *dst[4];
        int linesize[4];
        int need_copy;
        buffer = get_buffer(&s->input_mutex, &s->input_cond,
                            &s->num_free_in_buffers, s->free_in_buffers, 1);

        //buffer->nFilledLen = av_image_fill_arrays(dst, linesize, buffer->pBuffer, avctx->pix_fmt, s->stride, s->plane_size, 1);
        buffer->nFilledLen = s->stride * frame->height * 3 / 2;
        buffer->nAllocLen = buffer->nFilledLen;

        if (s->stride_padding == 0) {
            if (s->input_zerocopy) {
                AVFrame *local = av_frame_clone(frame);
                buffer->pAppPrivate = local;
                buffer->pOutputPortPrivate = NULL;
                buffer->pBuffer = ((OMX_BUFFERHEADERTYPE *)(local->opaque))->pBuffer;
            } else {
                memcpy(buffer->pBuffer, frame->data[0], frame->height * frame->width);
                memcpy(buffer->pBuffer + frame->height * frame->width, frame->data[1], frame->height * frame->width / 2);
            }
        } else if (s->stride_padding > 0) {
            omx_line_copy(avctx, frame, buffer);
        } else {
            av_log(avctx, AV_LOG_ERROR, "error stride padding size: %d\n", s->stride_padding);
            return AVERROR_UNKNOWN;
        }
        buffer->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
        buffer->nOffset = 0;
        // Convert the timestamps to microseconds; some encoders can ignore
        // the framerate and do VFR bit allocation based on timestamps.
        buffer->nTimeStamp = to_omx_ticks(av_rescale_q(frame->pts, avctx->time_base, AV_TIME_BASE_Q));
        if (frame->pict_type == AV_PICTURE_TYPE_I) {
#if CONFIG_OMX_RPI
            OMX_CONFIG_BOOLEANTYPE config = {0, };
            INIT_STRUCT(config);
            config.bEnabled = OMX_TRUE;
            err = OMX_SetConfig(s->handle, OMX_IndexConfigBrcmVideoRequestIFrame, &config);
            if (err != OMX_ErrorNone) {
                av_log(avctx, AV_LOG_ERROR, "OMX_SetConfig(RequestIFrame) failed: %x\n", err);
                return AVERROR_UNKNOWN;
            }
#else
            OMX_CONFIG_INTRAREFRESHVOPTYPE config = {0, };
            INIT_STRUCT(config);
            config.nPortIndex = s->out_port;
            config.IntraRefreshVOP = OMX_TRUE;
            err = OMX_SetConfig(s->handle, OMX_IndexConfigVideoIntraVOPRefresh, &config);
            if (err != OMX_ErrorNone) {
                av_log(avctx, AV_LOG_ERROR, "OMX_SetConfig(IntraVOPRefresh) failed: %x\n", err);
                return AVERROR_UNKNOWN;
            }
#endif
        }
        err = OMX_EmptyThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->input_mutex, &s->input_cond, &s->num_free_in_buffers, s->free_in_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "OMX_EmptyThisBuffer failed: %x\n", err);
            return AVERROR_UNKNOWN;
        }
    } else if (!s->eos_sent) {
#if 1
        buffer = get_buffer(&s->input_mutex, &s->input_cond,
                            &s->num_free_in_buffers, s->free_in_buffers, 1);

        buffer->nFilledLen = 0;
        buffer->nFlags = OMX_BUFFERFLAG_EOS;
        buffer->pAppPrivate = buffer->pOutputPortPrivate = NULL;
        err = OMX_EmptyThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->input_mutex, &s->input_cond, &s->num_free_in_buffers, s->free_in_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "OMX_EmptyThisBuffer failed: %x\n", err);
            return AVERROR_UNKNOWN;
        }
        s->eos_sent = 1;
#endif
    }

    while (!*got_packet && ret == 0 && !s->got_eos) {
        // If not flushing, just poll the queue if there's finished packets.
        // If flushing, do a blocking wait until we either get a completed
        // packet, or get EOS.
        buffer = get_buffer(&s->output_mutex, &s->output_cond,
                            &s->num_done_out_buffers, s->done_out_buffers,
                            !frame || had_partial);
        if (!buffer)
            break;

        if (buffer->nFlags & OMX_BUFFERFLAG_EOS)
            s->got_eos = 1;

        if (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG && avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER && 0) {
            if ((ret = av_reallocp(&avctx->extradata,
                                   avctx->extradata_size + buffer->nFilledLen + AV_INPUT_BUFFER_PADDING_SIZE)) < 0) {
                avctx->extradata_size = 0;
                goto end;
            }
            memcpy(avctx->extradata + avctx->extradata_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
            avctx->extradata_size += buffer->nFilledLen;
            memset(avctx->extradata + avctx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        } else {
            if (!(buffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) || !pkt->data) {
                // If the output packet isn't preallocated, just concatenate everything in our
                // own buffer
                int newsize = s->output_buf_size + buffer->nFilledLen + AV_INPUT_BUFFER_PADDING_SIZE;
                if ((ret = av_reallocp(&s->output_buf, newsize)) < 0) {
                    s->output_buf_size = 0;
                    goto end;
                }
                memcpy(s->output_buf + s->output_buf_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
                s->output_buf_size += buffer->nFilledLen;
                if (!s->output_buf_size) {
                    av_freep(pkt);
                    av_freep(&s->output_buf);
                    s->output_buf_size = 0;
                    goto end;
                }
                if (buffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
                    if ((ret = av_packet_from_data(pkt, s->output_buf, s->output_buf_size)) < 0) {
                        av_freep(&s->output_buf);
                        s->output_buf_size = 0;
                        goto end;
                    }
                    s->output_buf = NULL;
                    s->output_buf_size = 0;
                }
#if CONFIG_OMX_RPI
                had_partial = 1;
#endif
            } else {
                // End of frame, and the caller provided a preallocated frame
                if ((ret = ff_alloc_packet(avctx, pkt, s->output_buf_size + buffer->nFilledLen)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Error getting output packet of size %d.\n",
                           (int)(s->output_buf_size + buffer->nFilledLen));
                    goto end;
                }
                memcpy(pkt->data, s->output_buf, s->output_buf_size);
                memcpy(pkt->data + s->output_buf_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
                av_freep(&s->output_buf);
                s->output_buf_size = 0;
            }
            if (buffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
                pkt->pts = av_rescale_q(from_omx_ticks(buffer->nTimeStamp), AV_TIME_BASE_Q, avctx->time_base);
                // We don't currently enable B-frames for the encoders, so set
                // pkt->dts = pkt->pts. (The calling code behaves worse if the encoder
                // doesn't set the dts).
                if (pkt->pts < s->maxpts) {
                    pkt->pts = s->maxpts + 1;
                }
                s->maxpts = (s->maxpts > pkt->pts) ? s->maxpts : pkt->pts;
                pkt->dts = pkt->pts;
                if (buffer->nFlags & OMX_BUFFERFLAG_SYNCFRAME)
                    pkt->flags |= AV_PKT_FLAG_KEY;
                *got_packet = 1;
            }
        }
end:
        err = OMX_FillThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->output_mutex, &s->output_cond, &s->num_done_out_buffers, s->done_out_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
            ret = AVERROR_UNKNOWN;
        }
    }
    return ret;
}

static av_cold int omx_encode_end(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;
    cleanup(s);
    return 0;
}

#define OFFSET(x) offsetof(OMXCodecContext, x)
#define VDE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_ENCODING_PARAM
#define VE  AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options_jpeg[] = {
    { "omx_libname", "OpenMAX library name", OFFSET(libname), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VDE },
    { "omx_libprefix", "OpenMAX library prefix", OFFSET(libprefix), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VDE },
    { "zerocopy", "Try to avoid copying input frames if possible", OFFSET(input_zerocopy), AV_OPT_TYPE_INT, { .i64 = CONFIG_OMX_RPI }, 0, 1, VE },
    { "QFactor",  "Set the encoding QFactor", OFFSET(QFactor), AV_OPT_TYPE_INT,   {2}, 0, 10, VE},
    { NULL }
};



static const enum AVPixelFormat omx_encoder_pix_fmts[] = {
    AV_PIX_FMT_NV12, AV_PIX_FMT_NONE
};


static const AVClass omx_jpegenc_class = {
    .class_name = "jpeg_omx",
    .item_name  = av_default_item_name,
    .option     = options_jpeg,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_jpeg_omx_encoder = {
    .p.name             = "jpeg_omx",
    .p.long_name      = NULL_IF_CONFIG_SMALL("OpenMAX IL JPEG video encoder"),
    .p.type             = AVMEDIA_TYPE_VIDEO,
    .p.id               = AV_CODEC_ID_MJPEG,
    .p.pix_fmts         = omx_encoder_pix_fmts,
    .p.capabilities     = AV_CODEC_CAP_DELAY,
    .p.priv_class       = &omx_jpegenc_class,
    .priv_data_size   = sizeof(OMXCodecContext),
    .init             = omx_encode_init,
    FF_CODEC_ENCODE_CB(omx_encode_frame),
    .close            = omx_encode_end,
    .caps_internal    = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
    .p.wrapper_name = "jpeg_omx",
};


