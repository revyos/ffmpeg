/*decoders
*
* Copyright (c) 2023-2024 Huazhu Sun <sunhuazhu@coocaa.com>
* Copyright (c) 2023-2024 Kaiyuan Dong <dongkaiyuan.dky@alibaba-inc.com>
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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <dlfcn.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixfmt.h"
#include "libavutil/internal.h"
#include <libavutil/pixdesc.h>
#include "libavutil/time.h"
#include <libavutil/thread.h>
#include <libavcodec/pthread_internal.h>

#include <OMX_Core.h>
#include <OMX_Component.h>

#include "avcodec.h"
#include "decode.h"
#include "h264_parse.h"
#include "h264_ps.h"
#include "hevc_parse.h"
#include "hwconfig.h"
#include "internal.h"
#include <OMX_CsiExt.h>
extern int omx_load_count;
#define DEBUG_NO_OMX

#define TIMEOUT_MS 1000

#define kNumPictureBuffers 2
#define INIT_STRUCT(x) do {                                               \
        x.nSize = sizeof(x);                                              \
        x.nVersion = s->version;                                          \
    } while (0)
#define CHECK(x) do {                                                     \
        if (x != OMX_ErrorNone) {                                         \
            av_log(s->avctx, AV_LOG_ERROR,                                \
                   "err %x (%d) on line %d\n", x, x, __LINE__);           \
            return -1;                                       \
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


typedef struct OMXCodecDecoderContext {
    char *libname;
    char *libprefix;
    OMXContext *omx_context;
    AVCodecContext *avctx;
    char component_name[OMX_MAX_STRINGNAME_SIZE];
    OMX_VERSIONTYPE version;
    OMX_HANDLETYPE handle;
    int in_port, out_port;
    int width, height;
    int stride, plane_size;
    int filelen;
    int num_in_buffers, num_out_buffers;
    int in_buffer_size, out_buffer_size;
    OMX_BUFFERHEADERTYPE **in_buffer_headers;
    OMX_BUFFERHEADERTYPE **out_buffer_headers;

    int num_free_in_buffers;
    OMX_BUFFERHEADERTYPE **free_in_buffers;
    int num_done_out_buffers;
    OMX_BUFFERHEADERTYPE **done_out_buffers;

    int in_fill_index;
    int out_fill_index;
    pthread_mutex_t input_mutex;
    pthread_cond_t input_cond;

    pthread_mutex_t output_mutex;
    pthread_cond_t output_cond;

    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    OMX_EVENTTYPE event;
    OMX_COMMANDTYPE cmd;
    OMX_STATETYPE state;

    OMX_ERRORTYPE error;
    int have_init;
    int crop_top, crop_left;
    int portSettingChanged;
} OMXCodecDecoderContext;


static void say(OMXCodecDecoderContext *s, const char *message, ...)
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


static const char *dump_compression_format(OMXCodecDecoderContext *s, OMX_VIDEO_CODINGTYPE c)
{
    char *f;
    switch (c) {
    case OMX_VIDEO_CodingUnused:
        return "not used";
    case OMX_VIDEO_CodingAutoDetect:
        return "autodetect";
    case OMX_VIDEO_CodingMPEG2:
        return "MPEG2";
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

static const char *dump_color_format(OMXCodecDecoderContext *s, OMX_COLOR_FORMATTYPE c)
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

static void dump_portdef(OMXCodecDecoderContext *s, OMX_PARAM_PORTDEFINITIONTYPE *portdef)
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

static OMX_ERRORTYPE empty_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data,
                                       OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCodecDecoderContext *s = app_data;
    pthread_mutex_lock(&s->input_mutex);
    s->num_free_in_buffers++;
    pthread_mutex_unlock(&s->input_mutex);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE fill_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data,
                                      OMX_BUFFERHEADERTYPE *buffer)
{
    OMX_ERRORTYPE err;
    OMXCodecDecoderContext *s = app_data;
    pthread_mutex_lock(&s->output_mutex);
    s->num_done_out_buffers++;
    pthread_mutex_unlock(&s->output_mutex);

    return OMX_ErrorNone;
}

static void OnDecoderOutputChanged(OMXCodecDecoderContext *s)
{
    OMX_ERRORTYPE err;
    OMX_PARAM_PORTDEFINITIONTYPE portdef;
    av_log(s->avctx, AV_LOG_INFO, "OnDecoderOutputChanged\n");

    INIT_STRUCT(portdef);
    portdef.nPortIndex = s->out_port;
    OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);
    s->out_buffer_size = portdef.nBufferCountMin;
    portdef.nBufferCountActual = portdef.nBufferCountMin;
    OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);

    dump_portdef(s, &portdef);
    s->avctx->width = portdef.format.image.nFrameWidth;
    s->width = portdef.format.image.nFrameWidth;
    s->avctx->height = portdef.format.image.nFrameHeight;
    s->height = portdef.format.image.nFrameHeight;
    s->stride = portdef.format.image.nStride;
    s->plane_size = portdef.format.image.nSliceHeight;
    OMX_SendCommand(s->handle, OMX_CommandPortEnable, s->out_port, NULL);

    s->out_buffer_headers = malloc(s->out_buffer_size * sizeof(OMX_BUFFERHEADERTYPE *));
    for (int i = 0; i < s->out_buffer_size; i++) {
        err = OMX_AllocateBuffer(s->handle, &s->out_buffer_headers[i],  s->out_port,  s, portdef.nBufferSize);
    }

    for (int i = 0; i < s->out_buffer_size; i++) {
        err = OMX_FillThisBuffer(s->handle, s->out_buffer_headers[i]);
    }

    s->portSettingChanged = 1;
}

static OMX_ERRORTYPE event_handler(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_EVENTTYPE event,
                                   OMX_U32 data1, OMX_U32 data2, OMX_PTR event_data)
{
    OMXCodecDecoderContext *s = app_data;

    switch (event) {
    case OMX_EventError:
        pthread_mutex_lock(&s->state_mutex);
        s->error = data1;
        pthread_cond_broadcast(&s->state_cond);
        pthread_mutex_unlock(&s->state_mutex);
        switch (data1) {
        case OMX_ErrorInsufficientResources:
            av_log(s->avctx, AV_LOG_ERROR, "OMX_ErrorInsufficientResources, stop decode!\n");
            break;
        case OMX_ErrorInvalidState:
            av_log(s->avctx, AV_LOG_ERROR, "OMX_ErrorInvalidState!\n");
            s->state = OMX_StateInvalid;
            break;
        case OMX_ErrorNotReady:
            av_log(s->avctx, AV_LOG_ERROR, "OMX_ErrorNotReady!\n");
            break;
        case OMX_ErrorIncorrectStateOperation:
            av_log(s->avctx, AV_LOG_ERROR, "OMX_ErrorIncorrectStateOperation!\n");
            break;
        case OMX_ErrorTimeout:
            av_log(s->avctx, AV_LOG_ERROR, "OMX_ErrorTimeout!\n");
            break;
        case OMX_ErrorIncorrectStateTransition:
            av_log(s->avctx, AV_LOG_ERROR, "OMX_ErrorTimeout!\n");
            break;
        default:
            av_log(s->avctx, AV_LOG_ERROR, "OMX error 0x%x\n", (uint32_t) data1);
            break;
        }
        return OMX_ErrorNone;
    case OMX_EventCmdComplete:
        if (data1 == OMX_CommandStateSet) {
            switch (data2) {
            case  OMX_StateIdle:
                av_log(s->avctx, AV_LOG_INFO, "OMX_StateIdle\n");
                break;
            case OMX_StateLoaded:
                av_log(s->avctx, AV_LOG_INFO, "OMX_StateLoaded\n");
                break;
            case OMX_StateExecuting:
                av_log(s->avctx, AV_LOG_INFO, "OMX_StateExecuting\n");
                break;
            case OMX_StatePause:
                av_log(s->avctx, AV_LOG_INFO, "OMX_StatePause\n");
                break;
            default:
                av_log(s->avctx, AV_LOG_INFO, "OMX_State %X\n", data2);
                break;
            }

        } else if (data1 == OMX_CommandPortDisable) {
            av_log(s->avctx, AV_LOG_INFO, "OMX port%ld disabled\n", (uint32_t) data2);
        } else if (data1 == OMX_CommandPortEnable) {
            av_log(s->avctx, AV_LOG_INFO, "OMX port %ld enabled\n", (uint32_t) data2);
        } else if (data1 == OMX_CommandFlush) {
            av_log(s->avctx, AV_LOG_INFO, "OMX port %ld flushed\n", (uint32_t) data2);
        } else {
            av_log(s->avctx, AV_LOG_INFO, "OMX command complete, command %ld, value %ld\n",
                   (uint32_t) data1, (uint32_t) data2);
        }
        break;
    case OMX_EventPortSettingsChanged:
        if ((int)data1 == OMX_DirOutput) { //out is OMX_DirOutput
            OnDecoderOutputChanged(s);

        } else if (data1 == s->out_port && data2 == OMX_IndexConfigCommonOutputCrop) {

        } else if (data1 == s->out_port && data2 == OMX_IndexConfigCommonScale) {

        } else {
            av_log(s->avctx, AV_LOG_ERROR, "error event \n");
        }

        break;
    case OMX_EventBufferFlag:
        if (data1 == s->out_port) {
            //
        }
        break;
    default:
        av_log(s->avctx, AV_LOG_INFO, "OMX event %d %ld %ld\n",
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


static const OMX_CALLBACKTYPE decoder_callbacks = {
    event_handler,
    empty_buffer_done,
    fill_buffer_done
};

static av_cold int wait_for_state(OMXCodecDecoderContext *s, OMX_STATETYPE state)
{
    int ret = 0;
    pthread_mutex_lock(&s->state_mutex);
    while (s->state != state && s->error == OMX_ErrorNone)
        pthread_cond_wait(&s->state_cond, &s->state_mutex);
    if (s->error != OMX_ErrorNone)
        ret = AVERROR_DECODER_NOT_FOUND;
    pthread_mutex_unlock(&s->state_mutex);
    return ret;
}

static int wait_for_event(AVCodecContext *avctx, OMX_EVENTTYPE event, OMX_COMMANDTYPE cmd, OMX_STATETYPE state,
                          int timeout)
{
    int ret = 0;
    OMXCodecDecoderContext *s = avctx->priv_data;
    pthread_mutex_lock(&s->state_mutex);
    while ((s->state != state ||  s->cmd != cmd  || s->event != event) && s->error == OMX_ErrorNone) {
        pthread_cond_wait(&s->state_cond, &s->state_mutex);
    }
    if (s->error != OMX_ErrorNone)
        ret = -1;
    pthread_mutex_unlock(&s->state_mutex);
    return ret;
}

static  void *dlsym_prefixed(void *handle, const char *symbol, const char *prefix)
{
    char buf[50];
    snprintf(buf, sizeof(buf), "%s%s", prefix ? prefix : "", symbol);
    return dlsym(handle, buf);
}


static int omx_try_load(OMXContext *s, void *logctx,
                        const char *libname, const char *prefix,
                        const char *libname2)
{
    s->lib = dlopen(libname, RTLD_NOW | RTLD_GLOBAL);
    if (!s->lib) {
        av_log(logctx, AV_LOG_ERROR, "%s not found\n", libname);
        return -1;
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
        av_log(logctx, AV_LOG_ERROR, "Not all functions found in %s\n", libname);
        dlclose(s->lib);
        s->lib = NULL;
        if (s->lib2)
            dlclose(s->lib2);
        s->lib2 = NULL;
        return -1;
    }
    return 0;
}


static int print_omx_env(OMXContext *omx_context)
{
    OMX_ERRORTYPE err;
    OMX_U32 numComps = 0;
    OMX_U8 component[OMX_MAX_STRINGNAME_SIZE];
    OMX_U8 **roleList;

    if (omx_context == NULL) {
        return 0;
    }

    if (omx_context->ptr_ComponentNameEnum == NULL) {
        return 0;
    }
    numComps = 0;
    while (err == OMX_ErrorNone) {
        err = omx_context->ptr_ComponentNameEnum(component, OMX_MAX_STRINGNAME_SIZE, numComps);
        if (err == OMX_ErrorNone) {
            OMX_U32 numberofroles = 0;
            err = omx_context->ptr_GetRolesOfComponent(component, &numberofroles, NULL);
            if (numberofroles == 1) {
                roleList = malloc(numberofroles * sizeof(OMX_U8 *));
                roleList[0] =  malloc(OMX_MAX_STRINGNAME_SIZE);
                omx_context->ptr_GetRolesOfComponent(component, &numberofroles, roleList);
                free(roleList[0]);
                free(roleList);
            }
        }
        numComps++;
    }

    return 0;
}

static OMXContext *omx_init(void *logctx,  const char *prefix)
{
    static const char *const libnames[] = {
        "libomxil-bellagio.so.0", NULL,
        "libOMX_Core.so", NULL,
        "libOmxCore.so", NULL,
        NULL
    };
    const char *const *nameptr;
    int ret = -1;
    OMXContext *omx_context;
    OMX_ERRORTYPE error;
    omx_context = malloc(sizeof(*omx_context));
    if (!omx_context)
        return NULL;
    memset((void *)omx_context, 0, sizeof(*omx_context));

    for (nameptr = libnames; *nameptr; nameptr += 2)
        if (!(ret = omx_try_load(omx_context, logctx, nameptr[0], prefix, nameptr[1])))
            break;
    if (!*nameptr) {
        free(omx_context);
        return NULL;
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

static int omx_dec_find_component(OMXContext *omx_context,
                                  const char *role, char *str, int str_size)
{
    OMX_U32 i, num = 0;
    char **components;
    int ret = 0;
//OMX.hantro.VC8000D.image.decoder.jpeg
    omx_context->ptr_GetComponentsOfRole((OMX_STRING) role, &num, NULL);
    if (!num) {
        return -1;
    }
    components = calloc(num, sizeof(*components));
    if (!components)
        return -1;

    memset((void *)components, 0, num * sizeof(*components));
    for (i = 0; i < num; i++) {
        components[i] = malloc(OMX_MAX_STRINGNAME_SIZE);
        if (!components[i]) {
            ret = -1;
            goto end;
        }
        memset((void *)components[i], 0, OMX_MAX_STRINGNAME_SIZE);
    }
    omx_context->ptr_GetComponentsOfRole((OMX_STRING) role, &num, (OMX_U8 **) components);
    strncpy(str, components[0], str_size);
end:
    for (i = 0; i < num; i++)
        free(components[i]);
    free(components);
    return ret;
}


static void omx_deinit(OMXContext *omx_context)
{
    if (!omx_context)
        return;
    omx_load_count--;
    av_log(NULL, AV_LOG_INFO, "OMX_count %d\n", omx_load_count);
    if (omx_load_count == 0) {
        omx_context->ptr_Deinit();
        dlclose(omx_context->lib);
    }
    free(omx_context);
}



static int omx_component_init_decoder(AVCodecContext *avctx)
{
    OMXCodecDecoderContext *s = avctx->priv_data;
    OMX_PARAM_COMPONENTROLETYPE role_params = { 0 };
    OMX_PARAM_PORTDEFINITIONTYPE in_port_params = { 0 }, out_port_params = { 0 };

    OMX_PARAM_PORTDEFINITIONTYPE portdef;
    OMX_IMAGE_PARAM_PORTFORMATTYPE imagePortFormat;
    OMX_PORT_PARAM_TYPE port;
    OMX_CSI_BUFFER_MODE_CONFIGTYPE bufferMode = {0};

    OMX_ERRORTYPE err;
    int i;

    s->version.s.nVersionMajor = 1;
    s->version.s.nVersionMinor = 1;
    s->version.s.nRevision     = 2;
    err = s->omx_context->ptr_GetHandle(&s->handle, s->component_name, s, (OMX_CALLBACKTYPE *) &decoder_callbacks);
    if (err != OMX_ErrorNone) {
        av_log(s->avctx, AV_LOG_ERROR, "OMX_GetHandle(%s) failed: %x\n", s->component_name, err);
        return -1;
    }

    s->crop_left = 0;
    s->crop_top = 0;
    s->have_init = 1;
    s->portSettingChanged = 0;
    INIT_STRUCT(port);
    OMX_GetParameter(s->handle, OMX_IndexParamImageInit, &port);
    if (port.nPorts != 2) {
        return -1;
    }

    s->in_port = port.nStartPortNumber;
    s->out_port = port.nStartPortNumber + 1;

    OMX_SendCommand(s->handle, OMX_CommandPortDisable, s->in_port, NULL);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandPortDisable, s->in_port, TIMEOUT_MS);
    OMX_SendCommand(s->handle, OMX_CommandPortDisable, s->out_port, NULL);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandPortDisable, s->out_port, TIMEOUT_MS);

    memset((void *)&imagePortFormat, 0, sizeof(imagePortFormat));
    INIT_STRUCT(imagePortFormat);

    imagePortFormat.nPortIndex = s->in_port;
    imagePortFormat.eCompressionFormat = OMX_IMAGE_CodingJPEG;

    OMX_SetParameter(s->handle, OMX_IndexParamImagePortFormat, &imagePortFormat);

    memset((void *)&portdef, 0, sizeof(portdef));
    INIT_STRUCT(portdef);
    portdef.nPortIndex =  s->in_port;

    OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);
    int iBufferCount = portdef.nBufferCountMin;
    portdef.nBufferCountActual = portdef.nBufferCountMin;
    int iBufferSize = portdef.nBufferSize;
    if (s->stride != 0) {
        portdef.format.image.nFrameWidth = s->stride;
        portdef.format.image.nFrameHeight = s->plane_size;
    }

    if (s->filelen >  4096 * 1024) {
        portdef.nBufferSize = s->filelen;
    } else {
        portdef.nBufferSize = 4096 * 1024;
    }
    iBufferSize = portdef.nBufferSize;
    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);
    if (err != OMX_ErrorNone) {
        av_log(s->avctx, AV_LOG_ERROR, "set error failed\n");
        return AVERROR_UNKNOWN;
    }
    dump_portdef(s, &portdef);
    memset((void *)&portdef, 0, sizeof(portdef));
    INIT_STRUCT(portdef);
    portdef.nPortIndex =  s->out_port;

    OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);
    dump_portdef(s, &portdef);
    if (s->stride != 0) {
        portdef.format.image.nFrameWidth = s->stride;
        portdef.format.image.nFrameHeight = s->plane_size;
        OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &portdef);
    }

    INIT_STRUCT(bufferMode);
    bufferMode.nPortIndex = s->out_port;
    bufferMode.eMode = OMX_CSI_BUFFER_MODE_DMA;
    err = OMX_SetParameter(s->handle, OMX_CSI_IndexParamBufferMode, &bufferMode);
    if (err != OMX_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Unable to set DMA mode at port %d\n", s->out_port);
        return AVERROR_UNKNOWN;
    } else
        av_log(avctx, AV_LOG_INFO, "Set DMA mode at port %d\n", s->out_port);

    OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandStateSet,  OMX_StateIdle, TIMEOUT_MS);

    OMX_SendCommand(s->handle, OMX_CommandPortEnable, s->in_port, NULL);

    s->num_free_in_buffers = s->in_buffer_size = iBufferCount;
    s->num_done_out_buffers = 0;
    s->in_buffer_headers = malloc(iBufferCount * sizeof(OMX_BUFFERHEADERTYPE *));
    for (int i = 0; i < iBufferCount; i++) {
        OMX_AllocateBuffer(s->handle, &s->in_buffer_headers[i],  s->in_port,  s, iBufferSize);
    }

    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandPortEnable, s->in_port, TIMEOUT_MS);
    OMX_SendCommand(s->handle,  OMX_CommandStateSet, OMX_StateExecuting, NULL);
    wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateExecuting, TIMEOUT_MS);

    av_log(s->avctx, AV_LOG_INFO, "finish omx_component_init_decoder\n");
    return 0;
}


static int omx_decode_init2(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_INFO, "jpeg_omxcodec decoder init...\n");
    OMXCodecDecoderContext *s = avctx->priv_data;
    int ret = -1;
    const char *role = "image_decoder.jpeg";
    memset((void *)s, 0, sizeof(OMXCodecDecoderContext));

    s->avctx = avctx;
    s->width = 0;
    s->height = 0;
    s->omx_context = omx_init(s->libname, s->libprefix);
    if (!s->omx_context)
        return -1;
    pthread_mutex_init(&s->state_mutex, NULL);
    pthread_cond_init(&s->state_cond, NULL);
    pthread_mutex_init(&s->input_mutex, NULL);
    pthread_cond_init(&s->input_cond, NULL);
    pthread_mutex_init(&s->output_mutex, NULL);
    pthread_cond_init(&s->output_cond, NULL);

    s->state = OMX_StateLoaded;
    s->error = OMX_ErrorNone;

    strcpy(s->component_name, "OMX.hantro.VC8000D.image.decoder.jpeg");
    return 0;
}

static int omx_decode_init(AVCodecContext *avctx, int len)
{
    OMXCodecDecoderContext *s = avctx->priv_data;
    int ret = -1;
    s->filelen = len;
    av_log(avctx, AV_LOG_INFO, "Input File Len: %d\n", len);
    ret =  omx_component_init_decoder(avctx);
    if (ret < 0) {
        return ret;
    }
    return 0;
}



static int omx_decode_uninit(AVCodecContext *avctx)
{
    OMXCodecDecoderContext *s = avctx->priv_data;
    if (s->state == OMX_StateExecuting) {
        OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
        wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateIdle, TIMEOUT_MS);
        OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateLoaded, NULL);

        for (int i = 0; i < s->num_in_buffers; i++) {
            OMX_BUFFERHEADERTYPE *buffer = s->in_buffer_headers[i];
            OMX_FreeBuffer(s->handle, s->in_port, buffer);
        }

        for (int i = 0; i < s->num_out_buffers; i++) {
            OMX_BUFFERHEADERTYPE *buffer = s->out_buffer_headers[i];
            OMX_FreeBuffer(s->handle, s->out_port, buffer);
        }
        wait_for_event(avctx, OMX_EventCmdComplete, OMX_CommandStateSet, OMX_StateLoaded, TIMEOUT_MS);
        //wait_for_state(s, OMX_StateLoaded);
    }

    if (s->handle) {
        s->omx_context->ptr_FreeHandle(s->handle);
        s->handle = NULL;
    }

    omx_deinit(s->omx_context);
    s->omx_context = NULL;
    free(s->in_buffer_headers);
    free(s->out_buffer_headers);
    free(s->free_in_buffers);
    free(s->done_out_buffers);

    pthread_cond_destroy(&s->state_cond);
    pthread_mutex_destroy(&s->state_mutex);
    pthread_cond_destroy(&s->input_cond);
    pthread_mutex_destroy(&s->input_mutex);
    pthread_cond_destroy(&s->output_cond);
    pthread_mutex_destroy(&s->output_mutex);
    av_log(avctx, AV_LOG_INFO, "jpeg_omxcodec decoder finish...\n");
    return 0;
}

static int omx_try_fillbuffer(OMXCodecDecoderContext *s, OMX_BUFFERHEADERTYPE *buffer)
{
    OMX_ERRORTYPE err;
    err = OMX_FillThisBuffer(s->handle, buffer);
    if (err != OMX_ErrorNone) {
        av_log(s->avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
        return AVERROR_UNKNOWN;
    }
    return 0;
}

#define FORMAT_NV12

static int get_data_from_buffer(AVCodecContext *avctx, AVFrame *avframe, OMX_BUFFERHEADERTYPE *buffer)
{
    int ret = 0;
    OMXCodecDecoderContext *s = avctx->priv_data;
    int y_size =  s->width * s->height;
    int uv_size = y_size / 2;

    //uint8_t *y_src = (uint8_t *)(buffer->pBuffer);
    uint8_t *y_src = (uint8_t *)mmap(NULL, y_size * 3 / 2, PROT_READ, MAP_PRIVATE, buffer->pBuffer, 0);
    if (y_src == MAP_FAILED) {
        av_log(s->avctx, AV_LOG_ERROR, "Failed to map fd:%d to y_src.\n", buffer->pBuffer);
        return -1;
    }

    uint8_t *uv_src = y_src + y_size;
    int  outsize = s->width * s->height * 3 / 2;

    avframe->format =  AV_PIX_FMT_NV12;
    avframe->width = s->width;
    avframe->height = s->height;

    avframe->linesize[0] = avframe->width;
    avframe->linesize[1] = avframe->width;
    uint8_t *y_dst = avframe->data[0];
    uint8_t *u_dst = avframe->data[1];
    uint8_t *v_dst = avframe->data[2];

    av_freep(&avframe->data);
    av_freep(&avframe->buf);
    avframe->data[0] = y_src;
    avframe->data[1] = uv_src;
    //memcpy(y_dst, y_src, avframe->width * avframe->height);
    //memcpy(u_dst, uv_src, avframe->width * avframe->height / 2);
    avframe->opaque = buffer;
    avframe->buf[0] = av_buffer_create(avframe->opaque,
                                       sizeof(OMX_BUFFERHEADERTYPE),
                                       omx_try_fillbuffer,
                                       s, AV_BUFFER_FLAG_READONLY);
    return 0;
}



static int omx_decode_image(AVCodecContext *avctx, void *data, int len, AVFrame *frame)
{
    OMXCodecDecoderContext *s = avctx->priv_data;

    int done = 0;
    int in_pos = 0;
    int out_pos = 0;
    int in_index = 0;
    int out_index = 0;
    int ret = 0;
    OMX_BUFFERHEADERTYPE *pBufHeader = NULL;

    while (!done) {
        if ((s->num_free_in_buffers > 0) && (in_pos < len)) {
            pBufHeader = s->in_buffer_headers[in_index];
            in_index++;
            pthread_mutex_lock(&s->input_mutex);
            s->num_free_in_buffers--;
            pthread_mutex_unlock(&s->input_mutex);
            if (in_index >= s->in_buffer_size) {
                in_index = 0;
            }
            pBufHeader->nOffset = 0;
            pBufHeader->nFlags = 0;

            if ((in_pos + pBufHeader->nAllocLen) < len) {
                av_log(s->avctx, AV_LOG_INFO, "total len %d, currnet Filled buffer size %d, still %d left\n", len,
                       pBufHeader->nAllocLen, len - in_pos);
                memcpy(pBufHeader->pBuffer,  data + in_pos, pBufHeader->nAllocLen);
                pBufHeader->nFilledLen = pBufHeader->nAllocLen;
                in_pos +=  pBufHeader->nAllocLen;
            } else {
                memcpy(pBufHeader->pBuffer,  data + in_pos, len - in_pos);
                pBufHeader->nFilledLen = len - in_pos;
                pBufHeader->nFlags = OMX_BUFFERFLAG_EOS;
                pBufHeader->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;
                in_pos = len;
                av_log(s->avctx, AV_LOG_INFO, "input EOS reached\n");
            }
            OMX_EmptyThisBuffer(s->handle, pBufHeader);
        }

        if (s->portSettingChanged) {
            while (s->num_done_out_buffers > 0) {
                pBufHeader = s->out_buffer_headers[out_index];
                out_index++;
                if (out_index >= s->out_buffer_size) {
                    out_index = 0;
                }
                pthread_mutex_lock(&s->output_mutex);
                s->num_done_out_buffers--;
                pthread_mutex_unlock(&s->output_mutex);
                if (pBufHeader->nFilledLen > 0) {
                    done = 1;
                    av_log(s->avctx, AV_LOG_INFO, "Output YUV-NV12 len: %d\n", pBufHeader->nFilledLen);
                }
                if (pBufHeader->nFlags & OMX_BUFFERFLAG_EOS) {
                    done = 1;
                }
                if (pBufHeader->nFilledLen == 0)
                    OMX_FillThisBuffer(s->handle, pBufHeader);
            }
        } else {
            usleep(1);
        }
    }

#ifdef FORMAT_NV12
    avctx->pix_fmt = AV_PIX_FMT_NV12;
#endif
    ret = ff_get_buffer(avctx, frame, AV_GET_BUFFER_FLAG_REF);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "ff_get_buffer failed: %x\n", ret);
        return ret;
    }
    ret = get_data_from_buffer(avctx, frame, pBufHeader);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "get data from buffer failed: %x\n", ret);
        return ret;
    }
    return 0;
}

static int omx_decode_rec_frame(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
    OMXCodecDecoderContext *s = avctx->priv_data;
    AVFrame *frame = data;
    int ret = 0;
    int filelen = avpkt->size;
    *got_frame = 0;
    if (filelen) {
        if (avpkt->data[158] == 0xff && avpkt->data[159] == 0xc2) {
            av_log(s->avctx, AV_LOG_ERROR, "error: decoder not support progressive jpeg.\n");
            return AVERROR_EOF;
        }
        omx_decode_init(avctx, filelen);
        omx_decode_image(avctx, avpkt->data, filelen, frame);
        *got_frame = 1;
        return AVERROR(EAGAIN);
    }
    return AVERROR_EOF;
}


const AVCodec ff_jpeg_omx_decoder = {
    .name           = "jpeg_omx",
    .long_name      = NULL_IF_CONFIG_SMALL("OpenMAX IL decoder"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MJPEG,
    .priv_data_size = sizeof(OMXCodecDecoderContext),
    .init           = omx_decode_init2,
    .decode         = omx_decode_rec_frame,
    .close          = omx_decode_uninit,
    .capabilities   = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_SKIP_FRAME_FILL_PARAM,
};
