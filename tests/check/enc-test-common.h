#ifndef CERALIVE_ENC_TEST_COMMON_H
#define CERALIVE_ENC_TEST_COMMON_H

#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/video/video.h>
#include <rockchip/rk_mpi.h>

extern void mpp_mock_reset(void);
extern unsigned mpp_mock_control_count(int cmd);
extern int mpp_mock_last_cfg_s32(const char *name);
extern void mpp_mock_enc_arm_reset_drain(void);
extern void mpp_mock_enc_fail_put(unsigned count, MPP_RET error);
extern void mpp_mock_enc_fail_get(unsigned count, MPP_RET error);
extern unsigned mpp_mock_enc_packet_deinits(void);
extern unsigned mpp_mock_enc_create_calls(void);
extern unsigned mpp_mock_enc_destroy_calls(void);

static inline GstHarness *enc_test_harness(const char *factory,
                                           const char *caps) {
  GstHarness *h = gst_harness_new(factory);
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, "zero-copy-pkt", FALSE,
               NULL);
  gst_harness_set_src_caps_str(h, caps);
  gst_harness_set_drop_buffers(h, TRUE);
  gst_harness_play(h);
  return h;
}

static inline GstFlowReturn enc_test_push(GstHarness *h, guint index,
                                          gint fps) {
  GstBuffer *buffer = gst_buffer_new_allocate(NULL, 320 * 240 * 3 / 2, NULL);
  fail_unless(buffer != NULL);
  GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(index, GST_SECOND, fps);
  GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, fps);
  return gst_harness_push(h, buffer);
}

static inline gboolean enc_test_wait_uint(unsigned (*read_value)(void),
                                          unsigned expected) {
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (read_value() < expected) {
    if (g_get_monotonic_time() >= deadline)
      return FALSE;
    g_usleep(1000);
  }
  return TRUE;
}

#endif
