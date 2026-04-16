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
#include <errno.h>
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
            self->port_cmd_complete    = TRUE;
            self->port_enable_complete = TRUE;
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
        /* light_reconfig is armed after a full reset to signal that the
         * next PortSettingsChanged must be handled WITHOUT another
         * FreeHandle+GetHandle (which would loop forever). */
        if (self->light_reconfig) {
            fprintf(stderr,
                "[palmvideodec] PortSettingsChanged: post-reset → light reconfig path\n");
            /* light_reconfig flag stays TRUE — do_port_reconfig reads it */
        }
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
    GST_INFO_OBJECT(self, "EmptyBufferDone buf=%p", (void*)pBuf);
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
    GST_INFO_OBJECT(self, "FillBufferDone buf=%p filled=%d flags=0x%x",
                   (void*)pBuf, (int)pBuf->nFilledLen, (unsigned)pBuf->nFlags);
    /* Push filled output buffer to output queue */
    g_async_queue_push(self->out_queue, pBuf);
    return OMX_ErrorNone;
}

/* ── OMX IL port reconfiguration sequence ───────────────────────────────── */
/* Called from output thread when OMX_EventPortSettingsChanged fires.
 *
 * Per OMX IL spec, the correct order is:
 *   1. SendCommand(PortDisable)
 *   2. Wait for PortDisable CmdComplete  ← component returns all buffers FIRST
 *   3. FreeBuffer for all returned output buffers
 *   4. SendCommand(PortEnable)
 *   5. AllocateBuffer (same count & size)
 *   6. Wait for PortEnable CmdComplete
 *   7. FillThisBuffer for all new output buffers
 *
 * Previous attempts failed because FreeBuffer was called BEFORE PortDisable
 * CmdComplete, which freed in-flight buffers and triggered OMX_ErrorHardware. */
/* do_port_reconfig — full component reset on PortSettingsChanged.
 *
 * Qualcomm APQ8060 OMX quirk: AllocateBuffer always fails (0x80001000) after
 * FreeBuffer because pmem_adsp is held at the component level until the
 * component instance is destroyed.  PortEnable + AllocateBuffer therefore
 * never works.  Instead, after PortDisable + FreeBuffer, we destroy the old
 * component (FreeHandle) and create a fresh one (GetHandle), which gets a
 * clean pmem pool. */
static void do_port_reconfig(GstPalmVideoDec *self)
{
    OMX_ERRORTYPE err;

    /* ── Check for light-reconfig path (post full-reset PortSettingsChanged) ──
     *
     * After FreeHandle+GetHandle, the fresh component fires PortSettingsChanged
     * when it reads SPS.  We cannot do another full reset (that would loop).
     * Instead: PortDisable → drain output buffers (NO FreeBuffer) → PortEnable
     * → FillThisBuffer.  The component retains its AllocateBuffer'd buffers
     * throughout, so no AllocateBuffer is needed.
     *
     * Also flush PORT_IN to recover the stuck input SPS buffer. */
    pthread_mutex_lock(&self->reconfig_lock);
    gboolean do_light = self->light_reconfig;
    pthread_mutex_unlock(&self->reconfig_lock);

    if (do_light) {
        GST_INFO_OBJECT(self, "light reconfig: start (post-reset PortSettingsChanged)");

        /* Qualcomm APQ8060 OMX behaviour on post-reset PortSettingsChanged:
         *
         * PortDisable CmdComplete requires FreeBuffer calls — without them the
         * component never fires CmdComplete and the port stays locked.
         * (Confirmed: full-reset path fires CmdComplete mid-FreeBuffer sequence.)
         *
         * Correct OMX IL sequence for PORT_OUT PortSettingsChanged:
         *   L1.  Clear flags; send PortDisable PORT_OUT
         *   L2.  Drain out_queue (component returns bufs via FillBufferDone)
         *   L3.  FreeBuffer all returned bufs → triggers PortDisable CmdComplete
         *   L4.  Wait for PortDisable CmdComplete (port_cmd_complete)
         *   L5.  Clear port_enable_complete; send PortEnable PORT_OUT
         *   L6.  AllocateBuffer × n_out_bufs → populates port → PortEnable CmdComplete
         *   L7.  Wait for PortEnable CmdComplete (port_enable_complete)
         *   L8.  Flush PORT_IN to recover stuck SPS input buffer
         *   L9.  FillThisBuffer all new output buffers
         *   L10. Reset codec_config_sent (IN port flushed, re-send SPS)
         *   L11. Clear flags, broadcast reconfig_cond                          */

        /* ── L1. Clear flags + PortDisable ── */
        pthread_mutex_lock(&self->reconfig_lock);
        self->port_cmd_complete    = FALSE;
        self->port_enable_complete = FALSE;
        pthread_mutex_unlock(&self->reconfig_lock);

        err = OMX_SendCommand(self->omx_handle, OMX_CommandPortDisable, PALM_VD_PORT_OUT, NULL);
        GST_DEBUG_OBJECT(self, "light reconfig: PortDisable err=0x%x", (unsigned)err);

        /* ── L2. Drain output buffers (component returns them via FillBufferDone) ── */
        OMX_BUFFERHEADERTYPE *returned_bufs[PALM_VD_N_OUT_BUFS];
        guint n_returned = 0;
        {
            gint64 dl = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
            while (n_returned < self->n_out_bufs && g_get_monotonic_time() < dl) {
                OMX_BUFFERHEADERTYPE *b =
                    g_async_queue_timeout_pop(self->out_queue, 20000 /* 20ms */);
                if (b) returned_bufs[n_returned++] = b;
            }
            OMX_BUFFERHEADERTYPE *b;
            while ((b = g_async_queue_try_pop(self->out_queue)) != NULL) {
                if (n_returned < PALM_VD_N_OUT_BUFS) returned_bufs[n_returned++] = b;
            }
        }
        GST_INFO_OBJECT(self, "light reconfig: drained %d/%d output bufs", n_returned, self->n_out_bufs);

        /* ── L3. FreeBuffer all returned bufs → triggers PortDisable CmdComplete ── */
        for (guint i = 0; i < n_returned; i++) {
            OMX_ERRORTYPE ferr = OMX_FreeBuffer(self->omx_handle, PALM_VD_PORT_OUT, returned_bufs[i]);
            GST_DEBUG_OBJECT(self, "light reconfig: FreeBuffer[%d] err=0x%x", i, (unsigned)ferr);
            self->out_bufs[i] = NULL;
        }
        if (n_returned < self->n_out_bufs) {
            GST_WARNING_OBJECT(self,
                "light reconfig: only %d/%d bufs returned before FreeBuffer",
                n_returned, self->n_out_bufs);
        }

        /* ── L4. Wait for PortDisable CmdComplete ── */
        {
            gint64 dl = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
            for (;;) {
                pthread_mutex_lock(&self->reconfig_lock);
                gboolean done = self->port_cmd_complete;
                pthread_mutex_unlock(&self->reconfig_lock);
                if (done) break;
                if (g_get_monotonic_time() > dl) {
                    GST_WARNING_OBJECT(self, "light reconfig: PortDisable CmdComplete timeout");
                    break;
                }
                g_usleep(10000);
            }
            GST_INFO_OBJECT(self, "light reconfig: PortDisable CmdComplete done");
        }

        /* ── L5. PortEnable PORT_OUT ── */
        pthread_mutex_lock(&self->reconfig_lock);
        self->port_enable_complete = FALSE;
        pthread_mutex_unlock(&self->reconfig_lock);

        err = OMX_SendCommand(self->omx_handle, OMX_CommandPortEnable, PALM_VD_PORT_OUT, NULL);
        GST_DEBUG_OBJECT(self, "light reconfig: PortEnable err=0x%x", (unsigned)err);

        /* ── L6. AllocateBuffer × n_out_bufs → populates port → PortEnable CmdComplete ──
         * Per OMX IL spec: AllocateBuffer during PortEnable populates the port;
         * PortEnable CmdComplete fires when all buffers are allocated. */
        {
            OMX_PARAM_PORTDEFINITIONTYPE pd_out;
            port_def_init(&pd_out);
            pd_out.nPortIndex = PALM_VD_PORT_OUT;
            OMX_GetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &pd_out);
            guint n_new = pd_out.nBufferCountActual ? pd_out.nBufferCountActual : self->n_out_bufs;
            guint sz    = pd_out.nBufferSize         ? pd_out.nBufferSize         : self->out_buf_size;
            /* Update stride/slice_height from new port definition */
            if (pd_out.format.video.nStride)
                self->out_stride       = pd_out.format.video.nStride;
            if (pd_out.format.video.nSliceHeight)
                self->out_slice_height = pd_out.format.video.nSliceHeight;
            GST_INFO_OBJECT(self, "light reconfig: out port n=%u size=%u stride=%u slice_h=%u",
                            n_new, sz, self->out_stride, self->out_slice_height);

            guint n_alloc = 0;
            for (guint i = 0; i < n_new && i < PALM_VD_N_OUT_BUFS; i++) {
                OMX_BUFFERHEADERTYPE *hdr = NULL;
                OMX_ERRORTYPE aerr = OMX_AllocateBuffer(self->omx_handle, &hdr,
                                                        PALM_VD_PORT_OUT, self, sz);
                GST_DEBUG_OBJECT(self, "light reconfig: AllocateBuffer out[%d] err=0x%x",
                                 i, (unsigned)aerr);
                if (aerr == OMX_ErrorNone && hdr) {
                    self->out_bufs[i] = hdr;
                    n_alloc++;
                }
            }
            self->n_out_bufs  = n_alloc;
            self->out_buf_size = sz;
            GST_INFO_OBJECT(self, "light reconfig: allocated %d/%d output bufs", n_alloc, n_new);

            if (n_alloc == 0) {
                GST_ERROR_OBJECT(self,
                    "light reconfig: AllocateBuffer failed for all output bufs — cannot recover");
                pthread_mutex_lock(&self->reconfig_lock);
                self->light_reconfig = FALSE;
                self->port_reconfig  = FALSE;
                pthread_cond_broadcast(&self->reconfig_cond);
                pthread_mutex_unlock(&self->reconfig_lock);
                return;
            }
        }

        /* ── L7. Wait for PortEnable CmdComplete ── */
        {
            gint64 dl = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
            for (;;) {
                pthread_mutex_lock(&self->reconfig_lock);
                gboolean done = self->port_enable_complete;
                pthread_mutex_unlock(&self->reconfig_lock);
                if (done) break;
                if (g_get_monotonic_time() > dl) {
                    GST_WARNING_OBJECT(self, "light reconfig: PortEnable CmdComplete timeout");
                    break;
                }
                g_usleep(10000);
            }
            GST_INFO_OBJECT(self, "light reconfig: PortEnable CmdComplete done");
        }

        /* ── L8. Flush PORT_IN to recover stuck SPS input buffer ── */
        OMX_SendCommand(self->omx_handle, OMX_CommandFlush, PALM_VD_PORT_IN, NULL);
        g_usleep(80000);

        /* ── L9. FillThisBuffer all new output buffers ── */
        guint fill_count = 0;
        for (guint i = 0; i < self->n_out_bufs; i++) {
            err = OMX_FillThisBuffer(self->omx_handle, self->out_bufs[i]);
            GST_DEBUG_OBJECT(self, "light reconfig: FillThisBuffer[%d] err=0x%x",
                             i, (unsigned)err);
            if (err == OMX_ErrorNone) fill_count++;
        }

        /* ── L10. Force SPS re-send (IN port flushed, component needs SPS again) ── */
        self->codec_config_sent = FALSE;

        /* ── L11. Clear flags ── */
        pthread_mutex_lock(&self->reconfig_lock);
        self->light_reconfig       = FALSE;
        self->port_reconfig        = FALSE;
        self->port_cmd_complete    = FALSE;
        self->port_enable_complete = FALSE;
        pthread_cond_broadcast(&self->reconfig_cond);
        pthread_mutex_unlock(&self->reconfig_lock);

        GST_INFO_OBJECT(self, "light reconfig: complete — %d bufs resubmitted", fill_count);
        return;
    }

    GST_DEBUG_OBJECT(self, "port reconfig: start (full reset)");

    /* ── 0. Save actual output geometry from CURRENT component before destroy.
     *
     * After PortSettingsChanged, the component has parsed SPS and knows the
     * real output dimensions.  Read them NOW so we can configure the fresh
     * component identically — if we wait until after GetHandle, we only get
     * hardware defaults (1920×1088) which cause another PortSettingsChanged. ── */
    {
        OMX_PARAM_PORTDEFINITIONTYPE pd_pre;
        port_def_init(&pd_pre);
        pd_pre.nPortIndex = PALM_VD_PORT_OUT;
        if (OMX_GetParameter(self->omx_handle,
                             OMX_IndexParamPortDefinition, &pd_pre) == OMX_ErrorNone
            && pd_pre.format.video.nFrameWidth
            && pd_pre.format.video.nFrameHeight) {
            GST_DEBUG_OBJECT(self, "port reconfig: pre-destroy dims %dx%d",
                             (int)pd_pre.format.video.nFrameWidth,
                             (int)pd_pre.format.video.nFrameHeight);
            self->out_width  = (gint)pd_pre.format.video.nFrameWidth;
            self->out_height = (gint)pd_pre.format.video.nFrameHeight;
        }
    }

    /* ── 1. Disable output port ── */
    pthread_mutex_lock(&self->reconfig_lock);
    self->port_cmd_complete = FALSE;
    pthread_mutex_unlock(&self->reconfig_lock);

    err = OMX_SendCommand(self->omx_handle, OMX_CommandPortDisable, PALM_VD_PORT_OUT, NULL);
    GST_DEBUG_OBJECT(self, "port reconfig: PortDisable cmd err=0x%x", (unsigned)err);

    /* ── 2. Interleaved: drain FillBufferDone + FreeBuffer until CmdComplete ── */
    {
        gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
        pthread_mutex_lock(&self->reconfig_lock);
        while (!self->port_cmd_complete) {
            struct timespec ts20ms;
            clock_gettime(CLOCK_REALTIME, &ts20ms);
            ts20ms.tv_nsec += 20000000;
            if (ts20ms.tv_nsec >= 1000000000) { ts20ms.tv_sec++; ts20ms.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&self->reconfig_cond, &self->reconfig_lock, &ts20ms);
            pthread_mutex_unlock(&self->reconfig_lock);

            OMX_BUFFERHEADERTYPE *b;
            while ((b = g_async_queue_try_pop(self->out_queue)) != NULL) {
                for (guint i = 0; i < PALM_VD_N_OUT_BUFS; i++) {
                    if (self->out_bufs[i] == b) {
                        err = OMX_FreeBuffer(self->omx_handle, PALM_VD_PORT_OUT, b);
                        GST_DEBUG_OBJECT(self, "port reconfig: FreeBuffer out[%d] err=0x%x", i, (unsigned)err);
                        self->out_bufs[i] = NULL;
                        if (self->n_out_bufs > 0) self->n_out_bufs--;
                        break;
                    }
                }
            }

            if (g_get_monotonic_time() > deadline) {
                GST_WARNING_OBJECT(self, "port reconfig: PortDisable timeout, force-freeing");
                for (guint i = 0; i < PALM_VD_N_OUT_BUFS; i++) {
                    if (self->out_bufs[i]) {
                        OMX_FreeBuffer(self->omx_handle, PALM_VD_PORT_OUT, self->out_bufs[i]);
                        self->out_bufs[i] = NULL;
                    }
                }
                self->n_out_bufs = 0;
                break;
            }
            pthread_mutex_lock(&self->reconfig_lock);
        }
        if (self->port_cmd_complete) {
            self->port_cmd_complete = FALSE;
            pthread_mutex_unlock(&self->reconfig_lock);
        }
    }
    for (guint i = 0; i < PALM_VD_N_OUT_BUFS; i++) {
        if (self->out_bufs[i]) {
            OMX_FreeBuffer(self->omx_handle, PALM_VD_PORT_OUT, self->out_bufs[i]);
            self->out_bufs[i] = NULL;
        }
    }
    self->n_out_bufs = 0;
    GST_DEBUG_OBJECT(self, "port reconfig: PortDisable done");

    /* ── 3. Flush PORT_IN to recover any in-flight input buffers ── */
    OMX_SendCommand(self->omx_handle, OMX_CommandFlush, PALM_VD_PORT_IN, NULL);
    GST_DEBUG_OBJECT(self, "port reconfig: Flush PORT_IN sent");
    g_usleep(80000); /* 80 ms — let EmptyBufferDone push to in_queue */

    /* ── 4. FreeBuffer all input buffers ── */
    {
        OMX_BUFFERHEADERTYPE *b;
        while ((b = g_async_queue_try_pop(self->in_queue)) != NULL) {
            for (guint i = 0; i < PALM_VD_N_IN_BUFS; i++) {
                if (self->in_bufs[i] == b) {
                    OMX_FreeBuffer(self->omx_handle, PALM_VD_PORT_IN, b);
                    self->in_bufs[i] = NULL;
                    break;
                }
            }
        }
        for (guint i = 0; i < PALM_VD_N_IN_BUFS; i++) {
            if (self->in_bufs[i]) {
                OMX_FreeBuffer(self->omx_handle, PALM_VD_PORT_IN, self->in_bufs[i]);
                self->in_bufs[i] = NULL;
            }
        }
        self->n_in_bufs = 0;
    }

    /* ── 5. Destroy old component (releases all pmem_adsp) ── */
    self->omx_FreeHandle(self->omx_handle);
    self->omx_handle = NULL;
    GST_INFO_OBJECT(self, "port reconfig: old component destroyed");

    /* ── 6. Create fresh component ── */
    self->omx_state = OMX_StateLoaded;
    self->omx_error = OMX_ErrorNone;
    err = self->omx_GetHandle(&self->omx_handle,
                               self->comp_name, self, &self->omx_cbs);
    if (err != OMX_ErrorNone || !self->omx_handle) {
        GST_ERROR_OBJECT(self, "port reconfig: GetHandle(%s) failed: 0x%x",
                         self->comp_name, (unsigned)err);
        pthread_mutex_lock(&self->reconfig_lock);
        self->port_reconfig = FALSE;
        pthread_mutex_unlock(&self->reconfig_lock);
        return;
    }
    GST_INFO_OBJECT(self, "port reconfig: fresh component handle=%p", (void*)self->omx_handle);

    /* ── 7. Query new component's port parameters ── */
    OMX_PARAM_PORTDEFINITIONTYPE pd_out;
    port_def_init(&pd_out);
    pd_out.nPortIndex = PALM_VD_PORT_OUT;
    OMX_GetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &pd_out);
    /* NOTE: do NOT update out_width/out_height from the fresh component.
     * A freshly GetHandle'd component returns hardware defaults (1920×1088),
     * NOT the SPS-decoded geometry.  We saved the correct dims from the
     * pre-destroy component above (step 0) — use those instead. */
    guint new_n_out    = (pd_out.nBufferCountActual && pd_out.nBufferCountActual <= PALM_VD_N_OUT_BUFS)
                         ? pd_out.nBufferCountActual : 6u;
    guint new_buf_size = pd_out.nBufferSize ? pd_out.nBufferSize : self->out_buf_size;

    /* Set CORRECT dims (from step 0, not the fresh component's defaults) */
    pd_out.format.video.nFrameWidth  = (OMX_U32)self->out_width;
    pd_out.format.video.nFrameHeight = (OMX_U32)self->out_height;
    GST_DEBUG_OBJECT(self, "port reconfig: configuring fresh component for %dx%d",
                     self->out_width, self->out_height);
    OMX_SetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &pd_out);

    OMX_PARAM_PORTDEFINITIONTYPE pd_in;
    port_def_init(&pd_in);
    pd_in.nPortIndex = PALM_VD_PORT_IN;
    OMX_GetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &pd_in);
    guint new_n_in   = pd_in.nBufferCountActual ? pd_in.nBufferCountActual : 2u;
    if (new_n_in > PALM_VD_N_IN_BUFS) new_n_in = PALM_VD_N_IN_BUFS;
    guint new_in_sz  = pd_in.nBufferSize ? pd_in.nBufferSize : 1048576u;

    GST_DEBUG_OBJECT(self, "port reconfig: fresh in=%d×%db out=%d×%db dim=%dx%d",
                     new_n_in, new_in_sz, new_n_out, new_buf_size,
                     self->out_width, self->out_height);

    /* ── 8. Loaded → Idle: allocate all buffers ── */
    OMX_SendCommand(self->omx_handle, OMX_CommandStateSet, OMX_StateIdle, NULL);

    for (guint i = 0; i < new_n_in; i++) {
        err = OMX_AllocateBuffer(self->omx_handle, &self->in_bufs[i],
                                  PALM_VD_PORT_IN, self, new_in_sz);
        GST_DEBUG_OBJECT(self, "port reconfig: alloc in[%d] err=0x%x", i, (unsigned)err);
        if (err == OMX_ErrorNone) self->n_in_bufs++;
        else self->in_bufs[i] = NULL;
    }
    for (guint i = 0; i < new_n_out; i++) {
        err = OMX_AllocateBuffer(self->omx_handle, &self->out_bufs[i],
                                  PALM_VD_PORT_OUT, self, new_buf_size);
        GST_DEBUG_OBJECT(self, "port reconfig: alloc out[%d] err=0x%x", i, (unsigned)err);
        if (err == OMX_ErrorNone) self->n_out_bufs++;
        else self->out_bufs[i] = NULL;
    }
    self->out_buf_size = new_buf_size;
    wait_for_state(self, OMX_StateIdle, 3000);
    GST_INFO_OBJECT(self, "port reconfig: Idle — in=%d/%d out=%d/%d",
                    self->n_in_bufs, new_n_in, self->n_out_bufs, new_n_out);

    /* ── 9. Idle → Executing ── */
    OMX_SendCommand(self->omx_handle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
    wait_for_state(self, OMX_StateExecuting, 3000);

    /* ── 10. Submit output buffers + release input buffers to pipeline ── */
    for (guint i = 0; i < self->n_out_bufs; i++)
        OMX_FillThisBuffer(self->omx_handle, self->out_bufs[i]);
    for (guint i = 0; i < self->n_in_bufs; i++)
        g_async_queue_push(self->in_queue, self->in_bufs[i]);

    /* Decoder state reset — resend SPS/PPS with next frame */
    self->codec_config_sent = FALSE;
    self->eos_sent = FALSE;

    pthread_mutex_lock(&self->state_lock);
    self->omx_error = OMX_ErrorNone;
    pthread_mutex_unlock(&self->state_lock);

    pthread_mutex_lock(&self->reconfig_lock);
    /* Arm light_reconfig: the fresh component will fire PortSettingsChanged
     * once more when it reads the SPS we re-send.  That event must NOT
     * trigger another full FreeHandle+GetHandle reset (infinite loop).
     * The output thread will call do_port_reconfig which checks light_reconfig
     * and takes the PortDisable+drain+PortEnable path instead. */
    self->light_reconfig          = TRUE;
    self->port_reconfig           = FALSE;
    self->port_cmd_complete       = FALSE;
    /* Signal handle_frame (which is polling port_reconfig) that the reset
     * is done and that it must re-negotiate caps with the new dimensions. */
    self->need_output_state_update = TRUE;
    pthread_cond_broadcast(&self->reconfig_cond);
    pthread_mutex_unlock(&self->reconfig_lock);

    GST_INFO_OBJECT(self, "port reconfig: complete (full reset) out=%d×%db",
                    self->n_out_bufs, new_buf_size);
}

/* ── Qualcomm NV12T de-tiler (APQ8060) ──────────────────────────────────── */
/* Tiles are 64×32. Pairs of tile rows share a block of (tiles_w * 2) tiles.
 * Within each block, column-pairs of 4 tiles alternate which row leads:
 *   even col_pair: even row first  (T, T, T', T')
 *   odd  col_pair: odd  row first  (T', T', T, T)
 *
 * Buffer index → logical (tx, ty):
 *   block_size = tiles_w * 2
 *   pair_k   = idx / block_size
 *   P        = idx % block_size
 *   col_pair = P / 4
 *   within   = P % 4
 *   tx       = 2*col_pair + (within % 2)
 *   row_sel  = even_col_pair ? (within<2 ? 0 : 1) : (within<2 ? 1 : 0)
 *   ty       = 2*pair_k + row_sel
 *
 * Same ordering for both Y and UV planes.
 */

/* stride = output row stride in bytes (may differ from W due to alignment) */
static void detile_qcom_plane(const guint8 *src, guint8 *dst,
                               guint W, guint H, guint stride,
                               guint tiles_w, guint tiles_h)
{
    const guint tile_size  = 64u * 32u;
    const guint block_size = tiles_w * 2u;

    /* Qualcomm packs only VALID tiles into the buffer — invalid boustrophedon
     * positions (ty >= tiles_h) are omitted.  We must iterate the full logical
     * block range (rounded up to complete 2-row pairs) but use a separate
     * buf_idx that only advances for valid tiles so the buffer read pointer
     * stays correct.  This matters when tiles_h is odd: the last pair-block
     * has valid tiles interleaved with invalid ty positions in the boustrophedon
     * sequence; using the logical idx directly would read the wrong packed slot. */
    const guint total_logical = ((tiles_h + 1u) / 2u) * block_size;
    guint buf_idx = 0;

    for (guint idx = 0; idx < total_logical; idx++) {
        guint pair_k   = idx / block_size;
        guint P        = idx % block_size;
        guint col_pair = P / 4u;
        guint within   = P % 4u;
        guint tx       = 2u * col_pair + (within % 2u);
        guint row_sel  = (col_pair % 2u == 0u)
                         ? (within < 2u ? 0u : 1u)
                         : (within < 2u ? 1u : 0u);
        guint ty = 2u * pair_k + row_sel;

        if (tx >= tiles_w || ty >= tiles_h) continue;  /* invalid — no buf_idx bump */

        const guint8 *tile = src + buf_idx * tile_size;
        buf_idx++;
        for (guint row = 0; row < 32u; row++) {
            guint y = ty * 32u + row;
            if (y >= H) continue;
            guint x0     = tx * 64u;
            guint copy_w = (x0 + 64u <= W) ? 64u : (W - x0);
            memcpy(dst + y * stride + x0, tile + row * 64u, copy_w);
        }
    }
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

        GST_INFO_OBJECT(self, "output thread: got buf=%p filled=%d flags=0x%x ts=%lld",
                       (void*)buf, (int)buf->nFilledLen, (unsigned)buf->nFlags,
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
                    GstVideoFrame vframe;
                    if (gst_video_frame_map(&vframe, &self->vinfo,
                                            frame->output_buffer, GST_MAP_WRITE)) {
                        /* Qualcomm APQ8060 OMX decoder outputs NV12T tiled format.
                         * Use GstVideoFrame to get proper plane pointers and strides
                         * (GStreamer may align stride beyond width). */
                        guint w  = (guint)self->out_width;
                        guint h  = (guint)self->out_height;

                        guint8 *dst_y    = GST_VIDEO_FRAME_PLANE_DATA(&vframe, 0);
                        guint8 *dst_uv   = GST_VIDEO_FRAME_PLANE_DATA(&vframe, 1);
                        guint   stride_y = (guint)GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 0);
                        guint   stride_uv= (guint)GST_VIDEO_FRAME_PLANE_STRIDE(&vframe, 1);

                        enum { TW = 64, TH = 32 };
                        guint tiles_w    = (w + TW - 1) / TW;
                        guint tile_size  = (guint)(TW * TH);
                        /* Compute tile counts from actual frame dimensions.
                         * The old (total*2+2)/3 approximation broke for heights
                         * not divisible by 64 (e.g. 480×270 → tiles_h_y=9,
                         * tiles_h_uv=5, but formula gave 10 and 4). */
                        guint tiles_h_y  = (h          + TH - 1) / TH;
                        guint tiles_h_uv = (h / 2u     + TH - 1) / TH;
                        guint expected_len = tiles_w * (tiles_h_y + tiles_h_uv) * tile_size;
                        /* Drop frames that are too small — partial/garbage during
                         * OMX startup or after a seek. */
                        if (buf->nFilledLen < expected_len) {
                            GST_WARNING_OBJECT(self,
                                "dropping short frame: nFilledLen=%u expected=%u",
                                (guint)buf->nFilledLen, expected_len);
                            gst_video_frame_unmap(&vframe);
                            gst_video_codec_frame_unref(frame);
                            buf->nFilledLen = 0; buf->nOffset = 0;
                            OMX_FillThisBuffer(self->omx_handle, buf);
                            buf = NULL;
                            continue;
                        }
                        GST_LOG_OBJECT(self,
                            "NV12T detile: w=%u h=%u stride_y=%u stride_uv=%u "
                            "tiles_w=%u tiles_h_y=%u tiles_h_uv=%u nFilledLen=%u",
                            w, h, stride_y, stride_uv,
                            tiles_w, tiles_h_y, tiles_h_uv, (guint)buf->nFilledLen);

                        const guint8 *src = buf->pBuffer + buf->nOffset;

                        /* Clear to neutral — prevents stale tile data from prior video */
                        memset(dst_y,  16,  stride_y  * h);
                        memset(dst_uv, 128, stride_uv * (h / 2u));

                        /* De-tile Y and UV planes (Qualcomm NV12T boustrophedon) */
                        detile_qcom_plane(src, dst_y, w, h, stride_y,
                                          tiles_w, tiles_h_y);
                        const guint8 *src_uv = src + tiles_w * tiles_h_y * tile_size;
                        detile_qcom_plane(src_uv, dst_uv, w, h / 2u, stride_uv,
                                          tiles_w, tiles_h_uv);

                        gst_video_frame_unmap(&vframe);
                    }
                    GST_BUFFER_PTS(frame->output_buffer) =
                        (GstClockTime)((guint64)buf->nTimeStamp * 1000ULL); /* µs → ns */
                }
                /* Return OMX buf BEFORE finish_frame — finish_frame may block
                 * 40-500ms on clock sync. Data already memcpy'd into GstBuffer.
                 * Returning early keeps component fed and prevents stall. */
                buf->nFilledLen = 0;
                buf->nOffset    = 0;
                OMX_FillThisBuffer(self->omx_handle, buf);
                buf = NULL; /* already recycled */
                gst_video_decoder_finish_frame(dec, frame);
            }
        }

        /* Recycle output buffer if not already returned above */
        if (buf) {
            buf->nFilledLen = 0;
            buf->nOffset    = 0;
            OMX_FillThisBuffer(self->omx_handle, buf);
        }
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
    g_strlcpy(self->comp_name, comp_name, sizeof(self->comp_name));

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

    /* Update output dimensions — do NOT force nStride/nSliceHeight.
     * The Qualcomm OMX component pads stride to hardware alignment (typically
     * 128 bytes). If we force stride=width the component ignores it and uses
     * its own alignment, but the buffer size calculation uses our forced value,
     * causing a mismatch. Let OMX decide stride, then read it back. */
    port_def.format.video.nFrameWidth  = (OMX_U32)width;
    port_def.format.video.nFrameHeight = (OMX_U32)height;
    port_def.format.video.nStride      = 0; /* let OMX pick aligned stride */
    port_def.format.video.nSliceHeight = 0; /* let OMX pick aligned height */
    port_def.format.video.eColorFormat = (OMX_U32)OMX_COLOR_FormatYUV420SemiPlanar;
    port_def.format.video.xFramerate   = 0; /* don't constrain */

    err = OMX_SetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &port_def);
    GST_DEBUG_OBJECT(self, "OUT port SetParameter: err=0x%x", (unsigned)err);

    /* Re-read to get actual (hardware-aligned) stride and slice height */
    port_def_init(&port_def);
    port_def.nPortIndex = PALM_VD_PORT_OUT;
    OMX_GetParameter(self->omx_handle, OMX_IndexParamPortDefinition, &port_def);

    /* Use width/height as stride defaults. The Qualcomm OMX component reports
     * stride=1920/slice_h=1088 here regardless of actual video dimensions —
     * these are the pre-PortSettingsChanged values and are wrong. The correct
     * values arrive via light_reconfig (PortSettingsChanged). Using width/height
     * as safe defaults ensures pre-reconfig frames are copied correctly. */
    self->out_stride       = (guint)width;
    self->out_slice_height = (guint)height;
    GST_INFO_OBJECT(self, "OMX OUT stride=%d slice_height=%d (frame %dx%d) [OMX reported stride=%d slice_h=%d, ignored until light_reconfig]",
                    self->out_stride, self->out_slice_height, width, height,
                    (int)port_def.format.video.nStride, (int)port_def.format.video.nSliceHeight);

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

    /* Wait for any active port reconfiguration to complete.
     *
     * During a full OMX reset (FreeHandle + GetHandle), port_reconfig is TRUE.
     * Previously we dropped frames here, which caused "no valid frames decoded"
     * EOS errors because ALL frames were discarded before decoding could begin.
     *
     * handle_frame holds the GStreamer stream lock.  do_port_reconfig runs in
     * the output thread WITHOUT needing the stream lock, so polling here is
     * safe — no deadlock risk.  EOS processing also needs the stream lock, so
     * it blocks here too, which is the desired behaviour: EOS is deferred until
     * all frames have been fed to the reset OMX component. */
    {
        gint64 wait_until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
        for (;;) {
            pthread_mutex_lock(&self->reconfig_lock);
            gboolean active = self->port_reconfig;
            pthread_mutex_unlock(&self->reconfig_lock);
            if (!active) break;
            if (g_get_monotonic_time() > wait_until) {
                GST_ERROR_OBJECT(self, "port reconfig wait timeout, dropping frame");
                gst_video_codec_frame_unref(frame);
                return GST_FLOW_ERROR;
            }
            g_usleep(20000); /* poll every 20 ms */
        }
    }

    /* If do_port_reconfig updated output dimensions, re-negotiate caps.
     * We are still holding the stream lock here, which is required for
     * gst_video_decoder_set_output_state / gst_video_decoder_negotiate. */
    if (self->need_output_state_update) {
        self->need_output_state_update = FALSE;
        GstVideoCodecState *new_st = gst_video_decoder_set_output_state(
            dec, GST_VIDEO_FORMAT_NV12,
            (guint)self->out_width, (guint)self->out_height, NULL);
        gst_video_codec_state_unref(new_st);
        gst_video_decoder_negotiate(dec);
        GST_INFO_OBJECT(self, "handle_frame: output state updated to %dx%d",
                        self->out_width, self->out_height);
    }

    if (!self->omx_handle) {
        gst_video_codec_frame_unref(frame);
        return GST_FLOW_NOT_NEGOTIATED;
    }

    /* Get a free input buffer — block up to 500ms.
     * Release the stream lock while waiting so the output thread can call
     * get_oldest_frame / finish_frame, draining output bufs back to OMX.
     * Without this, the output thread starves: handle_frame holds the lock
     * for 500ms intervals, OMX stalls on full output bufs, EmptyBufferDone
     * stops, and in_queue drains → "no free input buffer" loop. */
    GST_VIDEO_DECODER_STREAM_UNLOCK(dec);
    OMX_BUFFERHEADERTYPE *buf =
        g_async_queue_timeout_pop(self->in_queue, 500000);
    GST_VIDEO_DECODER_STREAM_LOCK(dec);
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

    /* ── Prepend Annex-B SPS/PPS inline to first frame ──────────────────
     * Sending a separate CODECCONFIG buffer causes the Qualcomm OMX decoder
     * to stall (EmptyBufferDone never fires, OMX_ErrorHardware follows).
     * Instead, prepend the SPS/PPS directly before the first frame's NAL
     * data — this is what Android's MediaCodec does with Qualcomm decoders. */
    guint prefix_len = 0;
    if (self->is_avc && self->codec_config && !self->codec_config_sent) {
        prefix_len = self->codec_config_len;
        self->codec_config_sent = TRUE;
        GST_DEBUG_OBJECT(self, "prepending %d bytes Annex-B SPS/PPS to first frame", prefix_len);
    }

    guint frame_sz = (guint)map.size;
    guint copy = MIN(prefix_len + frame_sz, buf->nAllocLen);
    if (prefix_len > 0 && prefix_len < buf->nAllocLen) {
        memcpy(buf->pBuffer, self->codec_config, prefix_len);
        guint frame_copy = MIN(frame_sz, buf->nAllocLen - prefix_len);
        memcpy(buf->pBuffer + prefix_len, map.data, frame_copy);
        copy = prefix_len + frame_copy;
    } else {
        memcpy(buf->pBuffer, map.data, copy);
    }
    buf->nFilledLen = copy;
    buf->nOffset    = 0;
    buf->nTimeStamp = (long long)(GST_BUFFER_PTS(frame->input_buffer) / 1000LL); /* ns → µs */
    buf->nFlags     = 0;

    /* ── AVCC → Annex B in-place conversion ── */
    if (self->is_avc) {
        /* Skip over the prepended SPS/PPS prefix (already Annex B) */
        guint8 *p = buf->pBuffer + prefix_len;
        guint   remaining = buf->nFilledLen - prefix_len;
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

    GST_INFO_OBJECT(self, "EmptyThisBuffer: buf=%p filled=%d ts=%lld flags=0x%x",
                   (void*)buf, (int)buf->nFilledLen, (long long)buf->nTimeStamp, (unsigned)buf->nFlags);

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
