/* Minimal GStreamer 1.0 pipeline runner for webOS testing.
 * Usage: gst-test <pipeline-description>
 * E.g.:  gst-test "filesrc location=/media/internal/test.h264 ! h264parse ! palmvideodec ! fakesink sync=true"
 */
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar  *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        fprintf(stderr, "ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
        if (dbg) fprintf(stderr, "  debug: %s\n", dbg);
        g_error_free(err);
        g_free(dbg);
        g_main_loop_quit(loop);
        break;
    }
    case GST_MESSAGE_EOS:
        fprintf(stderr, "EOS reached\n");
        g_main_loop_quit(loop);
        break;
    case GST_MESSAGE_STATE_CHANGED:
        /* ignore per-element state changes */
        break;
    case GST_MESSAGE_WARNING: {
        GError *err = NULL;
        gchar  *dbg = NULL;
        gst_message_parse_warning(msg, &err, &dbg);
        fprintf(stderr, "WARNING from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
        g_error_free(err);
        g_free(dbg);
        break;
    }
    default:
        break;
    }
    return TRUE;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pipeline>\n", argv[0]);
        return 1;
    }

    /* join args into one string */
    size_t len = 0;
    for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 1;
    char *desc = malloc(len + 1);
    desc[0] = '\0';
    for (int i = 1; i < argc; i++) {
        strcat(desc, argv[i]);
        if (i < argc - 1) strcat(desc, " ");
    }

    gst_init(NULL, NULL);

    GError    *err  = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);
    free(desc);

    if (!pipeline || err) {
        fprintf(stderr, "Failed to parse pipeline: %s\n", err ? err->message : "unknown");
        return 1;
    }

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GstBus    *bus  = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, loop);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "Failed to set pipeline to PLAYING\n");
        gst_object_unref(pipeline);
        return 1;
    }
    fprintf(stderr, "Pipeline PLAYING (state change: %s)\n",
            ret == GST_STATE_CHANGE_ASYNC ? "ASYNC" : "SUCCESS");

    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
}
