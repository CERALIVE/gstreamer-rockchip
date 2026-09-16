#include <fcntl.h>
#include <glib/gstdio.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/video/gstvideometa.h>
#include <unistd.h>

#include <rga/im2d.h>

#include "../../gst/rockchiprga/gstrgacompositor.h"
/* Keep descriptor assertions below the real backend's im2d wrapping. */
#undef GST_CAT_DEFAULT
#include "../../gst/rockchipmpp/gstmpprgabackend.c"
#undef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT check_debug

static rga_buffer_t submitted_dst[3];
static guint submitted_calls;
static guint overlay_allocations;
static gboolean fail_overlay_allocation;
static IM_STATUS config_status = IM_STATUS_SUCCESS;
static IM_STATUS process_status = IM_STATUS_SUCCESS;
static gint injected_errno;

#define PRIMARY_CAPS \
  "video/x-raw(memory:DMABuf),format=NV12,width=1920,height=1080," \
  "framerate=30/1,interlace-mode=progressive"
#define SECONDARY_CAPS \
  "video/x-raw(memory:DMABuf),format=BGRA,width=1920,height=1080," \
  "framerate=30/1,interlace-mode=progressive"

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
  errno = injected_errno;
  return config_status;
}

IM_STATUS
improcess (rga_buffer_t src, rga_buffer_t dst, rga_buffer_t pat,
    im_rect src_rect, im_rect dst_rect, im_rect pat_rect, int usage)
{
  if (process_status != IM_STATUS_SUCCESS) {
    submitted_calls++;
    errno = injected_errno;
    return process_status;
  }
  fail_unless (src_rect.x >= 0 && src_rect.y >= 0 &&
      src_rect.width > 0 && src_rect.height > 0 &&
      src_rect.x + src_rect.width <= src.width &&
      src_rect.y + src_rect.height <= src.height);
  fail_unless (dst_rect.x >= 0 && dst_rect.y >= 0 &&
      dst_rect.width > 0 && dst_rect.height > 0 &&
      dst_rect.x + dst_rect.width <= dst.width &&
      dst_rect.y + dst_rect.height <= dst.height);
  fail_unless (fcntl (src.fd, F_GETFD) >= 0);
  fail_unless (fcntl (dst.fd, F_GETFD) >= 0);
  if (usage & IM_ALPHA_BLEND_MASK) {
    fail_unless (pat.fd > 0);
    fail_unless (fcntl (pat.fd, F_GETFD) >= 0);
    fail_unless (pat_rect.x >= 0 && pat_rect.y >= 0 &&
        pat_rect.x + pat_rect.width <= pat.width &&
        pat_rect.y + pat_rect.height <= pat.height);
    fail_unless (pat_rect.width == dst_rect.width &&
        pat_rect.height == dst_rect.height,
        "pat cannot scale: active pat %dx%d differs from dst %dx%d",
        pat_rect.width, pat_rect.height, dst_rect.width, dst_rect.height);
  }
  if (submitted_calls < G_N_ELEMENTS (submitted_dst))
    submitted_dst[submitted_calls] = dst;
  submitted_calls++;
  return IM_STATUS_SUCCESS;
}

typedef struct _GstTestDmaBufAllocator GstTestDmaBufAllocator;
typedef struct _GstTestDmaBufAllocatorClass GstTestDmaBufAllocatorClass;

struct _GstTestDmaBufAllocator
{
  GstDmaBufAllocator parent;
};

struct _GstTestDmaBufAllocatorClass
{
  GstDmaBufAllocatorClass parent_class;
};

#define GST_TYPE_TEST_DMABUF_ALLOCATOR \
  (gst_test_dmabuf_allocator_get_type ())

G_DEFINE_TYPE (GstTestDmaBufAllocator, gst_test_dmabuf_allocator,
    GST_TYPE_DMABUF_ALLOCATOR);

static GstMemory *
gst_test_dmabuf_allocator_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  gsize prefix = params ? params->prefix : 0;
  gsize padding = params ? params->padding : 0;
  gsize total;
  GstMemory *memory;
  GError *error = NULL;
  gchar *name = NULL;
  gint fd;

  if (size == 960 * 544 * 4) {
    overlay_allocations++;
    if (fail_overlay_allocation)
      return NULL;
  }
  if (G_MAXSIZE - prefix < size || G_MAXSIZE - prefix - size < padding)
    return NULL;
  total = prefix + size + padding;
  fd = g_file_open_tmp ("rgacompositor-pool-XXXXXX", &name, &error);
  if (fd < 0) {
    g_clear_error (&error);
    g_free (name);
    return NULL;
  }
  g_unlink (name);
  g_free (name);
  if (ftruncate (fd, total) < 0) {
    close (fd);
    return NULL;
  }

  memory = gst_dmabuf_allocator_alloc (allocator, fd, total);
  if (!memory) {
    close (fd);
    return NULL;
  }
  gst_memory_resize (memory, prefix, size);
  return memory;
}

static void
gst_test_dmabuf_allocator_init (GstTestDmaBufAllocator * self)
{
  (void) self;
}

static void
gst_test_dmabuf_allocator_class_init (GstTestDmaBufAllocatorClass * klass)
{
  GST_ALLOCATOR_CLASS (klass)->alloc = gst_test_dmabuf_allocator_alloc;
}

typedef struct
{
  gboolean available;
  gint process_result;
  gint composite_result;
  guint process_calls;
  guint composite_calls;
  gboolean submit_im2d;
  gboolean fail_overlay_scale;
  GstMppRgaIm2dRequest first_process;
  GstMppRgaIm2dRequest last_process;
  GstMppRgaIm2dCompositeRequest last_composite;
} FakeRga;

typedef struct
{
  GstHarness *output;
  GstHarness *primary;
  GstHarness *secondary;
  GstMppRgaBackend *backend;
  GstAllocator *allocator;
} TestHarness;

typedef struct
{
  GstHarness *harness;
  GstBuffer *buffer;
  GstFlowReturn flow;
} PushData;

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

  if (fake->process_calls == 0)
    fake->first_process = *request;
  fake->process_calls++;
  fake->last_process = *request;
  if (fake->fail_overlay_scale && request->src_format == RK_FORMAT_BGRA_8888)
    return IM_STATUS_FAILED;
  if (fake->submit_im2d)
    return gst_mpp_rga_real_process (request, NULL);
  return fake->process_result;
}

static gint
fake_composite (const GstMppRgaIm2dCompositeRequest * request,
    gpointer user_data)
{
  FakeRga *fake = user_data;

  fake->composite_calls++;
  fake->last_composite = *request;
  if (fake->submit_im2d)
    return gst_mpp_rga_real_composite (request, NULL);
  return fake->composite_result;
}

static const GstMppRgaBackendOps fake_ops = {
  .probe = fake_probe,
  .init = fake_init,
  .blit = fake_blit,
  .process = fake_process,
  .composite = fake_composite,
};

static GstAllocator *
test_allocator_new (void)
{
  GstAllocator *allocator = g_object_new (GST_TYPE_TEST_DMABUF_ALLOCATOR,
      NULL);

  return gst_object_ref_sink (allocator);
}

static GstBuffer *
new_video_buffer (GstVideoFormat format, guint width, guint height)
{
  GstAllocator *allocator = test_allocator_new ();
  gsize offsets[GST_VIDEO_MAX_PLANES] = { 0, };
  gint strides[GST_VIDEO_MAX_PLANES] = { 0, };
  guint planes = format == GST_VIDEO_FORMAT_NV12 ? 2 : 1;
  gsize size;
  GstBuffer *buffer;

  if (format == GST_VIDEO_FORMAT_NV12) {
    offsets[1] = width * height;
    strides[0] = width;
    strides[1] = width;
    size = width * height * 3 / 2;
  } else {
    fail_unless_equals_int (format, GST_VIDEO_FORMAT_BGRA);
    strides[0] = width * 4;
    size = width * height * 4;
  }

  buffer = gst_buffer_new_allocate (allocator, size, NULL);

  gst_object_unref (allocator);
  fail_unless (buffer != NULL);
  fail_unless (gst_buffer_add_video_meta_full (buffer,
          GST_VIDEO_FRAME_FLAG_NONE, format, width, height, planes,
          offsets, strides) != NULL);
  GST_BUFFER_PTS (buffer) = 0;
  GST_BUFFER_DTS (buffer) = 0;
  GST_BUFFER_DURATION (buffer) = GST_SECOND / 30;
  return buffer;
}

static GstBuffer *
new_nv12_buffer (guint width, guint height)
{
  return new_video_buffer (GST_VIDEO_FORMAT_NV12, width, height);
}

static GstBuffer *
new_bgra_buffer (guint width, guint height)
{
  return new_video_buffer (GST_VIDEO_FORMAT_BGRA, width, height);
}

static TestHarness
test_harness_new_with_caps (FakeRga * fake, gboolean with_secondary,
    const gchar * color, const gchar * secondary_caps)
{
  TestHarness test = { 0, };
  GstRgaCompositor *compositor = g_object_new (GST_TYPE_RGA_COMPOSITOR, NULL);
  GstPad *pad;
  gchar *primary_caps = color ? g_strdup_printf (PRIMARY_CAPS
      ",colorimetry=%s", color) : g_strdup (PRIMARY_CAPS);

  test.backend = gst_mpp_rga_backend_new (&fake_ops, fake);
  test.allocator = test_allocator_new ();
  gst_rga_compositor_set_backend_for_test (compositor, test.backend);
  gst_rga_compositor_set_allocator_for_test (compositor, test.allocator);

  test.output = gst_harness_new_with_element (GST_ELEMENT (compositor), NULL,
      "src");
  test.primary = gst_harness_new_with_element (test.output->element, NULL,
      NULL);
  pad = gst_element_request_pad_simple (test.output->element, "sink_0");
  fail_unless (pad != NULL);
  gst_harness_add_element_sink_pad (test.primary, pad);
  gst_object_unref (pad);
  if (with_secondary) {
    test.secondary = gst_harness_new_with_element (test.output->element, NULL,
        NULL);
    pad = gst_element_request_pad_simple (test.output->element, "sink_1");
    fail_unless (pad != NULL);
    gst_harness_add_element_sink_pad (test.secondary, pad);
    gst_object_unref (pad);
  }
  gst_object_unref (compositor);

  gst_harness_set_sink_caps_str (test.output, primary_caps);
  gst_harness_set_src_caps_str (test.primary, primary_caps);
  g_free (primary_caps);
  if (test.secondary)
    gst_harness_set_src_caps_str (test.secondary, secondary_caps);
  gst_harness_play (test.output);
  return test;
}

static TestHarness
test_harness_new_with_color (FakeRga * fake, gboolean with_secondary,
    const gchar * color)
{
  return test_harness_new_with_caps (fake, with_secondary, color, SECONDARY_CAPS);
}

static TestHarness
test_harness_new (FakeRga * fake, gboolean with_secondary)
{
  return test_harness_new_with_color (fake, with_secondary, NULL);
}

static void
test_harness_clear (TestHarness * test)
{
  if (test->secondary)
    gst_harness_teardown (test->secondary);
  gst_harness_teardown (test->primary);
  gst_harness_teardown (test->output);
  gst_mpp_rga_backend_free (test->backend);
  gst_object_unref (test->allocator);
}

static gpointer
push_buffer (gpointer user_data)
{
  PushData *data = user_data;

  data->flow = gst_harness_push (data->harness, data->buffer);
  return NULL;
}

static GstBuffer *
push_pair_and_pull_full (TestHarness * test, guint secondary_width,
    guint secondary_height, GstClockTime pts)
{
  PushData primary = {
    .harness = test->primary,
    .buffer = new_nv12_buffer (1920, 1080),
    .flow = GST_FLOW_ERROR,
  };
  GstBuffer *secondary = new_bgra_buffer (secondary_width, secondary_height);
  GThread *thread;
  GstFlowReturn secondary_flow;
  GstBuffer *output;

  GST_BUFFER_PTS (primary.buffer) = pts;
  GST_BUFFER_PTS (secondary) = pts;
  thread = g_thread_new ("rgacompositor-primary", push_buffer, &primary);
  secondary_flow = gst_harness_push (test->secondary, secondary);
  g_thread_join (thread);
  fail_unless_equals_int (primary.flow, GST_FLOW_OK);
  fail_unless_equals_int (secondary_flow, GST_FLOW_OK);
  output = gst_harness_pull (test->output);
  fail_unless (output != NULL);
  return output;
}

static GstBuffer *
push_pair_and_pull (TestHarness * test)
{
  return push_pair_and_pull_full (test, 1920, 1080, 0);
}

static GstMessage *
pop_error (GstBus * bus)
{
  GstMessage *message = gst_bus_timed_pop_filtered (bus, GST_SECOND,
      GST_MESSAGE_ERROR);

  fail_unless (message != NULL);
  return message;
}

GST_START_TEST (test_pad_factory_and_property_contract)
{
  GstElement *compositor = g_object_new (GST_TYPE_RGA_COMPOSITOR, NULL);
  GstElementClass *element_class = GST_ELEMENT_GET_CLASS (compositor);
  GstPadTemplate *template = gst_element_class_get_pad_template (element_class,
      "sink_%u");
  GstPadTemplate *src_template = gst_element_class_get_pad_template
      (element_class, "src");
  GstCaps *sink_caps;
  GstCaps *src_caps;
  GstCaps *primary_caps;
  GstCaps *secondary_caps;
  gchar *sink_caps_string;
  gchar *src_caps_string;
  GstPad *primary;
  GstPad *secondary;
  GstPad *third;
  GEnumClass *layouts;
  const gchar *layout_nicks[] = {
    "pip-top-right", "pip-top-left", "pip-bottom-right", "pip-bottom-left",
    "pbp-left-right", "pbp-top-bottom", "custom",
  };
  const gchar *pad_properties[] = {
    "xpos", "ypos", "width", "height", "alpha", "zorder",
  };
  const gchar *element_properties[] = {
    "layout", "conversion-fallback-frames", "conversion-dropped-frames",
    "layout-rejections",
  };
  guint zorder = 0;
  guint i;

  fail_unless (template != NULL);
  fail_unless (src_template != NULL);
  fail_unless (g_type_is_a (G_OBJECT_TYPE (compositor),
          GST_TYPE_VIDEO_AGGREGATOR));
  fail_unless_equals_int (template->presence, GST_PAD_REQUEST);
  sink_caps = gst_pad_template_get_caps (template);
  src_caps = gst_pad_template_get_caps (src_template);
  sink_caps_string = gst_caps_to_string (sink_caps);
  src_caps_string = gst_caps_to_string (src_caps);
  fail_unless (g_strstr_len (sink_caps_string, -1, "NV12") != NULL);
  fail_unless (g_strstr_len (sink_caps_string, -1, "BGRA") != NULL);
  fail_unless (g_strstr_len (src_caps_string, -1, "NV12") != NULL);
  fail_unless (g_strstr_len (src_caps_string, -1, "BGRA") == NULL);
  primary = gst_element_request_pad_simple (compositor, "sink_%u");
  secondary = gst_element_request_pad_simple (compositor, "sink_%u");
  third = gst_element_request_pad_simple (compositor, "sink_%u");
  fail_unless (primary != NULL);
  fail_unless (secondary != NULL);
  fail_unless (third == NULL);
  fail_unless_equals_string (GST_PAD_NAME (primary), "sink_0");
  fail_unless_equals_string (GST_PAD_NAME (secondary), "sink_1");
  primary_caps = gst_caps_from_string (PRIMARY_CAPS);
  secondary_caps = gst_caps_from_string (SECONDARY_CAPS);
  fail_unless (gst_pad_query_accept_caps (primary, primary_caps));
  fail_if (gst_pad_query_accept_caps (primary, secondary_caps));
  fail_unless (gst_pad_query_accept_caps (secondary, secondary_caps));
  fail_if (gst_pad_query_accept_caps (secondary, primary_caps));

  for (i = 0; i < G_N_ELEMENTS (pad_properties); i++)
    fail_unless (g_object_class_find_property (G_OBJECT_GET_CLASS (primary),
            pad_properties[i]) != NULL);
  for (i = 0; i < G_N_ELEMENTS (element_properties); i++)
    fail_unless (g_object_class_find_property (G_OBJECT_GET_CLASS (compositor),
            element_properties[i]) != NULL);
  g_object_set (secondary, "zorder", 7, NULL);
  g_object_get (secondary, "zorder", &zorder, NULL);
  fail_unless_equals_int (zorder, 7);

  layouts = g_type_class_ref (GST_TYPE_RGA_COMPOSITOR_LAYOUT);
  for (i = 0; i < G_N_ELEMENTS (layout_nicks); i++)
    fail_unless (g_enum_get_value_by_nick (layouts, layout_nicks[i]) != NULL);
  g_type_class_unref (layouts);
  gst_caps_unref (secondary_caps);
  gst_caps_unref (primary_caps);
  g_free (src_caps_string);
  g_free (sink_caps_string);
  gst_caps_unref (src_caps);
  gst_caps_unref (sink_caps);

  gst_element_release_request_pad (compositor, secondary);
  gst_element_release_request_pad (compositor, primary);
  gst_object_unref (secondary);
  gst_object_unref (primary);
  gst_object_unref (compositor);
}
GST_END_TEST;

GST_START_TEST (test_named_and_custom_layouts_obey_pat_no_scaling_contract)
{
  const struct
  {
    GstRgaCompositorLayout layout;
    gint primary_x;
    gint primary_y;
    gint primary_width;
    gint primary_height;
    gint secondary_x;
    gint secondary_y;
    gint secondary_width;
    gint secondary_height;
  } cases[] = {
    {GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_RIGHT,
        0, 0, 1920, 1080, 864, 54, 960, 540},
    {GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_LEFT,
        0, 0, 1920, 1080, 96, 54, 960, 540},
    {GST_RGA_COMPOSITOR_LAYOUT_PIP_BOTTOM_RIGHT,
        0, 0, 1920, 1080, 864, 486, 960, 540},
    {GST_RGA_COMPOSITOR_LAYOUT_PIP_BOTTOM_LEFT,
        0, 0, 1920, 1080, 96, 486, 960, 540},
    {GST_RGA_COMPOSITOR_LAYOUT_PBP_LEFT_RIGHT,
        0, 0, 960, 1080, 960, 0, 960, 1080},
    {GST_RGA_COMPOSITOR_LAYOUT_PBP_TOP_BOTTOM,
        0, 0, 1920, 540, 0, 540, 1920, 540},
    {GST_RGA_COMPOSITOR_LAYOUT_CUSTOM,
        0, 0, 1920, 1080, 100, 200, 640, 360},
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++) {
    FakeRga fake = {
      .available = TRUE,
      .process_result = IM_STATUS_SUCCESS,
      .composite_result = IM_STATUS_SUCCESS,
      .submit_im2d = TRUE,
    };
    TestHarness test = test_harness_new (&fake, TRUE);
    GstPad *primary = gst_element_get_static_pad (test.output->element,
        "sink_0");
    GstPad *secondary = gst_element_get_static_pad (test.output->element,
        "sink_1");
    GstBuffer *output;
    GstMemory *memory;

    g_object_set (test.output->element, "layout", cases[i].layout, NULL);
    g_object_set (secondary, "alpha", 0.4, NULL);
    if (cases[i].layout == GST_RGA_COMPOSITOR_LAYOUT_CUSTOM) {
      g_object_set (primary, "xpos", cases[i].primary_x,
          "ypos", cases[i].primary_y, "width", cases[i].primary_width,
          "height", cases[i].primary_height, NULL);
      g_object_set (secondary, "xpos", cases[i].secondary_x,
          "ypos", cases[i].secondary_y, "width", cases[i].secondary_width,
          "height", cases[i].secondary_height, NULL);
    }

    output = push_pair_and_pull (&test);
    fail_unless_equals_int (gst_buffer_n_memory (output), 1);
    memory = gst_buffer_peek_memory (output, 0);
    fail_unless (gst_is_dmabuf_memory (memory));
    fail_unless (memory->allocator == test.allocator);
    fail_unless (gst_buffer_get_video_meta (output) != NULL);
    fail_unless_equals_int (fake.process_calls, 2);
    fail_unless_equals_int (fake.composite_calls, 1);
    fail_unless_equals_int (fake.first_process.dst_x, cases[i].primary_x);
    fail_unless_equals_int (fake.first_process.dst_y, cases[i].primary_y);
    fail_unless_equals_int (fake.first_process.dst_rect_width,
        cases[i].primary_width);
    fail_unless_equals_int (fake.first_process.dst_rect_height,
        cases[i].primary_height);
    fail_unless_equals_int (fake.last_process.src_format, RK_FORMAT_BGRA_8888);
    fail_unless_equals_int (fake.last_process.dst_format, RK_FORMAT_BGRA_8888);
    fail_unless_equals_int (fake.last_process.src_x, 0);
    fail_unless_equals_int (fake.last_process.src_y, 0);
    fail_unless_equals_int (fake.last_process.src_rect_width, 1920);
    fail_unless_equals_int (fake.last_process.src_rect_height, 1080);
    fail_unless_equals_int (fake.last_process.dst_x, 0);
    fail_unless_equals_int (fake.last_process.dst_y, 0);
    fail_unless_equals_int (fake.last_process.dst_rect_width,
        cases[i].secondary_width);
    fail_unless_equals_int (fake.last_process.dst_rect_height,
        cases[i].secondary_height);
    fail_if (fake.last_process.src_fd == fake.last_process.dst_fd);
    fail_unless_equals_int (fake.last_process.dst_fd, fake.last_composite.pat_fd);
    fail_unless_equals_int (fake.last_composite.transform.dst_x,
        cases[i].secondary_x);
    fail_unless_equals_int (fake.last_composite.transform.dst_y,
        cases[i].secondary_y);
    fail_unless_equals_int (fake.last_composite.transform.dst_rect_width,
        cases[i].secondary_width);
    fail_unless_equals_int (fake.last_composite.transform.dst_rect_height,
        cases[i].secondary_height);
    fail_unless_equals_int (fake.last_composite.transform.src_x,
        cases[i].secondary_x);
    fail_unless_equals_int (fake.last_composite.transform.src_y,
        cases[i].secondary_y);
    fail_unless_equals_int (fake.last_composite.transform.src_rect_width,
        cases[i].secondary_width);
    fail_unless_equals_int (fake.last_composite.transform.src_rect_height,
        cases[i].secondary_height);
    fail_unless_equals_int (fake.last_composite.transform.src_fd,
        fake.last_composite.transform.dst_fd);
    fail_if (fake.last_composite.pat_fd ==
        fake.last_composite.transform.dst_fd);
    fail_unless_equals_int (fake.last_composite.pat_format,
        RK_FORMAT_BGRA_8888);
    fail_unless_equals_int (fake.last_composite.pat_width,
        cases[i].secondary_width);
    fail_unless_equals_int (fake.last_composite.pat_height,
        cases[i].secondary_height);
    fail_unless_equals_int (fake.last_composite.pat_rect_width,
        cases[i].secondary_width);
    fail_unless_equals_int (fake.last_composite.pat_rect_height,
        cases[i].secondary_height);
    fail_unless_equals_int (fake.last_composite.pat_wstride,
        GST_ROUND_UP_16 (cases[i].secondary_width));
    fail_unless_equals_int (fake.last_composite.pat_hstride,
        GST_ROUND_UP_16 (cases[i].secondary_height));
    fail_unless_equals_int (fake.last_composite.src_alpha, 255);
    fail_unless_equals_int (fake.last_composite.pat_alpha, 102);
    fail_unless ((fake.last_composite.transform.usage &
            IM_ALPHA_BLEND_DST_OVER) != 0);
    fail_unless ((fake.last_composite.transform.usage &
            IM_ALPHA_BLEND_PRE_MUL) != 0);

    gst_buffer_unref (output);
    gst_object_unref (secondary);
    gst_object_unref (primary);
    test_harness_clear (&test);
  }
}
GST_END_TEST;

GST_START_TEST (test_overlay_pool_reused_resized_and_released_on_stop)
{
  FakeRga fake = {.available = TRUE, .submit_im2d = TRUE};
  TestHarness test = test_harness_new (&fake, TRUE);
  GstPad *primary = gst_element_get_static_pad (test.output->element, "sink_0");
  GstPad *secondary = gst_element_get_static_pad (test.output->element, "sink_1");
  GstBuffer *output;
  gint old_fd;
  guint allocations;

  overlay_allocations = 0;
  output = push_pair_and_pull (&test);
  gst_buffer_unref (output);
  allocations = overlay_allocations;
  fail_unless (allocations > 0);
  output = push_pair_and_pull_full (&test, 1920, 1080, GST_SECOND / 30);
  gst_buffer_unref (output);
  fail_unless_equals_int (overlay_allocations, allocations);

  g_object_set (test.output->element, "layout", GST_RGA_COMPOSITOR_LAYOUT_CUSTOM,
      NULL);
  g_object_set (primary, "width", 1920, "height", 1080, NULL);
  g_object_set (secondary, "xpos", 100, "ypos", 200, "width", 642,
      "height", 362, NULL);
  output = push_pair_and_pull_full (&test, 1920, 1080, 2 * GST_SECOND / 30);
  gst_buffer_unref (output);
  fail_unless_equals_int (fake.last_composite.pat_width, 642);
  fail_unless_equals_int (fake.last_composite.pat_height, 362);
  fail_unless_equals_int (fake.last_composite.pat_wstride, 656);
  fail_unless_equals_int (fake.last_composite.pat_hstride, 368);
  old_fd = fake.last_composite.pat_fd;
  gst_object_unref (secondary);
  gst_object_unref (primary);
  fail_unless (gst_element_set_state (test.output->element, GST_STATE_NULL) !=
      GST_STATE_CHANGE_FAILURE);
  fail_unless_equals_int (fcntl (old_fd, F_GETFD), -1);
  fail_unless_equals_int (errno, EBADF);
  test_harness_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_already_sized_overlay_does_not_scale_or_allocate)
{
  FakeRga fake = {.available = TRUE, .submit_im2d = TRUE};
  TestHarness test = test_harness_new_with_caps (&fake, TRUE, NULL,
      "video/x-raw(memory:DMABuf),format=BGRA,width=960,height=540,"
      "framerate=30/1,interlace-mode=progressive");
  GstBuffer *output;
  overlay_allocations = 0;
  output = push_pair_and_pull_full (&test, 960, 540, 0);
  fail_unless_equals_int (fake.process_calls, 1);
  fail_unless_equals_int (fake.composite_calls, 1);
  fail_unless_equals_int (overlay_allocations, 0);
  fail_unless_equals_int (fake.last_composite.pat_width, 960);
  fail_unless_equals_int (fake.last_composite.pat_height, 540);
  gst_buffer_unref (output);
  test_harness_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_overlay_preparation_failure_never_blends_or_outputs)
{
  guint i;

  for (i = 0; i < 2; i++) {
    FakeRga fake = {.available = TRUE, .process_result = IM_STATUS_SUCCESS,
      .composite_result = IM_STATUS_SUCCESS, .fail_overlay_scale = i == 0};
    TestHarness test = test_harness_new (&fake, TRUE);
    GstBus *bus = gst_bus_new ();
    PushData primary = {test.primary, new_nv12_buffer (1920, 1080), GST_FLOW_ERROR};
    GThread *thread;
    GstMessage *message;
    GError *error = NULL;
    guint64 dropped;
    guint64 fallback;

    gst_element_set_bus (test.output->element, bus);
    fail_overlay_allocation = i == 1;
    thread = g_thread_new ("primary-failure", push_buffer, &primary);
    gst_harness_push (test.secondary, new_bgra_buffer (1920, 1080));
    g_thread_join (thread);
    message = pop_error (bus);
    gst_message_parse_error (message, &error, NULL);
    fail_unless (strstr (error->message, i == 0 ? "overlay scale" :
            "scaled BGRA overlay DMA-BUF") != NULL);
    fail_unless_equals_int (fake.composite_calls, 0);
    fail_unless (gst_harness_try_pull (test.output) == NULL);
    g_object_get (test.output->element, "conversion-dropped-frames", &dropped,
        "conversion-fallback-frames", &fallback, NULL);
    fail_unless_equals_uint64 (dropped, 1);
    fail_unless_equals_uint64 (fallback, 0);
    g_clear_error (&error);
    gst_message_unref (message);
    gst_object_unref (bus);
    fail_overlay_allocation = FALSE;
    test_harness_clear (&test);
  }
}
GST_END_TEST;

GST_START_TEST (test_zorder_controls_blend_direction_and_pad_alpha)
{
  FakeRga fake = {
    .available = TRUE,
    .process_result = IM_STATUS_SUCCESS,
    .composite_result = IM_STATUS_SUCCESS,
  };
  TestHarness test = test_harness_new (&fake, TRUE);
  GstPad *primary = gst_element_get_static_pad (test.output->element,
      "sink_0");
  GstPad *secondary = gst_element_get_static_pad (test.output->element,
      "sink_1");
  GstBuffer *output;

  g_object_set (primary, "alpha", 0.25, "zorder", 2, NULL);
  g_object_set (secondary, "alpha", 0.75, "zorder", 1, NULL);
  output = push_pair_and_pull (&test);

  fail_unless_equals_int (fake.process_calls, 2);
  fail_unless_equals_int (fake.composite_calls, 1);
  fail_unless ((fake.last_composite.transform.usage &
          IM_ALPHA_BLEND_SRC_OVER) != 0);
  fail_unless ((fake.last_composite.transform.usage &
          IM_ALPHA_BLEND_DST_OVER) == 0);
  fail_unless_equals_int (fake.last_composite.src_alpha, 64);
  fail_unless_equals_int (fake.last_composite.pat_alpha, 191);

  gst_buffer_unref (output);
  gst_object_unref (secondary);
  gst_object_unref (primary);
  test_harness_clear (&test);
}
GST_END_TEST;

static void
assert_input_allocation (TestHarness * test, const gchar * pad_name,
    const gchar * caps_string, guint minimum_size)
{
  GstAggregatorClass *klass = GST_AGGREGATOR_GET_CLASS
      (test->output->element);
  GstPad *pad = gst_element_get_static_pad (test->output->element, pad_name);
  GstCaps *caps = gst_caps_from_string (caps_string);
  GstQuery *query = gst_query_new_allocation (caps, TRUE);
  GstBufferPool *pool;
  GstStructure *config;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;
  guint size;
  guint min;
  guint max;
  guint meta_index;

  fail_unless (pad != NULL);
  fail_unless (caps != NULL);
  fail_unless (klass->propose_allocation (GST_AGGREGATOR
          (test->output->element), GST_AGGREGATOR_PAD (pad), NULL, query));
  fail_unless (gst_query_get_n_allocation_pools (query) > 0);
  fail_unless (gst_query_get_n_allocation_params (query) > 0);
  fail_unless (gst_query_find_allocation_meta (query,
          GST_VIDEO_META_API_TYPE, &meta_index));

  gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
  fail_unless (pool != NULL);
  fail_unless (size > minimum_size);
  config = gst_buffer_pool_get_config (pool);
  fail_unless (gst_buffer_pool_config_has_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_META));
  fail_unless (gst_buffer_pool_config_has_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT));
  gst_buffer_pool_config_get_allocator (config, &allocator, &params);
  fail_unless (allocator == test->allocator);
  gst_structure_free (config);

  gst_query_unref (query);
  gst_caps_unref (caps);
  gst_object_unref (pad);
}

GST_START_TEST (test_both_inputs_propose_shared_dmabuf_allocator)
{
  FakeRga fake = {
    .available = TRUE,
    .process_result = IM_STATUS_SUCCESS,
    .composite_result = IM_STATUS_SUCCESS,
  };
  TestHarness test = test_harness_new (&fake, TRUE);

  assert_input_allocation (&test, "sink_0",
      "video/x-raw(memory:DMABuf),format=NV12,width=1918,height=1078,framerate=30/1,interlace-mode=progressive",
      1918 * 1078 * 3 / 2);
  assert_input_allocation (&test, "sink_1",
      "video/x-raw(memory:DMABuf),format=BGRA,width=1919,height=1079,framerate=30/1,interlace-mode=progressive",
      1919 * 1079 * 4);

  test_harness_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_missing_secondary_passthroughs_primary_without_rga)
{
  FakeRga fake = {
    .available = TRUE,
    .process_result = IM_STATUS_SUCCESS,
    .composite_result = IM_STATUS_SUCCESS,
  };
  TestHarness test = test_harness_new (&fake, FALSE);
  GstBuffer *input = new_nv12_buffer (1920, 1080);
  GstBuffer *expected = gst_buffer_ref (input);
  GstBuffer *output;
  guint64 fallback = G_MAXUINT64;
  guint64 dropped = G_MAXUINT64;
  guint64 rejected = G_MAXUINT64;

  fail_unless_equals_int (gst_harness_push (test.primary, input), GST_FLOW_OK);
  output = gst_harness_pull (test.output);
  fail_unless (output == expected);
  fail_unless_equals_int (fake.process_calls, 0);
  fail_unless_equals_int (fake.composite_calls, 0);
  g_object_get (test.output->element, "conversion-fallback-frames", &fallback,
      "conversion-dropped-frames", &dropped, "layout-rejections", &rejected,
      NULL);
  fail_unless_equals_uint64 (fallback, 0);
  fail_unless_equals_uint64 (dropped, 0);
  fail_unless_equals_uint64 (rejected, 0);

  gst_buffer_unref (output);
  gst_buffer_unref (expected);
  test_harness_clear (&test);
}
GST_END_TEST;

GST_START_TEST (test_unavailable_backend_fails_ready_but_factory_remains)
{
  FakeRga fake = {
    .process_result = IM_STATUS_SUCCESS,
    .composite_result = IM_STATUS_SUCCESS,
  };
  GstMppRgaBackend *backend = gst_mpp_rga_backend_new (&fake_ops, &fake);
  GstElementFactory *factory = gst_element_factory_find ("rgacompositor");
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstElement *compositor;
  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *message;
  GstElementFactory *registered_factory;
  GError *error = NULL;
  gchar *debug = NULL;

  if (!factory) {
    fail_unless (gst_element_register (NULL, "rgacompositor", GST_RANK_NONE,
            GST_TYPE_RGA_COMPOSITOR));
    factory = gst_element_factory_find ("rgacompositor");
  }
  fail_unless (factory != NULL);
  fail_unless_equals_int (gst_plugin_feature_get_rank
      (GST_PLUGIN_FEATURE (factory)), GST_RANK_NONE);
  compositor = gst_element_factory_create (factory, NULL);
  fail_unless (GST_IS_RGA_COMPOSITOR (compositor));
  gst_rga_compositor_set_backend_for_test (GST_RGA_COMPOSITOR (compositor),
      backend);
  gst_bin_add (GST_BIN (pipeline), compositor);

  fail_unless_equals_int (gst_element_set_state (pipeline, GST_STATE_READY),
      GST_STATE_CHANGE_FAILURE);
  message = pop_error (bus);
  gst_message_parse_error (message, &error, &debug);
  fail_unless_equals_int (error->domain, GST_RESOURCE_ERROR);
  fail_unless_equals_int (error->code, GST_RESOURCE_ERROR_NOT_FOUND);
  fail_unless (g_strstr_len (error->message, -1,
          "driver-probed RGA backend") != NULL);
  registered_factory = gst_element_factory_find ("rgacompositor");
  fail_unless (registered_factory != NULL);
  gst_object_unref (registered_factory);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_clear_error (&error);
  g_free (debug);
  gst_message_unref (message);
  gst_object_unref (bus);
  gst_object_unref (pipeline);
  gst_object_unref (factory);
  gst_mpp_rga_backend_free (backend);
}
GST_END_TEST;

GST_START_TEST (test_blend_colorimetry_reaches_improcess)
{
  const struct
  {
    const gchar *color;
    gint mode;
  } cases[] = {
    {"bt601", IM_YUV_TO_RGB_BT601_LIMIT | IM_RGB_TO_YUV_BT601_LIMIT},
    {"bt709", IM_YUV_TO_RGB_BT709_LIMIT | IM_RGB_TO_YUV_BT709_LIMIT},
    {"1:4:16:4", IM_YUV_TO_RGB_BT601_FULL | IM_RGB_TO_YUV_BT601_FULL},
    {NULL, IM_YUV_TO_RGB_BT709_LIMIT | IM_RGB_TO_YUV_BT709_LIMIT},
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++) {
    FakeRga fake = {.available = TRUE, .submit_im2d = TRUE};
    TestHarness test = test_harness_new_with_color (&fake, TRUE, cases[i].color);
    GstBuffer *output;

    submitted_calls = 0;
    output = push_pair_and_pull (&test);
    fail_unless_equals_int (submitted_calls, 3);
    fail_unless_equals_int (submitted_dst[0].color_space_mode, 0);
    fail_unless_equals_int (submitted_dst[1].color_space_mode, 0);
    fail_unless_equals_int (submitted_dst[2].color_space_mode, cases[i].mode);
    gst_buffer_unref (output);
    test_harness_clear (&test);
  }
}
GST_END_TEST;

GST_START_TEST (test_blend_rejects_unrepresentable_colorimetry)
{
  GstMppRgaIm2dCompositeRequest request = { 0, };
  GstVideoInfo accumulator;
  GstVideoInfo overlay;

  gst_video_info_set_format (&accumulator, GST_VIDEO_FORMAT_NV12, 1920, 1080);
  gst_video_info_set_format (&overlay, GST_VIDEO_FORMAT_BGRA, 1920, 1080);
  accumulator.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;
  fail_if (gst_mpp_rga_composite_set_colorimetry (&request, &accumulator,
          &overlay));
  accumulator.colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
  accumulator.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
  fail_if (gst_mpp_rga_composite_set_colorimetry (&request, &accumulator,
          &overlay));
  accumulator.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
  overlay.colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
  fail_if (gst_mpp_rga_composite_set_colorimetry (&request, &accumulator,
          &overlay));
}
GST_END_TEST;

static void
capture_im2d_diagnostic (GstDebugCategory * category, GstDebugLevel level,
    const gchar * file, const gchar * function, gint line, GObject * object,
    GstDebugMessage * message, gpointer user_data)
{
  (void) category;
  (void) level;
  (void) file;
  (void) function;
  (void) line;
  (void) object;
  g_string_append (user_data, gst_debug_message_get (message));
  errno = EIO;
}

GST_START_TEST (test_im2d_diagnostics_preserve_status_errno_and_failure_stage)
{
  GstMppRgaIm2dCompositeRequest request = { 0, };
  GString *log = g_string_new (NULL);
  guint i;
  gint saved_errno;
  gint status;

  GST_DEBUG_CATEGORY_INIT (mpp_rga_backend_debug, "mpprgabackend", 0,
      "MPP RGA backend");
  gst_debug_category_set_threshold (mpp_rga_backend_debug, GST_LEVEL_WARNING);
  gst_debug_add_log_function (capture_im2d_diagnostic, log, NULL);
  for (i = 0; i < 3; i++) {
    g_string_truncate (log, 0);
    submitted_calls = 0;
    config_status = i == 0 ? IM_STATUS_SUCCESS : IM_STATUS_INVALID_PARAM;
    process_status = IM_STATUS_NOT_SUPPORTED;
    injected_errno = i == 0 ? 0 : ENODEV;
    request.transform.core_mask = i == 2 ? 1 : 0;
    errno = EBUSY;
    status = gst_mpp_rga_real_composite (&request, NULL);
    saved_errno = errno;
    fail_unless_equals_int (status,
        i == 0 ? IM_STATUS_NOT_SUPPORTED : IM_STATUS_INVALID_PARAM);
    fail_unless_equals_int (saved_errno, injected_errno);
    fail_unless_equals_int (submitted_calls, i == 0 ? 1 : 0);
    fail_unless (strstr (log->str, i == 0 ?
            "improcess composite returned status=-1 errno=0" :
            i == 1 ? "imconfig priority=" : "imconfig scheduler-core=") != NULL);
    if (i == 0)
      fail_unless (strstr (log->str, "pat={fd=") != NULL);
    else
      fail_unless (strstr (log->str, "errno=19") != NULL);
  }
  gst_debug_remove_log_function (capture_im2d_diagnostic);
  g_string_free (log, TRUE);
  config_status = process_status = IM_STATUS_SUCCESS;
  injected_errno = 0;
}
GST_END_TEST;

static Suite *
rgacompositor_suite (void)
{
  Suite *suite = suite_create ("rgacompositor");
  TCase *test_case = tcase_create ("element");

  tcase_set_timeout (test_case, 30);
  tcase_add_test (test_case, test_pad_factory_and_property_contract);
  tcase_add_test (test_case, test_blend_colorimetry_reaches_improcess);
  tcase_add_test (test_case, test_blend_rejects_unrepresentable_colorimetry);
  tcase_add_test (test_case,
      test_im2d_diagnostics_preserve_status_errno_and_failure_stage);
  tcase_add_test (test_case,
      test_named_and_custom_layouts_obey_pat_no_scaling_contract);
  tcase_add_test (test_case, test_overlay_pool_reused_resized_and_released_on_stop);
  tcase_add_test (test_case, test_already_sized_overlay_does_not_scale_or_allocate);
  tcase_add_test (test_case, test_overlay_preparation_failure_never_blends_or_outputs);
  tcase_add_test (test_case,
      test_zorder_controls_blend_direction_and_pad_alpha);
  tcase_add_test (test_case,
      test_both_inputs_propose_shared_dmabuf_allocator);
  tcase_add_test (test_case,
      test_missing_secondary_passthroughs_primary_without_rga);
  tcase_add_test (test_case,
      test_unavailable_backend_fails_ready_but_factory_remains);
  suite_add_tcase (suite, test_case);
  return suite;
}

GST_CHECK_MAIN (rgacompositor);
