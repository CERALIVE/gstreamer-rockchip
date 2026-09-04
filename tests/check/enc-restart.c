#include "enc-test-common.h"

#include <rockchip/rk_mpi_cmd.h>

GST_START_TEST(test_four_errors_exhaust_three_restart_budget) {
  mpp_mock_reset();
  mpp_mock_enc_fail_put(4, MPP_ERR_STREAM);
  GstHarness *h = enc_test_harness(
      "mpph264enc",
      "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  GstBus *bus = gst_bus_new();
  gst_element_set_bus(h->element, bus);
  unsigned creates_before = mpp_mock_enc_create_calls();
  unsigned destroys_before = mpp_mock_enc_destroy_calls();
  unsigned configs_before = mpp_mock_control_count(MPP_ENC_SET_CFG);

  fail_unless_equals_int(enc_test_push(h, 0, 30), GST_FLOW_OK);
  GstMessage *message =
      gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
  fail_unless(message != NULL, "restart exhaustion posted no stream error");
  GError *error = NULL;
  gchar *debug = NULL;
  gst_message_parse_error(message, &error, &debug);
  fail_unless(error->domain == GST_STREAM_ERROR);
  fail_unless_equals_int(error->code, GST_STREAM_ERROR_ENCODE);
  fail_unless(g_str_equal(error->message, "encoder restart budget exhausted"));

  guint64 restarts = 0;
  g_object_get(h->element, "encoder-restarts", &restarts, NULL);
  fail_unless_equals_uint64(restarts, 3);
  fail_unless_equals_int(mpp_mock_enc_create_calls(), creates_before + 3);
  fail_unless_equals_int(mpp_mock_enc_destroy_calls(), destroys_before + 3);
  fail_unless_equals_int(mpp_mock_control_count(MPP_ENC_SET_CFG),
                         configs_before + 3);
  g_clear_error(&error);
  g_free(debug);
  gst_message_unref(message);
  gst_element_set_bus(h->element, NULL);
  gst_harness_teardown(h);
  gst_object_unref(bus);
}
GST_END_TEST

GST_START_TEST(test_get_packet_error_restarts_but_timeout_does_not) {
  mpp_mock_reset();
  mpp_mock_enc_fail_get(1, MPP_ERR_TIMEOUT);
  GstHarness *h = enc_test_harness(
      "mpph264enc",
      "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  fail_unless_equals_int(enc_test_push(h, 0, 30), GST_FLOW_OK);
  fail_unless(enc_test_wait_uint(mpp_mock_enc_packet_deinits, 1));
  guint64 restarts = 1;
  g_object_get(h->element, "encoder-restarts", &restarts, NULL);
  fail_unless_equals_uint64(restarts, 0);
  gst_harness_teardown(h);

  mpp_mock_reset();
  mpp_mock_enc_fail_get(1, MPP_ERR_STREAM);
  h = enc_test_harness(
      "mpph264enc",
      "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  fail_unless_equals_int(enc_test_push(h, 0, 30), GST_FLOW_OK);
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  do {
    g_object_get(h->element, "encoder-restarts", &restarts, NULL);
    if (restarts == 1)
      break;
    g_usleep(1000);
  } while (g_get_monotonic_time() < deadline);
  fail_unless_equals_uint64(restarts, 1);
  gst_harness_teardown(h);
}
GST_END_TEST

static Suite *enc_restart_suite(void) {
  Suite *suite = suite_create("enc-restart");
  TCase *test_case = tcase_create("restart-budget");
  tcase_add_test(test_case, test_four_errors_exhaust_three_restart_budget);
  tcase_add_test(test_case,
                 test_get_packet_error_restarts_but_timeout_does_not);
  suite_add_tcase(suite, test_case);
  return suite;
}

GST_CHECK_MAIN(enc_restart)
