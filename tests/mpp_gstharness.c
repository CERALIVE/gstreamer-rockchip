#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/video/video.h>
#include <limits.h>
#include <rockchip/rk_mpi_cmd.h>
extern int mpp_mock_last_cfg_s32(const char *name);
extern unsigned mpp_mock_control_count(int cmd);
extern unsigned mpp_mock_frame_set_buffer_count(void);
extern void mpp_mock_reset(void);
static void check_factory(const char *name, const char *property) {
  GstElementFactory *f = gst_element_factory_find(name);
  fail_unless(f != NULL, "missing %s", name);
  GstElement *e = gst_element_factory_create(f, NULL);
  fail_unless(e != NULL);
  guint n = 0;
  GParamSpec **ps = g_object_class_list_properties(G_OBJECT_GET_CLASS(e), &n);
  fail_unless(n > 0);
  fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(e), property) !=
              NULL);
  g_free(ps);
  gst_object_unref(e);
  gst_object_unref(f);
}

GST_START_TEST(test_factories_properties) {
  check_factory("mpph264enc", "bitrate");
  check_factory("mpph265enc", "bitrate");
  check_factory("mppvideodec", "fbc");
  check_factory("mppjpegdec", "format");
}
GST_END_TEST
GST_START_TEST(test_jpeg_caps_with_harness) {
  GstHarness *h = gst_harness_new_empty();
  fail_unless(h != NULL);
  GstElement *e = gst_element_factory_make("mppjpegdec", NULL);
  fail_unless(e != NULL);
  GstPad *src = gst_element_get_static_pad(e, "src");
  fail_unless(src != NULL);
  gst_harness_add_element_src_pad(h, src);
  GstCaps *caps = gst_pad_get_pad_template_caps(src);
  GstCaps *nv12 = gst_caps_from_string("video/x-raw,format=NV12");
  GstCaps *nv12_range = gst_caps_from_string(
      "video/x-raw,format=NV12,width=[1,MAX],height=[1,MAX]");
  fail_unless(gst_caps_can_intersect(caps, nv12));
  fail_unless(gst_caps_can_intersect(caps, nv12_range));
  fail_unless(gst_caps_is_fixed(caps) == FALSE);
  gst_caps_unref(nv12);
  gst_caps_unref(nv12_range);
  gst_caps_unref(caps);
  gst_object_unref(src);
  gst_harness_teardown(h);
}
GST_END_TEST
GST_START_TEST(test_video_decoder_caps_truth) {
  GstElementFactory *factory = gst_element_factory_find("mppvideodec");
  fail_unless(factory != NULL);
  const GList *templates =
      gst_element_factory_get_static_pad_templates(factory);
  GstCaps *sink_caps = NULL;
  GstCaps *src_caps = NULL;
  for (const GList *item = templates; item != NULL; item = item->next) {
    GstStaticPadTemplate *templ = item->data;
    GstCaps *caps = gst_static_caps_get(&templ->static_caps);
    if (templ->direction == GST_PAD_SINK)
      sink_caps = caps;
    else if (templ->direction == GST_PAD_SRC)
      src_caps = caps;
    else
      gst_caps_unref(caps);
  }
  fail_unless(sink_caps != NULL);
  fail_unless(src_caps != NULL);
  GstCaps *h264 = gst_caps_from_string("video/x-h264,parsed=true,alignment=au");
  GstCaps *h265 = gst_caps_from_string("video/x-h265,parsed=true,alignment=au");
  GstCaps *av1 = gst_caps_from_string("video/x-av1,parsed=true");
  GstCaps *nv12 = gst_caps_from_string("video/x-raw,format=NV12");
  fail_unless(gst_caps_can_intersect(sink_caps, h264));
  fail_unless(gst_caps_can_intersect(sink_caps, h265));
  fail_unless(gst_caps_can_intersect(sink_caps, av1));
  fail_unless(gst_caps_can_intersect(src_caps, nv12));
  gst_caps_unref(h264);
  gst_caps_unref(h265);
  gst_caps_unref(av1);
  gst_caps_unref(nv12);
  gst_caps_unref(sink_caps);
  gst_caps_unref(src_caps);
  gst_object_unref(factory);
}
GST_END_TEST
static void check_encoder_lifecycle(const char *factory) {
  GstHarness *h = gst_harness_new(factory);
  fail_unless(h != NULL, "could not create %s", factory);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, "zero-copy-pkt", FALSE,
               NULL);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_play(h);
  GstBuffer *frame = gst_buffer_new_allocate(NULL, 115200, NULL);
  fail_unless(frame != NULL);
  GST_BUFFER_PTS(frame) = 0;
  GST_BUFFER_DURATION(frame) = GST_SECOND / 30;
  gst_harness_set_drop_buffers(h, TRUE);
  fail_unless_equals_int(gst_harness_push(h, frame), GST_FLOW_OK);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:width"), 320);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:height"), 240);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:mode"), 1);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_target"), 500);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:fps_in_num"), 30);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:fps_in_denorm"), 1);
  fail_unless(mpp_mock_control_count(MPP_ENC_SET_CFG) > 0);
  fail_unless(mpp_mock_control_count(MPP_ENC_SET_SEI_CFG) > 0);
  fail_unless(mpp_mock_control_count(MPP_ENC_SET_HEADER_MODE) > 0);
  fail_unless(mpp_mock_frame_set_buffer_count() > 0,
              "encoder frame must stay inside the mock MPP ABI");
  gst_harness_teardown(h);
}
GST_START_TEST(test_h264_encoder_lifecycle) {
  mpp_mock_reset();
  check_encoder_lifecycle("mpph264enc");
}
GST_END_TEST
GST_START_TEST(test_h265_encoder_lifecycle) {
  mpp_mock_reset();
  check_encoder_lifecycle("mpph265enc");
}
GST_END_TEST

Suite *mpp_gstharness_suite(void) {
  Suite *s = suite_create("rockchipmpp");
  TCase *tc = tcase_create("caps");
  tcase_add_test(tc, test_factories_properties);
  tcase_add_test(tc, test_jpeg_caps_with_harness);
  tcase_add_test(tc, test_video_decoder_caps_truth);
  tcase_add_test(tc, test_h264_encoder_lifecycle);
  tcase_add_test(tc, test_h265_encoder_lifecycle);
  suite_add_tcase(s, tc);
  return s;
}
GST_CHECK_MAIN(mpp_gstharness)
