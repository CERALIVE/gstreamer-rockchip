#include "enc-test-common.h"

#include <gst/video/video-event.h>
#include <rockchip/rk_mpi_cmd.h>

static gboolean contains_h264_idr(GstBuffer *buffer) {
  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
    return FALSE;
  gboolean found = FALSE;
  for (gsize i = 0; i + 4 < map.size; i++) {
    gsize nal = 0;
    if (map.data[i] == 0 && map.data[i + 1] == 0 && map.data[i + 2] == 1)
      nal = i + 3;
    else if (i + 5 < map.size && map.data[i] == 0 && map.data[i + 1] == 0 &&
             map.data[i + 2] == 0 && map.data[i + 3] == 1)
      nal = i + 4;
    if (nal && (map.data[nal] & 0x1f) == 5) {
      found = TRUE;
      break;
    }
  }
  gst_buffer_unmap(buffer, &map);
  return found;
}

GST_START_TEST(test_videotestsrc_force_event_yields_idr_within_one_frame) {
  mpp_mock_reset();
  GstHarness *source = gst_harness_new_parse("videotestsrc num-buffers=2");
  fail_unless(source != NULL);
  gst_harness_set_sink_caps_str(
      source, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_play(source);
  GstBuffer *raw_first = gst_harness_pull(source);
  GstBuffer *raw_second = gst_harness_pull(source);
  fail_unless(raw_first != NULL);
  fail_unless(raw_second != NULL);
  gst_harness_teardown(source);

  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, "zero-copy-pkt", FALSE,
               NULL);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_play(h);
  fail_unless_equals_int(gst_harness_push(h, raw_first), GST_FLOW_OK);
  GstBuffer *output = gst_harness_pull(h);
  fail_unless(output != NULL);
  fail_unless(!contains_h264_idr(output));
  gst_buffer_unref(output);

  GstPad *src = GST_VIDEO_ENCODER_SRC_PAD(h->element);
  fail_unless(
      gst_pad_send_event(src, gst_video_event_new_upstream_force_key_unit(
                                  GST_CLOCK_TIME_NONE, TRUE, 1)));
  fail_unless_equals_int(gst_harness_push(h, raw_second), GST_FLOW_OK);
  output = gst_harness_pull(h);
  fail_unless(output != NULL);
  fail_unless(contains_h264_idr(output),
              "the frame after force-key-unit carried no H.264 type-5 NAL");
  gst_buffer_unref(output);
  gst_harness_teardown(h);
}
GST_END_TEST

static void check_force_event(GstEvent *event) {
  mpp_mock_reset();
  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, "zero-copy-pkt", FALSE,
               "header-mode", 1, NULL);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_play(h);
  GstPad *pad = GST_EVENT_IS_UPSTREAM(event)
                    ? GST_VIDEO_ENCODER_SRC_PAD(h->element)
                    : GST_VIDEO_ENCODER_SINK_PAD(h->element);
  fail_unless(gst_pad_send_event(pad, event));
  fail_unless_equals_int(enc_test_push(h, 0, 30), GST_FLOW_OK);
  GstBuffer *output = gst_harness_pull(h);
  fail_unless(output != NULL);
  fail_unless(mpp_mock_control_count(MPP_ENC_SET_IDR_FRAME) == 1);
  fail_unless(GST_BUFFER_FLAG_IS_SET(output, GST_BUFFER_FLAG_HEADER));
  fail_unless_equals_uint64(GST_BUFFER_DTS(output), GST_BUFFER_PTS(output));
  gst_buffer_unref(output);
  gst_harness_teardown(h);
}

GST_START_TEST(test_upstream_and_downstream_events_call_force_idr) {
  check_force_event(gst_video_event_new_upstream_force_key_unit(
      GST_CLOCK_TIME_NONE, TRUE, 1));
  check_force_event(gst_video_event_new_downstream_force_key_unit(
      GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, TRUE, 2));
}
GST_END_TEST

static Suite *idr_nal_suite(void) {
  Suite *suite = suite_create("idr-nal");
  TCase *test_case = tcase_create("forced-idr");
  tcase_set_timeout(test_case, 15);
  tcase_add_test(test_case,
                 test_videotestsrc_force_event_yields_idr_within_one_frame);
  tcase_add_test(test_case, test_upstream_and_downstream_events_call_force_idr);
  suite_add_tcase(suite, test_case);
  return suite;
}

GST_CHECK_MAIN(idr_nal)
