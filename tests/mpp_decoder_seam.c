/*
 * Host-only decoder-seam regression tests for mppvideodec.
 *
 * The cases run entirely on the mocked MPP (tests/mock_mpp.c) and touch no
 * Rockchip hardware. Accounting tests use bufferless output; delivery tests use
 * unique fd-backed buffers so frame identity is observable downstream.
 */
#include <gst/allocators/gstdmabuf.h>
#include <gst/check/gstharness.h>
#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>
#include <rockchip/rk_mpi.h>
#include <unistd.h>

void mpp_mock_dec_arm (unsigned width, unsigned height);
void mpp_mock_dec_disarm (void);
void mpp_mock_dec_release_packets (void);
unsigned mpp_mock_dec_live_packets (void);
unsigned mpp_mock_dec_outputs (void);
unsigned mpp_mock_dec_queued (void);
void mpp_mock_dec_set_output_suppressed (int suppressed);
void mpp_mock_dec_set_output_pts (RK_S64 pts);
void mpp_mock_dec_set_output_buffers (int enabled);
void mpp_mock_dec_set_output_paused (int paused);
void mpp_mock_dec_plan_output (unsigned ordinal, RK_S64 pts,
    unsigned identity);
unsigned mpp_mock_dec_accounted_outputs (void);
unsigned mpp_mock_dec_live_output_buffers (void);
void mpp_mock_dec_set_frame_format (MppFrameFormat format);
void mpp_mock_dec_set_put_result (unsigned buffer_full_count, MPP_RET terminal);
unsigned mpp_mock_dec_put_calls (void);
unsigned mpp_mock_dec_reset_calls (void);
void mpp_mock_dec_set_next_packet_has_buffer (int has_buffer);
unsigned mpp_mock_dec_packet_buffer_queries (void);
unsigned mpp_mock_dec_packet_post_send_accesses (void);
unsigned mpp_mock_dec_packet_deinits (void);
unsigned mpp_mock_dec_wrong_owner_deinits (void);
unsigned mpp_mock_dec_double_deinits (void);
unsigned mpp_mock_dec_frame_deinits (void);
unsigned mpp_mock_internal_group_types (void);
void mpp_mock_jpeg_set_input_timeouts (unsigned count);
unsigned mpp_mock_jpeg_input_poll_calls (void);
void mpp_mock_jpeg_block_input_poll (void);
unsigned mpp_mock_jpeg_input_poll_entered (void);
unsigned mpp_mock_jpeg_input_enqueues (void);

#define DEC_WIDTH 320
#define DEC_HEIGHT 240
#define H264_BYTE_STREAM_CAPS                                          \
  "video/x-h264,parsed=(boolean)true,stream-format=(string)byte-stream," \
  "alignment=(string)au,width=(int)" G_STRINGIFY (DEC_WIDTH) ","        \
  "height=(int)" G_STRINGIFY (DEC_HEIGHT) ",framerate=(fraction)30/1"
#define H264_AVC_CAPS                                                   \
  "video/x-h264,parsed=(boolean)true,stream-format=(string)avc,"        \
  "alignment=(string)au,codec_data=(buffer)"                           \
  "014d4015ffe10017674d4015eca4bf2e0220000003002ee6b28001e2c5b2c001"  \
  "000468ebecb2,width=(int)" G_STRINGIFY (DEC_WIDTH) ",height=(int)"   \
  G_STRINGIFY (DEC_HEIGHT) ",framerate=(fraction)30/1"
#define H265_BYTE_STREAM_CAPS                                          \
  "video/x-h265,parsed=(boolean)true,stream-format=(string)byte-stream," \
  "alignment=(string)au,width=(int)" G_STRINGIFY (DEC_WIDTH) ","        \
  "height=(int)" G_STRINGIFY (DEC_HEIGHT) ",framerate=(fraction)30/1"
#define H265_HVC1_CAPS                                                  \
  "video/x-h265,parsed=(boolean)true,stream-format=(string)hvc1,"       \
  "alignment=(string)au,codec_data=(buffer)"                           \
  "0104080000009808000000003ff000fcfffcfc00000f,width=(int)"          \
  G_STRINGIFY (DEC_WIDTH) ",height=(int)" G_STRINGIFY (DEC_HEIGHT) "," \
  "framerate=(fraction)30/1"
#define H264_UNKNOWN_FORMAT_CAPS                                       \
  "video/x-h264,parsed=(boolean)true,alignment=(string)au,width=(int)" \
  G_STRINGIFY (DEC_WIDTH) ",height=(int)" G_STRINGIFY (DEC_HEIGHT) "," \
  "framerate=(fraction)30/1"
#define VP8_CAPS                                                        \
  "video/x-vp8,width=(int)" G_STRINGIFY (DEC_WIDTH) ",height=(int)"    \
  G_STRINGIFY (DEC_HEIGHT) ",framerate=(fraction)30/1"
#define VP9_CAPS                                                        \
  "video/x-vp9,width=(int)" G_STRINGIFY (DEC_WIDTH) ",height=(int)"    \
  G_STRINGIFY (DEC_HEIGHT) ",framerate=(fraction)30/1"
#define H263_CAPS                                                       \
  "video/x-h263,parsed=(boolean)true,width=(int)"                      \
  G_STRINGIFY (DEC_WIDTH) ",height=(int)" G_STRINGIFY (DEC_HEIGHT) "," \
  "framerate=(fraction)30/1"
#define AV1_CAPS                                                        \
  "video/x-av1,parsed=(boolean)true,width=(int)"                       \
  G_STRINGIFY (DEC_WIDTH) ",height=(int)" G_STRINGIFY (DEC_HEIGHT) "," \
  "framerate=(fraction)30/1"
#define MPEG4_CAPS                                                      \
  "video/mpeg,parsed=(boolean)true,mpegversion=(int)4,"                \
  "systemstream=(boolean)false,width=(int)" G_STRINGIFY (DEC_WIDTH) "," \
  "height=(int)" G_STRINGIFY (DEC_HEIGHT) ",framerate=(fraction)30/1"
#define JPEG_CAPS                                                       \
  "image/jpeg,parsed=(boolean)true,width=(int)"                        \
  G_STRINGIFY (DEC_WIDTH) ",height=(int)" G_STRINGIFY (DEC_HEIGHT) "," \
  "framerate=(fraction)30/1"
#define SINK_CAPS_STR H264_BYTE_STREAM_CAPS

#define PENDING_INPUTS 300
#define HEADER_INPUTS 300
#define NORMAL_INPUTS 8
#define DRAIN_TIMEOUT_US (20 * G_USEC_PER_SEC)
#define ACCOUNTING_TIMEOUT_US (2 * G_USEC_PER_SEC)

static guint accounting_failures;

/* Real parser fixtures copied from GStreamer's h264parse/h265parse tests. */
static const guint8 h264_parameter_sets_bytestream[] = {
  0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x40, 0x15,
  0xec, 0xa4, 0xbf, 0x2e, 0x02, 0x20, 0x00, 0x00,
  0x03, 0x00, 0x2e, 0xe6, 0xb2, 0x80, 0x01, 0xe2,
  0xc5, 0xb2, 0xc0,
  0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xec, 0xb2,
};

static const guint8 h264_parameter_sets_avc[] = {
  0x00, 0x00, 0x00, 0x17,
  0x67, 0x4d, 0x40, 0x15, 0xec, 0xa4, 0xbf, 0x2e,
  0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0x2e, 0xe6,
  0xb2, 0x80, 0x01, 0xe2, 0xc5, 0xb2, 0xc0,
  0x00, 0x00, 0x00, 0x04, 0x68, 0xeb, 0xec, 0xb2,
};

static const guint8 h265_parameter_sets_bytestream[] = {
  0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01,
  0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
  0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
  0x3f, 0x95, 0x98, 0x09,
  0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01,
  0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x03, 0x00, 0x3f, 0xa0, 0x88,
  0x45, 0x96, 0x56, 0x6a, 0xbc, 0xaf, 0xff, 0x00,
  0x01, 0x00, 0x01, 0x6a, 0x0c, 0x02, 0x0c, 0x08,
  0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03,
  0x00, 0xf0, 0x40,
  0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x73,
  0xd0, 0x89,
};

static const guint8 h265_parameter_sets_hvc1[] = {
  0x00, 0x00, 0x00, 0x18,
  0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60,
  0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x03, 0x00, 0x3f, 0x95, 0x98, 0x09,
  0x00, 0x00, 0x00, 0x2f,
  0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03,
  0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
  0x00, 0x3f, 0xa0, 0x88, 0x45, 0x96, 0x56, 0x6a,
  0xbc, 0xaf, 0xff, 0x00, 0x01, 0x00, 0x01, 0x6a,
  0x0c, 0x02, 0x0c, 0x08, 0x00, 0x00, 0x03, 0x00,
  0x08, 0x00, 0x00, 0x03, 0x00, 0xf0, 0x40,
  0x00, 0x00, 0x00, 0x06, 0x44, 0x01, 0xc1, 0x73,
  0xd0, 0x89,
};

static const guint8 h264_idr_frame[] = {
  0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00,
  0x10, 0xff, 0xfe, 0xf6, 0xf0, 0xfe, 0x05, 0x36,
  0x56, 0x04, 0x50, 0x96, 0x7b, 0x3f, 0x53, 0xe1,
};

static const guint8 h264_idr_frame_avc[] = {
  0x00, 0x00, 0x00, 0x14, 0x65, 0x88, 0x84, 0x00,
  0x10, 0xff, 0xfe, 0xf6, 0xf0, 0xfe, 0x05, 0x36,
  0x56, 0x04, 0x50, 0x96, 0x7b, 0x3f, 0x53, 0xe1,
};

static const guint8 h264_idr_slice_1[] = {
  0x00, 0x00, 0x00, 0x01, 0x65, 0xb8, 0x00, 0x04,
  0x00, 0x00, 0x11, 0xff, 0xff, 0xf8, 0x22, 0x8a,
  0x1f, 0x1c, 0x00, 0x04, 0x0a, 0x63, 0x80, 0x00,
  0x81, 0xec, 0x9a, 0x93, 0x93, 0x93, 0x93, 0x93,
  0x93, 0xad, 0x57, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
  0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
  0x75, 0xd7, 0x5d, 0x78,
};

static const guint8 h264_idr_slice_2[] = {
  0x00, 0x00, 0x00, 0x01, 0x65, 0x04, 0x2e, 0x00,
  0x01, 0x00, 0x00, 0x04, 0x7f, 0xff, 0xfe, 0x08,
  0xa2, 0x87, 0xc7, 0x00, 0x01, 0x02, 0x98, 0xe0,
  0x00, 0x20, 0x7b, 0x26, 0xa4, 0xe4, 0xe4, 0xe4,
  0xe4, 0xe4, 0xeb, 0x55, 0xd7, 0x5d, 0x75, 0xd7,
  0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
  0xd7, 0x5d, 0x75, 0xd7, 0x5e,
};

static const guint8 h264_p_slice[] = {
  0x00, 0x00, 0x00, 0x01, 0x61, 0xe0, 0x00,
  0x40, 0x00, 0x9c, 0x82, 0x3c, 0x10, 0xc0,
};

static GstClockTime
input_pts (guint index)
{
  return 10 * GST_SECOND + index * (GST_SECOND / 30);
}

static GstBuffer *
make_sized_input_buffer (gsize size, GstClockTime pts)
{
  GstBuffer *buf = gst_buffer_new_allocate (NULL, size, NULL);

  gst_buffer_memset (buf, 0, 0, size);
  GST_BUFFER_PTS (buf) = pts;
  GST_BUFFER_DTS (buf) = pts;
  GST_BUFFER_DURATION (buf) = GST_SECOND / 30;
  return buf;
}

static GstBuffer *
make_fixture_input_buffer (const guint8 * data, gsize size, GstClockTime pts)
{
  GstBuffer *buf = gst_buffer_new_allocate (NULL, size, NULL);

  gst_buffer_fill (buf, 0, data, size);
  GST_BUFFER_PTS (buf) = pts;
  GST_BUFFER_DTS (buf) = pts;
  GST_BUFFER_DURATION (buf) = GST_SECOND / 30;
  return buf;
}

static GstBuffer *
make_composite_input_buffer (GstClockTime pts, const guint8 * const * parts,
    const gsize * sizes, guint count)
{
  GstBuffer *buf = gst_buffer_new ();
  guint i;

  for (i = 0; i < count; i++)
    buf = gst_buffer_append (buf,
        make_fixture_input_buffer (parts[i], sizes[i], pts));

  GST_BUFFER_PTS (buf) = pts;
  GST_BUFFER_DTS (buf) = pts;
  GST_BUFFER_DURATION (buf) = GST_SECOND / 30;
  return buf;
}

static GstBuffer *
make_input_buffer (guint index)
{
  /* Far above the mock's stale output PTS so the decoder's display-order
   * matcher can never pair an output with a pending frame. */
  return make_sized_input_buffer (64, input_pts (index));
}

static GstBuffer *
make_untimestamped_input_buffer (gsize size)
{
  return make_sized_input_buffer (size, GST_CLOCK_TIME_NONE);
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
wait_for_accounted_outputs (guint want)
{
  gint64 deadline = g_get_monotonic_time () + DRAIN_TIMEOUT_US;

  while (mpp_mock_dec_accounted_outputs () < want) {
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

static GstCaps *
wait_for_current_src_caps (GstHarness * h)
{
  gint64 deadline = g_get_monotonic_time () + DRAIN_TIMEOUT_US;
  GstCaps *caps;

  while (!(caps = gst_pad_get_current_caps (
              GST_VIDEO_DECODER_SRC_PAD (h->element)))) {
    if (g_get_monotonic_time () > deadline)
      return NULL;
    g_usleep (1000);
  }

  return caps;
}

static guint
pending_frame_count (GstHarness * h)
{
  GList *frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (h->element));
  guint n = g_list_length (frames);

  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);
  return n;
}

static gboolean
wait_for_pending_count (GstHarness * h, guint want, gint64 timeout_us)
{
  gint64 deadline = g_get_monotonic_time () + timeout_us;

  while (pending_frame_count (h) != want) {
    if (g_get_monotonic_time () > deadline)
      return FALSE;
    g_usleep (1000);
  }
  return TRUE;
}

static GstBuffer *
wait_for_output_buffer (GstHarness * h)
{
  gint64 deadline = g_get_monotonic_time () + DRAIN_TIMEOUT_US;
  GstBuffer *buffer;

  while (!(buffer = gst_harness_try_pull (h))) {
    if (g_get_monotonic_time () > deadline)
      return NULL;
    g_usleep (1000);
  }
  return buffer;
}

static guint8
output_identity (GstBuffer * buffer)
{
  GstMapInfo map = GST_MAP_INFO_INIT;
  guint8 identity;

  g_assert_true (gst_buffer_map (buffer, &map, GST_MAP_READ));
  g_assert_cmpuint (map.size, >, 0);
  identity = map.data[0];
  gst_buffer_unmap (buffer, &map);
  return identity;
}

static GstHarness *
start_decoder_with_caps (gboolean dma_feature, const gchar * caps)
{
  GstHarness *h = gst_harness_new ("mppvideodec");

  g_assert_nonnull (h);
  g_object_set (h->element, "dma-feature", dma_feature, NULL);
  mpp_mock_dec_arm (DEC_WIDTH, DEC_HEIGHT);
  gst_harness_set_src_caps_str (h, caps);
  return h;
}

static GstHarness *
start_element_with_caps (const gchar * element, const gchar * caps)
{
  GstHarness *h;

  mpp_mock_dec_arm (DEC_WIDTH, DEC_HEIGHT);
  h = gst_harness_new (element);
  g_assert_nonnull (h);
  gst_harness_set_src_caps_str (h, caps);
  return h;
}

static GstHarness *
start_decoder (gboolean dma_feature)
{
  return start_decoder_with_caps (dma_feature, SINK_CAPS_STR);
}

/* Reclaim is only safe once the element has stopped, because it deinitializes
 * its own packets on the way down. Asserting emptiness here rather than
 * trusting a later arm/reset is what keeps the last test in the suite honest. */
static void
reclaim_mock_packets (void)
{
  mpp_mock_dec_release_packets ();
  g_assert_cmpuint (mpp_mock_dec_live_packets (), ==, 0);
  g_assert_cmpuint (mpp_mock_dec_live_output_buffers (), ==, 0);
}

/* Disarm first so the element can drain, tear down, and only then reclaim. */
static void
stop_decoder (GstHarness * h)
{
  mpp_mock_dec_disarm ();
  gst_harness_teardown (h);
  reclaim_mock_packets ();
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
static GstHarness *
start_afbc_decoder (void)
{
  GstHarness *h = gst_harness_new ("mppvideodec");

  g_assert_nonnull (h);
  g_object_set (h->element, "dma-feature", TRUE, "fbc", TRUE, NULL);
  mpp_mock_dec_arm (DEC_WIDTH, DEC_HEIGHT);
  mpp_mock_dec_set_frame_format (
      MPP_FMT_YUV420SP | MPP_FRAME_FBC_AFBC_V2);
  gst_harness_set_src_caps_str (h, SINK_CAPS_STR);
  return h;
}

static gboolean
query_dma_drm_only (GstPad * pad, GstObject * parent, GstQuery * query)
{
  if (GST_QUERY_TYPE (query) == GST_QUERY_CAPS) {
    GstCaps *accepted = gst_caps_from_string (
        "video/x-raw(memory:DMABuf),format=(string)DMA_DRM");
    GstCaps *result;
    GstCaps *filter;

    gst_query_parse_caps (query, &filter);
    result = filter ? gst_caps_intersect_full (accepted, filter,
        GST_CAPS_INTERSECT_FIRST) : gst_caps_ref (accepted);
    gst_query_set_caps_result (query, result);
    gst_caps_unref (result);
    gst_caps_unref (accepted);
    return TRUE;
  }

  return gst_pad_query_default (pad, parent, query);
}

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

static void
test_afbc_output_is_never_advertised_as_linear_dma_drm (void)
{
  GstHarness *h = start_afbc_decoder ();
  GstCaps *caps;
  const GstStructure *structure;
  const gchar *format;
  gint afbc = 0;
  gchar *caps_str;

  g_print ("== AFBC output rejects linear DMA_DRM caps ==\n");

  gst_pad_set_query_function (h->sinkpad,
      GST_DEBUG_FUNCPTR (query_dma_drm_only));

  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  caps = wait_for_current_src_caps (h);
  g_assert_nonnull (caps);
  structure = gst_caps_get_structure (caps, 0);
  format = gst_structure_get_string (structure, "format");
  caps_str = gst_caps_to_string (caps);
  g_print ("AFBC frame with DMA_DRM query peer -> negotiated caps %s\n",
      caps_str);

  g_assert_cmpstr (format, !=, "DMA_DRM");
  g_assert_true (gst_structure_get_int (structure, "arm-afbc", &afbc));
  g_assert_cmpint (afbc, ==, 1);

  g_free (caps_str);
  gst_caps_unref (caps);
  stop_decoder (h);
  g_print ("AFBC output rejects linear DMA_DRM caps: OK\n");
}
#endif

static void
test_decoder_allocator_requests_dma32 (void)
{
  GstHarness *h = start_decoder (FALSE);
  unsigned group_types;

  g_print ("== decoder allocator DMA32 request ==\n");

  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (1));
  group_types = mpp_mock_internal_group_types ();
  g_print ("internal MPP buffer group type flags: 0x%x\n", group_types);
  g_assert_cmpuint (group_types & MPP_BUFFER_FLAGS_DMA32, ==,
      MPP_BUFFER_FLAGS_DMA32);

  stop_decoder (h);
  g_print ("decoder allocator DMA32 request: OK\n");
}

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

static GstFlowReturn
finish_decoder (GstHarness * h)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (h->element);
  GstVideoDecoderClass *klass = GST_VIDEO_DECODER_GET_CLASS (decoder);
  GstFlowReturn ret;

  GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  ret = klass->finish (decoder);
  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
  return ret;
}

static void
test_video_drain_classifies_put_packet_errors (void)
{
  GstHarness *h;
  guint calls_before;
  gint64 started;

  g_print ("== video drain put-packet error classification ==\n");
  h = start_decoder (FALSE);
  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (1));
  calls_before = mpp_mock_dec_put_calls ();
  mpp_mock_dec_set_put_result (3, MPP_OK);
  g_assert_cmpint (finish_decoder (h), ==, GST_FLOW_OK);
  g_assert_cmpuint (mpp_mock_dec_put_calls (), ==, calls_before + 4);
  g_print ("drain retried three queue-full results and accepted EOS on call %u\n",
      mpp_mock_dec_put_calls ());
  stop_decoder (h);

  h = start_decoder (FALSE);
  g_assert_cmpint (gst_harness_push (h, make_input_buffer (1)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (1));
  calls_before = mpp_mock_dec_put_calls ();
  mpp_mock_dec_set_put_result (0, MPP_NOK);
  started = g_get_monotonic_time ();
  g_assert_cmpint (finish_decoder (h), ==, GST_FLOW_ERROR);
  g_assert_cmpint (g_get_monotonic_time () - started, <, G_USEC_PER_SEC);
  g_assert_cmpuint (mpp_mock_dec_put_calls (), ==, calls_before + 1);
  g_assert_cmpuint (mpp_mock_dec_reset_calls (), >, 0);
  g_print ("permanent drain error failed after one call and triggered reset\n");
  stop_decoder (h);
}

static void
test_jpeg_input_timeout_is_retried (void)
{
  GstHarness *h;
  GstBuffer *buffer;

  g_print ("== jpeg input timeout retry ==\n");
  mpp_mock_dec_arm (DEC_WIDTH, DEC_HEIGHT);
  mpp_mock_jpeg_set_input_timeouts (1);

  h = gst_harness_new ("mppjpegdec");
  g_assert_nonnull (h);
  gst_harness_set_src_caps_str (h,
      "image/jpeg,parsed=(boolean)true,width=(int)320,height=(int)240,"
      "framerate=(fraction)30/1");

  buffer = make_input_buffer (0);
  g_assert_cmpint (gst_harness_push (h, buffer), ==, GST_FLOW_OK);
  g_assert_cmpuint (mpp_mock_jpeg_input_poll_calls (), ==, 2);

  mpp_mock_dec_disarm ();
  gst_harness_teardown (h);
  reclaim_mock_packets ();
  g_print ("one MPP_ERR_TIMEOUT retried; accepted on poll 2\n");
}

typedef struct
{
  GstHarness *h;
  GstFlowReturn result;
  gint done;
} DecoderFinishCall;

static gpointer
finish_decoder_thread (gpointer data)
{
  DecoderFinishCall *call = data;

  call->result = finish_decoder (call->h);
  g_atomic_int_set (&call->done, TRUE);
  return NULL;
}

static void
test_jpeg_blocked_drain_is_cancelled_by_flush (void)
{
  DecoderFinishCall call = { 0, };
  GstVideoDecoder *decoder;
  GstVideoDecoderClass *klass;
  GstHarness *h;
  GThread *thread;
  guint enqueues_before;
  gint64 deadline;

  g_print ("== jpeg blocked drain flush cancellation ==\n");
  alarm (5);
  mpp_mock_dec_arm (DEC_WIDTH, DEC_HEIGHT);
  h = gst_harness_new ("mppjpegdec");
  g_assert_nonnull (h);
  gst_harness_set_src_caps_str (h, JPEG_CAPS);
  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  enqueues_before = mpp_mock_jpeg_input_enqueues ();

  mpp_mock_jpeg_block_input_poll ();
  call.h = h;
  thread = g_thread_new ("jpeg-drain", finish_decoder_thread, &call);
  deadline = g_get_monotonic_time () + G_USEC_PER_SEC;
  while (!mpp_mock_jpeg_input_poll_entered () &&
      g_get_monotonic_time () < deadline)
    g_usleep (1000);
  g_assert_true (mpp_mock_jpeg_input_poll_entered ());

  decoder = GST_VIDEO_DECODER (h->element);
  klass = GST_VIDEO_DECODER_GET_CLASS (decoder);
  GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  g_assert_true (klass->flush (decoder));
  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

  deadline = g_get_monotonic_time () + G_USEC_PER_SEC;
  while (!g_atomic_int_get (&call.done) && g_get_monotonic_time () < deadline)
    g_usleep (1000);
  g_assert_true (g_atomic_int_get (&call.done));
  g_thread_join (thread);
  g_assert_cmpuint (mpp_mock_jpeg_input_enqueues (), ==, enqueues_before);
  alarm (0);
  g_print ("non-draining reset woke blocked JPEG input poll within one second\n");

  mpp_mock_dec_disarm ();
  gst_harness_teardown (h);
  reclaim_mock_packets ();
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
  guint burst_pending;
  guint sequential_pending = 0;
  guint i;
  gboolean constant_search_domain = TRUE;

  g_print ("== stale-PTS orphan accounting ==\n");

  for (i = 0; i < PENDING_INPUTS; i++)
    g_assert_cmpint (gst_harness_push (h, make_input_buffer (i)), ==,
        GST_FLOW_OK);

  if (!wait_for_outputs (PENDING_INPUTS))
    g_error ("decoder produced %u/%u outputs before the drain timeout",
        mpp_mock_dec_outputs (), PENDING_INPUTS);

  wait_for_pending_count (h, 0, ACCOUNTING_TIMEOUT_US);
  burst_pending = pending_frame_count (h);

  g_print ("burst: %u inputs, %u stale-PTS outputs, %u pending frames\n",
      PENDING_INPUTS, mpp_mock_dec_outputs (), burst_pending);
  stop_decoder (h);

  h = start_decoder (FALSE);
  for (i = 0; i < PENDING_INPUTS; i++) {
    g_assert_cmpint (gst_harness_push (h, make_input_buffer (i)), ==,
        GST_FLOW_OK);
    g_assert_true (wait_for_outputs (i + 1));
    if (!wait_for_pending_count (h, 0, ACCOUNTING_TIMEOUT_US)) {
      constant_search_domain = FALSE;
      sequential_pending = pending_frame_count (h);
      break;
    }
  }

  g_print ("sequential: %u/%u stale-PTS outputs completed with a one-frame "
      "matching domain; pending=%u\n", mpp_mock_dec_outputs (),
      PENDING_INPUTS, sequential_pending);

  stop_decoder (h);

  if (burst_pending != 0 || !constant_search_domain) {
    g_printerr ("ERROR: stale-PTS outputs left pending frames (burst=%u, "
        "sequential=%u); "
        "each output must consume one oldest pending orphan\n", burst_pending,
        sequential_pending);
    accounting_failures++;
  } else {
    g_print ("stale-PTS orphan accounting: OK\n");
  }
}

static void
run_parameter_set_fixture (const gchar * label, const gchar * caps,
    const guint8 * data, gsize size)
{
  GstHarness *h = start_decoder_with_caps (FALSE, caps);
  guint pending;
  guint i;

  mpp_mock_dec_set_output_suppressed (TRUE);
  for (i = 0; i < HEADER_INPUTS; i++)
    g_assert_cmpint (gst_harness_push (h, make_fixture_input_buffer (data,
                size, GST_CLOCK_TIME_NONE)), ==, GST_FLOW_OK);

  pending = pending_frame_count (h);
  g_print ("%s: %u accepted, %u outputs, %u pending\n", label,
      mpp_mock_dec_put_calls (), mpp_mock_dec_outputs (), pending);
  stop_decoder (h);

  if (pending != 0) {
    g_printerr ("ERROR: %s retained %u/%u parameter-set frames\n", label,
        pending, HEADER_INPUTS);
    accounting_failures++;
  }
}

static void
test_parameter_set_only_inputs_are_released (void)
{
  g_print ("== codec-aware parameter-set accounting ==\n");
  run_parameter_set_fixture ("H.264 Annex-B SPS/PPS", H264_BYTE_STREAM_CAPS,
      h264_parameter_sets_bytestream,
      sizeof (h264_parameter_sets_bytestream));
  run_parameter_set_fixture ("H.264 AVC SPS/PPS", H264_AVC_CAPS,
      h264_parameter_sets_avc, sizeof (h264_parameter_sets_avc));
  run_parameter_set_fixture ("H.265 Annex-B VPS/SPS/PPS",
      H265_BYTE_STREAM_CAPS, h265_parameter_sets_bytestream,
      sizeof (h265_parameter_sets_bytestream));
  run_parameter_set_fixture ("H.265 hvc1 VPS/SPS/PPS", H265_HVC1_CAPS,
      h265_parameter_sets_hvc1, sizeof (h265_parameter_sets_hvc1));
  g_print ("codec-aware parameter-set accounting: complete\n");
}

static void
test_tiny_invalid_pts_vcl_is_never_header (void)
{
  static const guint8 * const exact_parts[] = {
    h264_idr_slice_1, h264_idr_slice_1, h264_idr_frame,
  };
  static const gsize exact_sizes[] = {
    sizeof (h264_idr_slice_1), sizeof (h264_idr_slice_1),
    sizeof (h264_idr_frame),
  };
  static const guint8 * const above_parts[] = {
    h264_idr_slice_1, h264_idr_slice_2, h264_idr_frame, h264_p_slice,
  };
  static const gsize above_sizes[] = {
    sizeof (h264_idr_slice_1), sizeof (h264_idr_slice_2),
    sizeof (h264_idr_frame), sizeof (h264_p_slice),
  };
  static const guint8 * const mixed_parts[] = {
    h264_parameter_sets_bytestream, h264_idr_frame,
  };
  static const gsize mixed_sizes[] = {
    sizeof (h264_parameter_sets_bytestream), sizeof (h264_idr_frame),
  };
  static const struct {
    const gchar *element;
    const gchar *label;
    const gchar *caps;
  } unsupported[] = {
    { "mppvideodec", "H.263", H263_CAPS },
    { "mppvideodec", "AV1", AV1_CAPS },
    { "mppvideodec", "VP8", VP8_CAPS },
    { "mppvideodec", "VP9", VP9_CAPS },
    { "mppvideodec", "MPEG-4", MPEG4_CAPS },
    { "mppjpegdec", "JPEG", JPEG_CAPS },
  };
  GstHarness *h;
  GstBuffer *buffer;
  guint pending;
  guint i;

  g_print ("== tiny invalid-PTS VCL negative controls ==\n");
  h = start_decoder_with_caps (FALSE, H264_BYTE_STREAM_CAPS);
  mpp_mock_dec_set_output_suppressed (TRUE);
  g_assert_cmpint (gst_harness_push (h, make_fixture_input_buffer (
              h264_idr_frame, sizeof (h264_idr_frame), GST_CLOCK_TIME_NONE)),
      ==, GST_FLOW_OK);
  buffer = make_composite_input_buffer (GST_CLOCK_TIME_NONE, exact_parts,
      exact_sizes, G_N_ELEMENTS (exact_parts));
  g_assert_cmpuint (gst_buffer_get_size (buffer), ==, 128);
  g_assert_cmpint (gst_harness_push (h, buffer), ==, GST_FLOW_OK);
  buffer = make_composite_input_buffer (GST_CLOCK_TIME_NONE, above_parts,
      above_sizes, G_N_ELEMENTS (above_parts));
  g_assert_cmpuint (gst_buffer_get_size (buffer), >, 128);
  g_assert_cmpint (gst_harness_push (h, buffer), ==, GST_FLOW_OK);
  g_assert_cmpint (gst_harness_push (h, make_composite_input_buffer (
              GST_CLOCK_TIME_NONE, mixed_parts, mixed_sizes,
              G_N_ELEMENTS (mixed_parts))), ==, GST_FLOW_OK);
  pending = pending_frame_count (h);
  g_print ("H.264 Annex-B VCL sizes %zu/128/%zu plus mixed AU: pending=%u\n",
      sizeof (h264_idr_frame),
      sizeof (h264_idr_slice_1) + sizeof (h264_idr_slice_2) +
      sizeof (h264_idr_frame) + sizeof (h264_p_slice), pending);
  stop_decoder (h);
  if (pending != 4) {
    g_printerr ("ERROR: H.264 VCL/mixed negatives retained %u/4 frames\n",
        pending);
    accounting_failures++;
  }

  h = start_decoder_with_caps (FALSE, H264_AVC_CAPS);
  mpp_mock_dec_set_output_suppressed (TRUE);
  g_assert_cmpint (gst_harness_push (h, make_fixture_input_buffer (
              h264_idr_frame_avc, sizeof (h264_idr_frame_avc),
              GST_CLOCK_TIME_NONE)), ==, GST_FLOW_OK);
  pending = pending_frame_count (h);
  stop_decoder (h);
  if (pending != 1) {
    g_printerr ("ERROR: AVC VCL negative retained %u/1 frames\n", pending);
    accounting_failures++;
  }

  for (i = 0; i < G_N_ELEMENTS (unsupported); i++) {
    h = start_element_with_caps (unsupported[i].element, unsupported[i].caps);
    mpp_mock_dec_set_output_suppressed (TRUE);
    g_assert_cmpint (gst_harness_push (h,
            make_untimestamped_input_buffer (32)), ==, GST_FLOW_OK);
    pending = pending_frame_count (h);
    g_print ("%s tiny invalid-PTS input: pending=%u\n",
        unsupported[i].label, pending);
    stop_decoder (h);
    if (pending != 1) {
      g_printerr ("ERROR: %s tiny invalid-PTS input retained %u/1 frames\n",
          unsupported[i].label, pending);
      accounting_failures++;
    }
  }

  h = start_decoder_with_caps (FALSE, H264_UNKNOWN_FORMAT_CAPS);
  mpp_mock_dec_set_output_suppressed (TRUE);
  g_assert_cmpint (gst_harness_push (h, make_fixture_input_buffer (
              h264_parameter_sets_bytestream,
              sizeof (h264_parameter_sets_bytestream), GST_CLOCK_TIME_NONE)),
      ==, GST_FLOW_OK);
  pending = pending_frame_count (h);
  stop_decoder (h);
  if (pending != 1) {
    g_printerr ("ERROR: unknown H.264 framing retained %u/1 frames\n",
        pending);
    accounting_failures++;
  }
}

static void
test_untimestamped_vcl_survives_later_match (void)
{
  GstHarness *h = start_decoder (FALSE);
  guint before_match;
  guint after_match;

  g_print ("== untimestamped VCL survives later match ==\n");

  g_assert_cmpint (gst_harness_push (h, make_input_buffer (0)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (1));
  g_assert_true (wait_for_pending_count (h, 0, ACCOUNTING_TIMEOUT_US));

  mpp_mock_dec_set_output_suppressed (TRUE);
  g_assert_cmpint (gst_harness_push (h, make_fixture_input_buffer (
              h264_idr_frame, sizeof (h264_idr_frame), GST_CLOCK_TIME_NONE)),
      ==, GST_FLOW_OK);
  before_match = pending_frame_count (h);

  mpp_mock_dec_set_output_suppressed (FALSE);
  mpp_mock_dec_set_output_pts ((RK_S64) input_pts (1));
  g_assert_cmpint (gst_harness_push (h, make_input_buffer (1)), ==, GST_FLOW_OK);
  g_assert_true (wait_for_outputs (2));
  wait_for_pending_count (h, 0, ACCOUNTING_TIMEOUT_US);
  after_match = pending_frame_count (h);

  g_print ("untimestamped VCL pending: %u before later match, %u after\n",
      before_match, after_match);
  stop_decoder (h);

  if (before_match != 1 || after_match != 1) {
    g_printerr ("ERROR: later match retired untimestamped VCL (%u -> %u)\n",
        before_match, after_match);
    accounting_failures++;
  } else {
    g_print ("untimestamped VCL survives later match: OK\n");
  }
}

static void
test_ordered_stream_output_identity (void)
{
  GstHarness *h = start_decoder (FALSE);
  static const guint8 expected_identity[NORMAL_INPUTS] = {
    0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
  };
  GList *frames;
  gboolean valid = TRUE;
  guint i;

  g_print ("== eight-frame ordered output identity ==\n");
  mpp_mock_dec_set_output_buffers (TRUE);
  mpp_mock_dec_set_output_paused (TRUE);

  for (i = 0; i < NORMAL_INPUTS; i++) {
    mpp_mock_dec_plan_output (i, i ? (RK_S64) input_pts (i) : -1,
        expected_identity[i]);
    g_assert_cmpint (gst_harness_push (h, make_fixture_input_buffer (
                h264_p_slice, sizeof (h264_p_slice), input_pts (i))), ==,
        GST_FLOW_OK);
  }
  g_assert_cmpuint (mpp_mock_dec_queued (), ==, NORMAL_INPUTS);

  mpp_mock_dec_set_output_paused (FALSE);
  g_assert_true (wait_for_accounted_outputs (NORMAL_INPUTS));
  g_assert_true (wait_for_pending_count (h, 1, ACCOUNTING_TIMEOUT_US));

  if (mpp_mock_dec_outputs () != NORMAL_INPUTS ||
      mpp_mock_dec_accounted_outputs () != NORMAL_INPUTS ||
      mpp_mock_dec_queued () != 0) {
    g_printerr ("ERROR: ordered stream accounting outputs=%u accounted=%u "
        "queued=%u; expected %u/8/0\n", mpp_mock_dec_outputs (),
        mpp_mock_dec_accounted_outputs (), mpp_mock_dec_queued (),
        NORMAL_INPUTS);
    valid = FALSE;
  }

  for (i = 0; i < NORMAL_INPUTS - 1; i++) {
    GstBuffer *output = wait_for_output_buffer (h);
    GstClockTime pts;
    guint8 identity;

    g_assert_nonnull (output);
    pts = GST_BUFFER_PTS (output);
    identity = output_identity (output);
    if (pts != input_pts (i) || identity != expected_identity[i]) {
      g_printerr ("ERROR: ordered output %u got pts=%" G_GUINT64_FORMAT
          " identity=0x%02x; expected pts=%" G_GUINT64_FORMAT
          " identity=0x%02x\n", i, pts, identity, input_pts (i),
          expected_identity[i]);
      valid = FALSE;
    }
    gst_buffer_unref (output);
  }

  {
    GstBuffer *unexpected = gst_harness_try_pull (h);

    if (unexpected) {
      g_printerr ("ERROR: ordered stream delivered an unexpected eighth "
          "downstream buffer with identity=0x%02x\n",
          output_identity (unexpected));
      gst_buffer_unref (unexpected);
      valid = FALSE;
    }
  }

  frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (h->element));
  if (g_list_length (frames) != 1) {
    g_printerr ("ERROR: ordered retained-tail accounting has %u frames; "
        "expected 1\n", g_list_length (frames));
    valid = FALSE;
  } else {
    GstVideoCodecFrame *tail = frames->data;
    guint8 identity = tail->output_buffer ?
        output_identity (tail->output_buffer) : 0;

    if (tail->system_frame_number != NORMAL_INPUTS - 1 ||
        tail->pts != input_pts (NORMAL_INPUTS - 1) ||
        identity != expected_identity[NORMAL_INPUTS - 1]) {
      g_printerr ("ERROR: ordered retained tail frame=%d pts=%"
          G_GUINT64_FORMAT " identity=0x%02x; expected frame=7 pts=%"
          G_GUINT64_FORMAT " identity=0x58\n", tail->system_frame_number,
          tail->pts, identity, input_pts (NORMAL_INPUTS - 1));
      valid = FALSE;
    }
    g_print ("ordered retained tail: frame=%d pts=%" G_GUINT64_FORMAT
        " identity=0x%02x (deferred by one-frame output queue)\n",
        tail->system_frame_number, tail->pts, identity);
  }
  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);

  stop_decoder (h);
  if (!valid)
    accounting_failures++;
  else
    g_print ("eight-frame ordered output identity: OK\n");
}

static void
test_reordered_mixed_pts_output_identity (void)
{
  GstHarness *h = start_decoder (FALSE);
  static const GstClockTime expected_pts[] = {
    10 * GST_SECOND,
    10 * GST_SECOND + GST_SECOND / 30,
    10 * GST_SECOND + 2 * (GST_SECOND / 30),
  };
  static const guint8 expected_identity[] = { 0x10, 0x40, 0x20 };
  GList *frames;
  GstBuffer *invalid_input;
  gboolean valid = TRUE;
  guint i;

  g_print ("== reordered mixed-PTS output identity ==\n");
  mpp_mock_dec_set_output_buffers (TRUE);
  mpp_mock_dec_plan_output (0, -1, 0x10);

  g_assert_cmpint (gst_harness_push (h, make_fixture_input_buffer (
              h264_idr_frame, sizeof (h264_idr_frame), 10 * GST_SECOND)),
      ==, GST_FLOW_OK);
  g_assert_true (wait_for_accounted_outputs (1));

  mpp_mock_dec_set_output_paused (TRUE);
  mpp_mock_dec_plan_output (1,
      (RK_S64)(10 * GST_SECOND + GST_SECOND / 30), 0x40);
  mpp_mock_dec_plan_output (2, -1, 0x20);
  mpp_mock_dec_plan_output (3,
      (RK_S64)(10 * GST_SECOND + 2 * (GST_SECOND / 30)), 0x30);

  invalid_input = make_fixture_input_buffer (h264_p_slice,
      sizeof (h264_p_slice), GST_CLOCK_TIME_NONE);
  g_assert_false (GST_BUFFER_PTS_IS_VALID (invalid_input));
  g_assert_cmpint (gst_harness_push (h, invalid_input), ==, GST_FLOW_OK);
  g_assert_cmpint (gst_harness_push (h, make_fixture_input_buffer (
              h264_idr_frame, sizeof (h264_idr_frame),
              10 * GST_SECOND + 2 * (GST_SECOND / 30))), ==, GST_FLOW_OK);
  g_assert_cmpint (gst_harness_push (h, make_fixture_input_buffer (
              h264_p_slice, sizeof (h264_p_slice),
              10 * GST_SECOND + GST_SECOND / 30)), ==, GST_FLOW_OK);
  g_assert_cmpuint (mpp_mock_dec_queued (), ==, 3);

  mpp_mock_dec_set_output_paused (FALSE);
  g_assert_true (wait_for_accounted_outputs (4));

  for (i = 0; i < G_N_ELEMENTS (expected_pts); i++) {
    GstBuffer *output;
    GstClockTime pts;
    guint8 identity;

    output = wait_for_output_buffer (h);
    g_assert_nonnull (output);
    pts = GST_BUFFER_PTS (output);
    identity = output_identity (output);
    if (pts != expected_pts[i] || identity != expected_identity[i]) {
      g_printerr ("ERROR: output %u got pts=%" G_GUINT64_FORMAT
          " identity=0x%02x; expected pts=%" G_GUINT64_FORMAT
          " identity=0x%02x\n", i, pts, identity, expected_pts[i],
          expected_identity[i]);
      valid = FALSE;
    }
    gst_buffer_unref (output);
  }

  frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (h->element));
  if (g_list_length (frames) != 1) {
    g_printerr ("ERROR: retained-tail accounting has %u frames; expected 1\n",
        g_list_length (frames));
    valid = FALSE;
  } else {
    GstVideoCodecFrame *tail = frames->data;
    guint8 identity = tail->output_buffer ?
        output_identity (tail->output_buffer) : 0;

    if (tail->system_frame_number != 2 ||
        tail->pts != 10 * GST_SECOND + 2 * (GST_SECOND / 30) ||
        identity != 0x30) {
      g_printerr ("ERROR: retained tail frame=%d pts=%" G_GUINT64_FORMAT
          " identity=0x%02x; expected frame=2 identity=0x30\n",
          tail->system_frame_number, tail->pts, identity);
      valid = FALSE;
    }
    g_print ("retained tail: frame=%d pts=%" G_GUINT64_FORMAT
        " identity=0x%02x (deferred by one-frame output queue)\n",
        tail->system_frame_number, tail->pts, identity);
  }
  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);

  g_print ("mixed invalid-PTS input delivered as identity=0x20 with "
      "decoder-derived pts=%" G_GUINT64_FORMAT "\n", expected_pts[2]);

  stop_decoder (h);
  if (!valid)
    accounting_failures++;
  else
    g_print ("reordered mixed-PTS output identity: OK\n");
}

static void
run_accounting_tests (void)
{
  test_unmatched_pts_pending_list_is_bounded ();
  test_parameter_set_only_inputs_are_released ();
  test_tiny_invalid_pts_vcl_is_never_header ();
  test_untimestamped_vcl_survives_later_match ();
  test_ordered_stream_output_identity ();
  test_reordered_mixed_pts_output_identity ();
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);

  if (g_getenv ("MPP_DECODER_SEAM_ACCOUNTING_ONLY")) {
    run_accounting_tests ();
    goto done;
  }

  test_reset_releases_cached_mpp_frame ();
  test_packet_ownership_is_decided_before_send ();
  test_put_packet_result_drives_fullness ();
  test_video_drain_classifies_put_packet_errors ();
  test_jpeg_input_timeout_is_retried ();
  test_jpeg_blocked_drain_is_cancelled_by_flush ();
  test_dma_feature_reaches_negotiated_caps ();
#if GST_CHECK_VERSION(1, 24, 0)
  test_dma_drm_peer_selects_dma_drm_caps ();
  test_afbc_output_is_never_advertised_as_linear_dma_drm ();
#else
  g_print ("DMA_DRM peer caps negotiation: SKIP (requires GStreamer >= 1.24)\n");
#endif
  test_decoder_allocator_requests_dma32 ();
  run_accounting_tests ();

done:
  /* Nothing arms after the last test, so the catch-all reclaim has to be here
   * or its arena outlives the suite. */
  reclaim_mock_packets ();
  g_print ("mock packet arena at exit: %u\n", mpp_mock_dec_live_packets ());

  if (accounting_failures)
    g_error ("%u decoder accounting scenario(s) failed", accounting_failures);

  g_print ("mpp-decoder-seam: OK\n");
  return 0;
}
