#include <fcntl.h>
#include <glib/gstdio.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/check/gstcheck.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <unistd.h>

#include <rga/im2d.h>

#include "../../gst/rockchiprga/gstrgaconvert.h"

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

rga_buffer_t
wrapbuffer_fd_t (int fd, int width, int height, int wstride, int hstride,
    int format)
{
  rga_buffer_t buffer = { 0, };

  buffer.fd = fd;
  buffer.width = width;
  buffer.height = height;
  buffer.wstride = wstride;
  buffer.hstride = hstride;
  buffer.format = format;
  return buffer;
}

IM_STATUS
imconfig (IM_CONFIG_NAME name, uint64_t value)
{
  (void) name;
  (void) value;
  return IM_STATUS_SUCCESS;
}

IM_STATUS
improcess (rga_buffer_t src, rga_buffer_t dst, rga_buffer_t pat,
    im_rect src_rect, im_rect dst_rect, im_rect pat_rect, int usage)
{
  (void) src;
  (void) dst;
  (void) pat;
  (void) src_rect;
  (void) dst_rect;
  (void) pat_rect;
  (void) usage;
  return IM_STATUS_SUCCESS;
}

typedef struct
{
  gboolean available;
  gint process_result;
  guint process_calls;
  GstMppRgaIm2dRequest last_request;
} FakeRga;

typedef struct
{
  GstRgaConvert *convert;
  GstMppRgaBackend *backend;
} TestConvert;

static gboolean
fake_probe (gpointer user_data, GstMppRgaDriverVersion * version,
    gint * error_number)
{
  FakeRga *fake = user_data;

  if (!fake->available) {
    *error_number = ENOENT;
    return FALSE;
  }
  version->major = 1;
  version->minor = 3;
  version->revision = 11;
  g_strlcpy (version->string, "1.3.11", sizeof (version->string));
  return TRUE;
}

static gint
fake_init (gpointer user_data)
{
  (void) user_data;
  return 0;
}

static gint
fake_blit (rga_info_t * src, rga_info_t * dst, gpointer user_data)
{
  (void) src;
  (void) dst;
  (void) user_data;
  return 0;
}

static gint
fake_process (const GstMppRgaIm2dRequest * request, gpointer user_data)
{
  FakeRga *fake = user_data;

  fake->process_calls++;
  fake->last_request = *request;
  return fake->process_result;
}

static const GstMppRgaBackendOps fake_ops = {
  .probe = fake_probe,
  .init = fake_init,
  .blit = fake_blit,
  .process = fake_process,
};

static TestConvert
test_convert_new (FakeRga * fake)
{
  TestConvert test;

  test.backend = gst_mpp_rga_backend_new (&fake_ops, fake);
  test.convert = g_object_new (GST_TYPE_RGA_CONVERT, NULL);
  gst_rga_convert_set_backend_for_test (test.convert, test.backend);
  return test;
}

static void
test_convert_clear (TestConvert * test)
{
  gst_object_unref (test->convert);
  gst_mpp_rga_backend_free (test->backend);
}

static GstCaps *
caps_from_string (const gchar * string)
{
  GstCaps *caps = gst_caps_from_string (string);

  fail_unless (caps != NULL);
  return caps;
}

static void
set_convert_caps (GstRgaConvert * convert, const gchar * input_string,
    const gchar * output_string)
{
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (convert);
  GstCaps *input = caps_from_string (input_string);
  GstCaps *output = caps_from_string (output_string);

  fail_unless (klass->set_caps (GST_BASE_TRANSFORM (convert), input, output));
  gst_caps_unref (input);
  gst_caps_unref (output);
}

static GstBuffer *
new_video_buffer (gboolean dmabuf, GstVideoFormat format, guint width,
    guint height, guint n_planes, const gsize * offsets, const gint * strides,
    gsize size)
{
  GstBuffer *buffer;

  if (dmabuf) {
    GstAllocator *allocator = gst_dmabuf_allocator_new ();
    GstMemory *memory;
    GError *error = NULL;
    gchar *name = NULL;
    gint fd = g_file_open_tmp ("rgaconvert-test-XXXXXX", &name, &error);

    fail_unless (fd >= 0, "%s", error ? error->message : "open failed");
    fail_unless_equals_int (ftruncate (fd, size), 0);
    g_unlink (name);
    g_clear_error (&error);
    g_free (name);
    memory = gst_dmabuf_allocator_alloc (allocator, fd, size);
    fail_unless (memory != NULL);
    buffer = gst_buffer_new ();
    gst_buffer_append_memory (buffer, memory);
    gst_object_unref (allocator);
  } else {
    buffer = gst_buffer_new_allocate (NULL, size, NULL);
  }

  fail_unless (gst_buffer_add_video_meta_full (buffer,
          GST_VIDEO_FRAME_FLAG_NONE, format, width, height, n_planes, offsets,
          strides) != NULL);
  return buffer;
}

static GstBuffer *
new_nv16_buffer (gboolean dmabuf, guint width, guint height, guint stride,
    guint hstride)
{
  gsize offsets[GST_VIDEO_MAX_PLANES] = { 0, stride * hstride, };
  gint strides[GST_VIDEO_MAX_PLANES] = { stride, stride, };

  return new_video_buffer (dmabuf, GST_VIDEO_FORMAT_NV16, width, height, 2,
      offsets, strides, stride * hstride * 2);
}

static GstBuffer *
new_nv12_buffer (gboolean dmabuf, guint width, guint height, guint stride,
    guint hstride)
{
  gsize offsets[GST_VIDEO_MAX_PLANES] = { 0, stride * hstride, };
  gint strides[GST_VIDEO_MAX_PLANES] = { stride, stride, };

  return new_video_buffer (dmabuf, GST_VIDEO_FORMAT_NV12, width, height, 2,
      offsets, strides, stride * hstride * 3 / 2);
}

static GstCaps *
fixate_output (GstRgaConvert * convert, GstCaps * input, GstCaps * filter)
{
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (convert);
  GstCaps *output = klass->transform_caps (GST_BASE_TRANSFORM (convert),
      GST_PAD_SINK, input, filter);

  fail_unless (output != NULL);
  fail_if (gst_caps_is_empty (output));
  return klass->fixate_caps (GST_BASE_TRANSFORM (convert), GST_PAD_SINK,
      input, output);
}

static GstMessage *
pop_error (GstBus * bus)
{
  GstMessage *message = gst_bus_timed_pop_filtered (bus, GST_SECOND,
      GST_MESSAGE_ERROR);

  fail_unless (message != NULL);
  return message;
}

static void
clear_cpu_copy_env (void)
{
  g_unsetenv ("GST_MPP_ALLOW_CPU_COPY");
}

GST_START_TEST (test_caps_and_property_contract)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (test.convert);
  GstPadTemplate *sink = gst_element_class_get_pad_template (klass, "sink");
  GstCaps *caps = gst_pad_template_get_caps (sink);
  gchar *caps_string = gst_caps_to_string (caps);
  const gchar *formats[] = {
    "NV12", "NV16", "NV21", "NV61", "I420", "YUY2", "UYVY", "BGR",
    "RGB", "BGRA", "RGBA", "RGB16",
  };
  const gchar *properties[] = {
    "rotation", "hflip", "vflip", "core-mask", "priority", "crop-x",
    "crop-y", "crop-w", "crop-h", "conversion-fallback-frames",
    "conversion-dropped-frames", "layout-rejections",
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (formats); i++)
    fail_unless (g_strstr_len (caps_string, -1, formats[i]) != NULL);
  fail_unless (g_strstr_len (caps_string, -1, "memory:DMABuf") != NULL);
  fail_unless (g_strstr_len (caps_string, -1, "P010") == NULL);
  fail_unless (g_strstr_len (caps_string, -1, "NV15") == NULL);
  for (i = 0; i < G_N_ELEMENTS (properties); i++)
    fail_unless (g_object_class_find_property (G_OBJECT_GET_CLASS
            (test.convert), properties[i]) != NULL);

  g_free (caps_string);
  gst_caps_unref (caps);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_rotation_and_crop_fixate_natural_output_dimensions)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstCaps *input = caps_from_string
      ("video/x-raw(memory:DMABuf),format=NV16,width=1920,height=1080,framerate=60/1,interlace-mode=progressive");
  GstCaps *output;
  GstStructure *structure;
  gint width;
  gint height;

  g_object_set (test.convert, "rotation", GST_RGA_ROTATION_90, "crop-x", 16,
      "crop-y", 8, "crop-w", 1280, "crop-h", 720, NULL);
  output = fixate_output (test.convert, input, NULL);
  structure = gst_caps_get_structure (output, 0);
  fail_unless (gst_structure_get_int (structure, "width", &width));
  fail_unless (gst_structure_get_int (structure, "height", &height));
  fail_unless_equals_int (width, 720);
  fail_unless_equals_int (height, 1280);

  gst_caps_unref (output);
  gst_caps_unref (input);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_caps_negotiation_matrix)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  const struct
  {
    const gchar *input;
    const gchar *output;
    GstRgaRotation rotation;
  } cases[] = {
    {"video/x-raw(memory:DMABuf),format=NV16,width=1920,height=1080,framerate=60/1,interlace-mode=progressive",
        "video/x-raw(memory:DMABuf),format=NV12,width=720,height=1280,framerate=60/1,interlace-mode=progressive",
        GST_RGA_ROTATION_90},
    {"video/x-raw(memory:DMABuf),format=BGR,width=3840,height=2160,framerate=60/1,interlace-mode=progressive",
        "video/x-raw(memory:DMABuf),format=RGBA,width=1920,height=1080,framerate=60/1,interlace-mode=progressive",
        GST_RGA_ROTATION_0},
    {"video/x-raw,format=I420,width=1280,height=720,framerate=30/1,interlace-mode=progressive",
        "video/x-raw(memory:DMABuf),format=RGB16,width=640,height=360,framerate=30/1,interlace-mode=progressive",
        GST_RGA_ROTATION_180},
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++) {
    GstCaps *input = caps_from_string (cases[i].input);
    GstCaps *filter = caps_from_string (cases[i].output);
    GstCaps *output;

    g_object_set (test.convert, "rotation", cases[i].rotation, NULL);
    output = fixate_output (test.convert, input, filter);
    fail_unless (gst_caps_is_subset (output, filter));
    gst_caps_unref (output);
    gst_caps_unref (filter);
    gst_caps_unref (input);
  }
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_one_process_call_honors_stride_crop_and_transform)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GstBuffer *input;
  GstBuffer *output;
  GstFlowReturn flow;
  guint64 fallback = G_MAXUINT64;
  guint64 dropped = G_MAXUINT64;
  guint64 rejected = G_MAXUINT64;

  set_convert_caps (test.convert,
      "video/x-raw(memory:DMABuf),format=NV16,width=640,height=480,framerate=60/1,interlace-mode=progressive",
      "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240,framerate=60/1,interlace-mode=progressive");
  g_object_set (test.convert, "rotation", GST_RGA_ROTATION_90, "hflip", TRUE,
      "core-mask", GST_RGA_CORE_RGA3_CORE0 | GST_RGA_CORE_RGA3_CORE1,
      "priority", 4, "crop-x", 16, "crop-y", 8, "crop-w", 600,
      "crop-h", 440, NULL);
  input = new_nv16_buffer (TRUE, 640, 480, 672, 496);
  output = new_nv12_buffer (TRUE, 320, 240, 352, 256);
  flow = klass->transform (GST_BASE_TRANSFORM (test.convert), input, output);

  fail_unless_equals_int (flow, GST_FLOW_OK);
  fail_unless_equals_int (fake.process_calls, 1);
  fail_unless_equals_int (fake.last_request.src_width, 640);
  fail_unless_equals_int (fake.last_request.src_height, 480);
  fail_unless_equals_int (fake.last_request.src_wstride, 672);
  fail_unless_equals_int (fake.last_request.src_hstride, 496);
  fail_unless_equals_int (fake.last_request.dst_wstride, 352);
  fail_unless_equals_int (fake.last_request.dst_hstride, 256);
  fail_unless_equals_int (fake.last_request.src_x, 16);
  fail_unless_equals_int (fake.last_request.src_y, 8);
  fail_unless_equals_int (fake.last_request.src_rect_width, 600);
  fail_unless_equals_int (fake.last_request.src_rect_height, 440);
  fail_unless_equals_int (fake.last_request.dst_rect_width, 320);
  fail_unless_equals_int (fake.last_request.dst_rect_height, 240);
  fail_unless ((fake.last_request.usage & IM_HAL_TRANSFORM_ROT_90) != 0);
  fail_unless ((fake.last_request.usage & IM_HAL_TRANSFORM_FLIP_H) != 0);
  fail_unless_equals_int (fake.last_request.core_mask,
      GST_RGA_CORE_RGA3_CORE0 | GST_RGA_CORE_RGA3_CORE1);
  fail_unless_equals_int (fake.last_request.priority, 4);
  fail_unless (gst_buffer_get_video_meta (output) != NULL);
  g_object_get (test.convert, "conversion-fallback-frames", &fallback,
      "conversion-dropped-frames", &dropped, "layout-rejections", &rejected,
      NULL);
  fail_unless_equals_uint64 (fallback, 0);
  fail_unless_equals_uint64 (dropped, 0);
  fail_unless_equals_uint64 (rejected, 0);

  gst_buffer_unref (output);
  gst_buffer_unref (input);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_system_memory_input_is_typed_refusal)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstBus *bus = gst_element_get_bus (pipeline);
  GstBuffer *input;
  GstBuffer *output;
  GstMessage *message;
  GError *error = NULL;
  gchar *debug = NULL;
  guint64 fallback = G_MAXUINT64;
  guint64 dropped = 0;

  gst_bin_add (GST_BIN (pipeline), GST_ELEMENT (test.convert));
  set_convert_caps (test.convert,
      "video/x-raw,format=NV16,width=640,height=480,framerate=60/1,interlace-mode=progressive",
      "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240,framerate=60/1,interlace-mode=progressive");
  input = new_nv16_buffer (FALSE, 640, 480, 640, 480);
  output = new_nv12_buffer (TRUE, 320, 240, 320, 240);
  fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
          input, output), GST_FLOW_NOT_NEGOTIATED);
  message = pop_error (bus);
  gst_message_parse_error (message, &error, &debug);
  fail_unless_equals_int (error->domain, GST_CORE_ERROR);
  fail_unless_equals_int (error->code, GST_CORE_ERROR_NEGOTIATION);
  fail_unless (g_strstr_len (error->message, -1, "system-memory input") !=
      NULL);
  g_object_get (test.convert, "conversion-fallback-frames", &fallback,
      "conversion-dropped-frames", &dropped, NULL);
  fail_unless_equals_uint64 (fallback, 0);
  fail_unless_equals_uint64 (dropped, 1);
  fail_unless_equals_int (fake.process_calls, 0);

  g_clear_error (&error);
  g_free (debug);
  gst_message_unref (message);
  gst_buffer_unref (output);
  gst_buffer_unref (input);
  gst_object_unref (bus);
  gst_object_unref (pipeline);
  gst_mpp_rga_backend_free (test.backend);
}
GST_END_TEST;

GST_START_TEST (test_invalid_stride_layout_updates_rejection_counters)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  gsize offsets[GST_VIDEO_MAX_PLANES] = { 0, 640 * 480 + 1, };
  gint strides[GST_VIDEO_MAX_PLANES] = { 640, 640, };
  GstBuffer *input;
  GstBuffer *output;
  guint64 dropped = 0;
  guint64 rejected = 0;

  set_convert_caps (test.convert,
      "video/x-raw(memory:DMABuf),format=NV16,width=640,height=480,framerate=60/1,interlace-mode=progressive",
      "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240,framerate=60/1,interlace-mode=progressive");
  input = new_video_buffer (TRUE, GST_VIDEO_FORMAT_NV16, 640, 480, 2,
      offsets, strides, 640 * 480 * 2 + 1);
  output = new_nv12_buffer (TRUE, 320, 240, 320, 240);
  fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
          input, output), GST_FLOW_NOT_NEGOTIATED);
  g_object_get (test.convert, "conversion-dropped-frames", &dropped,
      "layout-rejections", &rejected, NULL);
  fail_unless_equals_uint64 (dropped, 1);
  fail_unless_equals_uint64 (rejected, 1);
  fail_unless_equals_int (fake.process_calls, 0);

  gst_buffer_unref (output);
  gst_buffer_unref (input);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_backend_failure_updates_drop_counter)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_NOT_SUPPORTED };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GstBuffer *input;
  GstBuffer *output;
  guint64 dropped = 0;
  guint64 rejected = G_MAXUINT64;

  set_convert_caps (test.convert,
      "video/x-raw(memory:DMABuf),format=NV16,width=640,height=480,framerate=60/1,interlace-mode=progressive",
      "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240,framerate=60/1,interlace-mode=progressive");
  input = new_nv16_buffer (TRUE, 640, 480, 640, 480);
  output = new_nv12_buffer (TRUE, 320, 240, 320, 240);
  fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
          input, output), GST_FLOW_NOT_NEGOTIATED);
  g_object_get (test.convert, "conversion-dropped-frames", &dropped,
      "layout-rejections", &rejected, NULL);
  fail_unless_equals_uint64 (dropped, 1);
  fail_unless_equals_uint64 (rejected, 0);
  fail_unless_equals_int (fake.process_calls, 1);

  gst_buffer_unref (output);
  gst_buffer_unref (input);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_unavailable_backend_fails_ready_with_typed_error)
{
  FakeRga fake = {.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *message;
  GError *error = NULL;
  gchar *debug = NULL;

  gst_bin_add (GST_BIN (pipeline), GST_ELEMENT (test.convert));
  fail_unless_equals_int (gst_element_set_state (pipeline, GST_STATE_READY),
      GST_STATE_CHANGE_FAILURE);
  message = pop_error (bus);
  gst_message_parse_error (message, &error, &debug);
  fail_unless_equals_int (error->domain, GST_RESOURCE_ERROR);
  fail_unless_equals_int (error->code, GST_RESOURCE_ERROR_NOT_FOUND);
  fail_unless (g_strstr_len (error->message, -1, "trial-verified RGA backend")
      != NULL);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_clear_error (&error);
  g_free (debug);
  gst_message_unref (message);
  gst_object_unref (bus);
  gst_object_unref (pipeline);
  gst_mpp_rga_backend_free (test.backend);
}
GST_END_TEST;

GST_START_TEST (test_allocation_offers_and_accepts_dmabuf_pool)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GstAllocator *allocator = gst_dmabuf_allocator_new ();
  GstCaps *input_caps = caps_from_string
      ("video/x-raw(memory:DMABuf),format=BGR,width=1919,height=1079,framerate=60/1,interlace-mode=progressive");
  GstCaps *output_caps = caps_from_string
      ("video/x-raw(memory:DMABuf),format=NV12,width=1280,height=720,framerate=60/1,interlace-mode=progressive");
  GstQuery *propose = gst_query_new_allocation (input_caps, TRUE);
  GstQuery *decide = gst_query_new_allocation (output_caps, TRUE);
  GstBufferPool *downstream_pool = gst_video_buffer_pool_new ();
  GstStructure *config;
  GstBufferPool *pool;
  GstAllocator *pool_allocator = NULL;
  GstAllocationParams params;
  guint size;
  guint min;
  guint max;

  gst_rga_convert_set_allocator_for_test (test.convert, allocator);
  fail_unless (klass->propose_allocation (GST_BASE_TRANSFORM (test.convert),
          NULL, propose));
  fail_unless (gst_query_get_n_allocation_pools (propose) > 0);
  gst_query_parse_nth_allocation_pool (propose, 0, &pool, &size, &min, &max);
  fail_unless (pool != NULL);
  config = gst_buffer_pool_get_config (pool);
  fail_unless (gst_buffer_pool_config_has_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_META));
  fail_unless (gst_buffer_pool_config_has_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT));
  gst_buffer_pool_config_get_allocator (config, &pool_allocator, &params);
  fail_unless (pool_allocator == allocator);
  fail_unless (size > 1919 * 1079 * 3);
  gst_structure_free (config);

  config = gst_buffer_pool_get_config (downstream_pool);
  gst_buffer_pool_config_set_params (config, output_caps, 1280 * 720 * 3 / 2,
      2, 0);
  gst_buffer_pool_config_set_allocator (config, allocator, NULL);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  fail_unless (gst_buffer_pool_set_config (downstream_pool, config));
  gst_query_add_allocation_pool (decide, downstream_pool,
      1280 * 720 * 3 / 2, 2, 0);
  fail_unless (klass->decide_allocation (GST_BASE_TRANSFORM (test.convert),
          decide));
  gst_query_parse_nth_allocation_pool (decide, 0, &pool, &size, &min, &max);
  fail_unless (pool == downstream_pool);
  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_get_allocator (config, &pool_allocator, &params);
  fail_unless (pool_allocator == allocator);
  gst_structure_free (config);

  gst_object_unref (downstream_pool);
  gst_query_unref (decide);
  gst_query_unref (propose);
  gst_caps_unref (output_caps);
  gst_caps_unref (input_caps);
  gst_object_unref (allocator);
  test_convert_clear (&test);
}
GST_END_TEST;

static Suite *
rgaconvert_suite (void)
{
  Suite *suite = suite_create ("rgaconvert");
  TCase *test_case = tcase_create ("element");

  tcase_set_timeout (test_case, 15);
  tcase_add_checked_fixture (test_case, clear_cpu_copy_env,
      clear_cpu_copy_env);
  tcase_add_test (test_case, test_caps_and_property_contract);
  tcase_add_test (test_case,
      test_rotation_and_crop_fixate_natural_output_dimensions);
  tcase_add_test (test_case, test_caps_negotiation_matrix);
  tcase_add_test (test_case,
      test_one_process_call_honors_stride_crop_and_transform);
  tcase_add_test (test_case, test_system_memory_input_is_typed_refusal);
  tcase_add_test (test_case,
      test_invalid_stride_layout_updates_rejection_counters);
  tcase_add_test (test_case, test_backend_failure_updates_drop_counter);
  tcase_add_test (test_case,
      test_unavailable_backend_fails_ready_with_typed_error);
  tcase_add_test (test_case,
      test_allocation_offers_and_accepts_dmabuf_pool);
  suite_add_tcase (suite, test_case);
  return suite;
}

GST_CHECK_MAIN (rgaconvert);
