#pragma once

/* GStreamer 1.x Qualcomm OMX video decoder plugin for HP TouchPad (APQ8060)
 *
 * Wraps the on-device libOmxCore.so using OMX.qcom.video.decoder.{avc,mpeg4,h263}
 * components.  Outputs NV12 (semi-planar YUV420) frames via GstVideoDecoder.
 *
 * Copyright 2026 Prism Browser Project.  LGPL-2.1+.
 */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideodecoder.h>

#include <pthread.h>
#include <semaphore.h>

/* ── OMX IL types (subset we need) ─────────────────────────────────────── */
typedef unsigned int   OMX_U32;
typedef unsigned char  OMX_U8;
typedef int            OMX_BOOL;
typedef void          *OMX_HANDLETYPE;
typedef void          *OMX_PTR;

#define OMX_TRUE  1
#define OMX_FALSE 0

typedef enum {
    OMX_StateInvalid,
    OMX_StateLoaded,
    OMX_StateIdle,
    OMX_StateExecuting,
    OMX_StatePause,
    OMX_StateWaitForResources,
    OMX_StateMax = 0x7FFFFFFF
} OMX_STATETYPE;

typedef enum {
    OMX_CommandStateSet,
    OMX_CommandFlush,
    OMX_CommandPortDisable,
    OMX_CommandPortEnable,
    OMX_CommandMax = 0x7FFFFFFF
} OMX_COMMANDTYPE;

typedef enum {
    OMX_ErrorNone = 0,
    OMX_ErrorInsufficientResources  = 0x80001000,
    OMX_ErrorUndefined              = 0x80001001,
    OMX_ErrorComponentNotFound      = 0x80001003,
    OMX_ErrorInvalidState           = 0x80001007,
    OMX_ErrorMax                    = 0x7FFFFFFF
} OMX_ERRORTYPE;

typedef enum {
    OMX_EventCmdComplete,
    OMX_EventError,
    OMX_EventMark,
    OMX_EventPortSettingsChanged,
    OMX_EventBufferFlag,
    OMX_EventMax = 0x7FFFFFFF
} OMX_EVENTTYPE;

typedef enum {
    OMX_IndexParamPortDefinition   = 0x02000001,
    OMX_IndexParamVideoPortFormat  = 0x02000008,
    OMX_IndexParamVideoAvc         = 0x02000009,
    OMX_IndexParamVideoMpeg4       = 0x0200000A,
    OMX_IndexParamVideoH263        = 0x0200000B,
    OMX_IndexMax                   = 0x7FFFFFFF
} OMX_INDEXTYPE;

typedef enum {
    OMX_VIDEO_CodingUnused,
    OMX_VIDEO_CodingAutoDetect,
    OMX_VIDEO_CodingMPEG2,
    OMX_VIDEO_CodingH263,
    OMX_VIDEO_CodingMPEG4,
    OMX_VIDEO_CodingWMV,
    OMX_VIDEO_CodingRV,
    OMX_VIDEO_CodingAVC,
    OMX_VIDEO_CodingMax = 0x7FFFFFFF
} OMX_VIDEO_CODINGTYPE;

typedef enum {
    OMX_COLOR_FormatUnused,
    OMX_COLOR_FormatYUV420Planar       = 0x13,
    OMX_COLOR_FormatYUV420SemiPlanar   = 0x15,  /* NV12 — Qcom HW output */
    OMX_COLOR_FormatMax = 0x7FFFFFFF
} OMX_COLOR_FORMATTYPE;

typedef enum {
    OMX_DirInput,
    OMX_DirOutput,
    OMX_DirMax = 0x7FFFFFFF
} OMX_DIRTYPE;

/* OMX_PORTDOMAINTYPE — needed for correct OMX_PARAM_PORTDEFINITIONTYPE layout */
typedef enum {
    OMX_PortDomainAudio,
    OMX_PortDomainVideo,
    OMX_PortDomainImage,
    OMX_PortDomainOther,
    OMX_PortDomainMax = 0x7FFFFFFF
} OMX_PORTDOMAINTYPE;

/* OMX_VIDEO_PORTDEFINITIONTYPE — matches Khronos OMX IL 1.1.2 spec exactly.
 * On ARM32: pointers = 4 bytes, all fields 4-byte aligned. */
typedef struct {
    OMX_PTR  cMIMEType;             /* OMX_STRING = char* */
    OMX_PTR  pNativeRender;         /* void* */
    OMX_U32  nFrameWidth;
    OMX_U32  nFrameHeight;
    OMX_U32  nStride;               /* OMX_S32 but same size */
    OMX_U32  nSliceHeight;
    OMX_U32  nBitrate;
    OMX_U32  xFramerate;            /* Q16 */
    OMX_BOOL bFlagErrorConcealment;
    OMX_U32  eCompressionFormat;    /* OMX_VIDEO_CODINGTYPE */
    OMX_U32  eColorFormat;          /* OMX_COLOR_FORMATTYPE */
    OMX_PTR  pNativeWindow;         /* void* */
} OMX_VIDEO_PORTDEFINITIONTYPE;     /* 12 × 4 = 48 bytes */

/* OMX_PARAM_PORTDEFINITIONTYPE — full layout per spec.
 * Previous version was missing eDomain (4 bytes), shifting the format union.
 * Fixed: eDomain is at offset 36, format union at offset 40. */
typedef struct {
    OMX_U32  nSize;
    OMX_U32  nVersion;              /* OMX_VERSIONTYPE — 4 bytes on ARM32 */
    OMX_U32  nPortIndex;
    OMX_U32  eDir;                  /* OMX_DIRTYPE */
    OMX_U32  nBufferCountActual;
    OMX_U32  nBufferCountMin;
    OMX_U32  nBufferSize;
    OMX_BOOL bEnabled;
    OMX_BOOL bPopulated;
    OMX_U32  eDomain;               /* OMX_PORTDOMAINTYPE — was missing! */
    union {
        OMX_VIDEO_PORTDEFINITIONTYPE video;
        OMX_U8 _pad[128];           /* audio/image/other may be larger */
    } format;
    OMX_BOOL bBuffersContiguous;
    OMX_U32  nBufferAlignment;
    /* Qcom adds extra fields; pad to be safe */
    OMX_U32  _reserved[8];
} OMX_PARAM_PORTDEFINITIONTYPE;

typedef struct {
    OMX_U32         nSize;
    OMX_U32         nVersion;
    OMX_U8         *pBuffer;
    OMX_U32         nAllocLen;
    OMX_U32         nFilledLen;
    OMX_U32         nOffset;
    OMX_PTR         pAppPrivate;
    OMX_PTR         pPlatformPrivate;
    OMX_PTR         pInputPortPrivate;
    OMX_PTR         pOutputPortPrivate;
    OMX_HANDLETYPE  hMarkTargetComponent;
    OMX_PTR         pMarkData;
    OMX_U32         nTickCount;
    long long       nTimeStamp;   /* OMX_TICKS = int64 on ARM */
    OMX_U32         nFlags;
    OMX_U32         nOutputPortIndex;
    OMX_U32         nInputPortIndex;
} OMX_BUFFERHEADERTYPE;

#define OMX_BUFFERFLAG_EOS          0x00000001
#define OMX_BUFFERFLAG_CODECCONFIG  0x00000080

typedef struct {
    OMX_ERRORTYPE (*EventHandler)(OMX_HANDLETYPE, OMX_PTR, OMX_EVENTTYPE,
                                  OMX_U32, OMX_U32, OMX_PTR);
    OMX_ERRORTYPE (*EmptyBufferDone)(OMX_HANDLETYPE, OMX_PTR,
                                     OMX_BUFFERHEADERTYPE *);
    OMX_ERRORTYPE (*FillBufferDone)(OMX_HANDLETYPE, OMX_PTR,
                                    OMX_BUFFERHEADERTYPE *);
} OMX_CALLBACKTYPE;

/* libOmxCore function pointer types */
typedef OMX_ERRORTYPE (*pfn_OMX_Init)(void);
typedef OMX_ERRORTYPE (*pfn_OMX_Deinit)(void);
typedef OMX_ERRORTYPE (*pfn_OMX_GetHandle)(OMX_HANDLETYPE *, char *,
                                            OMX_PTR, OMX_CALLBACKTYPE *);
typedef OMX_ERRORTYPE (*pfn_OMX_FreeHandle)(OMX_HANDLETYPE);

/* OMX IL OMX_COMPONENTTYPE — function pointers are directly on the struct.
 * Layout per OMX IL 1.1.2 spec (Khronos):
 *   nSize, nVersion, pComponentPrivate, pApplicationPrivate,
 *   then function pointers in order. */
typedef struct _OmxComponentType {
    OMX_U32  nSize;
    OMX_U32  nVersion;          /* OMX_VERSIONTYPE (4 bytes on ARM32) */
    OMX_PTR  pComponentPrivate;
    OMX_PTR  pApplicationPrivate;
    OMX_ERRORTYPE (*GetComponentVersion)(OMX_HANDLETYPE, char *,
                                          OMX_U32 *, OMX_U32 *, OMX_U32 *);
    OMX_ERRORTYPE (*SendCommand)(OMX_HANDLETYPE, OMX_COMMANDTYPE,
                                 OMX_U32, OMX_PTR);
    OMX_ERRORTYPE (*GetParameter)(OMX_HANDLETYPE, OMX_INDEXTYPE, OMX_PTR);
    OMX_ERRORTYPE (*SetParameter)(OMX_HANDLETYPE, OMX_INDEXTYPE, OMX_PTR);
    OMX_ERRORTYPE (*GetConfig)(OMX_HANDLETYPE, OMX_INDEXTYPE, OMX_PTR);
    OMX_ERRORTYPE (*SetConfig)(OMX_HANDLETYPE, OMX_INDEXTYPE, OMX_PTR);
    OMX_ERRORTYPE (*GetExtensionIndex)(OMX_HANDLETYPE, char *, OMX_INDEXTYPE *);
    OMX_ERRORTYPE (*GetState)(OMX_HANDLETYPE, OMX_STATETYPE *);
    OMX_ERRORTYPE (*ComponentTunnelRequest)(OMX_HANDLETYPE, OMX_U32,
                                             OMX_HANDLETYPE, OMX_U32, OMX_PTR);
    OMX_ERRORTYPE (*UseBuffer)(OMX_HANDLETYPE, OMX_BUFFERHEADERTYPE **,
                                OMX_U32, OMX_PTR, OMX_U32, OMX_U8 *);
    OMX_ERRORTYPE (*AllocateBuffer)(OMX_HANDLETYPE, OMX_BUFFERHEADERTYPE **,
                                     OMX_U32, OMX_PTR, OMX_U32);
    OMX_ERRORTYPE (*FreeBuffer)(OMX_HANDLETYPE, OMX_U32,
                                 OMX_BUFFERHEADERTYPE *);
    OMX_ERRORTYPE (*EmptyThisBuffer)(OMX_HANDLETYPE, OMX_BUFFERHEADERTYPE *);
    OMX_ERRORTYPE (*FillThisBuffer)(OMX_HANDLETYPE, OMX_BUFFERHEADERTYPE *);
    OMX_ERRORTYPE (*SetCallbacks)(OMX_HANDLETYPE, OMX_CALLBACKTYPE *, OMX_PTR);
    OMX_ERRORTYPE (*ComponentDeInit)(OMX_HANDLETYPE);
    OMX_ERRORTYPE (*UseEGLImage)(OMX_HANDLETYPE, OMX_BUFFERHEADERTYPE **,
                                  OMX_U32, OMX_PTR, void *);
    OMX_ERRORTYPE (*ComponentRoleEnum)(OMX_HANDLETYPE, OMX_U8 *, OMX_U32);
} OmxComponentType;

#define OMX_COMP(h)             ((OmxComponentType *)(h))
#define OMX_SendCommand(h,cmd,p1,p2)     OMX_COMP(h)->SendCommand(h,cmd,p1,p2)
#define OMX_GetParameter(h,idx,p)        OMX_COMP(h)->GetParameter(h,idx,p)
#define OMX_SetParameter(h,idx,p)        OMX_COMP(h)->SetParameter(h,idx,p)
#define OMX_GetState(h,s)                OMX_COMP(h)->GetState(h,s)
#define OMX_EmptyThisBuffer(h,b)         OMX_COMP(h)->EmptyThisBuffer(h,b)
#define OMX_FillThisBuffer(h,b)          OMX_COMP(h)->FillThisBuffer(h,b)
#define OMX_AllocateBuffer(h,pp,port,priv,sz) \
    OMX_COMP(h)->AllocateBuffer(h,pp,port,priv,sz)
#define OMX_FreeBuffer(h,port,buf)       OMX_COMP(h)->FreeBuffer(h,port,buf)

/* ── Plugin element ─────────────────────────────────────────────────────── */
#define GST_TYPE_PALM_VIDEO_DEC       (gst_palm_video_dec_get_type())
#define GST_PALM_VIDEO_DEC(obj)       (G_TYPE_CHECK_INSTANCE_CAST(obj, GST_TYPE_PALM_VIDEO_DEC, GstPalmVideoDec))
#define GST_PALM_VIDEO_DEC_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST(klass, GST_TYPE_PALM_VIDEO_DEC, GstPalmVideoDecClass))
#define GST_IS_PALM_VIDEO_DEC(obj)    (G_TYPE_CHECK_INSTANCE_TYPE(obj, GST_TYPE_PALM_VIDEO_DEC))

#define PALM_VD_N_IN_BUFS   8
#define PALM_VD_N_OUT_BUFS  10  /* Qcom AVC decoder requests 6; leave headroom */
#define PALM_VD_PORT_IN     0
#define PALM_VD_PORT_OUT    1

typedef struct _GstPalmVideoDec      GstPalmVideoDec;
typedef struct _GstPalmVideoDecClass GstPalmVideoDecClass;

struct _GstPalmVideoDec {
    GstVideoDecoder parent;

    /* OMX */
    OMX_HANDLETYPE   omx_handle;
    OMX_CALLBACKTYPE omx_cbs;

    /* Buffer arrays */
    OMX_BUFFERHEADERTYPE *in_bufs[PALM_VD_N_IN_BUFS];
    OMX_BUFFERHEADERTYPE *out_bufs[PALM_VD_N_OUT_BUFS];
    guint                 n_in_bufs;
    guint                 n_out_bufs;

    /* Queues for available (free) buffers */
    GAsyncQueue *in_queue;    /* free input buffers  */
    GAsyncQueue *out_queue;   /* filled output buffers */

    /* State sync */
    pthread_mutex_t  state_lock;
    pthread_cond_t   state_cond;
    OMX_STATETYPE    omx_state;
    OMX_ERRORTYPE    omx_error;

    /* Port-reconfigure event */
    gboolean         port_reconfig;
    gboolean         port_cmd_complete;    /* set by EventHandler on PortDisable CmdComplete */
    gboolean         port_enable_complete; /* set by EventHandler on PortEnable CmdComplete */
    pthread_mutex_t  reconfig_lock;
    pthread_cond_t   reconfig_cond;

    /* Output thread */
    GThread         *out_thread;
    gboolean         out_thread_running;

    /* Video info (set by set_format) */
    GstVideoInfo     vinfo;
    gint             out_width;
    gint             out_height;
    guint            out_buf_size;
    guint            out_stride;       /* OMX hardware-aligned row stride (bytes) */
    guint            out_slice_height; /* OMX hardware-aligned frame height       */
    gint             stride_probed;    /* counts one-time stride probe dumps      */

    /* Dynamic library handle for libOmxCore.so */
    void            *omxcore_dl;
    pfn_OMX_Init     omx_Init;
    pfn_OMX_Deinit   omx_Deinit;
    pfn_OMX_GetHandle   omx_GetHandle;
    pfn_OMX_FreeHandle  omx_FreeHandle;

    gboolean         started;
    gboolean         eos_sent;

    /* H.264 stream format — if TRUE, input is AVCC (4-byte length prefix)
     * and must be converted to Annex B before sending to OMX */
    gboolean         is_avc;
    guint            nal_length_size; /* bytes per NAL length field (1,2,4) */

    /* Codec config (SPS/PPS) in Annex B format — sent once at stream start */
    guint8          *codec_config;
    guint            codec_config_len;
    gboolean         codec_config_sent;

    /* Component name (e.g. "OMX.qcom.video.decoder.avc") saved for full reset */
    char             comp_name[64];

    /* After a full FreeHandle+GetHandle reset the fresh component will fire
     * PortSettingsChanged when it reads the SPS we re-send.  We cannot do
     * another full reset (would loop forever).  Instead, do a lightweight
     * PortDisable+drain+PortEnable without FreeBuffer/AllocateBuffer. */
    gboolean         light_reconfig;

    /* Set by do_port_reconfig when the full reset completes with new dims.
     * handle_frame reads and clears this to call gst_video_decoder_set_output_state
     * with the corrected dimensions (while holding the stream lock). */
    gboolean         need_output_state_update;
};

struct _GstPalmVideoDecClass {
    GstVideoDecoderClass parent_class;
};

GType gst_palm_video_dec_get_type(void);
