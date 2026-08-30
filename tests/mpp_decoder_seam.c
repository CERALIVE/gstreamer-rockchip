/*
 * Host-only decoder-seam regression tests for mppvideodec.
 *
 * Both cases run entirely on the mocked MPP (tests/mock_mpp.c) and touch no
 * Rockchip hardware: the mock feeds the decode loop MppFrames that carry no
 * MppBuffer, so every output takes the loop's drop path -- which is exactly the
 * path that consumes a pending GstVideoCodecFrame.
 */
#include <gst/allocators/gstdmabuf.h>
#include <gst/check/gstharness.h>
#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>
#include <rockchip/rk_mpi.h>

void mpp_mock_dec_arm (unsigned width, unsigned height);
void mpp_mock_dec_disarm (void);
unsigned mpp_mock_dec_outputs (void);
void mpp_mock_dec_set_put_result (unsigned buffer_full_count, MPP_RET terminal);
unsigned mpp_mock_dec_put_calls (void);
void mpp_mock_dec_set_next_packet_has_buffer (int has_buffer);
unsigned mpp_mock_dec_packet_buffer_queries (void);
unsigned mpp_mock_dec_packet_post_send_accesses (void);
unsigned mpp_mock_dec_packet_deinits (void);
unsigned mpp_mock_dec_wrong_owner_deinits (void);
unsigned mpp_mock_dec_double_deinits (void);
unsigned mpp_mock_dec_frame_deinits (void);

#define DEC_WIDTH 320
#define DEC_HEIGHT 240
#define SINK_CAPS_STR                                                   \
  "video/x-h264,parsed=(boolean)true,stream-format=(string)byte-stream," \
  "alignment=(string)au,width=(int)" G_STRINGIFY (DEC_WIDTH) ","        \
  "height=(int)" G_STRINGIFY (DEC_HEIGHT) ",framerate=(fraction)30/1"

/* gstmppdec.c's own GST_MPP_DEC_MAX_PENDING_FRAMES. Restated rather than
 * included because it is private to the plugin. */
#define PENDING_BOUND 64
#define PENDING_INPUTS 300
#define PENDING_SLACK 8
#define DRAIN_TIMEOUT_US (20 * G_USEC_PER_SEC)

static GstBuffer *
make_input_buffer (guint index)
{
  GstBuffer *buf = gst_buffer_new_allocate (NULL, 64, NULL);

  gst_buffer_memset (buf, 0, 0, 64);
  /* Far above the mock's stale output PTS so the decoder's display-order
   * matcher can never pair an output with a pending frame. */
  GST_BUFFER_PTS (buf) = 10 * GST_SECOND + index * (GST_SECOND / 30);
  GST_BUFFER_DTS (buf) = GST_BUFFER_PTS (buf);
  GST_BUFFER_DURATION (buf) = GST_SECOND / 30;
  return buf;
}

static gboolean
wait_for_outputs (guint want)
{
  gint64 deadline = g_get_monotonic_time () + DRAIN_TIMEOUT_US;

  while (mpp_mock_dec_outputs () < want) {
    if (g_get_monotonic_time () > deadline)
      return FALSE;
    g_usleep (1000);
  }
  return TRUE;
}

static gboolean
wait_for_frame_deinits (guint want)
{
  gint64 deadline = g_get_monotonic_time () + DRAIN_TIMEOUT_US;

  while (mpp_mock_dec_frame_deinits () < want) {
    if (g_get_monotonic_time () > deadline)
      return FALSE;
    g_usleep (1000);
  }
  return TRUE;
}

static guint
pending_frame_count (GstHarness * h)
{
  GList *frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (h->element));
  guint n = g_list_length (frames);

  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);
  return n;
}

static GstHarness *
start_decoder (gboolean dma_feature)
{
  GstHarness *h = gst_harness_new ("mppvideodec");

  g_assert_nonnull (h);
  g_object_set (h->element, "dma-feature", dma_feature, NULL);
  mpp_mock_dec_arm (DEC_WIDTH, DEC_HEIGHT);
  gst_harness_set_src_caps_str (h, SINK_CAPS_STR);
  return h;
}

static void
stop_decoder (GstHarness * h)
{
  mpp_mock_dec_disarm ();
  gst_harness_teardown (h);
}

static gboolean
negotiated_caps_have_dmabuf (gboolean dma_feature)
{
  GstHarness *h = start_decoder (dma_feature);
  GstCaps *caps;
  gchar *caps_str;
  gboolean have_dmabuf;

  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (1));

  caps = gst_pad_get_current_caps (GST_VIDEO_DECODER_SRC_PAD (h->element));
  g_assert_nonnull (caps);
  have_dmabuf = gst_caps_features_contains (gst_caps_get_features (caps, 0),
      GST_CAPS_FEATURE_MEMORY_DMABUF);
  caps_str = gst_caps_to_string (caps);
  g_print ("dma-feature=%s -> negotiated caps %s\n",
      dma_feature ? "true" : "false", caps_str);

  g_free (caps_str);
  gst_caps_unref (caps);
  stop_decoder (h);
  return have_dmabuf;
}

#if GST_CHECK_VERSION(1, 24, 0)
static void
test_dma_drm_peer_selects_dma_drm_caps (void)
{
  GstHarness *h = start_decoder (TRUE);
  GstCaps *caps;
  const GstStructure *structure;
  const gchar *format;
  const gchar *drm_format;
  gchar *caps_str;

  g_print ("== DMA_DRM peer caps negotiation ==\n");

  gst_harness_set_sink_caps_str (h,
      "video/x-raw(memory:DMABuf),format=(string)DMA_DRM");
  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (1));

  caps = gst_pad_get_current_caps (GST_VIDEO_DECODER_SRC_PAD (h->element));
  g_assert_nonnull (caps);
  structure = gst_caps_get_structure (caps, 0);
  format = gst_structure_get_string (structure, "format");
  drm_format = gst_structure_get_string (structure, "drm-format");
  caps_str = gst_caps_to_string (caps);
  g_print ("DMA_DRM-only peer -> negotiated caps %s\n", caps_str);

  g_assert_cmpstr (format, ==, "DMA_DRM");
  g_assert_nonnull (drm_format);
  g_assert_true (g_str_has_prefix (drm_format, "NV12"));

  g_free (caps_str);
  gst_caps_unref (caps);
  stop_decoder (h);
  g_print ("DMA_DRM peer caps negotiation: OK\n");
}
#endif

static void
test_packet_ownership_is_decided_before_send (void)
{
  GstHarness *h = start_decoder (FALSE);

  g_print ("== packet ownership before send ==\n");

  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (1));
  g_assert_cmpuint (mpp_mock_dec_packet_buffer_queries (), ==, 1);
  g_assert_cmpuint (mpp_mock_dec_packet_post_send_accesses (), ==, 0);
  g_assert_cmpuint (mpp_mock_dec_packet_deinits (), ==, 1);
  g_assert_cmpuint (mpp_mock_dec_wrong_owner_deinits (), ==, 0);
  g_assert_cmpuint (mpp_mock_dec_double_deinits (), ==, 0);
  stop_decoder (h);

  h = start_decoder (FALSE);
  mpp_mock_dec_set_next_packet_has_buffer (TRUE);
  g_assert_cmpint (gst_harness_push (h, make_input_buffer (1)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (1));
  g_assert_cmpuint (mpp_mock_dec_packet_buffer_queries (), ==, 1);
  g_assert_cmpuint (mpp_mock_dec_packet_post_send_accesses (), ==, 0);
  g_assert_cmpuint (mpp_mock_dec_packet_deinits (), ==, 0);
  g_assert_cmpuint (mpp_mock_dec_wrong_owner_deinits (), ==, 0);
  g_assert_cmpuint (mpp_mock_dec_double_deinits (), ==, 0);

  g_print ("copy packet released once; buffered packet transferred without "
      "post-send access\n");
  stop_decoder (h);
  g_print ("packet ownership before send: OK\n");
}

static void
test_reset_releases_cached_mpp_frame (void)
{
  GstHarness *h = start_decoder (FALSE);
  guint before_reset;

  g_print ("== reset cached-frame cleanup ==\n");

  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (1));
  g_assert_true (wait_for_frame_deinits (1));
  before_reset = mpp_mock_dec_frame_deinits ();

  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_stop (TRUE)));
  g_assert_cmpuint (mpp_mock_dec_frame_deinits (), ==, before_reset + 1);

  g_print ("cached MPP frame deinits: %u -> %u on reset\n", before_reset,
      mpp_mock_dec_frame_deinits ());
  stop_decoder (h);
  g_print ("reset cached-frame cleanup: OK\n");
}

static void
test_put_packet_result_drives_fullness (void)
{
  GstHarness *h = start_decoder (FALSE);

  g_print ("== put_packet fullness detection ==\n");

  mpp_mock_dec_set_put_result (3, MPP_OK);
  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  g_assert_cmpuint (mpp_mock_dec_put_calls (), ==, 4);
  g_assert_true (wait_for_outputs (1));

  g_print ("three MPP_ERR_BUFFER_FULL results retried; accepted on call %u\n",
      mpp_mock_dec_put_calls ());
  stop_decoder (h);

  h = start_decoder (FALSE);
  mpp_mock_dec_set_put_result (0, MPP_NOK);
  g_assert_cmpint (gst_harness_push (h, make_input_buffer (1)), ==,
      GST_FLOW_ERROR);
  g_assert_cmpuint (mpp_mock_dec_put_calls (), ==, 1);

  g_print ("persistent MPP error rejected after %u call\n",
      mpp_mock_dec_put_calls ());
  stop_decoder (h);
  g_print ("put_packet fullness detection: OK\n");
}

static void
test_dma_feature_reaches_negotiated_caps (void)
{
  g_print ("== dmabuf caps negotiation ==\n");

  if (!negotiated_caps_have_dmabuf (TRUE))
    g_error ("dma-feature=true did not put memory:DMABuf on the output caps");

  /* Negative control: without the property the feature must stay off, so a
   * gate that unconditionally stamped DMABuf could not pass. */
  if (negotiated_caps_have_dmabuf (FALSE))
    g_error ("dma-feature=false still exposed memory:DMABuf");

  g_print ("dmabuf caps negotiation: OK\n");
}

static void
test_unmatched_pts_pending_list_is_bounded (void)
{
  GstHarness *h = start_decoder (FALSE);
  guint pending;
  guint i;

  g_print ("== unmatched-PTS pending bound ==\n");

  for (i = 0; i < PENDING_INPUTS; i++)
    g_assert_cmpint (gst_harness_push (h, make_input_buffer (i)), ==,
        GST_FLOW_OK);

  if (!wait_for_outputs (PENDING_INPUTS))
    g_error ("decoder produced %u/%u outputs before the drain timeout",
        mpp_mock_dec_outputs (), PENDING_INPUTS);

  g_usleep (200 * 1000);
  pending = pending_frame_count (h);

  g_print ("pushed %u inputs, %u MPP outputs, pending frames settled at %u "
      "(bound %u + slack %u)\n", PENDING_INPUTS, mpp_mock_dec_outputs (),
      pending, PENDING_BOUND, PENDING_SLACK);

  if (pending > PENDING_BOUND + PENDING_SLACK)
    g_error ("pending frame list grew to %u after %u unmatched-PTS outputs; "
        "expected it to stay at or below %u", pending, PENDING_INPUTS,
        PENDING_BOUND + PENDING_SLACK);

  stop_decoder (h);
  g_print ("unmatched-PTS pending bound: OK\n");
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);

  test_reset_releases_cached_mpp_frame ();
  test_packet_ownership_is_decided_before_send ();
  test_put_packet_result_drives_fullness ();
  test_dma_feature_reaches_negotiated_caps ();
#if GST_CHECK_VERSION(1, 24, 0)
  test_dma_drm_peer_selects_dma_drm_caps ();
#else
  g_print ("DMA_DRM peer caps negotiation: SKIP (requires GStreamer >= 1.24)\n");
#endif
  test_unmatched_pts_pending_list_is_bounded ();

  g_print ("mpp-decoder-seam: OK\n");
  return 0;
}
