/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/gstvideometa.h>
#include <rga/im2d.h>
#include <string.h>

#include "gstrgacompositor.h"
#include "gstrgautil.h"
#include "gstmppconversionstats.h"

#define GST_RGA_COMPOSITOR_SINK_CAPS \
  GST_VIDEO_CAPS_MAKE_WITH_FEATURES (GST_CAPS_FEATURE_MEMORY_DMABUF, \
      "{ NV12, BGRA }") ", interlace-mode = (string) progressive"
#define GST_RGA_COMPOSITOR_SRC_CAPS \
  GST_VIDEO_CAPS_MAKE_WITH_FEATURES (GST_CAPS_FEATURE_MEMORY_DMABUF, \
      "{ NV12 }") ", interlace-mode = (string) progressive"

GST_DEBUG_CATEGORY_STATIC (rga_compositor_debug);
#define GST_CAT_DEFAULT rga_compositor_debug

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK, GST_PAD_REQUEST,
    GST_STATIC_CAPS (GST_RGA_COMPOSITOR_SINK_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_RGA_COMPOSITOR_SRC_CAPS));

struct _GstRgaCompositorPad
{
  GstVideoAggregatorPad parent;
  gint xpos;
  gint ypos;
  gint width;
  gint height;
  gdouble alpha;
};

struct _GstRgaCompositor
{
  GstVideoAggregator parent;
  GMutex lock;
  GstMppRgaBackend *backend;
  GstAllocator *allocator;
  GstBufferPool *overlay_pool;
  GstVideoInfo overlay_info;
  GstRgaCompositorLayout layout;
};

typedef struct
{
  gint x;
  gint y;
  gint width;
  gint height;
} GstRgaCompositorRectangle;

typedef struct
{
  gint fd;
  GstVideoFormat format;
  RgaSURF_FORMAT rga_format;
  guint width;
  guint height;
  guint wstride;
  guint hstride;
} GstRgaCompositorFrame;

typedef struct
{
  gint xpos;
  gint ypos;
  gint width;
  gint height;
  gdouble alpha;
  guint zorder;
} GstRgaCompositorPadConfig;

typedef struct
{
  GstRgaCompositorPad *pad;
  GstBuffer *buffer;
  GstVideoInfo info;
  GstRgaCompositorPadConfig config;
  guint index;
} GstRgaCompositorInput;

enum
{
  PROP_PAD_0,
  PROP_PAD_XPOS,
  PROP_PAD_YPOS,
  PROP_PAD_WIDTH,
  PROP_PAD_HEIGHT,
  PROP_PAD_ALPHA,
};

enum
{
  PROP_0,
  PROP_LAYOUT,
  PROP_CONVERSION_FALLBACK_FRAMES,
  PROP_CONVERSION_DROPPED_FRAMES,
  PROP_LAYOUT_REJECTIONS,
  PROP_CSC_FALLBACK_FRAMES,
};

static void gst_rga_compositor_child_proxy_init (gpointer g_iface,
    gpointer iface_data);
static GstAllocator *gst_rga_compositor_get_allocator (GstRgaCompositor * self);

G_DEFINE_TYPE (GstRgaCompositorPad, gst_rga_compositor_pad,
    GST_TYPE_VIDEO_AGGREGATOR_PAD);
G_DEFINE_TYPE_WITH_CODE (GstRgaCompositor, gst_rga_compositor,
    GST_TYPE_VIDEO_AGGREGATOR,
    G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY,
        gst_rga_compositor_child_proxy_init));

GType
gst_rga_compositor_layout_get_type (void)
{
  static gsize type = 0;
  static const GEnumValue values[] = {
    {GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_RIGHT, "PiP top right",
        "pip-top-right"},
    {GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_LEFT, "PiP top left",
        "pip-top-left"},
    {GST_RGA_COMPOSITOR_LAYOUT_PIP_BOTTOM_RIGHT, "PiP bottom right",
        "pip-bottom-right"},
    {GST_RGA_COMPOSITOR_LAYOUT_PIP_BOTTOM_LEFT, "PiP bottom left",
        "pip-bottom-left"},
    {GST_RGA_COMPOSITOR_LAYOUT_PBP_LEFT_RIGHT, "PbP left to right",
        "pbp-left-right"},
    {GST_RGA_COMPOSITOR_LAYOUT_PBP_TOP_BOTTOM, "PbP top to bottom",
        "pbp-top-bottom"},
    {GST_RGA_COMPOSITOR_LAYOUT_CUSTOM, "Custom pad rectangles", "custom"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType registered = g_enum_register_static ("GstRgaCompositorLayout",
        values);
    g_once_init_leave (&type, registered);
  }
  return type;
}

static void
gst_rga_compositor_pad_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRgaCompositorPad *pad = GST_RGA_COMPOSITOR_PAD (object);

  GST_OBJECT_LOCK (pad);
  switch (prop_id) {
    case PROP_PAD_XPOS:
      g_value_set_int (value, pad->xpos);
      break;
    case PROP_PAD_YPOS:
      g_value_set_int (value, pad->ypos);
      break;
    case PROP_PAD_WIDTH:
      g_value_set_int (value, pad->width);
      break;
    case PROP_PAD_HEIGHT:
      g_value_set_int (value, pad->height);
      break;
    case PROP_PAD_ALPHA:
      g_value_set_double (value, pad->alpha);
      break;
    default:
      GST_OBJECT_UNLOCK (pad);
      G_OBJECT_CLASS (gst_rga_compositor_pad_parent_class)->get_property
          (object, prop_id, value, pspec);
      return;
  }
  GST_OBJECT_UNLOCK (pad);
}

static void
gst_rga_compositor_pad_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRgaCompositorPad *pad = GST_RGA_COMPOSITOR_PAD (object);

  GST_OBJECT_LOCK (pad);
  switch (prop_id) {
    case PROP_PAD_XPOS:
      pad->xpos = g_value_get_int (value);
      break;
    case PROP_PAD_YPOS:
      pad->ypos = g_value_get_int (value);
      break;
    case PROP_PAD_WIDTH:
      pad->width = g_value_get_int (value);
      break;
    case PROP_PAD_HEIGHT:
      pad->height = g_value_get_int (value);
      break;
    case PROP_PAD_ALPHA:
      pad->alpha = g_value_get_double (value);
      break;
    default:
      GST_OBJECT_UNLOCK (pad);
      G_OBJECT_CLASS (gst_rga_compositor_pad_parent_class)->set_property
          (object, prop_id, value, pspec);
      return;
  }
  GST_OBJECT_UNLOCK (pad);
}

static void
gst_rga_compositor_pad_init (GstRgaCompositorPad * pad)
{
  pad->alpha = 1.0;
}

static void
gst_rga_compositor_pad_class_init (GstRgaCompositorPadClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstVideoAggregatorPadClass *video_pad_class =
      GST_VIDEO_AGGREGATOR_PAD_CLASS (klass);

  gobject_class->get_property = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_pad_get_property);
  gobject_class->set_property = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_pad_set_property);

  g_object_class_install_property (gobject_class, PROP_PAD_XPOS,
      g_param_spec_int ("xpos", "X position",
          "Custom-layout left edge in output pixels", 0, G_MAXINT, 0,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PAD_YPOS,
      g_param_spec_int ("ypos", "Y position",
          "Custom-layout top edge in output pixels", 0, G_MAXINT, 0,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PAD_WIDTH,
      g_param_spec_int ("width", "Width",
          "Custom-layout width in output pixels", 0, G_MAXINT, 0,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PAD_HEIGHT,
      g_param_spec_int ("height", "Height",
          "Custom-layout height in output pixels", 0, G_MAXINT, 0,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PAD_ALPHA,
      g_param_spec_double ("alpha", "Alpha", "Global input alpha", 0.0,
          1.0, 1.0, G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
          G_PARAM_STATIC_STRINGS));

  video_pad_class->prepare_frame = NULL;
  video_pad_class->clean_frame = NULL;
}

static gboolean
gst_rga_compositor_name_index (const gchar * name, guint * index)
{
  gchar *end = NULL;
  guint64 parsed;

  if (!g_str_has_prefix (name, "sink_"))
    return FALSE;
  parsed = g_ascii_strtoull (name + 5, &end, 10);
  if (!end || *end != '\0' || parsed > G_MAXUINT)
    return FALSE;
  *index = (guint) parsed;
  return TRUE;
}

static gboolean
gst_rga_compositor_pad_index (GstPad * pad, guint * index)
{
  return gst_rga_compositor_name_index (GST_PAD_NAME (pad), index);
}

static GstCaps *
gst_rga_compositor_pad_caps (GstPad * pad)
{
  GstCaps *caps = gst_pad_get_pad_template_caps (pad);
  guint index;
  guint i;

  if (!gst_rga_compositor_pad_index (pad, &index) || index > 1)
    return caps;
  caps = gst_caps_make_writable (caps);
  for (i = 0; i < gst_caps_get_size (caps); i++)
    gst_structure_set (gst_caps_get_structure (caps, i), "format",
        G_TYPE_STRING, index == 0 ? "NV12" : "BGRA", NULL);
  return caps;
}

static gboolean
gst_rga_compositor_sink_query (GstAggregator * aggregator,
    GstAggregatorPad * aggregator_pad, GstQuery * query)
{
  GstCaps *caps;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter = NULL;
      GstCaps *result;

      caps = gst_rga_compositor_pad_caps (GST_PAD (aggregator_pad));
      gst_query_parse_caps (query, &filter);
      result = filter ? gst_caps_intersect_full (filter, caps,
          GST_CAPS_INTERSECT_FIRST) : gst_caps_ref (caps);
      gst_query_set_caps_result (query, result);
      gst_caps_unref (result);
      gst_caps_unref (caps);
      return TRUE;
    }
    case GST_QUERY_ACCEPT_CAPS:{
      GstCaps *candidate;
      gboolean accepted;

      gst_query_parse_accept_caps (query, &candidate);
      caps = gst_rga_compositor_pad_caps (GST_PAD (aggregator_pad));
      accepted = gst_caps_is_fixed (candidate) &&
          gst_caps_is_subset (candidate, caps);
      gst_query_set_accept_caps_result (query, accepted);
      gst_caps_unref (caps);
      return TRUE;
    }
    default:
      return GST_AGGREGATOR_CLASS
          (gst_rga_compositor_parent_class)->sink_query (aggregator,
          aggregator_pad, query);
  }
}

static void
gst_rga_compositor_input_clear (GstRgaCompositorInput * input)
{
  gst_clear_buffer (&input->buffer);
  gst_clear_object (&input->pad);
}

static void
gst_rga_compositor_input_snapshot (GstRgaCompositorInput * input)
{
  GstVideoAggregatorPad *video_pad = GST_VIDEO_AGGREGATOR_PAD (input->pad);

  GST_OBJECT_LOCK (input->pad);
  input->info = video_pad->info;
  GST_OBJECT_UNLOCK (input->pad);
  g_object_get (input->pad, "xpos", &input->config.xpos,
      "ypos", &input->config.ypos, "width", &input->config.width,
      "height", &input->config.height, "alpha", &input->config.alpha,
      "zorder", &input->config.zorder, NULL);
}

static guint
gst_rga_compositor_collect_inputs (GstVideoAggregator * videoaggregator,
    GstRgaCompositorInput inputs[2])
{
  GList *node;
  guint count = 0;
  guint i;

  GST_OBJECT_LOCK (videoaggregator);
  for (node = GST_ELEMENT (videoaggregator)->sinkpads; node; node = node->next) {
    GstVideoAggregatorPad *video_pad = GST_VIDEO_AGGREGATOR_PAD (node->data);
    GstBuffer *buffer = gst_video_aggregator_pad_get_current_buffer (video_pad);
    guint index;

    if (!buffer || (gst_buffer_get_size (buffer) == 0 &&
            GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_GAP)) ||
        !gst_rga_compositor_pad_index (GST_PAD (video_pad), &index) ||
        index > 1)
      continue;
    inputs[index].pad = gst_object_ref (video_pad);
    inputs[index].buffer = gst_buffer_ref (buffer);
    inputs[index].index = index;
    count++;
  }
  GST_OBJECT_UNLOCK (videoaggregator);

  for (i = 0; i < 2; i++)
    if (inputs[i].buffer)
      gst_rga_compositor_input_snapshot (&inputs[i]);
  return count;
}

static gboolean
gst_rga_compositor_frame_from_buffer (GstBuffer * buffer,
    const GstVideoInfo * info, GstVideoFormat expected_format,
    GstRgaCompositorFrame * frame, const gchar ** reason)
{
  GstVideoMeta *meta = gst_buffer_get_video_meta (buffer);
  GstMemory *memory;
  gsize offset[GST_VIDEO_MAX_PLANES] = { 0, };
  gint stride[GST_VIDEO_MAX_PLANES] = { 0, };
  gsize memory_offset;
  gsize chroma_rows;
  gsize required_size;
  guint expected_planes;

  if (!info->finfo || GST_VIDEO_INFO_FORMAT (info) != expected_format) {
    *reason = expected_format == GST_VIDEO_FORMAT_NV12 ?
        "sink_0 and output must negotiate NV12" :
        "sink_1 must negotiate BGRA for NV12-output composition";
    return FALSE;
  }

  frame->format = expected_format;
  frame->rga_format = expected_format == GST_VIDEO_FORMAT_NV12 ?
      RK_FORMAT_YCbCr_420_SP : RK_FORMAT_BGRA_8888;
  frame->width = GST_VIDEO_INFO_WIDTH (info);
  frame->height = GST_VIDEO_INFO_HEIGHT (info);
  expected_planes = expected_format == GST_VIDEO_FORMAT_NV12 ? 2 : 1;
  if (meta) {
    if (meta->format != expected_format ||
        meta->width != frame->width || meta->height != frame->height ||
        meta->n_planes != expected_planes) {
      *reason = "GstVideoMeta does not match negotiated compositor caps";
      return FALSE;
    }
    offset[0] = meta->offset[0];
    stride[0] = meta->stride[0];
    if (expected_planes == 2) {
      offset[1] = meta->offset[1];
      stride[1] = meta->stride[1];
    }
  } else {
    offset[0] = GST_VIDEO_INFO_PLANE_OFFSET (info, 0);
    stride[0] = GST_VIDEO_INFO_PLANE_STRIDE (info, 0);
    if (expected_planes == 2) {
      offset[1] = GST_VIDEO_INFO_PLANE_OFFSET (info, 1);
      stride[1] = GST_VIDEO_INFO_PLANE_STRIDE (info, 1);
    }
  }

  if (frame->width == 0 || frame->height == 0 || offset[0] != 0 ||
      stride[0] <= 0) {
    *reason = "input dimensions, offset, or stride are not RGA-linear";
    return FALSE;
  }

  if (expected_format == GST_VIDEO_FORMAT_NV12) {
    if ((frame->width | frame->height) & 1 || stride[1] != stride[0] ||
        (guint) stride[0] < frame->width || offset[1] == 0 ||
        offset[1] % (guint) stride[0] != 0) {
      *reason = "NV12 dimensions, offsets, or strides are not RGA-linear";
      return FALSE;
    }
    frame->wstride = stride[0];
    frame->hstride = offset[1] / stride[0];
    if (frame->hstride < frame->height || frame->hstride & 1) {
      *reason = "NV12 vertical stride is invalid";
      return FALSE;
    }
    chroma_rows = frame->hstride / 2;
    if (chroma_rows > (G_MAXSIZE - offset[1]) / frame->wstride) {
      *reason = "NV12 layout size overflows";
      return FALSE;
    }
    required_size = offset[1] + frame->wstride * chroma_rows;
  } else {
    if (stride[0] % 4 != 0 || (guint) stride[0] / 4 < frame->width ||
        frame->height > G_MAXSIZE / (guint) stride[0]) {
      *reason = "BGRA byte stride or layout size is not RGA-linear";
      return FALSE;
    }
    frame->wstride = stride[0] / 4;
    frame->hstride = frame->height;
    required_size = (gsize) stride[0] * frame->height;
  }
  if (gst_buffer_get_size (buffer) < required_size) {
    *reason = "buffer is smaller than its negotiated stride layout";
    return FALSE;
  }

  if (gst_buffer_n_memory (buffer) != 1) {
    *reason = "RGA requires one DMA-BUF containing both NV12 planes";
    return FALSE;
  }
  memory = gst_buffer_peek_memory (buffer, 0);
  if (!gst_is_dmabuf_memory (memory)) {
    *reason = "buffer memory is not DMA-BUF";
    return FALSE;
  }
  gst_memory_get_sizes (memory, &memory_offset, NULL);
  if (memory_offset != 0) {
    *reason = "DMA-BUF memory has a non-zero base offset";
    return FALSE;
  }
  frame->fd = gst_dmabuf_memory_get_fd (memory);
  if (frame->fd < 0) {
    *reason = "DMA-BUF has no importable file descriptor";
    return FALSE;
  }
  return TRUE;
}

static guint
gst_rga_compositor_even_floor (guint value)
{
  return value & ~1U;
}

static gboolean
gst_rga_compositor_rectangle_valid (const GstRgaCompositorRectangle * rect,
    guint output_width, guint output_height, const gchar ** reason)
{
  if (rect->x < 0 || rect->y < 0 || rect->width <= 0 || rect->height <= 0 ||
      (guint64) rect->x + rect->width > output_width ||
      (guint64) rect->y + rect->height > output_height) {
    *reason = "layout rectangle falls outside the output frame";
    return FALSE;
  }
  if ((rect->x | rect->y | rect->width | rect->height) & 1) {
    *reason = "NV12 layout coordinates and dimensions must be even";
    return FALSE;
  }
  return TRUE;
}

static gboolean
gst_rga_compositor_calculate_rectangle (GstRgaCompositorLayout layout,
    const GstRgaCompositorInput * input, guint output_width,
    guint output_height, GstRgaCompositorRectangle * rect,
    const gchar ** reason)
{
  guint half_width = gst_rga_compositor_even_floor (output_width / 2);
  guint half_height = gst_rga_compositor_even_floor (output_height / 2);
  guint margin_x = gst_rga_compositor_even_floor (output_width / 20);
  guint margin_y = gst_rga_compositor_even_floor (output_height / 20);

  if (input->index > 1) {
    *reason = "v1 accepts only sink_0 and sink_1";
    return FALSE;
  }

  if (layout == GST_RGA_COMPOSITOR_LAYOUT_CUSTOM) {
    rect->x = input->config.xpos;
    rect->y = input->config.ypos;
    rect->width = input->config.width;
    rect->height = input->config.height;
    return gst_rga_compositor_rectangle_valid (rect, output_width,
        output_height, reason);
  }

  if (input->index == 0 &&
      layout <= GST_RGA_COMPOSITOR_LAYOUT_PIP_BOTTOM_LEFT) {
    rect->x = 0;
    rect->y = 0;
    rect->width = output_width;
    rect->height = output_height;
    return gst_rga_compositor_rectangle_valid (rect, output_width,
        output_height, reason);
  }

  /* PiP uses half of each output axis (one-quarter area) and a 5% margin. */
  switch (layout) {
    case GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_RIGHT:
      rect->x = output_width - margin_x - half_width;
      rect->y = margin_y;
      rect->width = half_width;
      rect->height = half_height;
      break;
    case GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_LEFT:
      rect->x = margin_x;
      rect->y = margin_y;
      rect->width = half_width;
      rect->height = half_height;
      break;
    case GST_RGA_COMPOSITOR_LAYOUT_PIP_BOTTOM_RIGHT:
      rect->x = output_width - margin_x - half_width;
      rect->y = output_height - margin_y - half_height;
      rect->width = half_width;
      rect->height = half_height;
      break;
    case GST_RGA_COMPOSITOR_LAYOUT_PIP_BOTTOM_LEFT:
      rect->x = margin_x;
      rect->y = output_height - margin_y - half_height;
      rect->width = half_width;
      rect->height = half_height;
      break;
    case GST_RGA_COMPOSITOR_LAYOUT_PBP_LEFT_RIGHT:
      rect->x = input->index == 0 ? 0 : half_width;
      rect->y = 0;
      rect->width = input->index == 0 ? half_width :
          output_width - half_width;
      rect->height = output_height;
      break;
    case GST_RGA_COMPOSITOR_LAYOUT_PBP_TOP_BOTTOM:
      rect->x = 0;
      rect->y = input->index == 0 ? 0 : half_height;
      rect->width = output_width;
      rect->height = input->index == 0 ? half_height :
          output_height - half_height;
      break;
    default:
      *reason = "unknown compositor layout";
      return FALSE;
  }
  return gst_rga_compositor_rectangle_valid (rect, output_width,
      output_height, reason);
}

static void
gst_rga_compositor_fill_request (GstMppRgaIm2dRequest * request,
    const GstRgaCompositorFrame * input,
    const GstRgaCompositorFrame * output,
    const GstRgaCompositorRectangle * rectangle)
{
  request->src_fd = input->fd;
  request->src_width = input->width;
  request->src_height = input->height;
  request->src_wstride = input->wstride;
  request->src_hstride = input->hstride;
  request->src_format = input->rga_format;
  request->dst_fd = output->fd;
  request->dst_width = output->width;
  request->dst_height = output->height;
  request->dst_wstride = output->wstride;
  request->dst_hstride = output->hstride;
  request->dst_format = output->rga_format;
  request->src_rect_width = input->width;
  request->src_rect_height = input->height;
  request->dst_x = rectangle->x;
  request->dst_y = rectangle->y;
  request->dst_rect_width = rectangle->width;
  request->dst_rect_height = rectangle->height;
}

static GstFlowReturn
gst_rga_compositor_refuse_frame (GstRgaCompositor * self,
    const gchar * reason, gboolean layout_rejection)
{
  GstMppConversionStats *stats = gst_mpp_conversion_stats_get (G_OBJECT (self));

  if (layout_rejection)
    gst_mpp_conversion_stats_layout_rejected (stats);
  gst_mpp_conversion_stats_dropped (stats);
  GST_ELEMENT_ERROR (self, CORE, NEGOTIATION,
      ("no 2D compositor available for rgacompositor: %s", reason), (NULL));
  return GST_FLOW_NOT_NEGOTIATED;
}

static void
gst_rga_compositor_clear_overlay_pool (GstRgaCompositor * self)
{
  if (self->overlay_pool)
    gst_buffer_pool_set_active (self->overlay_pool, FALSE);
  gst_clear_object (&self->overlay_pool);
}

static gboolean
gst_rga_compositor_acquire_overlay (GstRgaCompositor * self,
    const GstRgaCompositorRectangle * rectangle, GstBuffer ** buffer,
    GstRgaCompositorFrame * frame, const gchar ** reason)
{
  if (self->overlay_pool &&
      (GST_VIDEO_INFO_WIDTH (&self->overlay_info) != rectangle->width ||
          GST_VIDEO_INFO_HEIGHT (&self->overlay_info) != rectangle->height))
    gst_rga_compositor_clear_overlay_pool (self);

  if (!self->overlay_pool) {
    GstAllocator *allocator = gst_rga_compositor_get_allocator (self);
    GstCaps *caps;
    guint size;

    *reason = "cannot allocate the scaled BGRA overlay DMA-BUF";
    if (!allocator)
      return FALSE;
    if (!gst_video_info_set_format (&self->overlay_info, GST_VIDEO_FORMAT_BGRA,
            rectangle->width, rectangle->height)) {
      gst_object_unref (allocator);
      return FALSE;
    }
    caps = gst_video_info_to_caps (&self->overlay_info);
    self->overlay_pool = gst_rga_dma_heap_pool_new (GST_OBJECT (self), allocator,
        caps, &self->overlay_info, &size);
    gst_caps_unref (caps);
    gst_object_unref (allocator);
    if (!self->overlay_pool)
      return FALSE;
    if (!gst_buffer_pool_set_active (self->overlay_pool, TRUE)) {
      gst_rga_compositor_clear_overlay_pool (self);
      return FALSE;
    }
  }

  if (gst_buffer_pool_acquire_buffer (self->overlay_pool, buffer, NULL) !=
      GST_FLOW_OK) {
    *reason = "cannot acquire the scaled BGRA overlay DMA-BUF";
    return FALSE;
  }
  if (!gst_rga_compositor_frame_from_buffer (*buffer, &self->overlay_info,
          GST_VIDEO_FORMAT_BGRA, frame, reason))
    return FALSE;
  frame->hstride = GST_ROUND_UP_N (frame->height, GST_RGA_DMA_HEAP_ALIGNMENT);
  return TRUE;
}

static GstFlowReturn
gst_rga_compositor_aggregate_frames (GstVideoAggregator * videoaggregator,
    GstBuffer * output_buffer)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (videoaggregator);
  GstRgaCompositorInput inputs[2] = { {0,}, {0,} };
  GstRgaCompositorFrame input_frame[2] = { {0,}, {0,} };
  GstRgaCompositorFrame output_frame = { 0, };
  GstRgaCompositorFrame pat_frame = { 0, };
  GstBuffer *scaled_overlay = NULL;
  GstRgaCompositorRectangle rectangles[2] = { {0,}, {0,} };
  GstMppRgaIm2dRequest copy_request = { 0, };
  GstMppRgaIm2dCompositeRequest composite_request = { 0, };
  GstMppRgaBackend *backend;
  GstRgaCompositorLayout layout;
  GstMppRgaResult result;
  const gchar *reason = NULL;
  guint count;
  guint i;
  GstFlowReturn flow = GST_FLOW_OK;

  count = gst_rga_compositor_collect_inputs (videoaggregator, inputs);
  if (count == 0)
    return GST_FLOW_OK;
  if (!inputs[0].buffer) {
    flow = gst_rga_compositor_refuse_frame (self,
        "sink_0 has no primary frame", FALSE);
    goto out;
  }
  if (!inputs[1].buffer && output_buffer == inputs[0].buffer)
    goto out;

  g_mutex_lock (&self->lock);
  backend = self->backend;
  layout = self->layout;
  g_mutex_unlock (&self->lock);

  if (!gst_rga_compositor_frame_from_buffer (output_buffer,
          &videoaggregator->info, GST_VIDEO_FORMAT_NV12, &output_frame,
          &reason)) {
    flow = gst_rga_compositor_refuse_frame (self, reason, TRUE);
    goto out;
  }
  if (!gst_rga_compositor_frame_from_buffer (inputs[0].buffer,
          &inputs[0].info, GST_VIDEO_FORMAT_NV12, &input_frame[0], &reason)) {
    flow = gst_rga_compositor_refuse_frame (self, reason, TRUE);
    goto out;
  }
  if (inputs[1].buffer &&
      !gst_rga_compositor_frame_from_buffer (inputs[1].buffer,
          &inputs[1].info, GST_VIDEO_FORMAT_BGRA, &input_frame[1], &reason)) {
    flow = gst_rga_compositor_refuse_frame (self, reason, TRUE);
    goto out;
  }

  if (!inputs[1].buffer) {
    rectangles[0].width = output_frame.width;
    rectangles[0].height = output_frame.height;
  } else {
    for (i = 0; i < 2; i++) {
      if (!gst_rga_compositor_calculate_rectangle (layout, &inputs[i],
              output_frame.width, output_frame.height, &rectangles[i],
              &reason)) {
        flow = gst_rga_compositor_refuse_frame (self, reason, TRUE);
        goto out;
      }
    }
  }

  gst_rga_compositor_fill_request (&copy_request, &input_frame[0],
      &output_frame, &rectangles[0]);
  gst_mpp_rga_request_set_colorimetry (&copy_request, &inputs[0].info,
      &videoaggregator->info);
  if (inputs[1].buffer)
    gst_mpp_rga_composite_set_colorimetry (&composite_request,
        &videoaggregator->info, &inputs[1].info);
  result = gst_mpp_rga_backend_process (backend,
      GST_MPP_RGA_OP_COMPOSITOR_COPY, GST_VIDEO_FORMAT_NV12,
      GST_VIDEO_FORMAT_NV12, &copy_request);
  if (result != GST_MPP_RGA_SUCCESS) {
    reason = result == GST_MPP_RGA_TUPLE_DEMOTED ?
        "the background-copy tuple is temporarily demoted" :
        "the driver-probed RGA backend rejected the background copy";
    flow = gst_rga_compositor_refuse_frame (self, reason, FALSE);
    flow = gst_mpp_rga_result_to_flow (result);
    goto out;
  }

  gst_mpp_rga_count_csc_fallback (&copy_request,
      gst_mpp_conversion_stats_get (G_OBJECT (self)));

  if (inputs[1].buffer) {
    pat_frame = input_frame[1];
    if (pat_frame.width != (guint) rectangles[1].width ||
        pat_frame.height != (guint) rectangles[1].height) {
      GstMppRgaIm2dRequest scale_request = { 0, };
      GstRgaCompositorRectangle scale_rect = {
        0, 0, rectangles[1].width, rectangles[1].height
      };

      /* pat/src1 cannot scale. A smaller prect alone would crop the picture. */
      if (!gst_rga_compositor_acquire_overlay (self, &scale_rect,
              &scaled_overlay, &pat_frame, &reason)) {
        flow = gst_rga_compositor_refuse_frame (self, reason, FALSE);
        goto out;
      }
      gst_rga_compositor_fill_request (&scale_request, &input_frame[1],
          &pat_frame, &scale_rect);
      result = gst_mpp_rga_backend_process (backend,
          GST_MPP_RGA_OP_COMPOSITOR_COPY, GST_VIDEO_FORMAT_BGRA,
          GST_VIDEO_FORMAT_BGRA, &scale_request);
      if (result != GST_MPP_RGA_SUCCESS) {
        reason = result == GST_MPP_RGA_TUPLE_DEMOTED ?
            "the overlay-scale tuple is temporarily demoted" :
            "the driver-probed RGA backend rejected the overlay scale";
        flow = gst_rga_compositor_refuse_frame (self, reason, FALSE);
        flow = gst_mpp_rga_result_to_flow (result);
        goto out;
      }
    }

    gst_rga_compositor_fill_request (&composite_request.transform,
        &output_frame, &output_frame, &rectangles[1]);
    composite_request.transform.src_x = rectangles[1].x;
    composite_request.transform.src_y = rectangles[1].y;
    composite_request.transform.src_rect_width = rectangles[1].width;
    composite_request.transform.src_rect_height = rectangles[1].height;
    composite_request.transform.usage =
        (inputs[1].config.zorder >= inputs[0].config.zorder ?
        IM_ALPHA_BLEND_DST_OVER : IM_ALPHA_BLEND_SRC_OVER) |
        IM_ALPHA_BLEND_PRE_MUL;
    composite_request.pat_fd = pat_frame.fd;
    composite_request.pat_width = pat_frame.width;
    composite_request.pat_height = pat_frame.height;
    composite_request.pat_wstride = pat_frame.wstride;
    composite_request.pat_hstride = pat_frame.hstride;
    composite_request.pat_format = pat_frame.rga_format;
    composite_request.pat_rect_width = pat_frame.width;
    composite_request.pat_rect_height = pat_frame.height;
    composite_request.src_alpha =
        (guint8) (inputs[0].config.alpha * 255.0 + 0.5);
    composite_request.pat_alpha =
        (guint8) (inputs[1].config.alpha * 255.0 + 0.5);
    result = gst_mpp_rga_backend_composite (backend, GST_VIDEO_FORMAT_NV12,
        GST_VIDEO_FORMAT_NV12, &composite_request);
    if (result != GST_MPP_RGA_SUCCESS) {
      reason = result == GST_MPP_RGA_TUPLE_DEMOTED ?
          "the composite tuple is temporarily demoted" :
          "the driver-probed RGA backend rejected the composite pass";
      flow = gst_rga_compositor_refuse_frame (self, reason, FALSE);
      flow = gst_mpp_rga_result_to_flow (result);
    } else if (!copy_request.csc_fallback) {
      gst_mpp_rga_count_csc_fallback (&composite_request.transform,
          gst_mpp_conversion_stats_get (G_OBJECT (self)));
    }
  }

out:
  /* Both im2d passes are IM_SYNC; keep the intermediate alive until they end. */
  gst_clear_buffer (&scaled_overlay);
  for (i = 0; i < 2; i++)
    gst_rga_compositor_input_clear (&inputs[i]);
  return flow;
}

static gboolean
gst_rga_compositor_caps_info (GstCaps * caps, GstVideoInfo * info,
    GstVideoFormat expected_format)
{
  if (!caps || gst_caps_is_empty (caps) || gst_caps_is_any (caps) ||
      !gst_caps_features_contains (gst_caps_get_features (caps, 0),
          GST_CAPS_FEATURE_MEMORY_DMABUF) ||
      !gst_video_info_from_caps (info, caps))
    return FALSE;
  return GST_VIDEO_INFO_FORMAT (info) == expected_format &&
      GST_VIDEO_INFO_IS_INTERLACED (info) == FALSE;
}

static GstAllocator *
gst_rga_compositor_get_allocator (GstRgaCompositor * self)
{
  GstAllocator *allocator;

  g_mutex_lock (&self->lock);
  if (!self->allocator)
    self->allocator = gst_rga_dma_heap_allocator_new ();
  allocator = self->allocator ? gst_object_ref (self->allocator) : NULL;
  g_mutex_unlock (&self->lock);
  return allocator;
}

static gboolean
gst_rga_compositor_propose_allocation (GstAggregator * aggregator,
    GstAggregatorPad * aggregator_pad, GstQuery * decide_query,
    GstQuery * query)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (aggregator);
  GstVideoAlignment alignment;
  GstVideoInfo aligned;
  GstVideoInfo info;
  GstAllocator *allocator;
  GstBufferPool *pool;
  GstStructure *params;
  GstCaps *caps;
  GstVideoFormat expected_format;
  gboolean proposed = FALSE;
  gboolean parent_result;
  guint index;
  guint size;

  gst_query_parse_allocation (query, &caps, NULL);
  if (!gst_rga_compositor_pad_index (GST_PAD (aggregator_pad), &index) ||
      index > 1)
    return FALSE;
  expected_format = index == 0 ? GST_VIDEO_FORMAT_NV12 :
      GST_VIDEO_FORMAT_BGRA;
  if (!gst_rga_compositor_caps_info (caps, &info, expected_format) ||
      !gst_rga_video_info_align (&info, &aligned, &alignment))
    return FALSE;

  params = gst_structure_new ("video-meta",
      "padding-top", G_TYPE_UINT, alignment.padding_top,
      "padding-bottom", G_TYPE_UINT, alignment.padding_bottom,
      "padding-left", G_TYPE_UINT, alignment.padding_left,
      "padding-right", G_TYPE_UINT, alignment.padding_right, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, params);
  gst_structure_free (params);

  allocator = gst_rga_compositor_get_allocator (self);
  if (allocator) {
    pool = gst_rga_dma_heap_pool_new (GST_OBJECT (self), allocator, caps,
        &info, &size);
    if (pool) {
      gst_query_add_allocation_pool (query, pool, size, 2, 0);
      gst_query_add_allocation_param (query, allocator, NULL);
      proposed = TRUE;
      gst_object_unref (pool);
    }
    gst_object_unref (allocator);
  }

  parent_result = GST_AGGREGATOR_CLASS
      (gst_rga_compositor_parent_class)->propose_allocation (aggregator,
      aggregator_pad, decide_query, query);
  return proposed || parent_result;
}

static gboolean
gst_rga_compositor_decide_allocation (GstAggregator * aggregator,
    GstQuery * query)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (aggregator);
  GstAllocationParams params = { .align = 15 };
  GstAllocator *allocator;
  GstBufferPool *pool;
  GstVideoInfo info;
  GstCaps *caps;
  guint size;

  gst_query_parse_allocation (query, &caps, NULL);
  if (!gst_rga_compositor_caps_info (caps, &info, GST_VIDEO_FORMAT_NV12))
    return FALSE;

  allocator = gst_rga_compositor_get_allocator (self);
  if (!allocator)
    return FALSE;
  pool = gst_rga_dma_heap_pool_new (GST_OBJECT (self), allocator, caps, &info,
      &size);
  if (!pool) {
    gst_object_unref (allocator);
    return FALSE;
  }

  if (gst_query_get_n_allocation_pools (query) == 0)
    gst_query_add_allocation_pool (query, pool, size, 2, 0);
  else
    gst_query_set_nth_allocation_pool (query, 0, pool, size, 2, 0);
  if (gst_query_get_n_allocation_params (query) == 0)
    gst_query_add_allocation_param (query, allocator, &params);
  else
    gst_query_set_nth_allocation_param (query, 0, allocator, &params);
  gst_object_unref (pool);
  gst_object_unref (allocator);

  /* Our aligned DMA-BUF pool is complete. Generic video allocation renegotiates
   * it and leaks an owned allocator reference on GStreamer 1.26.2. */
  return TRUE;
}

static gboolean
gst_rga_compositor_primary_info (GstRgaCompositor * self, GstVideoInfo * info)
{
  GstPad *pad = gst_element_get_static_pad (GST_ELEMENT (self), "sink_0");
  gboolean have_info = FALSE;

  if (!pad)
    return FALSE;
  GST_OBJECT_LOCK (pad);
  if (GST_VIDEO_AGGREGATOR_PAD (pad)->info.finfo) {
    *info = GST_VIDEO_AGGREGATOR_PAD (pad)->info;
    have_info = TRUE;
  }
  GST_OBJECT_UNLOCK (pad);
  gst_object_unref (pad);
  return have_info;
}

static GstCaps *
gst_rga_compositor_update_caps (GstVideoAggregator * videoaggregator,
    GstCaps * caps)
{
  GstVideoAggregatorClass *parent_class = GST_VIDEO_AGGREGATOR_CLASS
      (gst_rga_compositor_parent_class);
  GstCaps *updated = parent_class->update_caps (videoaggregator, caps);
  GstVideoInfo primary_info;
  guint i;

  if (!updated || gst_caps_is_empty (updated) || gst_caps_is_any (updated))
    return updated;
  updated = gst_caps_make_writable (updated);
  for (i = 0; i < gst_caps_get_size (updated); i++) {
    GstStructure *structure = gst_caps_get_structure (updated, i);

    gst_structure_set (structure, "format", G_TYPE_STRING, "NV12",
        "interlace-mode", G_TYPE_STRING, "progressive", NULL);
    gst_caps_set_features (updated, i,
        gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_DMABUF, NULL));
  }

  if (gst_rga_compositor_primary_info (GST_RGA_COMPOSITOR (videoaggregator),
          &primary_info)) {
    for (i = 0; i < gst_caps_get_size (updated); i++) {
      GstStructure *structure = gst_caps_get_structure (updated, i);

      gst_structure_set (structure, "width", G_TYPE_INT,
          GST_VIDEO_INFO_WIDTH (&primary_info), "height", G_TYPE_INT,
          GST_VIDEO_INFO_HEIGHT (&primary_info), NULL);
    }
  }
  return updated;
}

static void
gst_rga_compositor_find_best_format (GstVideoAggregator * videoaggregator,
    GstCaps * downstream_caps, GstVideoInfo * best_info,
    gboolean * at_least_one_alpha)
{
  (void) downstream_caps;
  *at_least_one_alpha = FALSE;
  /* The base format selector tests plain caps against DMA-BUF caps and falls back to
   * defaults. Our NV12 accumulator, not the BGRA overlay, defines output color. */
  gst_rga_compositor_primary_info (GST_RGA_COMPOSITOR (videoaggregator),
      best_info);
}

static GstFlowReturn
gst_rga_compositor_create_output_buffer (GstVideoAggregator * videoaggregator,
    GstBuffer ** output_buffer)
{
  GstRgaCompositorInput inputs[2] = { {0,}, {0,} };
  GstRgaCompositorFrame frame;
  const gchar *reason = NULL;
  guint count;

  count = gst_rga_compositor_collect_inputs (videoaggregator, inputs);
  if (!inputs[0].buffer) {
    /* A queued primary can start after this output interval. NULL output with
     * OK lets GstVideoAggregator advance time without publishing unwritten DMA
     * memory; NEED_DATA would retry this same interval with the same buffers. */
    GST_DEBUG_OBJECT (videoaggregator,
        "skipping output interval without a primary frame");
    *output_buffer = NULL;
    gst_rga_compositor_input_clear (&inputs[1]);
    return GST_FLOW_OK;
  }
  if (count == 1 && inputs[0].index == 0 &&
      inputs[0].info.finfo && videoaggregator->info.finfo &&
      GST_VIDEO_INFO_FORMAT (&inputs[0].info) == GST_VIDEO_FORMAT_NV12 &&
      GST_VIDEO_INFO_FORMAT (&videoaggregator->info) == GST_VIDEO_FORMAT_NV12 &&
      GST_VIDEO_INFO_WIDTH (&inputs[0].info) ==
      GST_VIDEO_INFO_WIDTH (&videoaggregator->info) &&
      GST_VIDEO_INFO_HEIGHT (&inputs[0].info) ==
      GST_VIDEO_INFO_HEIGHT (&videoaggregator->info) &&
      gst_rga_compositor_frame_from_buffer (inputs[0].buffer,
          &inputs[0].info, GST_VIDEO_FORMAT_NV12, &frame, &reason)) {
    *output_buffer = inputs[0].buffer;
    inputs[0].buffer = NULL;
    gst_rga_compositor_input_clear (&inputs[0]);
    return GST_FLOW_OK;
  }

  gst_rga_compositor_input_clear (&inputs[0]);
  gst_rga_compositor_input_clear (&inputs[1]);
  return GST_VIDEO_AGGREGATOR_CLASS
      (gst_rga_compositor_parent_class)->create_output_buffer
      (videoaggregator, output_buffer);
}

static GstPad *
gst_rga_compositor_request_new_pad (GstElement * element,
    GstPadTemplate * template, const gchar * requested_name,
    const GstCaps * caps)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (element);
  GstElementClass *parent_class = GST_ELEMENT_CLASS
      (gst_rga_compositor_parent_class);
  gboolean used[2] = { FALSE, FALSE };
  const gchar *name = requested_name;
  gchar generated_name[7];
  GstPad *pad = NULL;
  GList *node;
  guint index;

  if (template->direction != GST_PAD_SINK)
    return parent_class->request_new_pad (element, template, requested_name,
        caps);

  g_mutex_lock (&self->lock);
  GST_OBJECT_LOCK (element);
  for (node = element->sinkpads; node; node = node->next) {
    guint existing_index;

    if (gst_rga_compositor_pad_index (GST_PAD (node->data), &existing_index) &&
        existing_index < 2)
      used[existing_index] = TRUE;
  }
  GST_OBJECT_UNLOCK (element);

  if (!name || strchr (name, '%')) {
    index = used[0] ? 1 : 0;
    if (used[index])
      goto out;
    g_snprintf (generated_name, sizeof (generated_name), "sink_%u", index);
    name = generated_name;
  } else {
    if (!gst_rga_compositor_name_index (name, &index) || index > 1 ||
        used[index])
      goto out;
  }

  pad = parent_class->request_new_pad (element, template, name, caps);
  if (pad) {
    g_object_set (pad, "zorder", index, NULL);
    gst_child_proxy_child_added (GST_CHILD_PROXY (self), G_OBJECT (pad),
        GST_OBJECT_NAME (pad));
  }

out:
  g_mutex_unlock (&self->lock);
  if (!pad)
    GST_WARNING_OBJECT (self, "v1 accepts only request pads sink_0 and sink_1");
  return pad;
}

static void
gst_rga_compositor_release_pad (GstElement * element, GstPad * pad)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (element);

  g_mutex_lock (&self->lock);
  gst_child_proxy_child_removed (GST_CHILD_PROXY (self), G_OBJECT (pad),
      GST_OBJECT_NAME (pad));
  GST_ELEMENT_CLASS (gst_rga_compositor_parent_class)->release_pad (element,
      pad);
  g_mutex_unlock (&self->lock);
}

static GObject *
gst_rga_compositor_child_by_index (GstChildProxy * child_proxy, guint index)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (child_proxy);
  GObject *child;

  GST_OBJECT_LOCK (self);
  child = g_list_nth_data (GST_ELEMENT (self)->sinkpads, index);
  if (child)
    gst_object_ref (child);
  GST_OBJECT_UNLOCK (self);
  return child;
}

static guint
gst_rga_compositor_child_count (GstChildProxy * child_proxy)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (child_proxy);
  guint count;

  GST_OBJECT_LOCK (self);
  count = GST_ELEMENT (self)->numsinkpads;
  GST_OBJECT_UNLOCK (self);
  return count;
}

static void
gst_rga_compositor_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = g_iface;

  (void) iface_data;
  iface->get_child_by_index = gst_rga_compositor_child_by_index;
  iface->get_children_count = gst_rga_compositor_child_count;
}

static void
gst_rga_compositor_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (object);

  g_mutex_lock (&self->lock);
  switch (prop_id) {
    case PROP_LAYOUT:
      self->layout = g_value_get_enum (value);
      break;
    default:
      g_mutex_unlock (&self->lock);
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
  }
  g_mutex_unlock (&self->lock);
}

static void
gst_rga_compositor_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (object);

  if (prop_id == PROP_CONVERSION_FALLBACK_FRAMES ||
      prop_id == PROP_CONVERSION_DROPPED_FRAMES ||
      prop_id == PROP_LAYOUT_REJECTIONS || prop_id == PROP_CSC_FALLBACK_FRAMES) {
    GstMppConversionStatsSnapshot stats;

    gst_mpp_conversion_stats_snapshot (gst_mpp_conversion_stats_get (object),
        &stats);
    if (prop_id == PROP_CONVERSION_FALLBACK_FRAMES)
      g_value_set_uint64 (value, stats.fallback_frames);
    else if (prop_id == PROP_CONVERSION_DROPPED_FRAMES)
      g_value_set_uint64 (value, stats.dropped_frames);
    else if (prop_id == PROP_CSC_FALLBACK_FRAMES)
      g_value_set_uint64 (value, stats.csc_fallback_frames);
    else
      g_value_set_uint64 (value, stats.layout_rejections);
    return;
  }

  g_mutex_lock (&self->lock);
  switch (prop_id) {
    case PROP_LAYOUT:
      g_value_set_enum (value, self->layout);
      break;
    default:
      g_mutex_unlock (&self->lock);
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
  }
  g_mutex_unlock (&self->lock);
}

static GstStateChangeReturn
gst_rga_compositor_change_state (GstElement * element,
    GstStateChange transition)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (element);
  GstMppRgaBackend *backend;

  if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
    g_mutex_lock (&self->lock);
    backend = self->backend;
    g_mutex_unlock (&self->lock);
    if (!gst_mpp_rga_backend_init (backend)) {
      GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
          ("rgacompositor cannot enter READY: driver-probed RGA backend unavailable"),
          ("/dev/rga did not pass the driver-version probe"));
      return GST_STATE_CHANGE_FAILURE;
    }
  } else if (transition == GST_STATE_CHANGE_READY_TO_NULL) {
    GstMppConversionStatsSnapshot stats;

    gst_mpp_conversion_stats_snapshot (gst_mpp_conversion_stats_get
        (G_OBJECT (self)), &stats);
    GST_DEBUG_OBJECT (self,
        "conversion summary: fallback=%" G_GUINT64_FORMAT " dropped=%"
        G_GUINT64_FORMAT " layout-rejections=%" G_GUINT64_FORMAT,
        stats.fallback_frames, stats.dropped_frames, stats.layout_rejections);
  }
  return GST_ELEMENT_CLASS (gst_rga_compositor_parent_class)->change_state
      (element, transition);
}

static void
gst_rga_compositor_finalize (GObject * object)
{
  GstRgaCompositor *self = GST_RGA_COMPOSITOR (object);

  gst_rga_compositor_clear_overlay_pool (self);
  gst_clear_object (&self->allocator);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (gst_rga_compositor_parent_class)->finalize (object);
}

void
gst_rga_compositor_set_backend_for_test (GstRgaCompositor * self,
    GstMppRgaBackend * backend)
{
  g_return_if_fail (GST_IS_RGA_COMPOSITOR (self));
  g_return_if_fail (backend != NULL);
  g_mutex_lock (&self->lock);
  self->backend = backend;
  g_mutex_unlock (&self->lock);
}

void
gst_rga_compositor_set_allocator_for_test (GstRgaCompositor * self,
    GstAllocator * allocator)
{
  g_return_if_fail (GST_IS_RGA_COMPOSITOR (self));
  g_return_if_fail (allocator == NULL || GST_IS_DMABUF_ALLOCATOR (allocator));
  g_mutex_lock (&self->lock);
  gst_object_replace ((GstObject **) & self->allocator,
      (GstObject *) allocator);
  g_mutex_unlock (&self->lock);
}

static void
gst_rga_compositor_init (GstRgaCompositor * self)
{
  g_mutex_init (&self->lock);
  self->backend = gst_mpp_rga_backend_get_default ();
  self->layout = GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_RIGHT;
  gst_mpp_conversion_stats_attach (G_OBJECT (self));
}

static gboolean
gst_rga_compositor_stop (GstAggregator * aggregator)
{
  gboolean result = GST_AGGREGATOR_CLASS
      (gst_rga_compositor_parent_class)->stop (aggregator);

  gst_rga_compositor_clear_overlay_pool (GST_RGA_COMPOSITOR (aggregator));
  return result;
}

static void
gst_rga_compositor_class_init (GstRgaCompositorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAggregatorClass *aggregator_class = GST_AGGREGATOR_CLASS (klass);
  GstVideoAggregatorClass *videoaggregator_class =
      GST_VIDEO_AGGREGATOR_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (rga_compositor_debug, "rgacompositor", 0,
      "Rockchip RGA video compositor");
  gobject_class->set_property = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_rga_compositor_finalize);
  element_class->request_new_pad = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_request_new_pad);
  element_class->release_pad = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_release_pad);
  element_class->change_state = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_change_state);

  g_object_class_install_property (gobject_class, PROP_LAYOUT,
      g_param_spec_enum ("layout", "Layout", "PiP, PbP, or custom layout",
          GST_TYPE_RGA_COMPOSITOR_LAYOUT,
          GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_RIGHT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_CONVERSION_FALLBACK_FRAMES,
      g_param_spec_uint64 ("conversion-fallback-frames",
          "Conversion fallback frames",
          "Frames using a fallback conversion path", 0, G_MAXUINT64, 0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_CONVERSION_DROPPED_FRAMES,
      g_param_spec_uint64 ("conversion-dropped-frames",
          "Conversion dropped frames",
          "Frames dropped because no 2D compositor was available", 0,
          G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_LAYOUT_REJECTIONS,
      g_param_spec_uint64 ("layout-rejections", "Layout rejections",
          "Frames rejected for an unsupported compositor layout", 0,
          G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CSC_FALLBACK_FRAMES,
      g_param_spec_uint64 ("csc-fallback-frames", "CSC fallback frames",
          "Frames submitted with an unexpressible CSC using the library default",
          0, G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (element_class,
      "Rockchip RGA video compositor", "Filter/Editor/Video/Hardware",
      "Composite NV12 primary and BGRA overlay DMA-BUF streams with librga im2d",
      "CERALIVE <contact@ceralive.tv>");
  gst_element_class_add_static_pad_template_with_gtype (element_class,
      &sink_template, GST_TYPE_RGA_COMPOSITOR_PAD);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  aggregator_class->decide_allocation = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_decide_allocation);
  aggregator_class->stop = GST_DEBUG_FUNCPTR (gst_rga_compositor_stop);
  aggregator_class->propose_allocation = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_propose_allocation);
  aggregator_class->sink_query = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_sink_query);
  videoaggregator_class->update_caps = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_update_caps);
  videoaggregator_class->find_best_format = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_find_best_format);
  videoaggregator_class->create_output_buffer = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_create_output_buffer);
  videoaggregator_class->aggregate_frames = GST_DEBUG_FUNCPTR
      (gst_rga_compositor_aggregate_frames);
}
