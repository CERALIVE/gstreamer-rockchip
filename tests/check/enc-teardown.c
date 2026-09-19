#include "enc-test-common.h"
#include "../../gst/rockchipmpp/gstmppenc.h"

/* Widen the observed reset -> parent PAUSED_TO_READY window, without changing
 * the plugin's reset, dirty flag, frame admission, or negotiation code. */
typedef struct {
  GMutex mutex;
  GCond cond;
  GstElement *encoder;
  gboolean queued, reset_done, handled;
  guint buffers;
  GstFlowReturn flow;
  gint errors;
  guint configs;
  GstStateChangeReturn (*change_state)(GstElement *, GstStateChange);
  GstFlowReturn (*handle_frame)(GstVideoEncoder *, GstVideoCodecFrame *);
} Teardown;

static Teardown race;

static void wait_for(gboolean *condition) {
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!*condition)
    fail_unless(g_cond_wait_until(&race.cond, &race.mutex, deadline),
                "teardown rendezvous timed out");
}

static GstPadProbeReturn hold_queued_frame(GstPad *pad, GstPadProbeInfo *info,
                                           gpointer data) {
  (void)pad;
  (void)info;
  (void)data;
  g_mutex_lock(&race.mutex);
  if (++race.buffers == 2) {
    race.queued = TRUE;
    g_cond_broadcast(&race.cond);
    wait_for(&race.reset_done);
  }
  g_mutex_unlock(&race.mutex);
  return GST_PAD_PROBE_OK;
}

static GstFlowReturn observe_frame(GstVideoEncoder *encoder,
                                   GstVideoCodecFrame *frame) {
  GstFlowReturn flow = race.handle_frame(encoder, frame);
  g_mutex_lock(&race.mutex);
  if (race.reset_done) {
    race.flow = flow;
    race.handled = TRUE;
    g_cond_broadcast(&race.cond);
  }
  g_mutex_unlock(&race.mutex);
  return flow;
}

static GstStateChangeReturn after_reset(GstElement *element,
                                        GstStateChange transition) {
  if (element == race.encoder && transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    GstMppEnc *mpp = (GstMppEnc *)element;
    fail_unless(g_atomic_int_get(&mpp->flushing));
    fail_unless(mpp->prop_dirty);
    fail_unless(gst_pad_get_task_state(GST_VIDEO_ENCODER_SRC_PAD(element)) !=
                GST_TASK_STARTED);
    /* The real board trace has already lost its src caps at this point. */
    fail_unless(gst_pad_set_active(GST_VIDEO_ENCODER_SRC_PAD(element), FALSE));
    fail_if(gst_pad_has_current_caps(GST_VIDEO_ENCODER_SRC_PAD(element)));
    race.configs = mpp_mock_control_count(MPP_ENC_SET_CFG);
    g_mutex_lock(&race.mutex);
    race.reset_done = TRUE;
    g_cond_broadcast(&race.cond);
    wait_for(&race.handled);
    g_mutex_unlock(&race.mutex);
  }
  return race.change_state(element, transition);
}

static GstBusSyncReply observe_bus(GstBus *bus, GstMessage *message,
                                   gpointer data) {
  (void)bus;
  (void)data;
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError *error = NULL;
    gchar *debug = NULL;
    gst_message_parse_error(message, &error, &debug);
    g_printerr("BUS_ERROR %s: %s (%s)\n", GST_OBJECT_NAME(message->src),
               error->message, debug ? debug : "");
    g_atomic_int_inc(&race.errors);
    g_clear_error(&error);
    g_free(debug);
  }
  return GST_BUS_PASS;
}

static void teardown_race(const gchar *factory) {
  mpp_mock_reset();
  race = (Teardown){0};
  g_mutex_init(&race.mutex);
  g_cond_init(&race.cond);
  gchar *description = g_strdup_printf(
      "videotestsrc ! video/x-raw,format=NV12,width=320,height=240,"
      "framerate=30/1 ! queue ! %s name=enc zero-copy-pkt=false ! "
      "fakesink sync=false async=false", factory);
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(description, &error);
  g_free(description);
  fail_unless(error == NULL, "%s", error ? error->message : "");
  race.encoder = gst_bin_get_by_name(GST_BIN(pipeline), "enc");
  GstVideoEncoderClass *codec = GST_VIDEO_ENCODER_GET_CLASS(race.encoder);
  GstElementClass *base = g_type_class_ref(GST_TYPE_VIDEO_ENCODER);
  race.change_state = base->change_state;
  race.handle_frame = codec->handle_frame;
  base->change_state = after_reset;
  codec->handle_frame = observe_frame;
  GstPad *sink = gst_element_get_static_pad(race.encoder, "sink");
  gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, hold_queued_frame, NULL, NULL);
  GstBus *bus = gst_element_get_bus(pipeline);
  gst_bus_set_sync_handler(bus, observe_bus, NULL, NULL);
  fail_if(gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE);
  g_mutex_lock(&race.mutex);
  wait_for(&race.queued);
  g_mutex_unlock(&race.mutex);
  fail_unless_equals_int(g_atomic_int_get(&race.errors), 0);

  /* No FLUSH_START workaround: the upstream queue is still delivering while
   * the calling thread performs the real downward state transition. */
  fail_unless_equals_int(gst_element_set_state(pipeline, GST_STATE_READY),
                         GST_STATE_CHANGE_SUCCESS);
  guint configs = mpp_mock_control_count(MPP_ENC_SET_CFG) - race.configs;
  g_printerr("%s late-flow=%s property-applies=%u bus-errors=%d\n", factory,
             gst_flow_get_name(race.flow), configs, g_atomic_int_get(&race.errors));
  base->change_state = race.change_state;
  codec->handle_frame = race.handle_frame;
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_bus_set_sync_handler(bus, NULL, NULL, NULL);
  gst_object_unref(bus);
  gst_object_unref(sink);
  gst_object_unref(race.encoder);
  gst_object_unref(pipeline);
  g_type_class_unref(base);
  g_cond_clear(&race.cond);
  g_mutex_clear(&race.mutex);
  fail_unless_equals_int(race.flow, GST_FLOW_FLUSHING);
  fail_unless_equals_int(configs, 0);
  fail_unless_equals_int(g_atomic_int_get(&race.errors), 0);
}

GST_START_TEST(test_h265_late_frame) { teardown_race("mpph265enc"); } GST_END_TEST
GST_START_TEST(test_h264_late_frame) { teardown_race("mpph264enc"); } GST_END_TEST
GST_START_TEST(test_vp8_late_frame) { teardown_race("mppvp8enc"); } GST_END_TEST
GST_START_TEST(test_jpeg_late_frame) { teardown_race("mppjpegenc"); } GST_END_TEST

static Suite *enc_teardown_suite(void) {
  Suite *suite = suite_create("enc-teardown");
  TCase *tc = tcase_create("late-input");
  tcase_set_timeout(tc, 15);
  tcase_add_test(tc, test_h265_late_frame);
  tcase_add_test(tc, test_h264_late_frame);
  tcase_add_test(tc, test_vp8_late_frame);
  tcase_add_test(tc, test_jpeg_late_frame);
  suite_add_tcase(suite, tc);
  return suite;
}

GST_CHECK_MAIN(enc_teardown)
