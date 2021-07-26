/**
 *  Copyright 2021 SK Telecom Co., Ltd.
 *    Author: Jeongseok Kim <jeongseok.kim@sk.com>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include <gst/gst.h>
#include <glib.h>
#include <gst/gstpad.h>

 /**
  */

  /**
   * Sender Pipeline:
   *
   * +--------+     +---------+     +---------+
   * | Video  | --> | Video   | --> | RTP Pay | --+
   * | Source |     | Encoder |     | (Video) |   |
   * +--------+     +---------+     +---------+   |   +---------+     +----------+
   *                                              +-> | RTP Mux | --> | SRT Sink |
   * +--------+                     +---------+   |   +---------+     +----------+
   * | App    | ------------------> | RTP Pay | --+
   * | Source |                     | (Text)  |
   * +--------+                     +---------+
   *
   *
   *
   *
   *
   * Receiver Pipeline:
   *                                 +-----------+     +---------+     +-------+
   *                             +-> | RTP Depay | --> | Video   | --> | Video |
   *  +--------+     +-------+   |   |  (Video)  |     | Decoder |     | Sink  |
   *  | SRT    | --> | RTP   | --+   +-----------+     +---------+     +-------+
   *  | Source |     | Demux |   |
   *  +--------+     +-------+   |   +-----------+                     +------+
   *                             +-> | RTP Depay | ------------------> | App  |
   *                                 |   (Text)  |                     | Sink |
   *                                 +-----------+                     +------+
   */

static struct
{
    const gchar* uri;
    const gchar* user;
    const gchar* resource;
} options;

static struct
{
    GstElement* pipeline;
    GMainLoop* loop;
} app;

static gboolean
_parse_rest_arg_cb(const gchar* option_name, const gchar* value,
    gpointer data, GError** error)
{
    if (options.uri != NULL) {
        g_printerr("URI has already been set.\n");
        return FALSE;
    }

    if (!g_str_has_prefix(value, "srt://")) {
        g_printerr("Invalid SRT uri: %s\n", value);
        return FALSE;
    }

    options.uri = g_strdup(value);

    return TRUE;
}

static gchar*
build_streamid(const gchar* u, const gchar* r)
{
    g_autofree gchar* streamid = NULL;

    g_autofree gchar* u_tag = NULL;
    g_autofree gchar* r_tag = NULL;

    if (u != NULL)
        u_tag = g_strdup_printf("u=%s", u);
    if (r != NULL)
        r_tag = g_strdup_printf("r=%s", r);

    if (u || r) {

        /* *INDENT-OFF* */
        streamid = g_strconcat("#!::",
            u_tag != NULL ? u_tag : "",
            u && r ? "," : "",
            r_tag != NULL ? r_tag : "",
            NULL);
        /* *INDENT-ON* */

    }

    return (gchar *) g_steal_pointer(&streamid);
}

static gboolean
_bus_watch(GstBus* bus, GstMessage* message, gpointer user_data)
{
    switch (message->type) {
    case GST_MESSAGE_EOS:
    case GST_MESSAGE_ERROR:
        g_printerr("Terminated\n");
        g_main_loop_quit(app.loop);
        break;
    default:
        break;
    }

    return G_SOURCE_CONTINUE;
}

static GstCaps*
_request_pt_map_cb(GstElement* demux, guint pt, gpointer user_data)
{
    GstCaps* caps = NULL;

    if (pt == 96) {
        caps =
            gst_caps_from_string
            ("application/x-rtp, encoding-name=(string)H264, media=(string)video, clock-rate=(int)90000");
    }
    else if (pt == 99) {
        /* We are going to use '99' for 'text/x-raw'. */
        caps =
            gst_caps_from_string
            ("application/x-rtp, encoding-name=(string)X-GST, media=(string)application, clock-rate=(int)90000");
    }
    else {
        /* Must be handled as error status */
        g_print("wrong! pt\n");
    }
    return caps;
}

static GstPadProbeReturn
_link_cb(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    GstElement* pipeline = (GstElement * ) user_data;

    GstElement* sinkbin = NULL;

    g_autoptr(GstElement) first = NULL;
    /*
      g_autoptr (GstPad) gpad = NULL;
      g_autoptr (GstPad) qpad = NULL;
    */
    g_autoptr(GstPad) qpad = NULL;
    GstPad* gpad = NULL;

    g_print("pad link probe : %s\n", GST_PAD_NAME(pad));

    sinkbin =
        gst_parse_launch
        ("queue name=q ! rtph264depay ! h264parse ! decodebin ! autovideosink async=true",
            NULL);

    gst_bin_add(GST_BIN(pipeline), sinkbin);

    first = gst_bin_get_by_name(GST_BIN(sinkbin), "q");
    qpad = gst_element_get_static_pad(first, "sink");

    gpad = gst_ghost_pad_new(NULL, qpad);
    gst_element_add_pad(sinkbin, gpad);


    if (gst_pad_link(pad, gpad) != GST_PAD_LINK_OK) {
        g_error("failed to link pad to gpad");
    }

    gst_pad_set_active(gpad, TRUE);
    gst_element_sync_state_with_parent(sinkbin);

    g_print("linking done for video\n");

    return GST_PAD_PROBE_REMOVE;
}

static GstPadProbeReturn
_link_gstdepay_cb(GstPad* pad, GstPadProbeInfo* info, gpointer user_data)
{
    GstElement* pipeline =  (GstElement *)user_data;
    GstElement* sinkbin = NULL;

    g_autoptr(GstElement) first = NULL;
    g_autoptr(GstPad) qpad = NULL;
    GstPad* gpad = NULL;
    /*
      g_autoptr (GstPad) gpad = NULL;
    */

    sinkbin =
        gst_parse_launch
        ("queue name=q ! rtpgstdepay name=depay ! identity dump=true ! fakesink sync=false",
            NULL);

    gst_bin_add(GST_BIN(pipeline), sinkbin);

    first = gst_bin_get_by_name(GST_BIN(sinkbin), "q");
    qpad = gst_element_get_static_pad(first, "sink");

    gst_element_sync_state_with_parent(sinkbin);

    gpad = gst_ghost_pad_new(NULL, qpad);
    gst_element_add_pad(sinkbin, gpad);

    gst_pad_set_active(gpad, TRUE);
    gst_pad_link(pad, gpad);

    return GST_PAD_PROBE_REMOVE;
}


static void
_new_payload_type_cb(GstElement* element, guint pt, GstPad* pad,
    GstElement* user_data)
{
    GstElement* pipeline = user_data;

    g_print("new payload type pt: %d\n", pt);

    if (pt == 96) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_IDLE, _link_cb, pipeline, NULL);
    }
    else if (pt == 99) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_IDLE,
            _link_gstdepay_cb, pipeline, NULL);
    }
}

static GstElement*
build_recv_pipeline(const gchar* uri, const gchar* streamid, GError** error)
{
    g_autoptr(GstElement) pipeline = NULL;
    g_autoptr(GstElement) rtpdemux = NULL;
    g_autoptr(GstElement) srtsrc = NULL;

    g_autoptr(GstBus) bus = NULL;

    pipeline = gst_parse_launch("srtsrc name=srtsrc ! queue ! rtpptdemux name=rtpdemux ", error);

    if (pipeline == NULL)
        goto error;

    bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, _bus_watch, NULL);

    srtsrc = gst_bin_get_by_name(GST_BIN(pipeline), "srtsrc");
    rtpdemux = gst_bin_get_by_name(GST_BIN(pipeline), "rtpdemux");

    g_object_set(srtsrc, "uri", uri, "streamid", streamid, NULL);

    g_signal_connect(rtpdemux, "new-payload-type", G_CALLBACK(_new_payload_type_cb), pipeline);
    g_signal_connect(rtpdemux, "request-pt-map", G_CALLBACK(_request_pt_map_cb), NULL);

    return (GstElement *) g_steal_pointer(&pipeline);

error:
    return NULL;
}

int
main(int argc, char* argv[])
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;

    g_autofree gchar* streamid = NULL;

    gboolean help = FALSE;

    GOptionEntry entries[] = {
      {"user", 'u', 0, G_OPTION_ARG_STRING, &options.user, "Authorization Name",
          NULL},
      {"resource", 'r', 0, G_OPTION_ARG_STRING, &options.resource,
          "Resource Name", NULL},
      {"help", 'h', 0, G_OPTION_ARG_NONE, &help, "Show Help", NULL},
      {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_CALLBACK, _parse_rest_arg_cb, NULL,
          NULL},
      {NULL}
    };

    context = g_option_context_new("uri");
    g_option_context_set_help_enabled(context, FALSE);
    g_option_context_add_main_entries(context, entries, NULL);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);

        return -1;
    }

    if (help || options.uri == NULL) {
        g_autofree gchar* text = g_option_context_get_help(context, FALSE, NULL);
        g_printerr("%s\n", text);
        return -1;
    }

    gst_init(&argc, &argv);

    app.loop = g_main_loop_new(NULL, FALSE);

    /* Stream ID */
    streamid = build_streamid(options.user, options.resource);

    /* Build GStreamer Pipeline */
    app.pipeline =
        build_recv_pipeline(options.uri, streamid, &error);

    if (app.pipeline == NULL) {
        g_printerr("%s\n", error->message);

        return -1;
    }

    gst_element_set_state(app.pipeline, GST_STATE_PLAYING);

    g_main_loop_run(app.loop);

    gst_element_set_state(app.pipeline, GST_STATE_NULL);

    gst_object_unref(app.pipeline);
    g_main_loop_unref(app.loop);

    return 0;
}