/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "program-target.h"
#include <gst/check/gstharness.h>
#include <unistd.h>

extern void mpp_mock_reset(void);
extern unsigned mpp_mock_enc_create_calls(void);
extern unsigned mpp_mock_enc_destroy_calls(void);
extern unsigned mpp_mock_control_count(int cmd);
extern void mpi_fixture_payload(const guint8 *data, gsize size);
extern unsigned mpp_mock_enc_live_packets(void);
extern unsigned mpp_mock_enc_live_buffers(void);
extern unsigned mpp_mock_enc_packet_double_deinits(void);

static void write_bytes(const char *path, const GByteArray *bytes) {
  GError *error = NULL;
  g_assert_true(g_file_set_contents(path, (const char *)bytes->data,
                                   bytes->len, &error));
  g_assert_no_error(error);
}

static void decode(const char *path, guint first, guint count) {
  gchar *raw = g_strconcat(path, ".yuv", NULL);
  gchar *args[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                   "-xerror", "-err_detect", "explode", "-i", (gchar *)path,
                   "-threads", "1", "-pix_fmt", "yuv420p", "-f", "rawvideo",
                   "-y", raw, NULL};
  gchar **env = g_get_environ();
  env = g_environ_unsetenv(env, "LD_PRELOAD");
  gchar *stderr_text = NULL;
  gint status;
  GError *error = NULL;
  g_assert_true(g_spawn_sync(NULL, args, env, G_SPAWN_SEARCH_PATH, NULL, NULL,
                             NULL, &stderr_text, &status, &error));
  g_assert_no_error(error);
  if (!g_spawn_check_wait_status(status, &error))
    g_error("independent decoder failed: %s / %s", error->message, stderr_text);
  g_assert_cmpstr(stderr_text, ==, "");
  gchar *pixels = NULL;
  gsize size;
  g_assert_true(g_file_get_contents(raw, &pixels, &size, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(size, ==, count * 320 * 240 * 3 / 2);
  for (guint frame = 0; frame < count; frame++) {
    const guint8 *yuv = (const guint8 *)pixels + frame * 320 * 240 * 3 / 2;
    guint8 luma = (first + frame) % 2 ? 235 : 16;
    for (guint i = 0; i < 320 * 240; i++)
      g_assert_cmpuint(yuv[i], ==, luma);
    for (guint i = 320 * 240; i < 320 * 240 * 3 / 2; i++)
      g_assert_cmpuint(yuv[i], ==, 128);
  }
  g_print("independent-decode file=%s first=%u frames=%u alternating-luma=PASS\n",
          path, first, count);
  g_free(pixels);
  g_free(stderr_text);
  g_strfreev(env);
  g_free(raw);
}

static GstBuffer *pull(GstHarness *h) {
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  GstBuffer *buffer;
  while (!(buffer = gst_harness_try_pull(h))) {
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    g_usleep(1000);
  }
  return buffer;
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  g_assert_cmpint(argc, ==, 7);
  g_assert_true(g_str_equal(argv[5], "armed") || g_str_equal(argv[5], "disarmed"));
  g_assert_true(g_str_equal(argv[6], "zero-copy") || g_str_equal(argv[6], "copy"));
  const char *factory = argv[1], *black_path = argv[2], *white_path = argv[3];
  const char *output = argv[4];
  gboolean armed = g_str_equal(argv[5], "armed");
  gboolean zero_copy = g_str_equal(argv[6], "zero-copy");
  gchar *payloads[2];
  gsize sizes[2];
  g_assert_true(g_file_get_contents(black_path, &payloads[0], &sizes[0], NULL));
  g_assert_true(g_file_get_contents(white_path, &payloads[1], &sizes[1], NULL));
  GByteArray *all = g_byte_array_new(), *after = g_byte_array_new();
  GstElement *wrong_type = gst_element_factory_make("identity", "venc_bps");
  g_assert_nonnull(wrong_type);
  g_assert_cmpuint(mpi_test_program_id(wrong_type), ==, 0);
  g_assert_cmpint(mpi_test_arm_program(wrong_type, 1), ==, MPI_TEST_NOT_FOUND);
  gst_object_unref(wrong_type);

  mpp_mock_reset();
  GstHarness *h = gst_harness_new(factory);
  g_assert_nonnull(h);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1,
                "zero-copy-pkt", zero_copy, NULL);
  gst_harness_set_src_caps_str(h,
      "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_play(h);
  g_assert_cmpuint(mpi_test_program_id(h->element), ==, 0);
  g_assert_true(gst_object_set_name(GST_OBJECT(h->element), "venc_bps"));
  GstElement *identity = h->element;
  GstBus *bus = gst_bus_new();
  gst_element_set_bus(identity, bus);
  GstPad *src = gst_element_get_static_pad(identity, "src");
  pid_t pid = getpid();
  gchar *stream_id = NULL;
  guint64 original_id = 0, original_restarts = 0;
  unsigned creates = 0, destroys = 0, configs = 0;
  guint expected_delta = armed ? 1 : 0;
  for (guint i = 0; i < 8; i++) {
    if (i == 3) {
      write_bytes(output, all);
      decode(output, 0, 3);
      stream_id = gst_pad_get_stream_id(src);
      g_assert_nonnull(stream_id);
      original_id = mpi_test_program_id(identity);
      g_assert_cmpuint(original_id, >, 0);
      g_object_get(identity, "encoder-restarts", &original_restarts, NULL);
      g_assert_cmpuint(original_restarts, ==, 0);
      creates = mpp_mock_enc_create_calls();
      destroys = mpp_mock_enc_destroy_calls();
      configs = mpp_mock_control_count(MPP_ENC_SET_CFG);
      g_assert_cmpint(mpi_test_arm_program(identity, original_id + 1), ==,
                      MPI_TEST_NOT_FOUND);
      if (armed)
        g_assert_cmpint(mpi_test_arm_program(identity, original_id), ==,
                        MPI_TEST_ARMED);
      g_print("host-before pid=%ld element=%p stream=%s context=%"
              G_GUINT64_FORMAT " counter=%" G_GUINT64_FORMAT "\n",
              (long)pid, (void *)identity, stream_id, original_id,
              original_restarts);
    }
    mpi_fixture_payload((const guint8 *)payloads[i % 2], sizes[i % 2]);
    GstBuffer *input = gst_buffer_new_allocate(NULL, 320 * 240 * 3 / 2, NULL);
    g_assert_nonnull(input);
    GST_BUFFER_PTS(input) = gst_util_uint64_scale(i, GST_SECOND, 30);
    GST_BUFFER_DURATION(input) = gst_util_uint64_scale(1, GST_SECOND, 30);
    g_assert_cmpint(gst_harness_push(h, input), ==, GST_FLOW_OK);
    GstBuffer *buffer = pull(h);
    g_assert_cmpuint(GST_BUFFER_PTS(buffer), ==,
                     gst_util_uint64_scale(i, GST_SECOND, 30));
    GstMapInfo map;
    g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_READ));
    g_assert_cmpmem(map.data, map.size, payloads[i % 2], sizes[i % 2]);
    g_byte_array_append(all, map.data, map.size);
    if (i >= 3)
      g_byte_array_append(after, map.data, map.size);
    gst_buffer_unmap(buffer, &map);
    gst_buffer_unref(buffer);
  }

  guint64 restarts = 0;
  g_object_get(identity, "encoder-restarts", &restarts, NULL);
  guint64 replacement_id = mpi_test_program_id(identity);
  g_assert_cmpuint(restarts - original_restarts, ==, expected_delta);
  g_assert_cmpuint(mpp_mock_enc_create_calls() - creates, ==, expected_delta);
  g_assert_cmpuint(mpp_mock_enc_destroy_calls() - destroys, ==, expected_delta);
  g_assert_cmpuint(mpp_mock_control_count(MPP_ENC_SET_CFG) - configs, ==,
                   expected_delta);
  g_assert_true(armed ? replacement_id != original_id : replacement_id == original_id);
  MpiTestStats stats;
  g_assert_true(mpi_test_snapshot(replacement_id, &stats));
  g_assert_cmpuint(stats.injections, ==, 0);
  g_assert_cmpuint(stats.healthy_packets, ==, armed ? 5 : 8);
  g_assert_cmpint(getpid(), ==, pid);
  g_assert_true(h->element == identity);
  gchar *current_stream = gst_pad_get_stream_id(src);
  g_assert_cmpstr(current_stream, ==, stream_id);
  GstState state;
  g_assert_cmpint(gst_element_get_state(identity, &state, NULL, 0), ==,
                  GST_STATE_CHANGE_SUCCESS);
  g_assert_cmpint(state, ==, GST_STATE_PLAYING);
  g_assert_null(gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR));
  if (armed) {
    g_assert_false(mpi_test_snapshot(original_id, &stats));
    g_assert_cmpint(mpi_test_arm_program(identity, original_id), ==,
                    MPI_TEST_NOT_FOUND);
    g_assert_cmpint(mpi_test_arm_program(identity, replacement_id), ==,
                    MPI_TEST_SPENT);
  }
  write_bytes(output, all);
  decode(output, 0, 8);
  gchar *post_path = g_strconcat(output, ".post", NULL);
  write_bytes(post_path, after);
  decode(post_path, 3, 5);
  g_print("HOST-MOCK-PASS mode=%s zero_copy=%d pid=%ld element=%p stream=%s "
          "context_before=%" G_GUINT64_FORMAT " context_after=%" G_GUINT64_FORMAT
          " counter_before=%" G_GUINT64_FORMAT " counter_after=%" G_GUINT64_FORMAT
          " creates_delta=%u destroys_delta=%u configs_delta=%u decoded=8 post=5\n",
          argv[5], zero_copy, (long)getpid(), (void *)identity, current_stream,
          original_id, replacement_id, original_restarts, restarts,
          mpp_mock_enc_create_calls() - creates,
          mpp_mock_enc_destroy_calls() - destroys,
          mpp_mock_control_count(MPP_ENC_SET_CFG) - configs);
  gst_element_set_bus(identity, NULL);
  gst_object_unref(src);
  gst_harness_teardown(h);
  gst_object_unref(bus);
  g_assert_cmpuint(mpp_mock_enc_live_packets(), ==, 0);
  g_assert_cmpuint(mpp_mock_enc_live_buffers(), ==, 0);
  g_assert_cmpuint(mpp_mock_enc_packet_double_deinits(), ==, 0);
  mpi_fixture_payload(NULL, 0);
  g_free(payloads[0]);
  g_free(payloads[1]);
  g_free(stream_id);
  g_free(current_stream);
  g_free(post_path);
  g_byte_array_unref(all);
  g_byte_array_unref(after);
  return 0;
}
