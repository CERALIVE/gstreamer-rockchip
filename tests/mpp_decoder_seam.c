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

void mpp_mock_dec_arm (unsigned width, unsigned height);
void mpp_mock_dec_disarm (void);
unsigned mpp_mock_dec_outputs (void);

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

  test_dma_feature_reaches_negotiated_caps ();
  test_unmatched_pts_pending_list_is_bounded ();

  g_print ("mpp-decoder-seam: OK\n");
  return 0;
}
