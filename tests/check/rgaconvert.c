#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <rga/im2d.h>

#include "../../gst/rockchiprga/gstrgaconvert.h"
/* Exercise the real im2d submission below the injected hardware probe. */
#undef GST_CAT_DEFAULT
#include "../../gst/rockchipmpp/gstmpprgabackend.c"
#undef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT check_debug

static rga_buffer_t submitted_src;
static rga_buffer_t submitted_dst;
static guint submitted_calls;
static gint submitted_interp;
static gint submitted_usage;
static im_rect submitted_srect;
static guint legacy_calls;

static IM_STATUS
fake_process_opt (rga_buffer_t src, rga_buffer_t dst, rga_buffer_t pat,
    im_rect srect, im_rect drect, im_rect prect, int acquire, int *release,
    im_opt_t *opt, int usage)
{
  fail_unless_equals_int (acquire, -1);
  fail_unless (release == NULL);
  fail_unless (opt != NULL);
  fail_unless_equals_int (opt->version, RGA_CURRENT_API_HEADER_VERSION);
  fail_unless (usage & IM_SYNC);
  submitted_interp = opt->interp;
  return improcess (src, dst, pat, srect, drect, prect, usage);
}

int
c_RkRgaInit (void)
{
  return 0;
}

int
c_RkRgaBlit (rga_info_t * src, rga_info_t * dst, rga_info_t * src1)
{
  legacy_calls++;
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
  submitted_src = src;
  submitted_dst = dst;
  submitted_calls++;
  submitted_usage = usage;
  submitted_srect = src_rect;
  (void) pat;
  (void) src_rect;
  (void) dst_rect;
  (void) pat_rect;
  (void) usage;
  return IM_STATUS_SUCCESS;
}

#define FAKE_RGA_MAX_FENCES 16

typedef struct
{
  gboolean available;
  gint process_result;
  guint process_calls;
  gboolean submit_im2d;
  GstMppRgaIm2dRequest last_request;
  gboolean async_supported;
  gboolean unsignalled;
  gboolean missing_fence;
  gint async_result;
  guint process_async_calls;
  guint fences_handed_out;
  gint fences[FAKE_RGA_MAX_FENCES];
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
  if (fake->submit_im2d)
    return gst_mpp_rga_real_process (request, NULL);
  return fake->process_result;
}

static gboolean
fake_async_supported (gpointer user_data)
{
  FakeRga *fake = user_data;

  return fake->async_supported;
}

/* A signalled eventfd stands in for a release fence: poll() reports POLLIN at
 * once, so the tests measure ownership and ordering rather than timing. */
static gint
fake_process_async (const GstMppRgaIm2dRequest * request,
    gint * release_fence_fd, gpointer user_data)
{
  FakeRga *fake = user_data;

  fake->process_async_calls++;
  if (fake->missing_fence) {
    *release_fence_fd = -1;
    return fake->async_result;
  }
  fake->last_request = *request;
  *release_fence_fd = eventfd (fake->unsignalled ? 0 : 1,
      EFD_CLOEXEC | EFD_NONBLOCK);
  fail_unless (*release_fence_fd >= 0);
  fail_unless (fake->fences_handed_out < FAKE_RGA_MAX_FENCES);
  fake->fences[fake->fences_handed_out++] = *release_fence_fd;
  return fake->async_result;
}

static void
fail_unless_fences_closed (const FakeRga * fake)
{
  guint i;

  for (i = 0; i < fake->fences_handed_out; i++)
    fail_unless (fcntl (fake->fences[i], F_GETFD) == -1 && errno == EBADF,
        "release fence %d (index %u) was leaked", fake->fences[i], i);
}

static const GstMppRgaBackendOps fake_ops = {
  .probe = fake_probe,
  .init = fake_init,
  .blit = fake_blit,
  .process = fake_process,
  .async_supported = fake_async_supported,
  .process_async = fake_process_async,
};

static TestConvert
test_convert_new (FakeRga * fake)
{
  TestConvert test;

  test.backend = gst_mpp_rga_backend_new (&fake_ops, fake);
  test.convert = g_object_new (GST_TYPE_RGA_CONVERT, NULL);
  rga_process_opt = fake_process_opt;
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

typedef struct { GstDmaBufAllocator parent; } TestDmaAllocator;
typedef struct { GstDmaBufAllocatorClass parent; } TestDmaAllocatorClass;
G_DEFINE_TYPE (TestDmaAllocator, test_dma_allocator, GST_TYPE_DMABUF_ALLOCATOR);

static GstMemory *
test_dma_alloc (GstAllocator * allocator, gsize size, GstAllocationParams * params)
{
  gchar *name = NULL;
  gint fd = g_file_open_tmp ("rga-pool-test-XXXXXX", &name, NULL);
  (void) params;
  fail_unless (fd >= 0);
  fail_unless_equals_int (ftruncate (fd, size), 0);
  g_unlink (name);
  g_free (name);
  return gst_dmabuf_allocator_alloc (allocator, fd, size);
}

static void
test_dma_allocator_class_init (TestDmaAllocatorClass * klass)
{
  GST_ALLOCATOR_CLASS (klass)->alloc = test_dma_alloc;
}

static void
test_dma_allocator_init (TestDmaAllocator * allocator)
{
  (void) allocator;
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

GST_START_TEST (test_fixation_preserves_omitted_yuv_colorimetry)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GstCaps *input = caps_from_string
      ("video/x-raw(memory:DMABuf),format=NV16,width=3840,height=2160,colorimetry=2:4:7:1");
  GstCaps *output = caps_from_string
      ("video/x-raw(memory:DMABuf),format=NV12,width=3840,height=2160");
  const gchar *color;

  output = klass->fixate_caps (GST_BASE_TRANSFORM (test.convert), GST_PAD_SINK,
      input, output);
  color = gst_structure_get_string (gst_caps_get_structure (output, 0),
      "colorimetry");
  fail_unless (color != NULL, "NV16 to NV12 fixation dropped input colorimetry");
  fail_unless_equals_string (color, "2:4:7:1");
  gst_caps_unref (output);
  gst_caps_unref (input);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_transform_caps_prefers_identity_colorimetry_both_directions)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GstCaps *input = caps_from_string
      ("video/x-raw(memory:DMABuf),format=NV16,width=3840,height=2160,colorimetry=2:4:7:1");
  const GstPadDirection directions[] = { GST_PAD_SINK, GST_PAD_SRC };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (directions); i++) {
    GstCaps *output = klass->transform_caps (GST_BASE_TRANSFORM (test.convert),
        directions[i], input, NULL);
    const gchar *color = gst_structure_get_string
        (gst_caps_get_structure (output, 0), "colorimetry");

    fail_unless (color != NULL, "transform_caps lost identity colorimetry");
    fail_unless_equals_string (color, "2:4:7:1");
    gst_caps_unref (output);
  }
  gst_caps_unref (input);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_fixation_does_not_relabel_explicit_bt709_or_rgb)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
  TestConvert test = test_convert_new (&fake);
  GstCaps *input = caps_from_string
      ("video/x-raw(memory:DMABuf),format=NV16,width=3840,height=2160,colorimetry=2:4:7:1");
  GstCaps *filter = caps_from_string
      ("video/x-raw(memory:DMABuf),format=NV12,width=3840,height=2160,colorimetry=bt709");
  GstCaps *output = fixate_output (test.convert, input, filter);

  fail_unless_equals_string (gst_structure_get_string
      (gst_caps_get_structure (output, 0), "colorimetry"), "bt709");
  gst_caps_unref (output);
  gst_caps_unref (filter);
  filter = caps_from_string
      ("video/x-raw(memory:DMABuf),format=BGR,width=3840,height=2160");
  output = fixate_output (test.convert, input, filter);
  fail_if (gst_structure_has_field (gst_caps_get_structure (output, 0),
      "colorimetry"), "YUV matrix must not be copied into RGB caps");
  gst_caps_unref (output);
  gst_caps_unref (filter);
  gst_caps_unref (input);
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
  gst_object_unref (pool);

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

  gst_object_unref (pool);
  gst_object_unref (downstream_pool);
  gst_query_unref (decide);
  gst_query_unref (propose);
  gst_caps_unref (output_caps);
  gst_caps_unref (input_caps);
  gst_object_unref (allocator);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_allocation_query_pool_references_are_released)
{
  guint variant;

  for (variant = 0; variant < 4; variant++) {
    FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS };
    TestConvert test = test_convert_new (&fake);
    GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
    GstAllocator *allocator = gst_dmabuf_allocator_new ();
    GstCaps *caps = caps_from_string
        ("video/x-raw(memory:DMABuf),format=NV12,width=1280,height=720,framerate=30/1,interlace-mode=progressive");
    GstQuery *query = gst_query_new_allocation (caps, TRUE);
    GWeakRef weak[2];
    guint count;
    guint i;

    gst_rga_convert_set_allocator_for_test (test.convert, allocator);
    if (variant == 1 || variant == 3) {
      GstBufferPool *first = variant == 1 ? gst_video_buffer_pool_new () : NULL;

      gst_query_add_allocation_pool (query, first, 1280 * 720 * 3 / 2, 2, 0);
      gst_clear_object (&first);
    }
    if (variant != 2) {
      GstBufferPool *pool = gst_video_buffer_pool_new ();
      GstStructure *config = gst_buffer_pool_get_config (pool);

      gst_buffer_pool_config_set_params (config, caps, 1280 * 720 * 3 / 2, 2, 0);
      gst_buffer_pool_config_set_allocator (config, allocator, NULL);
      fail_unless (gst_buffer_pool_set_config (pool, config));
      gst_query_add_allocation_pool (query, pool, 1280 * 720 * 3 / 2, 2, 0);
      gst_object_unref (pool);
    }

    fail_unless (klass->decide_allocation (GST_BASE_TRANSFORM (test.convert),
            query));
    count = gst_query_get_n_allocation_pools (query);
    fail_unless (count > 0 && count <= G_N_ELEMENTS (weak));
    for (i = 0; i < count; i++) {
      GstBufferPool *pool;
      gpointer alive;

      gst_query_parse_nth_allocation_pool (query, i, &pool, NULL, NULL, NULL);
      g_weak_ref_init (&weak[i], pool);
      alive = g_weak_ref_get (&weak[i]);
      fail_unless (i != 0 || alive != NULL);
      gst_clear_object (&alive);
      gst_clear_object (&pool);
    }
    gst_query_unref (query);
    gst_caps_unref (caps);
    gst_object_unref (allocator);
    test_convert_clear (&test);
    for (i = 0; i < count; i++) {
      gpointer retained = g_weak_ref_get (&weak[i]);

      fail_unless (retained == NULL,
          "variant %u pool %u retained after query and converter teardown",
          variant, i);
      g_weak_ref_clear (&weak[i]);
    }
  }
}
GST_END_TEST;

GST_START_TEST (test_colorimetry_reaches_improcess)
{
  const struct
  {
    const gchar *color;
    gint y2r;
    gint r2y;
  } cases[] = {
    {"bt601", IM_YUV_TO_RGB_BT601_LIMIT, IM_RGB_TO_YUV_BT601_LIMIT},
    {"bt709", IM_YUV_TO_RGB_BT709_LIMIT, IM_RGB_TO_YUV_BT709_LIMIT},
    {"1:4:16:4", IM_YUV_TO_RGB_BT601_FULL, IM_RGB_TO_YUV_BT601_FULL},
    {"1:3:5:1", 0, 0},
  };
  FakeRga fake = {.available = TRUE, .submit_im2d = TRUE};
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  guint i;
  guint direction;

  for (i = 0; i < G_N_ELEMENTS (cases); i++) {
    for (direction = 0; direction < 2; direction++) {
      gchar *yuv_caps = g_strdup_printf (
          "video/x-raw(memory:DMABuf),format=NV12,width=640,height=480,colorimetry=%s",
          cases[i].color);
      const gchar *rgb_caps =
          "video/x-raw(memory:DMABuf),format=BGR,width=640,height=480,colorimetry=sRGB";
      gsize offsets[GST_VIDEO_MAX_PLANES] = { 0, };
      gint strides[GST_VIDEO_MAX_PLANES] = { 640 * 3, };
      GstBuffer *yuv = new_nv12_buffer (TRUE, 640, 480, 672, 496);
      GstBuffer *rgb = new_video_buffer (TRUE, GST_VIDEO_FORMAT_BGR,
          640, 480, 1, offsets, strides, 640 * 480 * 3);

      set_convert_caps (test.convert, direction ? rgb_caps : yuv_caps,
          direction ? yuv_caps : rgb_caps);
      submitted_calls = 0;
      fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
              direction ? rgb : yuv, direction ? yuv : rgb), GST_FLOW_OK);
      fail_unless_equals_int (submitted_calls, 1);
      fail_unless_equals_int (submitted_src.color_space_mode, 0);
      fail_unless_equals_int (submitted_dst.color_space_mode,
          direction ? cases[i].r2y : cases[i].y2r);
      gst_buffer_unref (rgb);
      gst_buffer_unref (yuv);
      g_free (yuv_caps);
    }
  }
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_unspecified_colorimetry_uses_video_info_defaults)
{
  const guint heights[] = { 480, 576, 578, 1080, 2160 };
  FakeRga fake = {.available = TRUE, .submit_im2d = TRUE};
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  guint i;
  guint direction;

  for (i = 0; i < G_N_ELEMENTS (heights); i++) {
    guint height = heights[i];
    gchar *yuv_caps = g_strdup_printf (
        "video/x-raw(memory:DMABuf),format=NV12,width=640,height=%u", height);
    const gchar *rgb_caps =
        "video/x-raw(memory:DMABuf),format=BGR,width=320,height=240";
    gsize offsets[GST_VIDEO_MAX_PLANES] = { 0, };
    gint strides[GST_VIDEO_MAX_PLANES] = { 320 * 3, };
    GstBuffer *yuv = new_nv12_buffer (TRUE, 640, height, 640, height);
    GstBuffer *rgb = new_video_buffer (TRUE, GST_VIDEO_FORMAT_BGR,
        320, 240, 1, offsets, strides, 320 * 240 * 3);

    for (direction = 0; direction < 2; direction++) {
      set_convert_caps (test.convert, direction ? rgb_caps : yuv_caps,
          direction ? yuv_caps : rgb_caps);
      fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
              direction ? rgb : yuv, direction ? yuv : rgb), GST_FLOW_OK);
      fail_unless_equals_int (submitted_src.color_space_mode, 0);
      fail_unless_equals_int (submitted_dst.color_space_mode,
          direction ? (height > 576 ? IM_RGB_TO_YUV_BT709_LIMIT :
              IM_RGB_TO_YUV_BT601_LIMIT) :
          (height > 576 ? IM_YUV_TO_RGB_BT709_LIMIT : IM_YUV_TO_RGB_BT601_LIMIT));
    }
    gst_buffer_unref (rgb);
    gst_buffer_unref (yuv);
    g_free (yuv_caps);
  }
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_stride_only_does_not_request_csc)
{
  FakeRga fake = {.available = TRUE, .submit_im2d = TRUE};
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  const gchar *caps =
      "video/x-raw(memory:DMABuf),format=NV12,width=640,height=480,colorimetry=bt709";
  GstBuffer *input = new_nv12_buffer (TRUE, 640, 480, 672, 496);
  GstBuffer *output = new_nv12_buffer (TRUE, 640, 480, 640, 480);

  set_convert_caps (test.convert, caps, caps);
  submitted_calls = 0;
  fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
          input, output), GST_FLOW_OK);
  fail_unless_equals_int (submitted_calls, 1);
  fail_unless_equals_int (submitted_src.color_space_mode, 0);
  fail_unless_equals_int (submitted_dst.color_space_mode, 0);
  gst_buffer_unref (output);
  gst_buffer_unref (input);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_color_space_changes_and_unsupported_modes)
{
  const struct
  {
    GstVideoFormat input_format;
    GstVideoFormat output_format;
    const gchar *input_color;
    const gchar *output_color;
    gboolean supported;
    gint src_mode;
    gint dst_mode;
  } cases[] = {
    {GST_VIDEO_FORMAT_NV12, GST_VIDEO_FORMAT_NV16, "bt709", "bt709",
        TRUE, 0, 0},
    {GST_VIDEO_FORMAT_BGR, GST_VIDEO_FORMAT_RGBA, "sRGB", "sRGB",
        TRUE, 0, 0},
    {GST_VIDEO_FORMAT_NV12, GST_VIDEO_FORMAT_NV12, "bt601", "bt709",
        FALSE, 0, 0},
    {GST_VIDEO_FORMAT_NV12, GST_VIDEO_FORMAT_NV12, "bt709", "1:3:5:1",
        FALSE, 0, 0},
    {GST_VIDEO_FORMAT_NV12, GST_VIDEO_FORMAT_BGR, "bt2020", "sRGB",
        FALSE, 0, 0},
    {GST_VIDEO_FORMAT_BGR, GST_VIDEO_FORMAT_NV12, "sRGB", "bt2020",
        FALSE, 0, 0},
    {GST_VIDEO_FORMAT_BGR, GST_VIDEO_FORMAT_NV12, "2:1:7:1", "bt709",
        FALSE, 0, 0},
  };
  FakeRga fake = {.available = TRUE, .submit_im2d = TRUE};
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++) {
    GstVideoInfo infos[2];
    GstBuffer *buffers[2];
    guint side;
    gchar *caps[2];
    guint64 before;
    guint64 after;

    for (side = 0; side < 2; side++) {
      GstVideoFormat format = side ? cases[i].output_format :
          cases[i].input_format;

      gst_video_info_set_format (&infos[side], format, 640, 480);
      caps[side] = g_strdup_printf (
          "video/x-raw(memory:DMABuf),format=%s,width=640,height=480,colorimetry=%s",
          gst_video_format_to_string (format),
          side ? cases[i].output_color : cases[i].input_color);
      buffers[side] = new_video_buffer (TRUE, format, 640, 480,
          GST_VIDEO_INFO_N_PLANES (&infos[side]), infos[side].offset,
          infos[side].stride, infos[side].size);
    }
    set_convert_caps (test.convert, caps[0], caps[1]);
    g_object_get (test.convert, "csc-fallback-frames", &before, NULL);
    submitted_calls = 0;
    fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
            buffers[0], buffers[1]), GST_FLOW_OK);
    fail_unless_equals_int (submitted_calls, 1);
    fail_unless_equals_int (submitted_src.color_space_mode, cases[i].src_mode);
    fail_unless_equals_int (submitted_dst.color_space_mode, cases[i].dst_mode);
    g_object_get (test.convert, "csc-fallback-frames", &after, NULL);
    fail_unless_equals_uint64 (after - before, cases[i].supported ? 0 : 1);
    for (side = 0; side < 2; side++) {
      gst_buffer_unref (buffers[side]);
      g_free (caps[side]);
    }
  }
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_interpolation_and_seven_argument_fallback)
{
  FakeRga fake = {.available = TRUE, .submit_im2d = TRUE};
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  const gchar *caps = "video/x-raw(memory:DMABuf),format=NV12,width=640,height=480,colorimetry=bt709";
  GstBuffer *input = new_nv12_buffer (TRUE, 640, 480, 672, 496);
  GstBuffer *output = new_nv12_buffer (TRUE, 640, 480, 640, 480);
  gint mode;

  set_convert_caps (test.convert, caps, caps);
  g_object_get (test.convert, "interpolation", &mode, NULL);
  fail_unless_equals_int (mode, IM_INTERP_DEFAULT);
  for (mode = IM_INTERP_DEFAULT; mode <= IM_INTERP_CUBIC; mode++) {
    g_object_set (test.convert, "interpolation", mode, NULL);
    submitted_interp = -1;
    fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
            input, output), GST_FLOW_OK);
    fail_unless_equals_int (submitted_interp, mode);
  }
  rga_process_opt = NULL;
  submitted_interp = -1;
  submitted_calls = 0;
  fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
          input, output), GST_FLOW_OK);
  fail_unless_equals_int (submitted_calls, 1);
  fail_unless_equals_int (submitted_interp, -1);
  gst_buffer_unref (input);
  gst_buffer_unref (output);
  test_convert_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_mpp_blit_uses_im2d_and_explicit_rollback)
{
  const gint rotations[] = { 0, HAL_TRANSFORM_ROT_90,
    HAL_TRANSFORM_ROT_180, HAL_TRANSFORM_ROT_270 };
  const gint usages[] = { 0, IM_HAL_TRANSFORM_ROT_90,
    IM_HAL_TRANSFORM_ROT_180, IM_HAL_TRANSFORM_ROT_270 };
  rga_info_t src = { 0, }, dst = { 0, };
  guint8 virtual_input;
  guint i;

  gst_mpp_rga_backend_get_default ();
  rga_process_opt = fake_process_opt;
  src.fd = 11;
  dst.fd = 12;
  rga_set_rect (&src.rect, 16, 32, 640, 480, 672, 528, RK_FORMAT_YCbCr_420_SP);
  rga_set_rect (&dst.rect, 0, 0, 320, 240, 320, 240, RK_FORMAT_BGR_888);
  dst.color_space_mode = IM_YUV_TO_RGB_BT709_LIMIT;
  g_unsetenv ("GST_MPP_RGA_LEGACY_BLIT");
  legacy_calls = submitted_calls = 0;
  for (i = 0; i < G_N_ELEMENTS (rotations); i++) {
    src.rotation = rotations[i];
    fail_unless_equals_int (gst_mpp_rga_real_blit (&src, &dst, NULL), 0);
    fail_unless_equals_int (submitted_usage, usages[i] | IM_SYNC);
    fail_unless_equals_int (submitted_srect.x, 16);
    fail_unless_equals_int (submitted_srect.y, 32);
    fail_unless_equals_int (submitted_src.wstride, 672);
    fail_unless_equals_int (submitted_src.hstride, 528);
    fail_unless_equals_int (submitted_src.width, 656);
    fail_unless_equals_int (submitted_src.height, 512);
    fail_unless_equals_int (submitted_dst.color_space_mode, IM_YUV_TO_RGB_BT709_LIMIT);
    src.virAddr = &virtual_input;
    fail_unless_equals_int (gst_mpp_rga_real_blit (&src, &dst, NULL), 0);
    fail_unless_equals_int (submitted_usage, usages[i] | IM_SYNC);
    fail_unless_equals_int (submitted_srect.x, 16);
    fail_unless_equals_int (submitted_srect.y, 32);
    fail_unless_equals_int (submitted_src.width, 656);
    fail_unless_equals_int (submitted_src.height, 512);
    fail_unless_equals_int (submitted_src.wstride, 672);
    fail_unless_equals_int (submitted_src.hstride, 528);
    src.virAddr = NULL;
  }
  fail_unless_equals_int (submitted_calls, 8);
  fail_unless_equals_int (legacy_calls, 0);
  g_setenv ("GST_MPP_RGA_LEGACY_BLIT", "1", TRUE);
  fail_unless_equals_int (gst_mpp_rga_real_blit (&src, &dst, NULL), 0);
  fail_unless_equals_int (legacy_calls, 1);
  fail_unless_equals_int (submitted_calls, 8);
  g_unsetenv ("GST_MPP_RGA_LEGACY_BLIT");
}
GST_END_TEST;

GST_START_TEST (test_async_depth_property_contract_and_default_is_synchronous)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_SUCCESS
  };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS
      (test.convert), "async-depth");
  GstBuffer *input;
  GstBuffer *output;
  guint depth = G_MAXUINT;

  fail_unless (pspec != NULL);
  fail_unless (G_IS_PARAM_SPEC_UINT (pspec));
  fail_unless_equals_int (((GParamSpecUInt *) pspec)->minimum, 0);
  fail_unless_equals_int (((GParamSpecUInt *) pspec)->maximum, 1);
  fail_unless_equals_int (((GParamSpecUInt *) pspec)->default_value, 0);

  g_object_get (test.convert, "async-depth", &depth, NULL);
  fail_unless_equals_int (depth, 0);

  set_convert_caps (test.convert,
      "video/x-raw(memory:DMABuf),format=NV16,width=640,height=480,framerate=60/1,interlace-mode=progressive",
      "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240,framerate=60/1,interlace-mode=progressive");
  input = new_nv16_buffer (TRUE, 640, 480, 640, 480);
  output = new_nv12_buffer (TRUE, 320, 240, 320, 240);
  fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
          input, output), GST_FLOW_OK);
  fail_unless_equals_int (fake.process_calls, 1);
  fail_unless_equals_int (fake.process_async_calls, 0);

  gst_buffer_unref (output);
  gst_buffer_unref (input);
  test_convert_clear (&test);
}

GST_END_TEST;

GST_START_TEST (test_async_depth_one_submits_async_only_when_the_runtime_can)
{
  const gboolean capable[] = { TRUE, FALSE };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (capable); i++) {
    FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
      .async_supported = capable[i],.async_result = IM_STATUS_SUCCESS
    };
    TestConvert test = test_convert_new (&fake);
    GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
    GstBuffer *input;
    GstBuffer *output;

    g_object_set (test.convert, "async-depth", 1, NULL);
    set_convert_caps (test.convert,
        "video/x-raw(memory:DMABuf),format=NV16,width=640,height=480,framerate=60/1,interlace-mode=progressive",
        "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240,framerate=60/1,interlace-mode=progressive");
    input = new_nv16_buffer (TRUE, 640, 480, 640, 480);
    output = new_nv12_buffer (TRUE, 320, 240, 320, 240);
    fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
            input, output), GST_FLOW_OK);

    if (capable[i]) {
      fail_unless_equals_int (fake.process_async_calls, 1);
      fail_unless_equals_int (fake.process_calls, 0);
      fail_unless_equals_int (fake.fences_handed_out, 1);
    } else {
      fail_unless_equals_int (fake.process_async_calls, 0);
      fail_unless_equals_int (fake.process_calls, 1);
      fail_unless_equals_int (fake.fences_handed_out, 0);
    }

    gst_buffer_unref (output);
    gst_buffer_unref (input);
    test_convert_clear (&test);
    fail_unless_fences_closed (&fake);
  }
}

GST_END_TEST;

GST_START_TEST (test_async_submit_is_never_reached_without_a_dmabuf_pair)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_SUCCESS
  };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GstBuffer *input;
  GstBuffer *output;

  g_object_set (test.convert, "async-depth", 1, NULL);
  set_convert_caps (test.convert,
      "video/x-raw,format=NV16,width=640,height=480,framerate=60/1,interlace-mode=progressive",
      "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240,framerate=60/1,interlace-mode=progressive");
  input = new_nv16_buffer (FALSE, 640, 480, 640, 480);
  output = new_nv12_buffer (TRUE, 320, 240, 320, 240);

  fail_unless_equals_int (klass->transform (GST_BASE_TRANSFORM (test.convert),
          input, output), GST_FLOW_NOT_NEGOTIATED);
  fail_unless_equals_int (fake.process_async_calls, 0);
  fail_unless_equals_int (fake.process_calls, 0);
  fail_unless_equals_int (fake.fences_handed_out, 0);

  gst_buffer_unref (output);
  gst_buffer_unref (input);
  test_convert_clear (&test);
}

GST_END_TEST;

static GstBuffer *
release_pipelined (GstRgaConvert * convert, FakeRga * fake, GstBuffer * frame)
{
  GstBuffer *out = NULL;
  gint fence = eventfd (1, EFD_CLOEXEC | EFD_NONBLOCK);

  fail_unless (fence >= 0);
  fail_unless (fake->fences_handed_out < FAKE_RGA_MAX_FENCES);
  fake->fences[fake->fences_handed_out++] = fence;
  fail_unless_equals_int (gst_rga_convert_release_pipelined (convert, frame,
          fence, &out), GST_FLOW_OK);
  return out;
}

GST_START_TEST (test_unsignalled_fence_timeout_keeps_descriptor_owned)
{
  gint fds[2];

  fail_unless_equals_int (pipe (fds), 0);
  fail_if (gst_mpp_rga_fence_wait (fds[0], 0));
  fail_unless (fcntl (fds[0], F_GETFD) >= 0,
      "timeout must not close an outstanding hardware fence");
  close (fds[0]);
  close (fds[1]);
}

GST_END_TEST;

GST_START_TEST (test_timeout_never_returns_unfinished_output)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_SUCCESS
  };
  TestConvert test = test_convert_new (&fake);
  GstBuffer *frame_one = gst_buffer_new ();
  GstBuffer *frame_two = gst_buffer_new ();
  GstBuffer *out = NULL;
  gint fds[2];
  gint signalled = eventfd (1, EFD_CLOEXEC | EFD_NONBLOCK);
  guint64 dropped = 0;

  fail_unless_equals_int (pipe (fds), 0);
  fail_unless_equals_int (gst_rga_convert_release_pipelined (test.convert,
          gst_buffer_ref (frame_one), fds[0], &out), GST_FLOW_OK);
  fail_unless (out == NULL);
  fail_unless_equals_int (gst_rga_convert_release_pipelined (test.convert,
          gst_buffer_ref (frame_two), signalled, &out), GST_FLOW_OK);
  fail_unless (out == NULL, "an unsignalled output must never reach downstream");
  fail_unless (fcntl (fds[0], F_GETFD) >= 0);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (frame_one), 2);
  g_object_get (test.convert, "conversion-dropped-frames", &dropped, NULL);
  fail_unless_equals_uint64 (dropped, 1);
  fail_unless_equals_int (write (fds[1], "x", 1), 1);
  test_convert_clear (&test);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (frame_one), 1);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (frame_two), 1);
  fail_unless (fcntl (fds[0], F_GETFD) == -1 && errno == EBADF);
  close (fds[1]);
  gst_buffer_unref (frame_one);
  gst_buffer_unref (frame_two);
}

GST_END_TEST;

GST_START_TEST (test_depth_one_holds_a_frame_and_eos_drains_it_in_order)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_SUCCESS
  };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GstPad *srcpad = GST_BASE_TRANSFORM_SRC_PAD (test.convert);
  GstPad *sinkpad = gst_pad_new ("sink", GST_PAD_SINK);
  GstCaps *caps = caps_from_string
      ("video/x-raw(memory:DMABuf),format=NV12,width=320,height=240");
  GstSegment segment;
  GstBuffer *frames[3];
  GstBuffer *out;
  guint i;

  gst_pad_set_chain_function (sinkpad, gst_check_chain_func);
  gst_pad_set_active (sinkpad, TRUE);
  gst_pad_set_active (srcpad, TRUE);
  fail_unless_equals_int (gst_pad_link (srcpad, sinkpad), GST_PAD_LINK_OK);
  gst_segment_init (&segment, GST_FORMAT_TIME);
  fail_unless (gst_pad_push_event (srcpad,
          gst_event_new_stream_start ("rgaconvert-async")));
  fail_unless (gst_pad_push_event (srcpad, gst_event_new_caps (caps)));
  fail_unless (gst_pad_push_event (srcpad, gst_event_new_segment (&segment)));

  g_object_set (test.convert, "async-depth", 1, NULL);
  for (i = 0; i < G_N_ELEMENTS (frames); i++) {
    frames[i] = new_nv12_buffer (TRUE, 320, 240, 320, 240);
    GST_BUFFER_PTS (frames[i]) = i * GST_SECOND;
  }

  out = release_pipelined (test.convert, &fake, gst_buffer_ref (frames[0]));
  fail_unless (out == NULL, "the first frame must be held, not pushed");

  out = release_pipelined (test.convert, &fake, gst_buffer_ref (frames[1]));
  fail_unless (out == frames[0], "frame 0 must be released when 1 is submitted");
  gst_buffer_unref (out);

  out = release_pipelined (test.convert, &fake, gst_buffer_ref (frames[2]));
  fail_unless (out == frames[1]);
  gst_buffer_unref (out);

  fail_unless_equals_int (g_list_length (buffers), 0);
  fail_unless (klass->sink_event (GST_BASE_TRANSFORM (test.convert),
          gst_event_new_eos ()));
  fail_unless_equals_int (g_list_length (buffers), 1);
  fail_unless (buffers->data == frames[2], "EOS must drain the held frame");

  fail_unless_fences_closed (&fake);

  gst_check_drop_buffers ();
  for (i = 0; i < G_N_ELEMENTS (frames); i++)
    gst_buffer_unref (frames[i]);
  gst_pad_set_active (sinkpad, FALSE);
  gst_pad_set_active (srcpad, FALSE);
  gst_pad_unlink (srcpad, sinkpad);
  gst_object_unref (sinkpad);
  gst_caps_unref (caps);
  test_convert_clear (&test);
}

GST_END_TEST;

static GstPadProbeReturn
observe_flush (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  (void) pad;
  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) == GST_EVENT_FLUSH_START)
    g_atomic_int_inc ((gint *) data);
  return GST_PAD_PROBE_OK;
}

GST_START_TEST (test_flush_start_quarantines_input_and_output_without_waiting)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_SUCCESS,.unsignalled = TRUE
  };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransform *base = GST_BASE_TRANSFORM (test.convert);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (base);
  GstPad *sink = gst_pad_new ("sink", GST_PAD_SINK);
  GstBuffer *input = new_nv16_buffer (TRUE, 640, 480, 640, 480);
  GstBuffer *output = new_nv12_buffer (TRUE, 320, 240, 320, 240);
  gint observed = 0;
  gint64 started;
  guint64 signal = 1;

  gst_pad_set_active (sink, TRUE);
  gst_pad_set_active (base->srcpad, TRUE);
  fail_unless_equals_int (gst_pad_link (base->srcpad, sink), GST_PAD_LINK_OK);
  gst_pad_add_probe (sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
      GST_PAD_PROBE_TYPE_EVENT_FLUSH, observe_flush, &observed, NULL);
  g_object_set (test.convert, "async-depth", 1, NULL);
  set_convert_caps (test.convert,
      "video/x-raw(memory:DMABuf),format=NV16,width=640,height=480",
      "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240");
  fail_unless_equals_int (klass->transform (base, input, output), GST_FLOW_OK);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (input), 2);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (output), 2);
  started = g_get_monotonic_time ();
  fail_unless (klass->sink_event (base, gst_event_new_flush_start ()));
  fail_unless (g_get_monotonic_time () - started < 100 * 1000);
  fail_unless_equals_int (g_atomic_int_get (&observed), 1);
  fail_unless (fcntl (fake.fences[0], F_GETFD) >= 0);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (input), 2);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (output), 2);
  fail_unless (klass->sink_event (base, gst_event_new_flush_stop (TRUE)));
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (input), 2);
  fail_unless_equals_int (write (fake.fences[0], &signal, sizeof signal), sizeof signal);
  fail_unless (klass->sink_event (base, gst_event_new_flush_stop (TRUE)));
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (input), 1);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (output), 1);
  fail_unless_fences_closed (&fake);
  gst_pad_unlink (base->srcpad, sink);
  gst_object_unref (sink);
  gst_buffer_unref (input);
  gst_buffer_unref (output);
  test_convert_clear (&test);
}

GST_END_TEST;

typedef struct
{
  GstRgaConvert *convert;
  gint started;
  gint finished;
} StopWait;

static gpointer
stop_wait_thread (gpointer data)
{
  StopWait *wait = data;
  GstBaseTransform *base = GST_BASE_TRANSFORM (wait->convert);

  g_atomic_int_set (&wait->started, 1);
  GST_BASE_TRANSFORM_GET_CLASS (base)->stop (base);
  g_atomic_int_set (&wait->finished, 1);
  return NULL;
}

GST_START_TEST (test_stop_retains_never_signalled_frame_past_error_deadline)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_SUCCESS
  };
  TestConvert test = test_convert_new (&fake);
  GstBuffer *frame = gst_buffer_new ();
  GstBuffer *out = NULL;
  GstBus *bus = gst_bus_new ();
  GstMessage *message;
  gint fds[2];
  StopWait wait = { .convert = test.convert };
  GThread *thread;

  gst_element_set_bus (GST_ELEMENT (test.convert), bus);
  fail_unless_equals_int (pipe (fds), 0);
  fail_unless_equals_int (gst_rga_convert_release_pipelined (test.convert,
          gst_buffer_ref (frame), fds[0], &out), GST_FLOW_OK);
  thread = g_thread_new ("stop-wait", stop_wait_thread, &wait);
  while (!g_atomic_int_get (&wait.started))
    g_usleep (1000);
  g_usleep (2200000);
  fail_unless_equals_int (g_atomic_int_get (&wait.finished), 0);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (frame), 2);
  fail_unless (fcntl (fds[0], F_GETFD) >= 0);
  message = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
  fail_unless (message != NULL);
  gst_message_unref (message);
  fail_unless (gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR) == NULL);
  fail_unless_equals_int (write (fds[1], "x", 1), 1);
  g_thread_join (thread);
  fail_unless_equals_int (g_atomic_int_get (&wait.finished), 1);
  fail_unless_equals_int (GST_MINI_OBJECT_REFCOUNT_VALUE (frame), 1);
  fail_unless (fcntl (fds[0], F_GETFD) == -1 && errno == EBADF);
  close (fds[1]);
  gst_buffer_unref (frame);
  gst_object_unref (bus);
  test_convert_clear (&test);
}

GST_END_TEST;

GST_START_TEST (test_real_transform_chain_orders_buffers_events_and_caps)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_SUCCESS
  };
  TestConvert test = test_convert_new (&fake);
  GstAllocator *allocator = g_object_new (test_dma_allocator_get_type (), NULL);
  GstHarness *h;
  GstBuffer *input, *out;
  guint i;

  gst_object_ref_sink (test.convert);
  gst_object_ref_sink (allocator);
  gst_rga_convert_set_allocator_for_test (test.convert, allocator);
  g_object_set (test.convert, "async-depth", 1, NULL);
  h = gst_harness_new_with_element (GST_ELEMENT (test.convert), "sink", "src");
  gst_harness_set_propose_allocator (h, allocator, NULL);
  gst_harness_set_caps_str (h,
      "video/x-raw(memory:DMABuf),format=NV16,width=320,height=240,framerate=30/1",
      "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240,framerate=30/1");
  for (i = 0; i < 2; i++) {
    input = new_nv16_buffer (TRUE, 320, 240, 320, 240);
    GST_BUFFER_PTS (input) = i * GST_SECOND;
    GST_BUFFER_DURATION (input) = GST_SECOND / 30;
    fail_unless_equals_int (gst_harness_push (h, input), GST_FLOW_OK);
    fail_unless_equals_int (gst_harness_buffers_in_queue (h), i);
  }
  out = gst_harness_pull (h);
  fail_unless_equals_uint64 (GST_BUFFER_PTS (out), 0);
  fail_unless_equals_uint64 (GST_BUFFER_DURATION (out), GST_SECOND / 30);
  gst_buffer_unref (out);
  fail_unless (gst_harness_push_event (h,
          gst_event_new_tag (gst_tag_list_new_empty ())));
  fail_unless_equals_int (gst_harness_buffers_in_queue (h), 1);
  out = gst_harness_pull (h);
  fail_unless_equals_uint64 (GST_BUFFER_PTS (out), GST_SECOND);
  gst_buffer_unref (out);

  input = new_nv16_buffer (TRUE, 320, 240, 320, 240);
  fail_unless_equals_int (gst_harness_push (h, input), GST_FLOW_OK);
  fail_unless (gst_harness_push_event (h, gst_event_new_custom (
              GST_EVENT_CUSTOM_DOWNSTREAM, gst_structure_new_empty ("ordered"))));
  fail_unless_equals_int (gst_harness_buffers_in_queue (h), 1);
  gst_buffer_unref (gst_harness_pull (h));

  input = new_nv16_buffer (TRUE, 320, 240, 320, 240);
  fail_unless_equals_int (gst_harness_push (h, input), GST_FLOW_OK);
  gst_harness_set_sink_caps_str (h,
      "video/x-raw(memory:DMABuf),format=NV12,width=640,height=480,framerate=30/1");
  gst_harness_set_src_caps_str (h,
      "video/x-raw(memory:DMABuf),format=NV16,width=640,height=480,framerate=30/1");
  fail_unless_equals_int (gst_harness_buffers_in_queue (h), 1);
  out = gst_harness_pull (h);
  fail_unless_equals_int (gst_buffer_get_video_meta (out)->width, 320);
  gst_buffer_unref (out);
  input = new_nv16_buffer (TRUE, 640, 480, 640, 480);
  fail_unless_equals_int (gst_harness_push (h, input), GST_FLOW_OK);
  fail_unless (gst_harness_push_event (h, gst_event_new_eos ()));
  out = gst_harness_pull (h);
  fail_unless_equals_int (gst_buffer_get_video_meta (out)->width, 640);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  fail_unless_fences_closed (&fake);
  test_convert_clear (&test);
}

GST_END_TEST;

GST_START_TEST (test_a_synchronous_frame_never_overtakes_a_pipelined_one)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_SUCCESS
  };
  TestConvert test = test_convert_new (&fake);
  GstPad *srcpad = GST_BASE_TRANSFORM_SRC_PAD (test.convert);
  GstPad *sinkpad = gst_pad_new ("sink", GST_PAD_SINK);
  GstCaps *caps = caps_from_string
      ("video/x-raw(memory:DMABuf),format=NV12,width=320,height=240");
  GstSegment segment;
  GstBuffer *pipelined = new_nv12_buffer (TRUE, 320, 240, 320, 240);
  GstBuffer *immediate = new_nv12_buffer (TRUE, 320, 240, 320, 240);
  GstBuffer *out = NULL;

  gst_pad_set_chain_function (sinkpad, gst_check_chain_func);
  gst_pad_set_active (sinkpad, TRUE);
  gst_pad_set_active (srcpad, TRUE);
  fail_unless_equals_int (gst_pad_link (srcpad, sinkpad), GST_PAD_LINK_OK);
  gst_segment_init (&segment, GST_FORMAT_TIME);
  fail_unless (gst_pad_push_event (srcpad,
          gst_event_new_stream_start ("rgaconvert-order")));
  fail_unless (gst_pad_push_event (srcpad, gst_event_new_caps (caps)));
  fail_unless (gst_pad_push_event (srcpad, gst_event_new_segment (&segment)));

  g_object_set (test.convert, "async-depth", 1, NULL);
  fail_unless (release_pipelined (test.convert, &fake,
          gst_buffer_ref (pipelined)) == NULL);

  fail_unless_equals_int (gst_rga_convert_release_pipelined (test.convert,
          gst_buffer_ref (immediate), -1, &out), GST_FLOW_OK);
  fail_unless (out == immediate);
  fail_unless_equals_int (g_list_length (buffers), 1);
  fail_unless (buffers->data == pipelined,
      "the held frame must be pushed before the synchronous one is returned");
  gst_buffer_unref (out);

  fail_unless_fences_closed (&fake);

  gst_check_drop_buffers ();
  gst_buffer_unref (immediate);
  gst_buffer_unref (pipelined);
  gst_pad_set_active (sinkpad, FALSE);
  gst_pad_set_active (srcpad, FALSE);
  gst_pad_unlink (srcpad, sinkpad);
  gst_object_unref (sinkpad);
  gst_caps_unref (caps);
  test_convert_clear (&test);
}

GST_END_TEST;

GST_START_TEST (test_flush_stop_discards_the_held_frame_and_never_pushes_it)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_SUCCESS
  };
  TestConvert test = test_convert_new (&fake);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (test.convert);
  GstPad *srcpad = GST_BASE_TRANSFORM_SRC_PAD (test.convert);
  GstPad *sinkpad = gst_pad_new ("sink", GST_PAD_SINK);
  GstBuffer *frame = new_nv12_buffer (TRUE, 320, 240, 320, 240);
  GstBuffer *out;

  gst_pad_set_chain_function (sinkpad, gst_check_chain_func);
  gst_pad_set_active (sinkpad, TRUE);
  gst_pad_set_active (srcpad, TRUE);
  fail_unless_equals_int (gst_pad_link (srcpad, sinkpad), GST_PAD_LINK_OK);

  g_object_set (test.convert, "async-depth", 1, NULL);
  out = release_pipelined (test.convert, &fake, gst_buffer_ref (frame));
  fail_unless (out == NULL);

  fail_unless (klass->sink_event (GST_BASE_TRANSFORM (test.convert),
          gst_event_new_flush_stop (TRUE)));
  fail_unless_equals_int (g_list_length (buffers), 0);
  fail_unless (GST_MINI_OBJECT_REFCOUNT (frame) == 1,
      "the discarded frame must be unreffed exactly once");
  fail_unless_fences_closed (&fake);

  gst_buffer_unref (frame);
  gst_pad_set_active (sinkpad, FALSE);
  gst_pad_set_active (srcpad, FALSE);
  gst_pad_unlink (srcpad, sinkpad);
  gst_object_unref (sinkpad);
  test_convert_clear (&test);
}

GST_END_TEST;

GST_START_TEST (test_backend_async_capability_and_fence_ownership)
{
  FakeRga fake = {.available = TRUE,.process_result = IM_STATUS_SUCCESS,
    .async_supported = TRUE,.async_result = IM_STATUS_NOT_SUPPORTED
  };
  GstMppRgaIm2dRequest request = { 0, };
  GstMppRgaBackendOps sync_only = fake_ops;
  GstMppRgaBackend *backend;
  gint fence = 0;

  sync_only.async_supported = NULL;
  sync_only.process_async = NULL;
  backend = gst_mpp_rga_backend_new (&sync_only, &fake);
  fail_if (gst_mpp_rga_backend_supports_async (backend));
  fail_unless_equals_int (gst_mpp_rga_backend_process_async (backend,
          GST_MPP_RGA_OP_CONVERT, GST_VIDEO_FORMAT_NV16, GST_VIDEO_FORMAT_NV12,
          &request, &fence), GST_MPP_RGA_UNAVAILABLE);
  fail_unless_equals_int (fence, -1);
  gst_mpp_rga_backend_free (backend);

  backend = gst_mpp_rga_backend_new (&fake_ops, &fake);
  fail_unless (gst_mpp_rga_backend_supports_async (backend));
  fail_unless_equals_int (gst_mpp_rga_backend_process_async (backend,
          GST_MPP_RGA_OP_CONVERT, GST_VIDEO_FORMAT_NV16, GST_VIDEO_FORMAT_NV12,
          &request, &fence), GST_MPP_RGA_NOT_SUPPORTED);
  fail_unless (fence >= 0);
  fail_unless (gst_mpp_rga_fence_wait (fence, 1000));
  fail_unless_equals_int (fake.process_async_calls, 1);
  fail_unless_fences_closed (&fake);

  fake.async_result = IM_STATUS_SUCCESS;
  fail_unless_equals_int (gst_mpp_rga_backend_process_async (backend,
          GST_MPP_RGA_OP_CONVERT, GST_VIDEO_FORMAT_NV16, GST_VIDEO_FORMAT_NV12,
          &request, &fence), GST_MPP_RGA_SUCCESS);
  fail_unless (fence >= 0);
  fail_unless (gst_mpp_rga_fence_wait (fence, 1000));
  fail_unless (gst_mpp_rga_fence_wait (-1, 0));
  fail_unless_fences_closed (&fake);
  fake.missing_fence = TRUE;
  fail_unless_equals_int (gst_mpp_rga_backend_process_async (backend,
          GST_MPP_RGA_OP_CONVERT, GST_VIDEO_FORMAT_NV16, GST_VIDEO_FORMAT_NV12,
          &request, &fence), GST_MPP_RGA_BLIT_FAILED);
  fail_unless_equals_int (fence, GST_MPP_RGA_FENCE_MISSING);
  fail_unless_equals_int (gst_mpp_rga_fence_status (fence, 0),
      GST_MPP_RGA_FENCE_PENDING);
  gst_mpp_rga_backend_free (backend);
}

GST_END_TEST;

static Suite *
rgaconvert_suite (void)
{
  Suite *suite = suite_create ("rgaconvert");
  TCase *test_case = tcase_create ("element");
  tcase_add_test (test_case, test_interpolation_and_seven_argument_fallback);
  tcase_add_test (test_case, test_mpp_blit_uses_im2d_and_explicit_rollback);

  tcase_set_timeout (test_case, 15);
  tcase_add_checked_fixture (test_case, clear_cpu_copy_env,
      clear_cpu_copy_env);
  tcase_add_test (test_case, test_caps_and_property_contract);
  tcase_add_test (test_case,
      test_rotation_and_crop_fixate_natural_output_dimensions);
  tcase_add_test (test_case, test_caps_negotiation_matrix);
  tcase_add_test (test_case, test_fixation_preserves_omitted_yuv_colorimetry);
  tcase_add_test (test_case,
      test_transform_caps_prefers_identity_colorimetry_both_directions);
  tcase_add_test (test_case, test_fixation_does_not_relabel_explicit_bt709_or_rgb);
  tcase_add_test (test_case, test_colorimetry_reaches_improcess);
  tcase_add_test (test_case, test_unspecified_colorimetry_uses_video_info_defaults);
  tcase_add_test (test_case, test_stride_only_does_not_request_csc);
  tcase_add_test (test_case, test_color_space_changes_and_unsupported_modes);
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
  tcase_add_test (test_case,
      test_allocation_query_pool_references_are_released);
  tcase_add_test (test_case,
      test_async_depth_property_contract_and_default_is_synchronous);
  tcase_add_test (test_case,
      test_async_depth_one_submits_async_only_when_the_runtime_can);
  tcase_add_test (test_case,
      test_async_submit_is_never_reached_without_a_dmabuf_pair);
  tcase_add_test (test_case,
      test_depth_one_holds_a_frame_and_eos_drains_it_in_order);
  tcase_add_test (test_case, test_unsignalled_fence_timeout_keeps_descriptor_owned);
  tcase_add_test (test_case, test_timeout_never_returns_unfinished_output);
  tcase_add_test (test_case,
      test_flush_start_quarantines_input_and_output_without_waiting);
  tcase_add_test (test_case,
      test_stop_retains_never_signalled_frame_past_error_deadline);
  tcase_add_test (test_case,
      test_real_transform_chain_orders_buffers_events_and_caps);
  tcase_add_test (test_case,
      test_a_synchronous_frame_never_overtakes_a_pipelined_one);
  tcase_add_test (test_case,
      test_flush_stop_discards_the_held_frame_and_never_pushes_it);
  tcase_add_test (test_case, test_backend_async_capability_and_fence_ownership);
  suite_add_tcase (suite, test_case);
  return suite;
}

GST_CHECK_MAIN (rgaconvert);
