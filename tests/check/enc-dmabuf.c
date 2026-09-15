#define _GNU_SOURCE
#include <gst/allocators/gstdmabuf.h>
#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/video/video.h>
#include <sys/mman.h>
#include <unistd.h>

extern void mpp_mock_reset(void);
extern void mpp_mock_rga_set_enabled(int enabled);
extern unsigned mpp_mock_buffer_import_calls(void);
extern int mpp_mock_last_import_fd(void);

static const char *codecs[] = {"mpph265enc", "mpph264enc"};

GST_START_TEST(test_compositor_links_through_queue_and_preview_tee) {
  for (guint codec = 0; codec < G_N_ELEMENTS(codecs); codec++) {
    for (gint layout = 0; layout < 6; layout++) {
      GstElement *pipeline = gst_pipeline_new(NULL);
      GstElement *comp = gst_element_factory_make("rgacompositor", NULL);
      GstElement *queue = gst_element_factory_make("queue", NULL);
      GstElement *tee = gst_element_factory_make("tee", "preview_tee");
      GstElement *enc = gst_element_factory_make(codecs[codec], "venc_bps");
      fail_unless(comp && queue && tee && enc);
      g_object_set(comp, "layout", layout, NULL);
      gst_bin_add_many(GST_BIN(pipeline), comp, queue, tee, enc, NULL);
      fail_unless(gst_element_link_many(comp, queue, tee, NULL));
      GstPad *src = gst_element_request_pad_simple(tee, "src_%u");
      GstPad *sink = gst_element_get_static_pad(enc, "sink");
      GstPadLinkReturn result = gst_pad_link(src, sink);
      fail_unless(result == GST_PAD_LINK_OK,
                  "layout %d preview_tee -> %s refused: %s", layout,
                  codecs[codec], gst_pad_link_get_name(result));
      GstCaps *caps = gst_pad_query_caps(src, NULL);
      fail_unless(!gst_caps_is_empty(caps) && !gst_caps_is_any(caps));
      for (guint i = 0; i < gst_caps_get_size(caps); i++)
        fail_unless(gst_caps_features_contains(gst_caps_get_features(caps, i),
                                               GST_CAPS_FEATURE_MEMORY_DMABUF));
      gst_caps_unref(caps);
      gst_pad_unlink(src, sink);
      gst_element_release_request_pad(tee, src);
      gst_object_unref(src);
      gst_object_unref(sink);
      gst_object_unref(pipeline);
    }
  }
}
GST_END_TEST

static void check_import(const char *codec, gboolean explicit_dmabuf,
                         guint width, guint height, const char *rate) {
  mpp_mock_reset();
  mpp_mock_rga_set_enabled(0);
  GstHarness *h = gst_harness_new(codec);
  fail_unless(h != NULL);
  g_object_set(h->element, "level", g_str_equal(codec, "mpph265enc") ? 153 : 51,
               NULL);
  gchar *caps = g_strdup_printf(
      "video/x-raw%s,format=NV12,width=%u,height=%u,framerate=%s,"
      "interlace-mode=progressive,colorimetry=bt709",
      explicit_dmabuf ? "(memory:DMABuf)" : "", width, height, rate);
  gst_harness_set_src_caps_str(h, caps);
  g_free(caps);

  GstVideoInfo info;
  GstVideoAlignment align;
  gst_video_info_set_format(&info, GST_VIDEO_FORMAT_NV12, width, height);
  gst_video_alignment_reset(&align);
  align.padding_bottom = GST_ROUND_UP_16(height) - height;
  fail_unless(gst_video_info_align(&info, &align));
  int fd = memfd_create("enc-dmabuf-test", MFD_CLOEXEC);
  fail_unless(fd >= 0);
  fail_unless_equals_int(ftruncate(fd, info.size), 0);
  GstAllocator *allocator = gst_dmabuf_allocator_new();
  GstMemory *memory = gst_dmabuf_allocator_alloc(allocator, fd, info.size);
  fail_unless(memory != NULL);
  /* A host memfd exercises import dispatch, not DMA hardware. Pixel mapping
   * must not be needed by this path, even in the software-only test. */
  GST_MINI_OBJECT_FLAG_SET(memory, GST_MEMORY_FLAG_NOT_MAPPABLE);
  GstBuffer *buffer = gst_buffer_new();
  gst_buffer_append_memory(buffer, memory);
  gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_FORMAT_NV12, width, height, 2, info.offset, info.stride);
  GST_BUFFER_PTS(buffer) = 0;
  GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
  fail_unless_equals_int(gst_harness_push(h, buffer), GST_FLOW_OK);
  fail_unless(gst_harness_push_event(h, gst_event_new_eos()));
  GstBuffer *encoded = gst_harness_try_pull(h);
  fail_unless(encoded != NULL, "%s emitted no mock encoded frame", codec);
  gst_buffer_unref(encoded);
  fail_unless_equals_int(mpp_mock_buffer_import_calls(), 1);
  fail_unless_equals_int(mpp_mock_last_import_fd(), fd);
  guint64 fallback, dropped, rejected;
  g_object_get(h->element, "conversion-fallback-frames", &fallback,
      "conversion-dropped-frames", &dropped, "layout-rejections", &rejected, NULL);
  fail_unless_equals_uint64(fallback, 0);
  fail_unless_equals_uint64(dropped, 0);
  fail_unless_equals_uint64(rejected, 0);
  GstPad *sink = gst_element_get_static_pad(h->element, "sink");
  GstCaps *negotiated = gst_pad_get_current_caps(sink);
  fail_unless(negotiated != NULL);
  fail_unless_equals_int(gst_caps_features_contains(
      gst_caps_get_features(negotiated, 0), GST_CAPS_FEATURE_MEMORY_DMABUF),
      explicit_dmabuf);
  gst_caps_unref(negotiated);
  gst_object_unref(sink);
  gst_harness_teardown(h);
  gst_object_unref(allocator);
}

GST_START_TEST(test_explicit_dmabuf_import_without_pixel_copy) {
  for (guint i = 0; i < G_N_ELEMENTS(codecs); i++) {
    check_import(codecs[i], TRUE, 1920, 1080, "30/1");
    check_import(codecs[i], TRUE, 3840, 2160, "60000/1001");
  }
}
GST_END_TEST

GST_START_TEST(test_plain_caps_keep_existing_dmabuf_import) {
  for (guint i = 0; i < G_N_ELEMENTS(codecs); i++) {
    check_import(codecs[i], FALSE, 1920, 1080, "30/1");
    check_import(codecs[i], FALSE, 3840, 2160, "60000/1001");
  }
}
GST_END_TEST

static Suite *enc_dmabuf_suite(void) {
  Suite *suite = suite_create("enc-dmabuf");
  TCase *test_case = tcase_create("composition-boundary");
  tcase_set_timeout(test_case, 30);
  tcase_add_test(test_case, test_compositor_links_through_queue_and_preview_tee);
  tcase_add_test(test_case, test_explicit_dmabuf_import_without_pixel_copy);
  tcase_add_test(test_case, test_plain_caps_keep_existing_dmabuf_import);
  suite_add_tcase(suite, test_case);
  return suite;
}

GST_CHECK_MAIN(enc_dmabuf)
