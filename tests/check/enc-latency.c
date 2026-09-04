#include "enc-test-common.h"

GST_START_TEST(test_latency_tracks_current_pending_depth) {
  const GstClockTime frame_duration = GST_SECOND / 25;
  mpp_mock_reset();
  mpp_mock_enc_arm_reset_drain();
  GstHarness *h = enc_test_harness(
      "mpph264enc",
      "video/x-raw,format=NV12,width=320,height=240,framerate=25/1");
  gst_harness_set_live(h, TRUE);
  gst_harness_set_upstream_latency(h, 0);

  fail_unless_equals_int(enc_test_push(h, 0, 25), GST_FLOW_OK);
  fail_unless_equals_int(enc_test_push(h, 1, 25), GST_FLOW_OK);
  g_object_set(h->element, "bitrate", 1000, NULL);
  fail_unless_equals_int(enc_test_push(h, 2, 25), GST_FLOW_OK);

  GstQuery *query = gst_query_new_latency();
  fail_unless(gst_element_query(h->element, query));
  gboolean live = FALSE;
  GstClockTime minimum = GST_CLOCK_TIME_NONE;
  GstClockTime maximum = GST_CLOCK_TIME_NONE;
  gst_query_parse_latency(query, &live, &minimum, &maximum);
  fail_unless_equals_uint64(minimum, 3 * frame_duration);
  fail_unless_equals_uint64(maximum, GST_CLOCK_TIME_NONE);
  gst_video_encoder_get_latency(GST_VIDEO_ENCODER(h->element), &minimum,
                                &maximum);
  fail_unless_equals_uint64(minimum, 3 * frame_duration);
  fail_unless_equals_uint64(maximum, 3 * frame_duration);
  gst_query_unref(query);
  gst_harness_teardown(h);
}
GST_END_TEST

static Suite *enc_latency_suite(void) {
  Suite *suite = suite_create("enc-latency");
  TCase *test_case = tcase_create("latency");
  tcase_add_test(test_case, test_latency_tracks_current_pending_depth);
  suite_add_tcase(suite, test_case);
  return suite;
}

GST_CHECK_MAIN(enc_latency)
