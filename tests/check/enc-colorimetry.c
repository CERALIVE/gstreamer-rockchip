#include "enc-test-common.h"

#include <rockchip/mpp_frame.h>

typedef struct {
  GstVideoColorMatrix matrix;
  GstVideoColorPrimaries primaries;
  GstVideoTransferFunction transfer;
  gint mpp_matrix;
  gint mpp_primaries;
  gint mpp_transfer;
} ColorCase;

static void check_color_case(const ColorCase *color, GstVideoColorRange range,
                             gint mpp_range) {
  mpp_mock_reset();
  GstHarness *h = gst_harness_new("mpph265enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, NULL);

  GstVideoInfo info;
  gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, 320, 240);
  GST_VIDEO_INFO_FPS_N(&info) = 30;
  GST_VIDEO_INFO_FPS_D(&info) = 1;
  info.colorimetry.range = range;
  info.colorimetry.matrix = color->matrix;
  info.colorimetry.primaries = color->primaries;
  info.colorimetry.transfer = color->transfer;
  gst_harness_set_src_caps(h, gst_video_info_to_caps(&info));

  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colorspace"),
                         color->mpp_matrix);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colorprim"),
                         color->mpp_primaries);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colortrc"),
                         color->mpp_transfer);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:range"), mpp_range);
  gst_harness_teardown(h);
}

GST_START_TEST(test_601_709_2020_full_and_limited_cfg_keys) {
  static const ColorCase cases[] = {
      {GST_VIDEO_COLOR_MATRIX_BT601, GST_VIDEO_COLOR_PRIMARIES_SMPTE170M,
       GST_VIDEO_TRANSFER_BT601, MPP_FRAME_SPC_SMPTE170M,
       MPP_FRAME_PRI_SMPTE170M, MPP_FRAME_TRC_SMPTE170M},
      {GST_VIDEO_COLOR_MATRIX_BT709, GST_VIDEO_COLOR_PRIMARIES_BT709,
       GST_VIDEO_TRANSFER_BT709, MPP_FRAME_SPC_BT709, MPP_FRAME_PRI_BT709,
       MPP_FRAME_TRC_BT709},
      {GST_VIDEO_COLOR_MATRIX_BT2020, GST_VIDEO_COLOR_PRIMARIES_BT2020,
       GST_VIDEO_TRANSFER_BT2020_10, MPP_FRAME_SPC_BT2020_NCL,
       MPP_FRAME_PRI_BT2020, MPP_FRAME_TRC_BT2020_10},
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    check_color_case(&cases[i], GST_VIDEO_COLOR_RANGE_0_255,
                     MPP_FRAME_RANGE_JPEG);
    check_color_case(&cases[i], GST_VIDEO_COLOR_RANGE_16_235,
                     MPP_FRAME_RANGE_MPEG);
  }
}
GST_END_TEST

GST_START_TEST(test_renegotiation_to_unspecified_removes_old_color_cfg) {
  mpp_mock_reset();
  GstHarness *h = gst_harness_new("mpph265enc");
  fail_unless(h != NULL);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1,"
         "colorimetry=bt709");
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colorspace"),
                         MPP_FRAME_SPC_BT709);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=640,height=480,framerate=30/1");
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colorspace"), INT32_MIN);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colorprim"), INT32_MIN);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colortrc"), INT32_MIN);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:range"), INT32_MIN);
  gst_harness_teardown(h);
}
GST_END_TEST

GST_START_TEST(test_unspecified_colorimetry_writes_no_cfg_keys) {
  mpp_mock_reset();
  GstHarness *h = gst_harness_new("mpph265enc");
  fail_unless(h != NULL);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colorspace"), INT32_MIN);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colorprim"), INT32_MIN);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:colortrc"), INT32_MIN);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:range"), INT32_MIN);
  gst_harness_teardown(h);
}
GST_END_TEST

static Suite *enc_colorimetry_suite(void) {
  Suite *suite = suite_create("enc-colorimetry");
  TCase *test_case = tcase_create("cfg-keys");
  tcase_add_test(test_case, test_601_709_2020_full_and_limited_cfg_keys);
  tcase_add_test(test_case, test_unspecified_colorimetry_writes_no_cfg_keys);
  tcase_add_test(test_case,
                 test_renegotiation_to_unspecified_removes_old_color_cfg);
  suite_add_tcase(suite, test_case);
  return suite;
}

GST_CHECK_MAIN(enc_colorimetry)
