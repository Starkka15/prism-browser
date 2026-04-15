/* GStreamer 1.x Qualcomm OMX video decoder for HP TouchPad (APQ8060)
 *
 * Uses on-device /usr/lib/libOmxCore.so via dlopen.
 * Supports: H.264/AVC, MPEG-4 Part 2, H.263
 * Output: NV12 (OMX_COLOR_FormatYUV420SemiPlanar) — hardware native format.
 *
 * Copyright 2026 Prism Browser Project.  LGPL-2.1+.
 */

#define PACKAGE "gst-palm-video-decoder"
#define PACKAGE_VERSION "1.0"
#define GST_PACKAGE_ORIGIN "https://github.com/prism-browser"
#define GST_PACKAGE_NAME "Prism Browser GStreamer plugins"

#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include "gstpalmvideodec.h"

/* Forward declaration — set_omx_state defined after wait_for_state uses it */
static void set_omx_state(GstPalmVideoDec *self, OMX_STATETYPE s);

GST_DEBUG_CATEGORY_STATIC(gst_palm_video_dec_debug);
#define GST_CAT_DEFAULT gst_palm_video_dec_debug

/* ── Pad templates ─────────────────────────────────────────────────────── */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-h264, stream-format=(string)byte-stream, "
            "alignment=(string)nal; "
        "video/x-h264, stream-format=(string)avc, "
            "alignment=(string)au; "
        "video/mpeg, mpegversion=(int)4, "
            "systemstream=(boolean)false; "
        "video/x-h263"
    )
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=(string)NV12")
);

G_DEFINE_TYPE(GstPalmVideoDec, gst_palm_video_dec, GST_TYPE_VIDEO_DECODER)

/* ── Helpers ────────────────────────────────────────────────────────────── */

/* port_def_init: fill nSize/nVersion before any GetParameter/SetParameter */
static void port_def_init(OMX_PARAM_PORTDEFINITIONTYPE *pd)
{
    memset(pd, 0, sizeof(*pd));
    pd->nSize    = sizeof(*pd);
    pd->nVersion = 0x00000101; /* OMX IL 1.1 */
}

/* wait_for_state: wait up to timeout_ms for OMX component to reach target state.
 *
 * Dual mechanism:
 *  1. pthread_cond_wait woken by omx_event_handler (callback path)
 *  2. OMX_GetState polling every 20ms (fallback if callbacks don't fire)
 *
 * On some Qualcomm OMX implementations running outside the media server
 * process, CmdComplete callbacks may not be delivered if the media IPC
 * channel is not set up. Polling ensures state transitions are detected
 * even in that case.
 */
static gboolean wait_for_state(GstPalmVideoDec *self, OMX_STATETYPE target,
                                guint timeout_ms)
{
    struct timespec deadline, poll_ts, now;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    gboolean ok = FALSE;

    while (1) {
        /* ── 1. Check callback-updated state ── */
        pthread_mutex_lock(&self->state_lock);
        if (self->omx_state == target) {
            pthread_mutex_unlock(&self->state_lock);
            ok = TRUE;
            break;
        }
        if (self->omx_error != OMX_ErrorNone) {
            GST_ERROR_OBJECT(self,
                "OMX error 0x%08x while waiting for state %d",
                self->omx_error, (int)target);
            pthread_mutex_unlock(&self->state_lock);
            break;
        }

        /* ── 2. Short timed wait (20ms) — woken early by EventHandler ── */
        clock_gettime(CLOCK_REALTIME, &poll_ts);
        poll_ts.tv_nsec += 20 * 1000000L;
        if (poll_ts.tv_nsec >= 1000000000L) {
            poll_ts.tv_sec++;
            poll_ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&self->state_cond, &self->state_lock, &poll_ts);
        pthread_mutex_unlock(&self->state_lock);

        /* ── 3. Poll OMX_GetState directly ── */
        if (self->omx_handle) {
            OMX_STATETYPE cur = OMX_StateInvalid;
            OMX_ERRORTYPE gerr = OMX_GetState(self->omx_handle, &cur);
            GST_LOG_OBJECT(self,
                "OMX_GetState poll: cur=%d target=%d gerr=0x%x",
                (int)cur, (int)target, (unsigned)gerr);
            if (cur == target) {
                GST_DEBUG_OBJECT(self,
                    "OMX state %d reached via polling (callback may not have fired)",
                    (int)target);
                set_omx_state(self, target);
                ok = TRUE;
                break;
            }
        }

        /* ── 4. Deadline check ── */
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            break;
    }

    if (!ok) {
        OMX_STATETYPE final = OMX_StateInvalid;
        if (self->omx_handle)
            OMX_GetState(self->omx_handle, &final);
        GST_ERROR_OBJECT(self,
            "wait_for_state TIMEOUT: wanted=%d actual=%d callback_state=%d error=0x%x",
            (int)target, (int)final, (int)self->omx_state, (unsigned)self->omx_error);
    }
    return ok;
}

static void set_omx_state(GstPalmVideoDec *self, OMX_STATETYPE s)
{
    pthread_mutex_lock(&self->state_lock);
    self->omx_state = s;
    pthread_cond_broadcast(&self->state_cond);
    pthread_mutex_unlock(&self->state_lock);
}

/* ── OMX Callbacks ──────────────────────────────────────────────────────── */
static OMX_ERRORTYPE omx_event_handler(OMX_HANDLETYPE hComp, OMX_PTR pAppData,
                                        OMX_EVENTTYPE eEvent,
                                        OMX_U32 nData1, OMX_U32 nData2,
                                        OMX_PTR pEventData)
{
    /* Log unconditionally to stderr so we can verify callbacks are firing
     * even if GST_DEBUG is misconfigured or the log file isn't flushed. */
    fprintf(stderr, "[palmvideodec] EventHandler: event=%d d1=0x%x d2=0x%x pAppData=%p\n",
            (int)eEvent, (unsigned)nData1, (unsigned)nData2, pAppData);
    fflush(stderr);

    if (!pAppData) {
        fprintf(stderr, "[palmvideodec] EventHandler: pAppData is NULL! Callback broken.\n");
        return OMX_ErrorNone;
    }

    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(pAppData);
    (void)hComp; (void)pEventData;

    switch (eEvent) {
    case OMX_EventCmdComplete:
        GST_DEBUG_OBJECT(self, "OMX EventCmdComplete cmd=%d data2=%d",
                         (int)nData1, (int)nData2);
        if (nData1 == (OMX_U32)OMX_CommandStateSet) {
            GST_DEBUG_OBJECT(self, "OMX state → %d", (int)nData2);
            set_omx_state(self, (OMX_STATETYPE)nData2);
        } else if (nData1 == (OMX_U32)OMX_CommandFlush) {
            GST_DEBUG_OBJECT(self, "OMX flush complete port=%d", (int)nData2);
        } else if (nData1 == (OMX_U32)OMX_CommandPortDisable) {
            GST_DEBUG_OBJECT(self, "OMX port disable complete port=%d", (int)nData2);
            pthread_mutex_lock(&self->reconfig_lock);
            self->port_cmd_complete = TRUE;
            pthread_cond_broadcast(&self->reconfig_cond);
            pthread_mutex_unlock(&self->reconfig_lock);
        } else if (nData1 == (OMX_U32)OMX_CommandPortEnable) {
            GST_DEBUG_OBJECT(self, "OMX port enable complete port=%d", (int)nData2);
            pthread_mutex_lock(&self->reconfig_lock);
            self->port_cmd_complete = TRUE;
            pthread_cond_broadcast(&self->reconfig_cond);
            pthread_mutex_unlock(&self->reconfig_lock);
        }
        break;
    case OMX_EventError:
        GST_ERROR_OBJECT(self, "OMX EventError 0x%08x", (unsigned)nData1);
        fprintf(stderr, "[palmvideodec] OMX Error: 0x%08x\n", (unsigned)nData1);
        pthread_mutex_lock(&self->state_lock);
        self->omx_error = (OMX_ERRORTYPE)nData1;
        pthread_cond_broadcast(&self->state_cond);
        pthread_mutex_unlock(&self->state_lock);
        break;
    case OMX_EventPortSettingsChanged:
        GST_DEBUG_OBJECT(self, "OMX port reconfig port=%d", (int)nData1);
        fprintf(stderr, "[palmvideodec] PortSettingsChanged port=%d\n", (int)nData1);
        pthread_mutex_lock(&self->reconfig_lock);
        self->port_reconfig = TRUE;
        pthread_cond_broadcast(&self->reconfig_cond);
        pthread_mutex_unlock(&self->reconfig_lock);
        break;
    default:
        GST_DEBUG_OBJECT(self, "OMX event %d (ignored)", (int)eEvent);
        break;
    }
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE omx_empty_buffer_done(OMX_HANDLETYPE hComp,
                                             OMX_PTR pAppData,
                                             OMX_BUFFERHEADERTYPE *pBuf)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(pAppData);
    (void)hComp;
    GST_LOG_OBJECT(self, "EmptyBufferDone buf=%p", (void*)pBuf);
    /* Return input buffer to free pool */
    g_async_queue_push(self->in_queue, pBuf);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE omx_fill_buffer_done(OMX_HANDLETYPE hComp,
                                            OMX_PTR pAppData,
                                            OMX_BUFFERHEADERTYPE *pBuf)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(pAppData);
    (void)hComp;
    GST_LOG_OBJECT(self, "FillBufferDone buf=%p filled=%d flags=0x%x",
                   (void*)pBuf, (int)pBuf->nFilledLen, (unsigned)pBuf->nFlags);
    /* Push filled output buffer to output queue */
    g_async_queue_push(self->out_queue, pBuf);
    return OMX_ErrorNone;
}

/* ── OMX IL port reconfiguration sequence ───────────────────────────────── */
/* Called from output thread when OMX_EventPortSettingsChanged fires.
 * Performs: PortDisable → FreeBuffer(all out) → GetParameter → PortEnable
 *           → AllocateBuffer(all out) → FillThisBuffer(all). */
static void do_port_reconfig(GstPalmVideoDec *self)
{
    OMX_ERRORTYPE err;
    struct timespec ts;

    GST_DEBUG_OBJECT(self, "port reconfig: start");

    /* ── 1. Disable output port ── */
    pthread_mutex_lock(&self->reconfig_lock);
    self->port_cmd_complete = FALSE;
    pthread_mutex_unlock(&self->reconfig_lock);

    err = OMX_SendCommand(self->omx_handle, OMX_CommandPortDisable, PALM_VD_PORT_OUT, NULL);
    GST_DEBUG_OBJECT(self, "port reconfig: PortDisable cmd err=0x%x", (unsigned)err);

    /* ── 2. Free all output buffers (must happen while port is disabling) ── */
    for (guint i = 0; i < self->n_out_bufs; i++) {
        if (self->out_bufs[i]) {
            OMX_FreeBuffer(self->omx_handle, PALM_VD_PORT_OUT, self->out_bufs[i]);
            self->out_bufs[i] = NULL;
        }
    }
    self->n_out_bufs = 0;

    /* ── 3. Wait for PortDisable complete ── */
    pthread_mutex_lock(&self->reconfig_lock);
    clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 2;
    while (!self->port_cmd_complete)
        pthread_cond_timedwait(&self->reconfig_cond, &self->reconfig_lock, &ts);
    self->port_cmd_complete = FALSE;
    pthread_mutex_unlock(&self->reconfig_lock);
    GST_DEBUG_OBJECT(self, "port reconfig: PortDisable done");

    /* ── 4. Re-read output port definition ── */
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    port_def_init(&port_def);
    port_def.nPortIndex = PALM_VD_PORT_OUT;
    OMX_GetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &port_def);

    guint new_buf_size = port_def.nBufferSize ? port_def.nBufferSize : self->out_buf_size;
    guint new_n_out    = port_def.nBufferCountActual ? port_def.nBufferCountActual : PALM_VD_N_OUT_BUFS;
    if (new_n_out > PALM_VD_N_OUT_BUFS) {
        GST_WARNING_OBJECT(self, "port reconfig: out count %d > array %d, clamping", new_n_out, PALM_VD_N_OUT_BUFS);
        new_n_out = PALM_VD_N_OUT_BUFS;
    }
    if (port_def.format.video.nFrameWidth && port_def.format.video.nFrameHeight) {
        self->out_width  = (gint)port_def.format.video.nFrameWidth;
        self->out_height = (gint)port_def.format.video.nFrameHeight;
    }
    self->out_buf_size = new_buf_size;
    GST_DEBUG_OBJECT(self, "port reconfig: new out=%d×%db dim=%dx%d",
                     new_n_out, new_buf_size, self->out_width, self->out_height);

    /* ── 5. Enable output port ── */
    pthread_mutex_lock(&self->reconfig_lock);
    self->port_cmd_complete = FALSE;
    pthread_mutex_unlock(&self->reconfig_lock);

    err = OMX_SendCommand(self->omx_handle, OMX_CommandPortEnable, PALM_VD_PORT_OUT, NULL);
    GST_DEBUG_OBJECT(self, "port reconfig: PortEnable cmd err=0x%x", (unsigned)err);

    /* ── 6. Allocate new output buffers (must happen while port is enabling) ── */
    for (guint i = 0; i < new_n_out; i++) {
        err = OMX_AllocateBuffer(self->omx_handle, &self->out_bufs[i],
                                  PALM_VD_PORT_OUT, self, new_buf_size);
        GST_DEBUG_OBJECT(self, "port reconfig: AllocateBuffer out[%d] err=0x%x", i, (unsigned)err);
        if (err == OMX_ErrorNone)
            self->n_out_bufs++;
    }

    /* ── 7. Wait for PortEnable complete ── */
    pthread_mutex_lock(&self->reconfig_lock);
    clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 2;
    while (!self->port_cmd_complete)
        pthread_cond_timedwait(&self->reconfig_cond, &self->reconfig_lock, &ts);
    self->port_cmd_complete = FALSE;
    self->port_reconfig     = FALSE;
    pthread_mutex_unlock(&self->reconfig_lock);
    GST_DEBUG_OBJECT(self, "port reconfig: PortEnable done");

    /* ── 8. Submit all new output buffers ── */
    for (guint i = 0; i < self->n_out_bufs; i++)
        OMX_FillThisBuffer(self->omx_handle, self->out_bufs[i]);

    GST_INFO_OBJECT(self, "port reconfig: complete out=%d×%db", self->n_out_bufs, new_buf_size);
}

/* ── Output thread ──────────────────────────────────────────────────────── */
static gpointer output_thread_func(gpointer data)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(data);
    GstVideoDecoder *dec  = GST_VIDEO_DECODER(self);

    GST_DEBUG_OBJECT(self, "output thread started");

    while (self->out_thread_running) {
        /* Handle port reconfiguration before waiting for output buffers */
        pthread_mutex_lock(&self->reconfig_lock);
        gboolean reconfig = self->port_reconfig;
        pthread_mutex_unlock(&self->reconfig_lock);
        if (reconfig) {
            do_port_reconfig(self);
            continue;
        }

        OMX_BUFFERHEADERTYPE *buf =
            g_async_queue_timeout_pop(self->out_queue, 100000 /* 100ms */);
        if (!buf)
            continue;

        GST_LOG_OBJECT(self, "output thread: got buf filled=%d flags=0x%x ts=%lld",
                       (int)buf->nFilledLen, (unsigned)buf->nFlags,
                       (long long)buf->nTimeStamp);

        if (buf->nFlags & OMX_BUFFERFLAG_EOS) {
            GST_DEBUG_OBJECT(self, "output thread: EOS");
            OMX_FillThisBuffer(self->omx_handle, buf);
            gst_video_decoder_have_frame(dec);
            break;
        }

        if (buf->nFilledLen > 0) {
            GstVideoCodecFrame *frame =
                gst_video_decoder_get_oldest_frame(dec);
            if (frame) {
                if (gst_video_decoder_allocate_output_frame(dec, frame)
                        == GST_FLOW_OK) {
                    GstMapInfo map;
                    if (gst_buffer_map(frame->output_buffer, &map, GST_MAP_WRITE)) {
                        guint copy = MIN((guint)buf->nFilledLen, (guint)map.size);
                        memcpy(map.data, buf->pBuffer + buf->nOffset, copy);
                        gst_buffer_unmap(frame->output_buffer, &map);
                    }
                    GST_BUFFER_PTS(frame->output_buffer) =
                        (GstClockTime)((guint64)buf->nTimeStamp * 1000ULL); /* µs → ns */
                }
                gst_video_decoder_finish_frame(dec, frame);
            }
        }

        /* Recycle output buffer */
        buf->nFilledLen = 0;
        buf->nOffset    = 0;
        OMX_FillThisBuffer(self->omx_handle, buf);
    }

    GST_DEBUG_OBJECT(self, "output thread exiting");
    return NULL;
}

/* ── GstVideoDecoder vfuncs ─────────────────────────────────────────────── */
static gboolean palm_vd_open(GstVideoDecoder *dec)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(dec);

    GST_DEBUG_OBJECT(self, "opening libOmxCore.so");

    /* RTLD_GLOBAL so libOmxCore's dependencies resolve against the global
     * symbol table (libOmxVdec, libmm-qdsp-fx, etc.) */
    self->omxcore_dl = dlopen("/usr/lib/libOmxCore.so", RTLD_NOW | RTLD_GLOBAL);
    if (!self->omxcore_dl) {
        GST_ERROR_OBJECT(self, "dlopen libOmxCore.so: %s", dlerror());
        return FALSE;
    }

    GST_DEBUG_OBJECT(self, "dlopen libOmxCore.so OK");

#define LOAD(fn) do { \
    self->omx_##fn = dlsym(self->omxcore_dl, "OMX_" #fn); \
    if (!self->omx_##fn) { \
        GST_ERROR_OBJECT(self, "dlsym OMX_" #fn " failed: %s", dlerror()); \
        dlclose(self->omxcore_dl); self->omxcore_dl = NULL; return FALSE; \
    } \
    GST_DEBUG_OBJECT(self, "dlsym OMX_" #fn " = %p", (void*)self->omx_##fn); \
    } while(0)
    LOAD(Init);
    LOAD(Deinit);
    LOAD(GetHandle);
    LOAD(FreeHandle);
#undef LOAD

    OMX_ERRORTYPE err = self->omx_Init();
    if (err != OMX_ErrorNone) {
        GST_ERROR_OBJECT(self, "OMX_Init failed: 0x%08x", (unsigned)err);
        dlclose(self->omxcore_dl); self->omxcore_dl = NULL;
        return FALSE;
    }
    GST_DEBUG_OBJECT(self, "OMX_Init OK");

    self->in_queue  = g_async_queue_new();
    self->out_queue = g_async_queue_new();
    pthread_mutex_init(&self->state_lock,   NULL);
    pthread_cond_init (&self->state_cond,   NULL);
    pthread_mutex_init(&self->reconfig_lock, NULL);
    pthread_cond_init (&self->reconfig_cond, NULL);

    return TRUE;
}

static gboolean palm_vd_close(GstVideoDecoder *dec)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(dec);

    if (self->omxcore_dl) {
        self->omx_Deinit();
        dlclose(self->omxcore_dl);
        self->omxcore_dl = NULL;
    }
    if (self->in_queue)  { g_async_queue_unref(self->in_queue);  self->in_queue  = NULL; }
    if (self->out_queue) { g_async_queue_unref(self->out_queue); self->out_queue = NULL; }
    pthread_mutex_destroy(&self->state_lock);
    pthread_cond_destroy (&self->state_cond);
    pthread_mutex_destroy(&self->reconfig_lock);
    pthread_cond_destroy (&self->reconfig_cond);
    return TRUE;
}

static gboolean palm_vd_set_format(GstVideoDecoder *dec,
                                    GstVideoCodecState *state)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(dec);
    OMX_ERRORTYPE err;

    /* Tear down existing OMX handle if re-configuring */
    if (self->omx_handle) {
        self->out_thread_running = FALSE;
        if (self->out_thread) { g_thread_join(self->out_thread); self->out_thread = NULL; }

        /* Executing → Idle */
        OMX_SendCommand(self->omx_handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
        wait_for_state(self, OMX_StateIdle, 2000);

        /* Idle → Loaded (free buffers DURING this transition) */
        OMX_SendCommand(self->omx_handle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
        for (guint i = 0; i < self->n_in_bufs;  i++)
            OMX_FreeBuffer(self->omx_handle, PALM_VD_PORT_IN,  self->in_bufs[i]);
        for (guint i = 0; i < self->n_out_bufs; i++)
            OMX_FreeBuffer(self->omx_handle, PALM_VD_PORT_OUT, self->out_bufs[i]);
        self->n_in_bufs = self->n_out_bufs = 0;

        wait_for_state(self, OMX_StateLoaded, 2000);
        self->omx_FreeHandle(self->omx_handle);
        self->omx_handle = NULL;
    }

    /* Pick component name from caps */
    GstStructure *s  = gst_caps_get_structure(state->caps, 0);
    const char   *mn = gst_structure_get_name(s);
    const char   *comp_name;
    if      (g_str_equal(mn, "video/x-h264")) comp_name = "OMX.qcom.video.decoder.avc";
    else if (g_str_equal(mn, "video/mpeg"))   comp_name = "OMX.qcom.video.decoder.mpeg4";
    else if (g_str_equal(mn, "video/x-h263")) comp_name = "OMX.qcom.video.decoder.h263";
    else {
        GST_ERROR_OBJECT(self, "unsupported caps: %s", mn);
        return FALSE;
    }
    GST_INFO_OBJECT(self, "OMX component: %s", comp_name);

    self->omx_cbs.EventHandler    = omx_event_handler;
    self->omx_cbs.EmptyBufferDone = omx_empty_buffer_done;
    self->omx_cbs.FillBufferDone  = omx_fill_buffer_done;
    self->omx_state = OMX_StateLoaded;
    self->omx_error = OMX_ErrorNone;

    GST_DEBUG_OBJECT(self, "OMX_GetHandle: comp=%s pAppData=%p cbs=%p",
                     comp_name, (void*)self, (void*)&self->omx_cbs);
    GST_DEBUG_OBJECT(self, "  cb.EventHandler=%p cb.EmptyBufferDone=%p cb.FillBufferDone=%p",
                     (void*)self->omx_cbs.EventHandler,
                     (void*)self->omx_cbs.EmptyBufferDone,
                     (void*)self->omx_cbs.FillBufferDone);

    err = self->omx_GetHandle(&self->omx_handle,
                               (char *)comp_name,
                               self, &self->omx_cbs);
    if (err != OMX_ErrorNone || !self->omx_handle) {
        GST_ERROR_OBJECT(self, "OMX_GetHandle(%s) failed: 0x%08x handle=%p",
                         comp_name, (unsigned)err, (void*)self->omx_handle);
        return FALSE;
    }
    GST_INFO_OBJECT(self, "OMX_GetHandle OK: handle=%p", (void*)self->omx_handle);

    /* Get width/height from caps (use defaults if missing) */
    gint width = 0, height = 0;
    gst_structure_get_int(s, "width",  &width);
    gst_structure_get_int(s, "height", &height);
    if (!width)  width  = 480;
    if (!height) height = 320;
    GST_DEBUG_OBJECT(self, "video size: %dx%d", width, height);

    /* Detect H.264 stream format and extract codec_data (SPS/PPS) */
    g_free(self->codec_config);
    self->codec_config     = NULL;
    self->codec_config_len = 0;
    self->is_avc           = FALSE;
    self->nal_length_size  = 4;
    self->codec_config_sent = FALSE;

    if (g_str_equal(mn, "video/x-h264")) {
        const gchar *fmt = gst_structure_get_string(s, "stream-format");
        if (fmt && g_str_equal(fmt, "avc")) {
            self->is_avc = TRUE;
            const GValue *cdv = gst_structure_get_value(s, "codec_data");
            if (cdv) {
                GstBuffer *cdbuf = gst_value_get_buffer(cdv);
                GstMapInfo cdmap;
                if (cdbuf && gst_buffer_map(cdbuf, &cdmap, GST_MAP_READ)) {
                    /* AVCC: [ver][prof][compat][level][0xff|lenSz-1][0xe1|numSPS]
                     *       [2B SPS len][SPS]... [numPPS][2B PPS len][PPS]... */
                    const guint8 *d = cdmap.data;
                    guint dlen = cdmap.size;
                    if (dlen >= 7) {
                        self->nal_length_size = (d[4] & 0x03) + 1;
                        /* Build Annex-B codec_config: SPS then PPS */
                        guint8 *out = g_malloc(dlen * 2 + 16); /* upper bound */
                        guint   out_len = 0;
                        guint   pos = 5;
                        /* SPS */
                        guint n_sps = d[pos++] & 0x1f;
                        for (guint i = 0; i < n_sps && pos + 2 <= dlen; i++) {
                            guint sz = (d[pos] << 8) | d[pos+1]; pos += 2;
                            if (pos + sz > dlen) break;
                            out[out_len++] = 0; out[out_len++] = 0;
                            out[out_len++] = 0; out[out_len++] = 1;
                            memcpy(out + out_len, d + pos, sz);
                            out_len += sz; pos += sz;
                        }
                        /* PPS */
                        if (pos < dlen) {
                            guint n_pps = d[pos++];
                            for (guint i = 0; i < n_pps && pos + 2 <= dlen; i++) {
                                guint sz = (d[pos] << 8) | d[pos+1]; pos += 2;
                                if (pos + sz > dlen) break;
                                out[out_len++] = 0; out[out_len++] = 0;
                                out[out_len++] = 0; out[out_len++] = 1;
                                memcpy(out + out_len, d + pos, sz);
                                out_len += sz; pos += sz;
                            }
                        }
                        self->codec_config     = out;
                        self->codec_config_len = out_len;
                        GST_DEBUG_OBJECT(self, "AVC codec_data: nal_length_size=%d "
                                         "annexb_config=%d bytes",
                                         self->nal_length_size, out_len);
                    }
                    gst_buffer_unmap(cdbuf, &cdmap);
                }
            }
        }
    }

    /* ── Read input port definition ── */
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    port_def_init(&port_def);
    port_def.nPortIndex = PALM_VD_PORT_IN;
    err = OMX_GetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &port_def);
    GST_DEBUG_OBJECT(self,
        "IN port GetParameter: err=0x%x nBufferSize=%d nBufferCountActual=%d nBufferCountMin=%d",
        (unsigned)err, (int)port_def.nBufferSize,
        (int)port_def.nBufferCountActual, (int)port_def.nBufferCountMin);

    guint in_buf_size = port_def.nBufferSize ? port_def.nBufferSize : 512 * 1024;
    guint n_in        = port_def.nBufferCountActual ? port_def.nBufferCountActual : PALM_VD_N_IN_BUFS;
    if (n_in > PALM_VD_N_IN_BUFS) {
        GST_WARNING_OBJECT(self, "in buf count %d > array size %d, clamping", n_in, PALM_VD_N_IN_BUFS);
        n_in = PALM_VD_N_IN_BUFS;
    }

    /* ── Read & configure output port definition ── */
    port_def_init(&port_def);
    port_def.nPortIndex = PALM_VD_PORT_OUT;
    err = OMX_GetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &port_def);
    GST_DEBUG_OBJECT(self,
        "OUT port GetParameter: err=0x%x nBufferSize=%d nBufferCountActual=%d domain=%d",
        (unsigned)err, (int)port_def.nBufferSize,
        (int)port_def.nBufferCountActual, (int)port_def.eDomain);

    /* Update output dimensions for the decoder */
    port_def.format.video.nFrameWidth  = (OMX_U32)width;
    port_def.format.video.nFrameHeight = (OMX_U32)height;
    port_def.format.video.nStride      = (OMX_U32)width;
    port_def.format.video.nSliceHeight = (OMX_U32)height;
    port_def.format.video.eColorFormat = (OMX_U32)OMX_COLOR_FormatYUV420SemiPlanar;
    port_def.format.video.xFramerate   = 0; /* don't constrain */

    err = OMX_SetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &port_def);
    GST_DEBUG_OBJECT(self, "OUT port SetParameter: err=0x%x", (unsigned)err);

    /* Re-read to get actual (possibly updated) buffer size */
    port_def_init(&port_def);
    port_def.nPortIndex = PALM_VD_PORT_OUT;
    OMX_GetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &port_def);

    guint out_buf_size = port_def.nBufferSize ? port_def.nBufferSize
                         : (guint)(width * height * 3 / 2);
    guint n_out = port_def.nBufferCountActual ? port_def.nBufferCountActual : PALM_VD_N_OUT_BUFS;
    if (n_out > PALM_VD_N_OUT_BUFS) {
        GST_WARNING_OBJECT(self, "out buf count %d > array size %d, clamping — WILL HANG",
                           n_out, PALM_VD_N_OUT_BUFS);
        n_out = PALM_VD_N_OUT_BUFS;
    }

    GST_DEBUG_OBJECT(self,
        "buffers: in=%d×%d out=%d×%d", n_in, in_buf_size, n_out, out_buf_size);

    self->out_width    = width;
    self->out_height   = height;
    self->out_buf_size = out_buf_size;

    /* ── Loaded → Idle: send command, allocate buffers, wait ── */
    GST_DEBUG_OBJECT(self, "SendCommand StateSet→Idle");
    err = OMX_SendCommand(self->omx_handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
    GST_DEBUG_OBJECT(self, "SendCommand→Idle: err=0x%x", (unsigned)err);

    for (guint i = 0; i < n_in; i++) {
        err = OMX_AllocateBuffer(self->omx_handle, &self->in_bufs[i],
                                  PALM_VD_PORT_IN, self, in_buf_size);
        GST_DEBUG_OBJECT(self, "AllocateBuffer in[%d]: err=0x%x buf=%p",
                         i, (unsigned)err, (void*)(self->in_bufs[i]));
        if (err != OMX_ErrorNone) {
            GST_ERROR_OBJECT(self, "AllocateBuffer in[%d] failed: 0x%08x", i, (unsigned)err);
            return FALSE;
        }
        self->n_in_bufs++;
        g_async_queue_push(self->in_queue, self->in_bufs[i]);
    }
    for (guint i = 0; i < n_out; i++) {
        err = OMX_AllocateBuffer(self->omx_handle, &self->out_bufs[i],
                                  PALM_VD_PORT_OUT, self, out_buf_size);
        GST_DEBUG_OBJECT(self, "AllocateBuffer out[%d]: err=0x%x buf=%p",
                         i, (unsigned)err, (void*)(self->out_bufs[i]));
        if (err != OMX_ErrorNone) {
            GST_ERROR_OBJECT(self, "AllocateBuffer out[%d] failed: 0x%08x", i, (unsigned)err);
            return FALSE;
        }
        self->n_out_bufs++;
    }

    if (!wait_for_state(self, OMX_StateIdle, 3000)) {
        GST_ERROR_OBJECT(self, "FAILED to reach OMX Idle state");
        return FALSE;
    }
    GST_INFO_OBJECT(self, "OMX state: Idle");

    /* ── Idle → Executing ── */
    err = OMX_SendCommand(self->omx_handle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
    GST_DEBUG_OBJECT(self, "SendCommand→Executing: err=0x%x", (unsigned)err);

    if (!wait_for_state(self, OMX_StateExecuting, 3000)) {
        GST_ERROR_OBJECT(self, "FAILED to reach OMX Executing state");
        return FALSE;
    }
    GST_INFO_OBJECT(self, "OMX state: Executing");

    /* Submit all output buffers to the component */
    for (guint i = 0; i < self->n_out_bufs; i++) {
        err = OMX_FillThisBuffer(self->omx_handle, self->out_bufs[i]);
        GST_DEBUG_OBJECT(self, "FillThisBuffer[%d]: err=0x%x", i, (unsigned)err);
    }

    /* Set output state for downstream */
    gst_video_info_set_format(&self->vinfo, GST_VIDEO_FORMAT_NV12, width, height);
    GstVideoCodecState *out_state =
        gst_video_decoder_set_output_state(dec, GST_VIDEO_FORMAT_NV12,
                                            width, height, state);
    gst_video_codec_state_unref(out_state);

    /* Start output thread */
    self->out_thread_running = TRUE;
    self->out_thread = g_thread_new("palm-vd-out", output_thread_func, self);

    GST_INFO_OBJECT(self, "set_format OK: %dx%d comp=%s in=%d×%db out=%d×%db",
                    width, height, comp_name, n_in, in_buf_size, n_out, out_buf_size);
    return TRUE;
}

static GstFlowReturn palm_vd_handle_frame(GstVideoDecoder *dec,
                                           GstVideoCodecFrame *frame)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(dec);

    if (!self->omx_handle) {
        gst_video_codec_frame_unref(frame);
        return GST_FLOW_NOT_NEGOTIATED;
    }

    /* ── Send Annex-B SPS/PPS config once before first frame ── */
    if (self->is_avc && self->codec_config && !self->codec_config_sent) {
        OMX_BUFFERHEADERTYPE *cbuf =
            g_async_queue_timeout_pop(self->in_queue, 500000);
        if (!cbuf) {
            GST_WARNING_OBJECT(self, "no buffer for codec_config, dropping");
        } else {
            guint csz = MIN(self->codec_config_len, cbuf->nAllocLen);
            memcpy(cbuf->pBuffer, self->codec_config, csz);
            cbuf->nFilledLen = csz;
            cbuf->nOffset    = 0;
            cbuf->nTimeStamp = 0;
            cbuf->nFlags     = OMX_BUFFERFLAG_CODECCONFIG;
            GST_DEBUG_OBJECT(self, "Sending codec_config %d bytes (Annex-B SPS/PPS)", csz);
            OMX_ERRORTYPE cerr = OMX_EmptyThisBuffer(self->omx_handle, cbuf);
            if (cerr != OMX_ErrorNone) {
                GST_ERROR_OBJECT(self, "EmptyThisBuffer codec_config failed: 0x%08x", (unsigned)cerr);
                g_async_queue_push(self->in_queue, cbuf);
            }
        }
        self->codec_config_sent = TRUE;
    }

    /* Get a free input buffer — block up to 500ms */
    OMX_BUFFERHEADERTYPE *buf =
        g_async_queue_timeout_pop(self->in_queue, 500000);
    if (!buf) {
        GST_WARNING_OBJECT(self, "no free input buffer, dropping frame");
        gst_video_codec_frame_unref(frame);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    if (!gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ)) {
        g_async_queue_push(self->in_queue, buf);
        gst_video_codec_frame_unref(frame);
        return GST_FLOW_ERROR;
    }

    guint copy = MIN((guint)map.size, buf->nAllocLen);
    memcpy(buf->pBuffer, map.data, copy);
    buf->nFilledLen = copy;
    buf->nOffset    = 0;
    buf->nTimeStamp = (long long)(GST_BUFFER_PTS(frame->input_buffer) / 1000LL); /* ns → µs */
    buf->nFlags     = 0;

    /* ── AVCC → Annex B in-place conversion ── */
    if (self->is_avc) {
        guint8 *p = buf->pBuffer;
        guint   remaining = buf->nFilledLen;
        guint   nls = self->nal_length_size;
        while (remaining >= nls) {
            /* Read NAL length (big-endian) */
            guint nal_len = 0;
            for (guint i = 0; i < nls; i++)
                nal_len = (nal_len << 8) | p[i];
            /* Replace length prefix with Annex-B start code.
             * Length field is always nls bytes; start code is 4 bytes.
             * When nls < 4, zero-pad: write 0s then 0x01.
             * When nls == 4 this is a direct 4-byte → 4-byte swap. */
            if (nls == 4) {
                p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1;
            } else if (nls == 3) {
                p[0] = 0; p[1] = 0; p[2] = 1;
            } else if (nls == 2) {
                p[0] = 0; p[1] = 1;
            } else { /* nls == 1 */
                p[0] = 1;
            }
            if (remaining < nls + nal_len) break;
            p         += nls + nal_len;
            remaining -= nls + nal_len;
        }
        GST_LOG_OBJECT(self, "AVCC→AnnexB converted %d bytes", (int)buf->nFilledLen);
    }

    GST_LOG_OBJECT(self, "EmptyThisBuffer: filled=%d ts=%lld flags=0x%x",
                   (int)buf->nFilledLen, (long long)buf->nTimeStamp, (unsigned)buf->nFlags);

    gst_buffer_unmap(frame->input_buffer, &map);
    gst_video_codec_frame_unref(frame);

    OMX_ERRORTYPE err = OMX_EmptyThisBuffer(self->omx_handle, buf);
    if (err != OMX_ErrorNone) {
        GST_ERROR_OBJECT(self, "EmptyThisBuffer failed: 0x%08x", (unsigned)err);
        g_async_queue_push(self->in_queue, buf);
    }
    return GST_FLOW_OK;
}

static gboolean palm_vd_flush(GstVideoDecoder *dec)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(dec);
    /* Re-send codec_config after flush (seek re-initialises stream) */
    self->codec_config_sent = FALSE;
    if (self->omx_handle) {
        OMX_SendCommand(self->omx_handle, OMX_CommandFlush, PALM_VD_PORT_IN,  NULL);
        OMX_SendCommand(self->omx_handle, OMX_CommandFlush, PALM_VD_PORT_OUT, NULL);
        /* Drain queued buffers */
        OMX_BUFFERHEADERTYPE *b;
        while ((b = g_async_queue_try_pop(self->out_queue)) != NULL) {
            b->nFilledLen = 0; b->nOffset = 0;
            OMX_FillThisBuffer(self->omx_handle, b);
        }
    }
    return TRUE;
}

static GstFlowReturn palm_vd_finish(GstVideoDecoder *dec)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(dec);
    if (self->omx_handle && !self->eos_sent) {
        OMX_BUFFERHEADERTYPE *buf =
            g_async_queue_timeout_pop(self->in_queue, 500000);
        if (buf) {
            buf->nFilledLen = 0;
            buf->nFlags     = OMX_BUFFERFLAG_EOS;
            OMX_EmptyThisBuffer(self->omx_handle, buf);
            self->eos_sent = TRUE;
        }
    }
    return GST_FLOW_OK;
}

/* ── GObject boilerplate ────────────────────────────────────────────────── */
static void gst_palm_video_dec_finalize(GObject *obj)
{
    GstPalmVideoDec *self = GST_PALM_VIDEO_DEC(obj);
    self->out_thread_running = FALSE;
    if (self->out_thread) { g_thread_join(self->out_thread); self->out_thread = NULL; }
    G_OBJECT_CLASS(gst_palm_video_dec_parent_class)->finalize(obj);
}

static void gst_palm_video_dec_class_init(GstPalmVideoDecClass *klass)
{
    GObjectClass        *gobject = G_OBJECT_CLASS(klass);
    GstElementClass     *element = GST_ELEMENT_CLASS(klass);
    GstVideoDecoderClass *vdec   = GST_VIDEO_DECODER_CLASS(klass);

    gobject->finalize = gst_palm_video_dec_finalize;

    gst_element_class_set_static_metadata(element,
        "Palm/Qualcomm OMX Video Decoder",
        "Codec/Decoder/Video",
        "Hardware H.264/MPEG4/H.263 decoder via Qualcomm OMX IL (libOmxCore.so)",
        "Prism Browser Project");

    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);

    vdec->open         = palm_vd_open;
    vdec->close        = palm_vd_close;
    vdec->set_format   = palm_vd_set_format;
    vdec->handle_frame = palm_vd_handle_frame;
    vdec->flush        = palm_vd_flush;
    vdec->finish       = palm_vd_finish;
}

static void gst_palm_video_dec_init(GstPalmVideoDec *self)
{
    gst_video_decoder_set_packetized(GST_VIDEO_DECODER(self), TRUE);
}

/* ── Plugin entry point ─────────────────────────────────────────────────── */
static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(gst_palm_video_dec_debug, "palmvideodec", 0,
                             "Palm/Qualcomm OMX video decoder");
    return gst_element_register(plugin, "palmvideodec",
                                 GST_RANK_PRIMARY + 10,
                                 GST_TYPE_PALM_VIDEO_DEC);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    palmvideodec,
    "Palm/Qualcomm OMX hardware video decoder for HP TouchPad",
    plugin_init,
    "1.0",
    "LGPL",
    "prism-browser",
    "https://github.com/prism-browser"
)
