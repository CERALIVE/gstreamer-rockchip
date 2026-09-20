#define _GNU_SOURCE

#include <fcntl.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct
{
  gboolean seen;
  gboolean dmabuf;
} OutputCheck;

static int
allocate_dmabuf (gsize size)
{
  static const char *const heaps[] = {
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/system",
  };
  struct dma_heap_allocation_data allocation = { 0, };

  allocation.len = size;
  allocation.fd_flags = O_RDWR | O_CLOEXEC;
  for (guint i = 0; i < G_N_ELEMENTS (heaps); i++) {
    int heap_fd = open (heaps[i], O_RDWR | O_CLOEXEC);

    if (heap_fd < 0)
      continue;
    if (ioctl (heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation) == 0) {
      close (heap_fd);
      g_print ("INPUT_HEAP=%s\n", heaps[i]);
      return allocation.fd;
    }
    close (heap_fd);
  }
  return -1;
}

static gboolean
load_input (const char *path, void *mapping, gsize size)
{
  FILE *file = fopen (path, "rb");
  gboolean complete;

  if (!file)
    return FALSE;
  complete = fread (mapping, 1, size, file) == size && fgetc (file) == EOF;
  fclose (file);
  return complete;
}

static GstCaps *
video_caps (const char *format, guint width, guint height)
{
  GstCaps *caps = gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, format,
      "width", G_TYPE_INT, width,
      "height", G_TYPE_INT, height,
      "framerate", GST_TYPE_FRACTION, 1, 1, NULL);

  gst_caps_set_simple (caps, "colorimetry", G_TYPE_STRING,
      strcmp (format, "BGR") == 0 ? "sRGB" : "bt709", NULL);

  gst_caps_set_features (caps, 0,
      gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_DMABUF, NULL));
  return caps;
}

static void
on_handoff (GstElement * identity, GstBuffer * buffer, gpointer user_data)
{
  OutputCheck *check = user_data;
  GstMemory *memory = gst_buffer_n_memory (buffer) == 1 ?
      gst_buffer_peek_memory (buffer, 0) : NULL;

  (void) identity;
  check->seen = TRUE;
  check->dmabuf = memory && gst_is_dmabuf_memory (memory);
}

static gboolean
configure_operation (GstElement * dut, const char *operation)
{
  if (strcmp (operation, "csc") == 0 || strcmp (operation, "scale") == 0)
    return TRUE;
  if (strcmp (operation, "crop") == 0) {
    g_object_set (dut, "crop-x", 64u, "crop-y", 48u,
        "crop-w", 1024u, "crop-h", 576u, NULL);
    return TRUE;
  }
  if (strcmp (operation, "rotate") == 0) {
    g_object_set (dut, "rotation", 90, NULL);
    return TRUE;
  }
  return FALSE;
}

static gboolean
report_bus_result (GstElement * pipeline)
{
  GstBus *bus = gst_element_get_bus (pipeline);
  GstMessage *message = gst_bus_timed_pop_filtered (bus, 30 * GST_SECOND,
      GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  gboolean passed = message && GST_MESSAGE_TYPE (message) == GST_MESSAGE_EOS;

  if (message && GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
    GError *error = NULL;
    gchar *debug = NULL;

    gst_message_parse_error (message, &error, &debug);
    g_printerr ("PIPELINE_ERROR=%s DEBUG=%s\n", error->message,
        debug ? debug : "none");
    g_clear_error (&error);
    g_free (debug);
  } else if (!message) {
    g_printerr ("PIPELINE_ERROR=timeout\n");
  }
  if (message)
    gst_message_unref (message);
  gst_object_unref (bus);
  return passed;
}

int
main (int argc, char **argv)
{
  const guint input_width = 1280;
  const guint input_height = 720;
  const char *input_path;
  const char *input_format;
  const char *output_path;
  const char *output_format;
  const char *operation;
  guint output_width;
  guint output_height;
  GstVideoFormat gst_input_format;
  GstVideoInfo input_info;
  GstElement *pipeline;
  GstElement *source;
  GstElement *dut;
  GstElement *filter;
  GstElement *identity;
  GstElement *sink;
  GstCaps *input_caps;
  GstCaps *output_caps;
  GstAllocator *allocator;
  GstBuffer *buffer;
  GstMemory *memory;
  OutputCheck output_check = { 0, };
  guint64 fallback = G_MAXUINT64;
  guint64 dropped = G_MAXUINT64;
  guint64 rejected = G_MAXUINT64;
  guint64 csc_fallback = G_MAXUINT64;
  void *mapping;
  int dmabuf_fd;
  gboolean passed;

  if (argc != 8) {
    g_printerr ("usage: %s INPUT IN_FORMAT OUTPUT OUT_FORMAT OUT_W OUT_H OP\n",
        argv[0]);
    return 2;
  }
  input_path = argv[1];
  input_format = argv[2];
  output_path = argv[3];
  output_format = argv[4];
  output_width = (guint) g_ascii_strtoull (argv[5], NULL, 10);
  output_height = (guint) g_ascii_strtoull (argv[6], NULL, 10);
  operation = argv[7];

  gst_init (&argc, &argv);
  gst_input_format = gst_video_format_from_string (input_format);
  if (gst_input_format == GST_VIDEO_FORMAT_UNKNOWN || output_width == 0 ||
      output_height == 0)
    return 2;
  gst_video_info_set_format (&input_info, gst_input_format, input_width,
      input_height);

  dmabuf_fd = allocate_dmabuf (GST_VIDEO_INFO_SIZE (&input_info));
  if (dmabuf_fd < 0)
    return 1;
  mapping = mmap (NULL, GST_VIDEO_INFO_SIZE (&input_info),
      PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
  if (mapping == MAP_FAILED ||
      !load_input (input_path, mapping, GST_VIDEO_INFO_SIZE (&input_info))) {
    if (mapping != MAP_FAILED)
      munmap (mapping, GST_VIDEO_INFO_SIZE (&input_info));
    close (dmabuf_fd);
    return 1;
  }
  munmap (mapping, GST_VIDEO_INFO_SIZE (&input_info));

  pipeline = gst_pipeline_new ("dmabuf-rgaconvert");
  source = gst_element_factory_make ("appsrc", "source");
  dut = gst_element_factory_make ("rgaconvert", "dut");
  filter = gst_element_factory_make ("capsfilter", "output-caps");
  identity = gst_element_factory_make ("identity", "output-check");
  sink = gst_element_factory_make ("filesink", "sink");
  if (!pipeline || !source || !dut || !filter || !identity || !sink ||
      !configure_operation (dut, operation))
    return 1;

  input_caps = video_caps (input_format, input_width, input_height);
  output_caps = video_caps (output_format, output_width, output_height);
  g_object_set (source, "caps", input_caps, "format", GST_FORMAT_TIME, NULL);
  g_object_set (filter, "caps", output_caps, NULL);
  g_object_set (identity, "signal-handoffs", TRUE, "silent", TRUE, NULL);
  g_object_set (sink, "location", output_path, NULL);
  g_signal_connect (identity, "handoff", G_CALLBACK (on_handoff), &output_check);
  gst_caps_unref (input_caps);
  gst_caps_unref (output_caps);

  gst_bin_add_many (GST_BIN (pipeline), source, dut, filter, identity, sink, NULL);
  if (!gst_element_link_many (source, dut, filter, identity, sink, NULL))
    return 1;
  if (gst_element_set_state (pipeline,
          GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    return 1;

  allocator = gst_dmabuf_allocator_new ();
  memory = gst_dmabuf_allocator_alloc (allocator, dmabuf_fd,
      GST_VIDEO_INFO_SIZE (&input_info));
  gst_object_unref (allocator);
  buffer = gst_buffer_new ();
  gst_buffer_append_memory (buffer, memory);
  gst_buffer_add_video_meta_full (buffer, GST_VIDEO_FRAME_FLAG_NONE,
      gst_input_format, input_width, input_height,
      GST_VIDEO_INFO_N_PLANES (&input_info), input_info.offset,
      input_info.stride);
  GST_BUFFER_PTS (buffer) = 0;
  GST_BUFFER_DURATION (buffer) = GST_SECOND;
  g_print ("INPUT_DMABUF=%u INPUT_MEMORIES=%u\n",
      gst_is_dmabuf_memory (memory), gst_buffer_n_memory (buffer));

  passed = gst_app_src_push_buffer (GST_APP_SRC (source), buffer) == GST_FLOW_OK;
  passed = passed && gst_app_src_end_of_stream (GST_APP_SRC (source)) ==
      GST_FLOW_OK && report_bus_result (pipeline);
  g_object_get (dut, "conversion-fallback-frames", &fallback,
      "conversion-dropped-frames", &dropped,
      "layout-rejections", &rejected, "csc-fallback-frames", &csc_fallback, NULL);
  g_print ("DUT_CSC_FALLBACK=%" G_GUINT64_FORMAT "\n", csc_fallback);
  g_print ("OUTPUT_SEEN=%u OUTPUT_DMABUF=%u\n", output_check.seen,
      output_check.dmabuf);
  g_print ("DUT_FALLBACK=%" G_GUINT64_FORMAT
      " DUT_DROPPED=%" G_GUINT64_FORMAT
      " DUT_LAYOUT_REJECTIONS=%" G_GUINT64_FORMAT "\n",
      fallback, dropped, rejected);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);

  return passed && output_check.seen && output_check.dmabuf && fallback == 0 &&
      dropped == 0 && rejected == 0 && csc_fallback == 0 ? 0 : 1;
}
