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

#include "config.h"
#include <dlfcn.h>
#include <OMX_Core.h>
#include <OMX_Component.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#include <pthread.h>

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

#include "avcodec.h"
#include "decode.h"
#include "h264_parse.h"
#include "h264_ps.h"
#include "hevc_parse.h"
#include "hwconfig.h"
#include "internal.h"

#include "avcodec.h"
#include "h264.h"
#include "libswscale/swscale.h"

#include "vsi_vendor_ext.h"
static int omx_load_count = 0;
#define kNumPictureBuffers 2

#ifdef c920v
#define __riscv
#define __riscv_vector
#include "riscv_vector.h"
#endif
typedef enum __DecoderStatus {
    INITIALIZING,
    RESETTING,
    DESTROYING,
    ERRORING,
    PortSettingFinished,
    ILLEGAL_STATE
} DecoderStatus;


#define to_omx_ticks(x) (x)
#define from_omx_ticks(x) (x)


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
    av_log(logctx, AV_LOG_INFO, "omx init libname %s\n", libname);

    s->lib = dlopen(libname, RTLD_NOW | RTLD_GLOBAL);
    if (!s->lib) {
        av_log(logctx, AV_LOG_WARNING, "%s not found\n", libname);
        return AVERROR_DECODER_NOT_FOUND;
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
        return AVERROR_DECODER_NOT_FOUND;
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
        //omx_env unready
        return 0;
    }

    if (omx_context->ptr_ComponentNameEnum == NULL) {
        //ptr_ComponentNameEnum unready
        return 0;

    }
    numComps = 0;
    while (err == OMX_ErrorNone) {
        err = omx_context->ptr_ComponentNameEnum(component, OMX_MAX_STRINGNAME_SIZE, numComps);
        if (err == OMX_ErrorNone) {
            OMX_U32 numberofroles = 0;
            omx_context->ptr_GetRolesOfComponent(component, &numberofroles, NULL);

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

static av_cold OMXContext *omx_init(void *logctx, const char *libname, const char *prefix)
{
    static const char *const libnames[] = {
#if 0
        "/opt/vc/lib/libopenmaxil.so", "/opt/vc/lib/libbcm_host.so",
#else
        "libomxil-bellagio.so.0", NULL,
        "libOMX_Core.so", NULL,
        "libOmxCore.so", NULL,
#endif
        NULL
    };
    const char *const *nameptr;
    int ret = AVERROR_DECODER_NOT_FOUND;
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
    print_omx_env(omx_context);
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

typedef struct OMXCodecDecoderContext {
    const AVClass *class;
    char *libname;
    char *libprefix;
    OMXContext *omx_context;

    AVCodecContext *avctx;
    //OmxCodecDecContext *ctx;

    char component_name[OMX_MAX_STRINGNAME_SIZE];
    OMX_VERSIONTYPE version;
    OMX_HANDLETYPE handle;
    int in_port, out_port;
    //OMX_COLOR_FORMATTYPE color_format;
    int stride, plane_size;

    int num_in_buffers, num_out_buffers;
    int in_buffer_size, out_buffer_size;
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

    pthread_mutex_t eof_mutex;
    pthread_cond_t eof_cond;

    OMX_STATETYPE state;
    OMX_ERRORTYPE error;

    unsigned mutex_cond_inited_cnt;

    int eos_sent, got_eos, draining, need_sendeos, eos_reach;

    int input_zerocopy;
    int profile;
    //for receiving frame
    int flushing;

    int delay_flush;
    int fast_render;
    int mirror;
    int rotation;/*0 ~3 means 0 90 180 270 degree*/
    atomic_int serial;
    int first_pkt;
    AVPacket buffered_pkt;
    int out_stride, out_slice_height;
    int crop_top, crop_left;

    int format_changed;
    int input_count;
    int output_count;
    DecoderStatus status;
    pthread_t output_tid;
    //AVThread *thread_out;
    int portSettingidle;
    int reconfigPending;
    int outport_disabled;
    int pkt_full;
    int sent_pkt_num;
    int offscreen;
    int now_pts;
    int pkt_duration;
    int resolution_changed;
    int output_width;
    int output_height;
    OMX_BUFFERHEADERTYPE *temp_buffer;

    OMX_COLOR_FORMATTYPE outformat;
    //AsyncQueue *pic_queue;
} OMXCodecDecoderContext;

#define NB_MUTEX_CONDS 8
#define OFF(field) offsetof(OMXCodecDecoderContext, field)

static void append_buffer(pthread_mutex_t *mutex, pthread_cond_t *cond,
                          int *array_size, OMX_BUFFERHEADERTYPE **array,
                          OMX_BUFFERHEADERTYPE *buffer)
{
    pthread_mutex_lock(mutex);
    array[(*array_size)++] = buffer;
    pthread_cond_broadcast(cond);
    pthread_mutex_unlock(mutex);
}

/*0 ,return imediately, -1 wait buffer return, else such as 100, mean timeout is 100ms */
static OMX_BUFFERHEADERTYPE *get_buffer(pthread_mutex_t *mutex, pthread_cond_t *cond,
                                        int *array_size, OMX_BUFFERHEADERTYPE ***array,
                                        int timeOut)
{
    struct timeval  start, end;
    long int elapse;
    OMX_BUFFERHEADERTYPE *buffer;
    if (timeOut != 0) {
        gettimeofday(&start, NULL);
        while (!*array_size) {
            if (timeOut > 0) {
                gettimeofday(&end, NULL);
                elapse = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000;
                if (elapse > timeOut) {
                    break;
                }
            } else if (timeOut == -1) {
                break;
            }
        }
    }

    if (*array_size > 0) {
        pthread_mutex_lock(mutex);
        buffer = *array[0];
        (*array_size)--;
        memmove(&(*array)[0], &(*array)[1], (*array_size) * sizeof(OMX_BUFFERHEADERTYPE *));
        pthread_mutex_unlock(mutex);
    } else {
        buffer = NULL;
    }
    return buffer;
}

static void checkstate(OMXCodecDecoderContext *s, OMX_U32 state)
{
    switch (state) {
    case OMX_StateMax:
        av_log(s->avctx, AV_LOG_WARNING, "OMX_StateMax\n");
        break;
    case OMX_StateLoaded:
        av_log(s->avctx, AV_LOG_WARNING, "OMX_StateLoaded\n");
        break;
    case OMX_StateIdle:
        av_log(s->avctx, AV_LOG_WARNING, "OMX_StateIdle\n");
        break;
    case OMX_StateExecuting:
        av_log(s->avctx, AV_LOG_WARNING, "OMX_StateExecuting\n");
        break;
    case OMX_StatePause:
        av_log(s->avctx, AV_LOG_WARNING, "OMX_StatePause\n");
        break;
    case OMX_StateWaitForResources:
        av_log(s->avctx, AV_LOG_WARNING, "OMX_StateWaitForResources\n");
        break;
    }
}

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
            av_log(s->avctx, AV_LOG_ERROR, "Failed to allocate memory\n");
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
            av_log(s->avctx, AV_LOG_ERROR, "Failed to allocate memory\n");
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

static av_cold int wait_for_state(OMXCodecDecoderContext *s, OMX_STATETYPE state);

static void OnOutputPortEnabled(OMXCodecDecoderContext *s)
{
    av_log(s->avctx, AV_LOG_WARNING, "OnOutputPortEnabled\n");
    s->outport_disabled = 0;
    s->reconfigPending = 0;
}

static int OnOutputPortDisabled(OMXCodecDecoderContext *s)
{
    int i;
    AVCodecContext *avctx = s->avctx;
    OMX_ERRORTYPE err;
    OMX_PARAM_PORTDEFINITIONTYPE out_port_params;
    av_log(s->avctx, AV_LOG_WARNING, "OnOutputPortDisabled\n");
    if (s->reconfigPending) {
        INIT_STRUCT(out_port_params);
        out_port_params.nPortIndex = s->out_port;
        OMX_GetParameter(
            s->handle, OMX_IndexParamPortDefinition, &out_port_params);

        pthread_mutex_lock(&s->output_mutex);
        s->num_out_buffers = out_port_params.nBufferCountMin + 5;
        out_port_params.nBufferCountActual = out_port_params.nBufferCountMin + 5;

        err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
        CHECK(err);

        s->out_buffer_headers = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE *) * s->num_out_buffers);
        s->done_out_buffers   = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE *) * s->num_out_buffers);
        if (!s->out_buffer_headers || !s->done_out_buffers) {
            s->num_done_out_buffers = 0;
            pthread_mutex_unlock(&s->output_mutex);
            av_log(s->avctx, AV_LOG_ERROR, "Something wrong with out_buffers.\n");
            return AVERROR(ENOMEM);
        }
        for (i = 0; i < s->num_out_buffers ; i++) {
            err = OMX_AllocateBuffer(s->handle, &s->out_buffer_headers[i], s->out_port, s, out_port_params.nBufferSize);
            s->out_buffer_headers[i]->pAppPrivate = s->out_buffer_headers[i]->pOutputPortPrivate = NULL;
            s->out_buffer_headers[i]->pAppPrivate = NULL;
            s->out_buffer_headers[i]->nTimeStamp = -1;
            s->out_buffer_headers[i]->nOutputPortIndex =  s->out_port;
            CHECK(err);
        }

        s->num_out_buffers = i;
        s->num_done_out_buffers = 0;
        for (i = 0; i < s->num_out_buffers && err == OMX_ErrorNone; i++) {
            err = OMX_FillThisBuffer(s->handle, s->out_buffer_headers[i]);
        }

        if (err != OMX_ErrorNone) {
            av_log(avctx, AV_LOG_WARNING, "FillOutBuffer failed , so set header\n");
            for (i = 0; i < s->num_out_buffers; i++) {
                s->done_out_buffers[s->num_done_out_buffers++] = s->out_buffer_headers[i];
            }
        }

        av_log(s->avctx, AV_LOG_WARNING, "Send OMX_CommandPortEnable\n");
        err = OMX_SendCommand(s->handle, OMX_CommandPortEnable, s->out_port, NULL);
        CHECK(err);
        pthread_mutex_unlock(&s->output_mutex);
    }
    return 0;
}

static int onPortSettingChanged(OMXCodecDecoderContext *s, OMX_PARAM_PORTDEFINITIONTYPE *out_port_params)
{
    AVCodecContext *avctx = s->avctx;

    av_log(s->avctx, AV_LOG_WARNING, "onPortSettingChanged\n");

    //if ((out_port_params->nBufferCountActual < out_port_params->nBufferCountMin)
    //    || (out_port_params->nBufferSize > s->out_buffer_size)) {
    OMX_ERRORTYPE err;
    int i = 0;
    s->outport_disabled = 1;
    av_log(s->avctx, AV_LOG_WARNING, "nBufferCountActual modify!\n");
    err = OMX_SendCommand(s->handle, OMX_CommandPortDisable, s->out_port, NULL);
    CHECK(err);
    s->reconfigPending = 1;
    pthread_mutex_lock(&s->output_mutex);
    if (&s->out_buffer_headers) {
        av_freep(&s->out_buffer_headers);
    }
    if (&s->done_out_buffers) {
        av_freep(&s->done_out_buffers);
    }
    s->num_out_buffers = 0;
    s->num_done_out_buffers = 0;
    pthread_mutex_unlock(&s->output_mutex);
    //} else {
    //    OMX_ERRORTYPE err;
    //    err = OMX_SendCommand(s->handle, OMX_CommandPortEnable, s->out_port, NULL);
    //    CHECK(err);
    //}

    return 0;
}

static int onIdleState(OMXCodecDecoderContext *s)
{
    if (s->portSettingidle == 1) {
        av_log(s->avctx, AV_LOG_ERROR, "onIdleState after portSettingidle\n");

        if (OMX_ErrorNone != OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateExecuting, NULL)) {
            av_log(s->avctx, AV_LOG_ERROR, "unable to set OMX_StateExecuting state\n");
        }

        s->portSettingidle = 0;
    }

    return  0;
}

static OMX_ERRORTYPE event_handler(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_EVENTTYPE event,
                                   OMX_U32 data1, OMX_U32 data2, OMX_PTR event_data)
{
    OMXCodecDecoderContext *s = app_data;
    // This uses casts in the printfs, since OMX_U32 actually is a typedef for
    // unsigned long in official header versions (but there are also modified
    // versions where it is something else).
    //av_log(s->avctx, AV_LOG_WARNING, "event_handler OMXCodecDecoderContext %p\n", s);
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
            av_log(s->avctx, AV_LOG_ERROR, "OMX error %"PRIx32"\n", (uint32_t) data1);
            break;
        }
        return OMX_ErrorNone;
    case OMX_EventCmdComplete:
        if (data1 == OMX_CommandStateSet) {
            pthread_mutex_lock(&s->state_mutex);
            s->state = data2;
            checkstate(s, data2);
            if (data2 == OMX_StateIdle) {
                onIdleState(s);
            }

            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else if (data1 == OMX_CommandPortDisable) {
            av_log(s->avctx, AV_LOG_INFO, "OMX port %"PRIu32" disabled\n", (uint32_t) data2);
            if (data2 == s->out_port) {
                OnOutputPortDisabled(s);
            }
        } else if (data1 == OMX_CommandPortEnable) {
            av_log(s->avctx, AV_LOG_INFO, "OMX port %"PRIu32" enabled\n", (uint32_t) data2);
            if (data2 == s->out_port) {
                OnOutputPortEnabled(s);
            }
        } else if (data1 == OMX_CommandFlush) {
            av_log(s->avctx, AV_LOG_WARNING, "OMX port %"PRIu32" flushed\n", (uint32_t) data2);
        } else {
            av_log(s->avctx, AV_LOG_WARNING, "OMX command complete, command %"PRIu32", value %"PRIu32"\n",
                   (uint32_t) data1, (uint32_t) data2);
        }
        break;
    case OMX_EventPortSettingsChanged:
        if ((int)data1 == OMX_DirOutput) { //out is OMX_DirOutput
            OMX_PARAM_PORTDEFINITIONTYPE out_port_params;
            av_log(s->avctx, AV_LOG_INFO, "OMX outport settings changed: out_port: %d\n", s->out_port);

            INIT_STRUCT(out_port_params);
            out_port_params.nPortIndex = s->out_port;
            OMX_GetParameter(
                s->handle, OMX_IndexParamPortDefinition, &out_port_params);
            dump_portdef(s, &out_port_params);
            s->outformat = out_port_params.format.video.eColorFormat ;

            s->out_stride       = out_port_params.format.video.nStride;
            s->out_slice_height = out_port_params.format.video.nSliceHeight;

            onPortSettingChanged(s, &out_port_params);

        } else if (data1 == s->out_port && data2 == OMX_IndexConfigCommonOutputCrop) {
            // TODO: Handle video crop rect.
        } else if (data1 == s->out_port && data2 == OMX_IndexConfigCommonScale) {
            // TODO: Handle video SAR change.
        } else {
            av_log(s->avctx, AV_LOG_WARNING, "error event \n");
        }

        break;
    case OMX_EventBufferFlag:
        if (data1 == s->out_port) {
            //
        }
        break;
    default:
        av_log(s->avctx, AV_LOG_WARNING, "OMX event %d %"PRIx32" %"PRIx32"\n",
               event, (uint32_t) data1, (uint32_t) data2);
        break;
    }
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE empty_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data,
                                       OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCodecDecoderContext *s = app_data;
    append_buffer(&s->input_mutex, &s->input_cond,
                  &s->num_free_in_buffers, s->free_in_buffers, buffer);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE fill_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data,
                                      OMX_BUFFERHEADERTYPE *buffer)
{
    OMX_ERRORTYPE err;
    OMXCodecDecoderContext *s = app_data;
    if (!buffer->nFilledLen) {
        int i = 0;
        for (i = 0; i < s->num_out_buffers; i++) {
            if (s->out_buffer_headers[i] == buffer && s->out_buffer_headers[i]) {
                break;
            }
        }

        if (i == s->num_out_buffers) {
            OMX_FreeBuffer(s->handle, 1, buffer);
            return OMX_ErrorNone;
        }
    }
    append_buffer(&s->output_mutex, &s->output_cond,
                  &s->num_done_out_buffers, s->done_out_buffers, buffer);


    return OMX_ErrorNone;
}


static const OMX_CALLBACKTYPE decoder_callbacks = {
    event_handler,
    empty_buffer_done,
    fill_buffer_done
};

static av_cold int omx_dec_find_component(OMXContext *omx_context, void *logctx,
        const char *role, char *str, int str_size)
{
    OMX_U32 i, num = 0;
    char **components;
    int ret = 0;

    omx_context->ptr_GetComponentsOfRole((OMX_STRING) role, &num, NULL);
    if (!num) {
        av_log(logctx, AV_LOG_WARNING, "No component for role %s found\n", role);
        return AVERROR_DECODER_NOT_FOUND;
    }
    components = av_calloc(num, sizeof(*components));
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

static av_cold int wait_for_eof(OMXCodecDecoderContext *s)
{
    int ret = 0;
    pthread_mutex_lock(&s->eof_mutex);
    while (s->got_eos != 1)
        pthread_cond_wait(&s->eof_cond, &s->eof_mutex);

    pthread_mutex_unlock(&s->eof_mutex);
    return ret;
}

static int omx_send_extradata(AVCodecContext *avctx)
{
    OMX_BUFFERHEADERTYPE *buffer;
    OMX_ERRORTYPE err;
    if (avctx->extradata_size > 0) {
        int64_t timeout = 60;
        OMXCodecDecoderContext *s = avctx->priv_data;

        buffer = get_buffer(&s->input_mutex, &s->input_cond,
                            &s->num_free_in_buffers, &s->free_in_buffers, timeout);
        buffer->nFilledLen = avctx->extradata_size;
        buffer->nFlags = OMX_BUFFERFLAG_EXTRADATA;
        buffer->nOffset = 0;
        memcpy(buffer->pBuffer, avctx->extradata, avctx->extradata_size);
        err = OMX_EmptyThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->input_mutex, &s->input_cond, &s->num_free_in_buffers, s->free_in_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "omx_send_extradata OMX_EmptyThisBuffer failed: %x\n", err);
            return -1;
        }

        av_log(avctx, AV_LOG_INFO, "omx_send_extradata finished: %x\n");
    }
    return 0;
}

#define FORMAT_NV12

static void copyNV12toDst(OMXCodecDecoderContext *s, AVFrame *avframe, OMX_BUFFERHEADERTYPE *buffer)
{
    avframe->width = (OMX_U32) s->avctx->width;
    avframe->height = (OMX_U32) s->avctx->height;
    avframe->linesize[0] = avframe->width;
    avframe->linesize[1] = avframe->width;
    int y_size = avframe->linesize[0] * avframe->height;
    int uv_size = y_size / 2;
    int src_stride = s->out_stride;
    uint8_t *y_src = (uint8_t *)(buffer->pBuffer);
    uint8_t *y_dst = avframe->data[0];
    uint8_t *uv_src = (uint8_t *)(buffer->pBuffer) + y_size;
    uint8_t *uv_dst = avframe->data[1];
    if (s->offscreen && !s->resolution_changed) {
        avframe->data[0] = y_src;
        avframe->data[1] = uv_src;
    } else if (s->resolution_changed) {
        for (int i = 0; i < avframe->height; i++) {
            memcpy(y_dst + i * avframe->linesize[0], y_src + i * src_stride, avframe->linesize[0]);
        }
        for (int i = 0; i < avframe->height / 2; i++) {
            memcpy(uv_dst + i * avframe->linesize[1], y_src + src_stride * avframe->height + i * src_stride, avframe->linesize[1]);
        }
    } else {
        memcpy(y_dst, y_src, avframe->width *  avframe->height);
        memcpy(uv_dst, uv_src, uv_size);
    }
}

static void convertNV12toYUV420(OMXCodecDecoderContext *s, AVFrame *avframe, OMX_BUFFERHEADERTYPE *buffer)
{
#ifdef __OMX_ENABLE_SWCALE
    const uint8_t *src_data[3];
    int srclinesize[3];

    srclinesize[0] = avframe->width;
    srclinesize[1] = avframe->width;
    srclinesize[2] = 0;

    src_data[0] = (uint8_t *)(buffer->pBuffer);
    src_data[1] = (uint8_t *)(buffer->pBuffer) + (avframe->width *  avframe->height) ;
    src_data[2] = 0;

    if (m_pSwsCtx == NULL) {
        m_pSwsCtx = sws_getContext(avctx->width,
                                   avctx->height,
                                   AV_PIX_FMT_NV12,
                                   avctx->width,
                                   avctx->height,
                                   AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    }

    if (m_pSwsCtx == NULL) {
        printf("Error converting\n");
    } else {
        sws_scale(m_pSwsCtx, src_data, srclinesize, 0,  avframe->height, avframe->data, avframe->linesize);
    }
#else
    avframe->width = (OMX_U32) s->avctx->width;
    avframe->height = (OMX_U32) s->avctx->height;
    avframe->linesize[0] = avframe->width;
    avframe->linesize[1] = avframe->width / 2;
    avframe->linesize[2] = avframe->width / 2;
    uint8_t *y_src = (uint8_t *)(buffer->pBuffer);
    uint8_t *y_dst = avframe->data[0];

    uint8_t *uv_src, *u_dst, *v_dst;
    int src_stride = s->out_stride;
    int y_src_size =  s->out_stride *  s->out_slice_height;
    uint8_t *uv_src_start = (uint8_t *)(buffer->pBuffer) + y_src_size;

    for (int i = 0; i < avframe->height; i++) {
        memcpy(y_dst + i * avframe->linesize[0], y_src + i * src_stride, src_stride);
    }


    for (int i = 0; i < avframe->height / 2; i++) {
        u_dst = avframe->data[1] + i * avframe->linesize[1];
        v_dst = avframe->data[2] + i * avframe->linesize[2];
        uv_src = uv_src_start + i *  src_stride ;

        for (int j = 0; j < (avframe->width + 1) / 2; j++) {
            *(u_dst++) = *(uv_src++);
            *(v_dst++) = *(uv_src++);
        }
    }
#endif
}

static int omx_try_filltempbuffer(AVCodecContext *avctx)
{
    OMXCodecDecoderContext *s = avctx->priv_data;
    OMX_ERRORTYPE err;
    if (s->offscreen) {
        if (s->temp_buffer) {
            err = OMX_FillThisBuffer(s->handle, s->temp_buffer);
            if (err != OMX_ErrorNone) {
                append_buffer(&s->output_mutex, &s->output_cond, &s->num_done_out_buffers, s->done_out_buffers, s->temp_buffer);
                av_log(avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
            }
            s->temp_buffer = NULL;
        }
    }
    return 0;
}

static void free_frame_buffer(void *opaque, uint8_t *data)
{
    OMXCodecDecoderContext *s = opaque;
    AVCodecContext *avctx = s->avctx;

    if (s->offscreen) {
        omx_try_filltempbuffer(avctx);
    } else {
        if (data != NULL) {
            av_free(data);
        }
    }
}

static int check_buffer_outsize(OMXCodecDecoderContext *s, OMX_BUFFERHEADERTYPE *buffer)
{
    OMX_ERRORTYPE err;
    AVCodecContext *avctx = s->avctx;
    OMX_PARAM_PORTDEFINITIONTYPE out_port_params = {0};
    INIT_STRUCT(out_port_params);
    out_port_params.nPortIndex = s->out_port;
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);
    if (avctx->width != out_port_params.format.video.nFrameWidth ||
        avctx->height != out_port_params.format.video.nFrameHeight) {
        avctx->width = out_port_params.format.video.nFrameWidth;
        avctx->height = out_port_params.format.video.nFrameHeight;
        avctx->coded_width = out_port_params.format.video.nFrameWidth;
        avctx->coded_height = out_port_params.format.video.nFrameHeight;
        s->resolution_changed = 1;
    }
    return 0;
}

static int ff_omx_dec_receive(AVCodecContext *avctx, OMXCodecDecoderContext *s,
                              AVFrame *avframe, bool wait)
{
    OMX_ERRORTYPE err;
    int ret;
    if (s->draining && s->got_eos) {
        return AVERROR_EOF;
    }

    if (s->offscreen) {
        if (s->temp_buffer) {
            err = OMX_FillThisBuffer(s->handle, s->temp_buffer);
            if (err != OMX_ErrorNone) {
                append_buffer(&s->output_mutex, &s->output_cond, &s->num_done_out_buffers, s->done_out_buffers, s->temp_buffer);
                av_log(avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
            }
            s->temp_buffer = NULL;
        }
    }
    OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                   &s->num_done_out_buffers, &s->done_out_buffers,
                                   -1);
    if (!buffer) {
        //omx_outputbuffer_thread  error
        return AVERROR(EAGAIN);
    }

    if (buffer->nFlags & OMX_BUFFERFLAG_EOS) {
        av_log(avctx, AV_LOG_WARNING, "OMX_BUFFERFLAG_EOS reached\n");

        pthread_mutex_lock(&s->eof_mutex);
        s->got_eos = 1;
        pthread_cond_broadcast(&s->eof_cond);
        pthread_mutex_unlock(&s->eof_mutex);
    }

    if (buffer->nFilledLen == 0) {
        err = OMX_FillThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->output_mutex, &s->output_cond, &s->num_done_out_buffers, s->done_out_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
        }
        return AVERROR(EAGAIN);
    }

    s->output_count++;
    check_buffer_outsize(s, buffer);
#ifdef FORMAT_NV12
    avctx->pix_fmt = AV_PIX_FMT_NV12;
#endif
    ret = ff_get_buffer(avctx, avframe, AV_GET_BUFFER_FLAG_REF);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "ff_get_buffer failed: %x\n", ret);
        return ret;
    }

    if (buffer->nFlags == OMX_BUFFERFLAG_SYNCFRAME) {
        avframe->flags |= AV_PICTURE_TYPE_I;
    }
    avframe->pts = av_rescale_q(from_omx_ticks(buffer->nTimeStamp), AV_TIME_BASE_Q, avctx->time_base);
    avframe->pkt_dts = AV_NOPTS_VALUE;
    if (avframe->pts <= 0 && s->pkt_duration) {
        avframe->pts = s->now_pts;
        s->now_pts += s->pkt_duration;
    }

#ifdef FORMAT_NV12
    copyNV12toDst(s, avframe, buffer);
#else
    convertNV12toYUV420(s, avframe, buffer);
#endif

    if (s->offscreen) {
        s->temp_buffer = buffer;
    } else {
        err = OMX_FillThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->output_mutex, &s->output_cond, &s->num_done_out_buffers, s->done_out_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
        }
    }
    //OMX_FillThisBuffer finished
    return 0;
}

static av_cold int omx_component_init_decoder(AVCodecContext *avctx, const char *role)
{
    OMXCodecDecoderContext *s = avctx->priv_data;
    OMX_PARAM_COMPONENTROLETYPE role_params = { 0 };
    OMX_PORT_PARAM_TYPE video_port_params = { 0 };
    OMX_PARAM_PORTDEFINITIONTYPE in_port_params = { 0 }, out_port_params = { 0 };
    OMX_VIDEO_PARAM_PORTFORMATTYPE formatIn = {0};

    OMX_ERRORTYPE err;
    int i;

    s->version.s.nVersionMajor = 1;
    s->version.s.nVersionMinor = 1;
    s->version.s.nRevision     = 2;
    s->input_count  = 0;
    s->portSettingidle = 0;

    s->crop_left = 0;
    s->crop_top = 0;
    s->status = INITIALIZING;
#if 1
    //set component_name OMX.hantro.VC8000D.video.decoder for test
    av_log(avctx, AV_LOG_INFO, "OMX_GetHandle with component name %s \n", s->component_name);

#endif
    err = s->omx_context->ptr_GetHandle(&s->handle, s->component_name, s, (OMX_CALLBACKTYPE *) &decoder_callbacks);
    if (err != OMX_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "OMX_GetHandle(%s) failed: %x\n", s->component_name, err);
        return AVERROR_UNKNOWN;
    }

    // This one crashes the mediaserver on qcom, if used over IOMX
    INIT_STRUCT(role_params);
    av_strlcpy(role_params.cRole, role, sizeof(role_params.cRole));
    // Intentionally ignore errors on this one
    OMX_SetParameter(s->handle, OMX_IndexParamStandardComponentRole, &role_params);

#if 1
    if (s->fast_render) {
        if (avctx->codec->id == AV_CODEC_ID_H264) {
            OMX_VIDEO_PARAM_VIDEOFASTUPDATETYPE  codec_data;
            OMX_INDEXTYPE index = OMX_IndexParamVideoFastUpdate;
            av_log(avctx, AV_LOG_WARNING, "fast_render enabled");

            INIT_STRUCT(codec_data);
            codec_data.bEnableVFU = OMX_TRUE;
            codec_data.nFirstGOB = 0;
            codec_data.nFirstMB  = 0;
            codec_data.nNumMBs   = 32;

            err = OMX_SetConfig(s->handle, index, &codec_data);
            if (err != OMX_ErrorNone) {
                av_log(avctx, AV_LOG_WARNING, "OMX_IndexParamVideoFastUpdate not support yet");
            }
        }

    }

    if (s->mirror) {
        OMX_CONFIG_MIRRORTYPE  mirrordata;
        OMX_INDEXTYPE index = OMX_IndexConfigCommonMirror;
        av_log(avctx, AV_LOG_WARNING, "mirror enabled %d", s->mirror);

        INIT_STRUCT(mirrordata);
        if (s->mirror == 1)
            mirrordata.eMirror = OMX_MirrorVertical;
        else
            mirrordata.eMirror = OMX_MirrorHorizontal;

        err = OMX_SetConfig(s->handle, index, &mirrordata);
        if (err != OMX_ErrorNone) {
            av_log(avctx, AV_LOG_WARNING, "OMX_IndexConfigCommonMirror not support yet");
        }
    }


    if (s->rotation != 0) {

        OMX_CONFIG_ROTATIONTYPE rot;
        OMX_INDEXTYPE index = OMX_IndexConfigCommonRotate;
        av_log(avctx, AV_LOG_WARNING, "rotation enabled %d", s->rotation);

        INIT_STRUCT(rot);

        err = OMX_SetConfig(s->handle, index, &rot);
        if (err != OMX_ErrorNone) {
            av_log(avctx, AV_LOG_WARNING, "OMX_IndexConfigCommonRotate not support yet");
        }
    }
#endif

    INIT_STRUCT(video_port_params);
    err = OMX_GetParameter(s->handle, OMX_IndexParamVideoInit, &video_port_params);
    CHECK(err);

    s->in_port = s->out_port = -1;
    s->format_changed = 0;
    s->reconfigPending = 0;
    s->outport_disabled = 0;
    s->pkt_full = 0;
    s->sent_pkt_num = 0;
    s->temp_buffer = NULL;
    s->now_pts = 0;
    s->resolution_changed = 0;
    if (avctx->framerate.num) {
        s->pkt_duration = abs(avctx->pkt_timebase.den / avctx->framerate.num);
    } else {
        s->pkt_duration = 0;
    }
    if (s->output_width)
        avctx->width = s->output_width;
    if (s->output_height)
        avctx->height = s->output_height;

    INIT_STRUCT(in_port_params);
    in_port_params.nPortIndex =  s->in_port = OMX_DirInput;
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);

    INIT_STRUCT(out_port_params);
    out_port_params.nPortIndex = s->out_port = OMX_DirOutput;
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);

    in_port_params.format.video.nFrameWidth  = (OMX_U32) avctx->width;
    in_port_params.format.video.nFrameHeight = (OMX_U32) avctx->height;
    in_port_params.format.video.nStride      = (OMX_U32) avctx->width;
    in_port_params.format.video.nSliceHeight = (OMX_U32) avctx->height;
    in_port_params.nBufferCountActual    = kNumPictureBuffers;
    in_port_params.nBufferCountMin       = kNumPictureBuffers;


    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);

    out_port_params.format.video.nFrameWidth   = avctx->width;
    out_port_params.format.video.nFrameHeight  = avctx->height;
    out_port_params.format.video.nStride      = (OMX_U32) avctx->width;
    out_port_params.format.video.nSliceHeight = (OMX_U32) avctx->height;
    out_port_params.nBufferCountActual   = kNumPictureBuffers;
    out_port_params.nBufferCountMin      = kNumPictureBuffers;
    out_port_params.nBufferSize = (OMX_U32) avctx->width * (OMX_U32) avctx->height * 3;
    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);

    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);

    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);

    s->stride         = in_port_params.format.video.nStride;
    s->plane_size     = in_port_params.format.video.nSliceHeight;
    s->num_in_buffers = in_port_params.nBufferCountMin;
    s->in_buffer_size = in_port_params.nBufferSize;
    s->num_out_buffers = out_port_params.nBufferCountMin + 5;
    s->out_stride       = out_port_params.format.video.nStride;
    s->out_slice_height = out_port_params.format.video.nSliceHeight;

    in_port_params.nBufferCountActual = in_port_params.nBufferCountMin;
    out_port_params.nBufferCountActual = out_port_params.nBufferCountMin + 5;

    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);
    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);

    dump_portdef(s, &in_port_params);
    dump_portdef(s, &out_port_params);

    INIT_STRUCT(formatIn);
    formatIn.nPortIndex = s->in_port;
    err = OMX_GetParameter(s->handle, OMX_IndexParamVideoPortFormat, &formatIn);

    switch (avctx->codec->id) {
    case AV_CODEC_ID_MPEG4:
        formatIn.eCompressionFormat = OMX_VIDEO_CodingMPEG4;
        break;
    case AV_CODEC_ID_H264:
        formatIn.eCompressionFormat = OMX_VIDEO_CodingAVC;
        break;
    case AV_CODEC_ID_RV10:
    case AV_CODEC_ID_RV20:
    case AV_CODEC_ID_RV30:
        formatIn.eCompressionFormat = OMX_VIDEO_CodingAVC;
        break;

    case AV_CODEC_ID_HEVC:
        formatIn.eCompressionFormat = OMX_VIDEO_CodingHEVC;
        break;
    case AV_CODEC_ID_VP9:
        formatIn.eCompressionFormat = OMX_VIDEO_CodingVP9;
        break;
    default:
        formatIn.eCompressionFormat = OMX_VIDEO_CodingAutoDetect;
        break;

    }

    formatIn.eColorFormat = OMX_COLOR_FormatUnused;
    formatIn.xFramerate = (OMX_U32)av_q2d(avctx->framerate);
    err = OMX_SetParameter(s->handle, OMX_IndexParamVideoPortFormat, &formatIn);

    if (OMX_ErrorNone != OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL)) {
        av_log(avctx, AV_LOG_ERROR, "Unable to set IDLE state\n");
    }

    //allocate input buffers
    s->in_buffer_headers  = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE *) * s->num_in_buffers);
    s->free_in_buffers    = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE *) * s->num_in_buffers);
    if (!s->in_buffer_headers || !s->free_in_buffers)
        return AVERROR(ENOMEM);

    for (i = 0; i < s->num_in_buffers && err == OMX_ErrorNone; i++) {
        err = OMX_AllocateBuffer(s->handle, &s->in_buffer_headers[i],  s->in_port,  s, in_port_params.nBufferSize);
        if (err == OMX_ErrorNone) {
            s->in_buffer_headers[i]->nInputPortIndex = s->in_port;
            s->in_buffer_headers[i]->nOffset = 0;
            s->in_buffer_headers[i]->nFlags  = 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "OMX_AllocateBuffer for input[%d] failed\n", i);
        }
    }

    CHECK(err);
    s->num_in_buffers = i;
    for (i = 0; i < s->num_in_buffers; i++) {
        s->free_in_buffers[i] = s->in_buffer_headers[i];
    }
    s->num_free_in_buffers =  s->num_in_buffers;
    av_log(avctx, AV_LOG_INFO, "OMX_AllocateBuffer for inputs %d finished\n", s->num_free_in_buffers);

    //allocate output buffers
    s->out_buffer_headers = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE *) * s->num_out_buffers);
    s->done_out_buffers   = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE *) * s->num_out_buffers);
    if (!s->out_buffer_headers || !s->done_out_buffers)
        return AVERROR(ENOMEM);
    for (i = 0; i < s->num_out_buffers && err == OMX_ErrorNone; i++) {
        err = OMX_AllocateBuffer(s->handle, &s->out_buffer_headers[i], s->out_port, s, out_port_params.nBufferSize);
        s->out_buffer_headers[i]->pAppPrivate = s->out_buffer_headers[i]->pOutputPortPrivate = NULL;
        s->out_buffer_headers[i]->pAppPrivate = NULL;
        s->out_buffer_headers[i]->nTimeStamp = -1;
        s->out_buffer_headers[i]->nOutputPortIndex =  s->out_port;
        CHECK(err);
    }

    s->num_out_buffers = i;

#if 0
    //check if port enabled
    if (in_port_params.bEnabled == OMX_FALSE) {
        OMX_SendCommand(s->handle, OMX_CommandPortEnable, OMX_DirInput, NULL);
    }

    if (out_port_params.bEnabled == OMX_FALSE) {
        OMX_SendCommand(s->handle, OMX_CommandPortEnable, OMX_DirOutput, NULL);
    }
#endif

    if (wait_for_state(s, OMX_StateIdle) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Didn't get OMX_StateIdle\n");
        return AVERROR_UNKNOWN;
    }

    err = OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
    CHECK(err);
    if (wait_for_state(s, OMX_StateExecuting) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Didn't get OMX_StateExecuting\n");
        return AVERROR_UNKNOWN;
    }


    for (i = 0; i < s->num_out_buffers && err == OMX_ErrorNone; i++) {
        err = OMX_FillThisBuffer(s->handle, s->out_buffer_headers[i]);
    }

    if (err != OMX_ErrorNone) {
        av_log(avctx, AV_LOG_WARNING, "FillOutBuffer failed , so set header\n");
        for (; i < s->num_out_buffers; i++) {
            s->done_out_buffers[s->num_done_out_buffers++] = s->out_buffer_headers[i];
        }
    }
#ifdef MPEG4_OMX
    omx_send_extradata(avctx);
#endif
    return err != OMX_ErrorNone ? AVERROR_UNKNOWN : 0;
}

static av_cold void omx_cleanup(OMXCodecDecoderContext *s)
{
    int executing;
    av_log(s->avctx, AV_LOG_WARNING, "OMX Cleanup\n");
    /* If the mutexes/condition variables have not been properly initialized,
     * nothing has been initialized and locking the mutex might be unsafe. */
    if (s->mutex_cond_inited_cnt == NB_MUTEX_CONDS) {
        pthread_mutex_lock(&s->state_mutex);
        executing = s->state == OMX_StateExecuting;
        pthread_mutex_unlock(&s->state_mutex);

        if (executing) {
            OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
            wait_for_state(s, OMX_StateIdle);
            OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateLoaded, NULL);

            for (int i = 0; i < s->num_in_buffers; i++) {
                OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->input_mutex, &s->input_cond,
                                               &s->num_free_in_buffers, &s->free_in_buffers, -1);
                OMX_FreeBuffer(s->handle, s->in_port, buffer);
            }

            for (int i = 0; i < s->num_out_buffers; i++) {
                OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                               &s->num_done_out_buffers, &s->done_out_buffers, -1);
                OMX_FreeBuffer(s->handle, s->out_port, buffer);
            }

            pthread_mutex_lock(&s->output_mutex);
            s->num_out_buffers = 0;
            s->num_done_out_buffers = 0;
            pthread_mutex_unlock(&s->output_mutex);

            wait_for_state(s, OMX_StateLoaded);
        }

        if (s->handle) {
            s->omx_context->ptr_FreeHandle(s->handle);
            s->handle = NULL;
        }

        //omx_deinit
        omx_deinit(s->omx_context);
        s->omx_context = NULL;
        av_freep(&s->in_buffer_headers);
        av_freep(&s->out_buffer_headers);
        av_freep(&s->free_in_buffers);
        av_freep(&s->done_out_buffers);

    }
    if (s->mutex_cond_inited_cnt) {
        pthread_cond_destroy(&s->state_cond);
        pthread_mutex_destroy(&s->state_mutex);
        pthread_cond_destroy(&s->input_cond);
        pthread_mutex_destroy(&s->input_mutex);
        pthread_cond_destroy(&s->output_cond);
        pthread_mutex_destroy(&s->output_mutex);
        pthread_cond_destroy(&s->eof_cond);
        pthread_mutex_destroy(&s->eof_mutex);
        s->mutex_cond_inited_cnt = 0;
    }
}

static av_cold int omx_decode_init(AVCodecContext *avctx)
{

    OMXCodecDecoderContext *s = avctx->priv_data;
    int ret = AVERROR_DECODER_NOT_FOUND;
    const char *role;
    av_log(avctx, AV_LOG_INFO, "omx_decode_init enter\n");
    /* cleanup relies on the mutexes/conditions being initialized first. */
    s->omx_context = omx_init(avctx, s->libname, s->libprefix);
    if (!s->omx_context)
        return AVERROR_DECODER_NOT_FOUND;

    pthread_mutex_init(&s->state_mutex, NULL);
    pthread_cond_init(&s->state_cond, NULL);
    pthread_mutex_init(&s->input_mutex, NULL);
    pthread_cond_init(&s->input_cond, NULL);
    pthread_mutex_init(&s->output_mutex, NULL);
    pthread_cond_init(&s->output_cond, NULL);
    pthread_mutex_init(&s->eof_mutex, NULL);
    pthread_cond_init(&s->eof_cond, NULL);
    s->mutex_cond_inited_cnt = 1;
    s->avctx = avctx;
    s->state = OMX_StateLoaded;
    s->error = OMX_ErrorNone;
    s->first_pkt = 1;

    switch (avctx->codec->id) {
    case AV_CODEC_ID_MPEG4:
        role = "video_decoder.mpeg4";
        break;
    case AV_CODEC_ID_H263:
        role = "video_decoder.h263";
        break;
    case AV_CODEC_ID_H264:
        role = "video_decoder.avc";
        break;
    case AV_CODEC_ID_HEVC:
        role = "video_decoder.hevc";
        break;
    case AV_CODEC_ID_WMV1:
    case AV_CODEC_ID_WMV2:
    case AV_CODEC_ID_WMV3:
        role = "video_decoder.wmv";
        break;
    case AV_CODEC_ID_VP6:
        role = "video_decoder.vp6";
        break;
    case AV_CODEC_ID_VP8:
        role = "video_decoder.vp8";
        break;
    case AV_CODEC_ID_VP9:
        role = "video_decoder.vp9";
        break;
    default:
        return AVERROR(ENOSYS);
    }

    ret = omx_dec_find_component(s->omx_context, avctx, role, s->component_name, sizeof(s->component_name));
    if (ret < 0) {
        return ret;
    }

    av_log(avctx, AV_LOG_INFO, "Using %s\n", s->component_name);
    ret =  omx_component_init_decoder(avctx, role);
    if (ret < 0) {
        return ret;
    }

    av_log(avctx, AV_LOG_INFO, "%p, omx_decode_init:num_done_out_buffers %d, num_free_in_buffers %d\n", s,
           s->num_out_buffers, s->num_free_in_buffers);
    return 0;
}

static int ff_omx_dec_send(AVCodecContext *avctx, OMXCodecDecoderContext *s,
                           AVPacket *pkt, bool wait)
{
    OMX_BUFFERHEADERTYPE *buffer;
    OMX_ERRORTYPE err;
    int need_draining = 0;
    int64_t timeout = 0;

    if (wait) {
        timeout = 80;
    }

    if (pkt->size == 0) {
        need_draining = 1;
    }

    if (s->draining && s->got_eos) {
        av_log(avctx, AV_LOG_INFO, "got eof:%d\n", s->got_eos);
        return AVERROR_EOF;
    }

    if (s->reconfigPending) {
        //    av_usleep(1000);
        return AVERROR_EOF;
    }

    if (pkt->data) {
        buffer = get_buffer(&s->input_mutex, &s->input_cond,
                            &s->num_free_in_buffers, &s->free_in_buffers, timeout);

        if (buffer == NULL) {
            return AVERROR(EAGAIN);
        }

        buffer->nFilledLen = pkt->size;
        buffer->nAllocLen = buffer->nFilledLen;
        buffer->nFlags = 0;

        if (pkt->flags & AV_PKT_FLAG_KEY) {
            buffer->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;
        }
        if (pkt->flags & AV_PKT_FLAG_DISCARD) {
            av_log(avctx, AV_LOG_ERROR, "AV_PKT_FLAG_DISCARD\n");
        } else if (pkt->flags & AV_PKT_FLAG_CORRUPT) {
            av_log(avctx, AV_LOG_ERROR, "AV_PKT_FLAG_CORRUPT\n");
        } else {
            buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;
        }

        buffer->nOffset = 0;
        buffer->nTimeStamp = to_omx_ticks(av_rescale_q(pkt->pts, avctx->time_base, AV_TIME_BASE_Q));
        memcpy(buffer->pBuffer, pkt->data, pkt->size);
        err = OMX_EmptyThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->input_mutex, &s->input_cond, &s->num_free_in_buffers, s->free_in_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "OMX_EmptyThisBuffer failed: %x\n", err);
            return AVERROR_UNKNOWN;
        }
        s->input_count++;
    } else if (!s->eos_sent) {
        //flush, end of stream
        //ff_omx_dec_send reach eos
        buffer = get_buffer(&s->input_mutex, &s->input_cond,
                            &s->num_free_in_buffers, &s->free_in_buffers, timeout);

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
    }
    return 0;
}

static int ff_omx_dec_is_flushing(AVCodecContext *avctx, OMXCodecDecoderContext *s)
{
    return s->flushing;
}

static int ff_omx_dec_flush(AVCodecContext *avctx, OMXCodecDecoderContext *s)
{
    OMX_ERRORTYPE err;
    OMX_BUFFERHEADERTYPE *buffer;
    int i = 0;

    s->draining = 0;
    s->got_eos = 0;
    if (!s->flushing) {
        av_log(avctx, AV_LOG_INFO, "ff_omx_dec_flush\n");
        err = OMX_SendCommand(s->handle, OMX_CommandFlush, s->in_port, NULL);
        if (err != OMX_ErrorNone)
            return -1;

        err = OMX_SendCommand(s->handle, OMX_CommandFlush, s->out_port, NULL);
        if (err != OMX_ErrorNone)
            return -1;

        while (s->num_done_out_buffers > 0) {
            buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                &s->num_done_out_buffers, &s->done_out_buffers,
                                -1);
            if (!buffer) {
                //omx_outputbuffer_thread  error
                return AVERROR(EAGAIN);
            }
            err = OMX_FillThisBuffer(s->handle, buffer);
            if (err != OMX_ErrorNone) {
                append_buffer(&s->output_mutex, &s->output_cond, &s->num_done_out_buffers, s->done_out_buffers, buffer);
                av_log(avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
            }
        }
        s->flushing = 1;
    }
    return 0;
}

static int omx_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    OMXCodecDecoderContext *s = avctx->priv_data;
    int ret;
    int out_value = 0;
    /* feed decoder */
    while (1) {
        ret = omx_try_filltempbuffer(avctx);
        if (s->num_done_out_buffers > 0) {
            ret = ff_omx_dec_receive(avctx, s, frame, false);
            if (ret != AVERROR(EAGAIN)) {
                //receive success!
                s->sent_pkt_num--;
                return ret;
            }
        } else if (s->num_done_out_buffers == 0 && s->got_eos && s->eos_sent) {
            return AVERROR_EOF;
        }

        /* try to flush any buffered packet data */
        if (s->buffered_pkt.size > 0 && !s->outport_disabled) {
            ret = ff_omx_dec_send(avctx, s, &s->buffered_pkt, true);
            if (ret >= 0) {
                //ff_omx_dec_send success
                s->sent_pkt_num++;
                s->pkt_full = 0;
                av_packet_unref(&s->buffered_pkt);
                s->flushing = 0;
            } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
                return ret;
            }
            /* poll for space again */
            continue;
        }

        /* fetch new packet or eof */
        if (s->pkt_full == 0) {
            ret = ff_decode_get_packet(avctx, &s->buffered_pkt);
            if (ret == AVERROR_EOF) {
                AVPacket null_pkt = { 0 };
                ret = ff_omx_dec_send(avctx, s, &null_pkt, true);
                if (ret < 0) {
                    s->need_sendeos = 1;
                    return ret;
                }

                s->need_sendeos = 0;
                s->eos_reach = 1;
                continue;

            } else if (ret < 0) {
                return ret;
            } else {
                //success
                s->pkt_full = 1;
            }
        }
    }
    return AVERROR(EAGAIN);
}


static void omx_decode_flush(AVCodecContext *avctx)
{
    OMXCodecDecoderContext *s = avctx->priv_data;
    if (s->buffered_pkt.size > 0) {
        av_packet_unref(&s->buffered_pkt);
        s->buffered_pkt.size = 0;
    }
    ff_omx_dec_flush(avctx, s);
}

static av_cold int omx_decode_end(AVCodecContext *avctx)
{
    OMXCodecDecoderContext *s = avctx->priv_data;

    omx_cleanup(s);
    return 0;
}

#define OFFSET(x) offsetof(OMXCodecDecoderContext, x)
#define VDE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_ENCODING_PARAM
#define VE  AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
#define VD  AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM


static const AVCodecHWConfigInternal *const omx_hw_configs[] = {
    &(const AVCodecHWConfigInternal)
    {
        .public          = {
            .pix_fmt     = AV_PIX_FMT_NV12,
            .methods     = AV_CODEC_HW_CONFIG_METHOD_AD_HOC |
            AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX,
            .device_type = AV_HWDEVICE_TYPE_OMX,
        },
        .hwaccel         = NULL,
    },
    NULL
};


static const AVOption ff_omxcodec_vdec_options[] = {
    {
        "delay_flush", "Delay flush until hw output buffers are returned to the decoder(not support)",
        OFFSET(delay_flush), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD
    },
    { "fast_render", "Fast render(not support)",  OFFSET(fast_render), AV_OPT_TYPE_BOOL,   {.i64 = 0}, 0, 1, VD},
    { "mirror", "mirror image(not support)",      OFFSET(mirror), AV_OPT_TYPE_INT,         {.i64 = 0}, 0, 2, VD},
    { "rotation", "rotation angle(not support)",  OFFSET(rotation), AV_OPT_TYPE_INT,       {.i64 = 0}, 0, 3, VD},
    { "output_width", "output width(must smaller than the original width)",  OFFSET(output_width), AV_OPT_TYPE_INT,       {.i64 = 0}, 0, OMX_VIDEO_HEVCLevelMax, VD},
    { "output_height", "output height(must smaller than the original height)",  OFFSET(output_height), AV_OPT_TYPE_INT,       {.i64 = 0}, 0, OMX_VIDEO_HEVCLevelMax, VD},
    { "offscreen", "off screen mode",  OFFSET(offscreen), AV_OPT_TYPE_INT,       {.i64 = 0}, 0, 1, VD},
    { NULL }
};



#define DECLARE_OMX_VCLASS(short_name)                          \
static const AVClass ff_##short_name##_omxcodec_dec_class = {   \
    .class_name = #short_name "_libomx",                        \
    .item_name  = av_default_item_name,                         \
    .option     = ff_omxcodec_vdec_options,                     \
    .version    = LIBAVUTIL_VERSION_INT,                        \
};

#ifdef FORMAT_NV12
#define DECLARE_OMX_VDEC(short_name, full_name, codec_id, bsf)                                 \
DECLARE_OMX_VCLASS(short_name)                                                                 \
const AVCodec ff_ ## short_name ## _omx_decoder = {                                            \
    .name           = #short_name "_omx",                                                      \
    .long_name      = NULL_IF_CONFIG_SMALL("OpenMAX IL decoder"),                                \
    .type           = AVMEDIA_TYPE_VIDEO,                                                        \
    .id             = codec_id,                                                                  \
    .priv_class     = &ff_##short_name##_omxcodec_dec_class,                                     \
    .priv_data_size = sizeof(OMXCodecDecoderContext),                                          \
    .init           = omx_decode_init,                                                         \
    .receive_frame  = omx_receive_frame,                                                       \
    .close          = omx_decode_end,                                                          \
    .flush          = omx_decode_flush,                                                        \
    .capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING ,                          \
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS ,                                              \
    .bsfs           = bsf,                                                                     \
    .hw_configs    = (const AVCodecHWConfigInternal *const []) { NULL },                       \
    .wrapper_name = "omxcodec",                                                                \
    .pix_fmts       = (const enum AVPixelFormat[]) {                                           \
                                 AV_PIX_FMT_NV12,                                              \
                                 AV_PIX_FMT_NONE },                                            \
};

#else
#define DECLARE_OMX_VDEC(short_name, full_name, codec_id, bsf)                                 \
DECLARE_OMX_VCLASS(short_name)                                                                 \
const AVCodec ff_ ## short_name ## _omx_decoder = {                                            \
    .name           = #short_name "_omx",                                                      \
    .long_name      = NULL_IF_CONFIG_SMALL("OpenMAX IL decoder"),                                \
    .type           = AVMEDIA_TYPE_VIDEO,                                                        \
    .id             = codec_id,                                                                  \
    .priv_class     = &ff_##short_name##_omxcodec_dec_class,                                     \
    .priv_data_size = sizeof(OMXCodecDecoderContext),                                          \
    .init           = omx_decode_init,                                                         \
    .receive_frame  = omx_receive_frame,                                                       \
    .close          = omx_decode_end,                                                          \
    .flush          = omx_decode_flush,                                                        \
    .capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING ,                          \
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS ,                                              \
    .bsfs           = bsf,                                                                     \
    .hw_configs    = (const AVCodecHWConfigInternal *const []) { NULL },                       \
};

#endif



DECLARE_OMX_VDEC(h264, "H.264", AV_CODEC_ID_H264, "h264_mp4toannexb")
DECLARE_OMX_VDEC(hevc, "H.265", AV_CODEC_ID_HEVC, "hevc_mp4toannexb")
DECLARE_OMX_VDEC(vp9, "VP9", AV_CODEC_ID_VP9, NULL)

#ifdef MPEG4_OMX
DECLARE_OMX_VDEC(mpeg4, "MPEG4", AV_CODEC_ID_MPEG4, NULL)
#endif
