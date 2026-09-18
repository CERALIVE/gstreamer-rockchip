#include <errno.h>
#include <gst/check/gstcheck.h>
#include <rga/im2d.h>

#include "../../gst/rockchipmpp/gstmpprgabackend.h"

int
c_RkRgaInit (void)
{
  return 0;
}

int
c_RkRgaBlit (rga_info_t * src, rga_info_t * dst, rga_info_t * src1)
{
  (void) src;
  (void) dst;
  (void) src1;
  return 0;
}

typedef struct
{
  gboolean probe_available;
  gint probe_errno;
  gint init_result;
  gint blit_result;
  gint blit_errno;
  guint blit_calls;
  guint32 version_major;
  guint32 version_minor;
  guint32 version_revision;
} FakeRga;

static gboolean
fake_probe (gpointer user_data, GstMppRgaDriverVersion * version,
    gint * error_number)
{
  FakeRga *fake = user_data;

  if (!fake->probe_available) {
    *error_number = fake->probe_errno;
    return FALSE;
  }

  version->major = fake->version_major ? fake->version_major : 1;
  version->minor = fake->version_minor ? fake->version_minor : 3;
  version->revision = fake->version_revision ? fake->version_revision : 11;
  g_snprintf (version->string, sizeof (version->string), "%u.%u.%u",
      version->major, version->minor, version->revision);
  return TRUE;
}

static gint
fake_init (gpointer user_data)
{
  return ((FakeRga *) user_data)->init_result;
}

static gint
fake_blit (rga_info_t * src, rga_info_t * dst, gpointer user_data)
{
  FakeRga *fake = user_data;

  (void) src;
  (void) dst;
  fake->blit_calls++;
  errno = fake->blit_errno;
  return fake->blit_result;
}

static const GstMppRgaBackendOps fake_ops = {
  .probe = fake_probe,
  .init = fake_init,
  .blit = fake_blit,
};

static gint
fake_process (const GstMppRgaIm2dRequest * request, gpointer user_data)
{
  (void) request;
  return ((FakeRga *) user_data)->blit_result;
}

static gint
fake_composite (const GstMppRgaIm2dCompositeRequest * request, gpointer user_data)
{
  return fake_process (&request->transform, user_data);
}

GST_START_TEST (test_im_status_boundary_rejects_unknown_positive_values)
{
  const gint statuses[] = { IM_STATUS_SUCCESS, IM_STATUS_NOERROR, 0, 3, 42,
    IM_STATUS_FAILED, IM_STATUS_NOT_SUPPORTED, IM_STATUS_OUT_OF_MEMORY,
    IM_STATUS_INVALID_PARAM, IM_STATUS_ILLEGAL_PARAM, IM_STATUS_ERROR_VERSION,
    IM_STATUS_NO_SESSION };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (statuses); i++) {
    FakeRga fake = {.probe_available = TRUE, .blit_result = statuses[i]};
    GstMppRgaBackendOps ops = fake_ops;
    GstMppRgaBackend *backend;
    GstMppRgaIm2dCompositeRequest request = { 0, };
    GstFlowReturn expected = GST_FLOW_ERROR;
    ops.process = fake_process;
    ops.composite = fake_composite;
    backend = gst_mpp_rga_backend_new (&ops, &fake);
    if (statuses[i] == IM_STATUS_SUCCESS || statuses[i] == IM_STATUS_NOERROR)
      expected = GST_FLOW_OK;
    else if (statuses[i] == IM_STATUS_NOT_SUPPORTED ||
        statuses[i] == IM_STATUS_INVALID_PARAM ||
        statuses[i] == IM_STATUS_ILLEGAL_PARAM ||
        statuses[i] == IM_STATUS_ERROR_VERSION)
      expected = GST_FLOW_NOT_NEGOTIATED;
    fail_unless_equals_int (gst_mpp_rga_result_to_flow (
            gst_mpp_rga_backend_process (backend, GST_MPP_RGA_OP_CONVERT,
                GST_VIDEO_FORMAT_NV12, GST_VIDEO_FORMAT_BGR,
                &request.transform)), expected);
    fail_unless_equals_int (gst_mpp_rga_result_to_flow (
            gst_mpp_rga_backend_composite (backend, GST_VIDEO_FORMAT_NV12,
                GST_VIDEO_FORMAT_NV12, &request)), expected);
    gst_mpp_rga_backend_free (backend);
  }
}
GST_END_TEST;

GST_START_TEST (test_csc_direction_matrix_range_table)
{
  const GstVideoColorMatrix matrices[] = { GST_VIDEO_COLOR_MATRIX_BT601,
    GST_VIDEO_COLOR_MATRIX_BT709, GST_VIDEO_COLOR_MATRIX_BT2020,
    GST_VIDEO_COLOR_MATRIX_UNKNOWN };
  const GstVideoColorRange ranges[] = { GST_VIDEO_COLOR_RANGE_16_235,
    GST_VIDEO_COLOR_RANGE_0_255, GST_VIDEO_COLOR_RANGE_UNKNOWN };
  const gint expected[2][2][2] = {
    {{IM_YUV_TO_RGB_BT601_LIMIT, IM_YUV_TO_RGB_BT601_FULL},
      {IM_YUV_TO_RGB_BT709_LIMIT, 0}},
    {{IM_RGB_TO_YUV_BT601_LIMIT, IM_RGB_TO_YUV_BT601_FULL},
      {IM_RGB_TO_YUV_BT709_LIMIT, 0}} };
  guint m, r, direction;
  GstVideoInfo rgb, yuv;
  GstMppRgaIm2dRequest request;

  gst_video_info_set_format (&rgb, GST_VIDEO_FORMAT_BGR, 640, 480);
  gst_video_info_set_format (&yuv, GST_VIDEO_FORMAT_NV12, 640, 480);
  for (direction = 0; direction < 2; direction++) {
    for (m = 0; m < G_N_ELEMENTS (matrices); m++) {
      for (r = 0; r < G_N_ELEMENTS (ranges); r++) {
        gint flag = m < 2 && r < 2 ? expected[direction][m][r] : 0;
        yuv.colorimetry.matrix = matrices[m];
        yuv.colorimetry.range = ranges[r];
        memset (&request, 0, sizeof (request));
        fail_unless_equals_int (gst_mpp_rga_request_set_colorimetry (&request,
                direction ? &rgb : &yuv, direction ? &yuv : &rgb), flag != 0);
        fail_unless_equals_int (request.src_color_space_mode, 0);
        fail_unless_equals_int (request.dst_color_space_mode, flag);
        fail_unless_equals_int (request.csc_fallback, flag == 0);
        fail_unless (gst_video_colorimetry_is_equal (&request.colorspace_in,
                direction ? &rgb.colorimetry : &yuv.colorimetry));
      }
    }
  }
  fail_unless (gst_mpp_rga_request_set_colorimetry (&request, &rgb, &rgb));
  fail_unless (gst_mpp_rga_request_set_colorimetry (&request, &yuv, &yuv));
  {
    GstVideoInfo different = yuv;
    GstMppConversionStats stats;
    GstMppConversionStatsSnapshot snapshot;
    different.colorimetry.primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
    fail_if (gst_mpp_rga_request_set_colorimetry (&request, &yuv, &different));
    gst_mpp_conversion_stats_init (&stats);
    gst_mpp_rga_count_csc_fallback (&request, &stats);
    gst_mpp_rga_count_csc_fallback (&request, &stats);
    gst_mpp_conversion_stats_snapshot (&stats, &snapshot);
    fail_unless_equals_uint64 (snapshot.csc_fallback_frames, 2);
    fail_unless_equals_uint64 (snapshot.fallback_frames, 0);
    fail_unless (stats.csc_warned);
    gst_mpp_conversion_stats_clear (&stats);
  }
}
GST_END_TEST;

static GstMppRgaBackend *
new_backend (FakeRga * fake)
{
  return gst_mpp_rga_backend_new (&fake_ops, fake);
}

GST_START_TEST (test_unavailable_backend_takes_typed_refusal_path)
{
  FakeRga fake = {.probe_errno = ENOENT };
  GstMppRgaBackend *backend = new_backend (&fake);

  fail_if (gst_mpp_rga_backend_init (backend));
  fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
          GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_NV16,
          GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_UNAVAILABLE);
  fail_unless_equals_int (fake.blit_calls, 0);

  gst_mpp_rga_backend_free (backend);
}
GST_END_TEST;

GST_START_TEST (test_old_driver_version_is_unavailable_even_if_librga_init_succeeds)
{
  FakeRga fake = {
    .probe_available = TRUE,
    .version_major = 1,
    .version_minor = 2,
    .version_revision = 3,
  };
  GstMppRgaBackend *backend = new_backend (&fake);

  fail_if (gst_mpp_rga_backend_init (backend));
  fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
          GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_NV16,
          GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_UNAVAILABLE);
  fail_unless_equals_int (fake.blit_calls, 0);

  gst_mpp_rga_backend_free (backend);
}
GST_END_TEST;

GST_START_TEST (test_librga_init_failure_is_informational_after_probe_success)
{
  FakeRga fake = {.probe_available = TRUE,.init_result = -1,.blit_result = 0 };
  GstMppRgaBackend *backend = new_backend (&fake);

  fail_unless (gst_mpp_rga_backend_init (backend));
  fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
          GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_NV16,
          GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_SUCCESS);
  fail_unless_equals_int (fake.blit_calls, 1);

  gst_mpp_rga_backend_free (backend);
}
GST_END_TEST;

GST_START_TEST (test_eight_failures_demote_only_the_exact_tuple)
{
  FakeRga fake = {.probe_available = TRUE,.blit_result = -1,.blit_errno = EIO };
  GstMppRgaBackend *backend = new_backend (&fake);
  guint i;

  fail_unless (gst_mpp_rga_backend_init (backend));
  for (i = 0; i < GST_MPP_RGA_DEMOTION_THRESHOLD; i++)
    fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
            GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_NV16,
            GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_BLIT_FAILED);

  fail_unless_equals_int (fake.blit_calls, GST_MPP_RGA_DEMOTION_THRESHOLD);
  fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
          GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_NV16,
          GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_TUPLE_DEMOTED);
  fail_unless_equals_int (fake.blit_calls, GST_MPP_RGA_DEMOTION_THRESHOLD);

  fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
          GST_MPP_RGA_OP_DECODE_CONVERT, GST_VIDEO_FORMAT_NV16,
          GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_BLIT_FAILED);
  fail_unless_equals_int (fake.blit_calls,
      GST_MPP_RGA_DEMOTION_THRESHOLD + 1);

  gst_mpp_rga_backend_free (backend);
}
GST_END_TEST;

GST_START_TEST (test_successful_retrial_clears_tuple_demotion)
{
  FakeRga fake = {.probe_available = TRUE,.blit_result = -1,.blit_errno = EIO };
  GstMppRgaBackend *backend = new_backend (&fake);
  guint i;

  fail_unless (gst_mpp_rga_backend_init (backend));
  for (i = 0; i < GST_MPP_RGA_DEMOTION_THRESHOLD; i++)
    gst_mpp_rga_backend_blit (backend, GST_MPP_RGA_OP_ENCODE_CONVERT,
        GST_VIDEO_FORMAT_BGR, GST_VIDEO_FORMAT_NV12, NULL, NULL);

  for (i = 0; i < GST_MPP_RGA_RETRY_INTERVAL; i++)
    fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
            GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_BGR,
            GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_TUPLE_DEMOTED);

  fake.blit_result = 0;
  fake.blit_errno = 0;
  fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
          GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_BGR,
          GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_SUCCESS);
  fail_unless_equals_int (gst_mpp_rga_backend_tuple_failures (backend,
          GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_BGR,
          GST_VIDEO_FORMAT_NV12), 0);
  fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
          GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_BGR,
          GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_SUCCESS);

  gst_mpp_rga_backend_free (backend);
}
GST_END_TEST;

GST_START_TEST (test_enodev_demotes_the_process)
{
  FakeRga fake = {
    .probe_available = TRUE,
    .blit_result = -1,
    .blit_errno = ENODEV,
  };
  GstMppRgaBackend *backend = new_backend (&fake);

  fail_unless (gst_mpp_rga_backend_init (backend));
  fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
          GST_MPP_RGA_OP_DECODE_CONVERT, GST_VIDEO_FORMAT_NV12,
          GST_VIDEO_FORMAT_BGRA, NULL, NULL), GST_MPP_RGA_DEVICE_LOST);
  fail_unless_equals_int (gst_mpp_rga_backend_blit (backend,
          GST_MPP_RGA_OP_ENCODE_CONVERT, GST_VIDEO_FORMAT_BGR,
          GST_VIDEO_FORMAT_NV12, NULL, NULL), GST_MPP_RGA_UNAVAILABLE);
  fail_unless_equals_int (fake.blit_calls, 1);

  gst_mpp_rga_backend_free (backend);
}
GST_END_TEST;

GST_START_TEST (test_conversion_counters_and_cpu_copy_env_gate)
{
  GstMppConversionStats stats;
  GstMppConversionStatsSnapshot snapshot;

  gst_mpp_conversion_stats_init (&stats);
  g_unsetenv ("GST_MPP_ALLOW_CPU_COPY");
  fail_if (gst_mpp_conversion_finish_without_rga (&stats, TRUE, TRUE));
  gst_mpp_conversion_stats_layout_rejected (&stats);
  gst_mpp_conversion_stats_snapshot (&stats, &snapshot);
  fail_unless_equals_uint64 (snapshot.fallback_frames, 0);
  fail_unless_equals_uint64 (snapshot.dropped_frames, 1);
  fail_unless_equals_uint64 (snapshot.layout_rejections, 1);

  g_setenv ("GST_MPP_ALLOW_CPU_COPY", "1", TRUE);
  fail_unless (gst_mpp_conversion_finish_without_rga (&stats, TRUE, TRUE));
  gst_mpp_conversion_stats_snapshot (&stats, &snapshot);
  fail_unless_equals_uint64 (snapshot.fallback_frames, 1);
  fail_unless_equals_uint64 (snapshot.dropped_frames, 1);
  fail_unless_equals_uint64 (snapshot.layout_rejections, 1);

  g_unsetenv ("GST_MPP_ALLOW_CPU_COPY");
  gst_mpp_conversion_stats_clear (&stats);
}
GST_END_TEST;

static Suite *
mpp_rga_backend_suite (void)
{
  Suite *suite = suite_create ("mpp_rga_backend");
  TCase *test_case = tcase_create ("backend");
  tcase_add_test (test_case, test_im_status_boundary_rejects_unknown_positive_values);
  tcase_add_test (test_case, test_csc_direction_matrix_range_table);

  tcase_add_test (test_case, test_unavailable_backend_takes_typed_refusal_path);
  tcase_add_test (test_case,
      test_old_driver_version_is_unavailable_even_if_librga_init_succeeds);
  tcase_add_test (test_case,
      test_librga_init_failure_is_informational_after_probe_success);
  tcase_add_test (test_case, test_eight_failures_demote_only_the_exact_tuple);
  tcase_add_test (test_case, test_successful_retrial_clears_tuple_demotion);
  tcase_add_test (test_case, test_enodev_demotes_the_process);
  tcase_add_test (test_case, test_conversion_counters_and_cpu_copy_env_gate);
  suite_add_tcase (suite, test_case);
  return suite;
}

GST_CHECK_MAIN (mpp_rga_backend);
