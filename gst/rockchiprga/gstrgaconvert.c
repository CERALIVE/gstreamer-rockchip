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
#include <gst/video/gstvideopool.h>
#if GST_CHECK_VERSION(1, 24, 0)
#include <gst/video/video-info-dma.h>
#endif
#include <rga/im2d.h>

#include "gstrgaconvert.h"
#include "gstrgautil.h"
#include "gstmppconversionstats.h"

#define GST_RGA_FORMATS \
  "{ NV12, NV16, NV21, NV61, I420, YUY2, UYVY, BGR, RGB, BGRA, RGBA, RGB16 }"
#define GST_RGA_CAPS_SUFFIX ", interlace-mode = (string) progressive"
#if GST_CHECK_VERSION(1, 24, 0)
#define GST_RGA_DMA_DRM_FORMATS \
  "{ NV12, NV16, NV21, NV61, YU12, YUYV, UYVY, RG24, BG24, AR24, AB24, RG16 }"
#define GST_RGA_DMA_DRM_CAPS \
  "; " GST_VIDEO_DMA_DRM_CAPS_MAKE \
  ", drm-format = (string) " GST_RGA_DMA_DRM_FORMATS GST_RGA_CAPS_SUFFIX
#else
#define GST_RGA_DMA_DRM_CAPS ""
#endif
#define GST_RGA_CAPS \
  GST_VIDEO_CAPS_MAKE_WITH_FEATURES (GST_CAPS_FEATURE_MEMORY_DMABUF, \
      GST_RGA_FORMATS) GST_RGA_CAPS_SUFFIX \
  "; " GST_VIDEO_CAPS_MAKE (GST_RGA_FORMATS) GST_RGA_CAPS_SUFFIX \
  GST_RGA_DMA_DRM_CAPS

GST_DEBUG_CATEGORY_STATIC (rga_convert_debug);
#define GST_CAT_DEFAULT rga_convert_debug

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS (GST_RGA_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS (GST_RGA_CAPS));

typedef enum
{
  GST_RGA_MEMORY_SYSTEM,
  GST_RGA_MEMORY_DMABUF,
  GST_RGA_MEMORY_DMA_DRM,
} GstRgaMemoryKind;

typedef struct
{
  GstVideoFormat format;
  RgaSURF_FORMAT rga_format;
  guint width;
  guint height;
  guint wstride;
  guint hstride;
  guint n_planes;
  gsize offset[GST_VIDEO_MAX_PLANES];
  gint stride[GST_VIDEO_MAX_PLANES];
  gsize required_size;
} GstRgaVideoLayout;

struct _GstRgaConvert
{
  GstBaseTransform parent;
  GMutex lock;
  GstVideoInfo in_info;
  GstVideoInfo out_info;
  gboolean have_caps;
  GstRgaMemoryKind in_memory;
  GstRgaMemoryKind out_memory;
  GstMppRgaBackend *backend;
  GstAllocator *allocator;
  GstRgaRotation rotation;
  gboolean hflip;
  gboolean vflip;
  guint core_mask;
  gint priority;
  guint crop_x;
  guint crop_y;
  guint crop_w;
  guint crop_h;
};

#define gst_rga_convert_parent_class parent_class
G_DEFINE_TYPE (GstRgaConvert, gst_rga_convert, GST_TYPE_BASE_TRANSFORM);

enum
{
  PROP_0,
  PROP_ROTATION,
  PROP_HFLIP,
  PROP_VFLIP,
  PROP_CORE_MASK,
  PROP_PRIORITY,
  PROP_CROP_X,
  PROP_CROP_Y,
  PROP_CROP_W,
  PROP_CROP_H,
  PROP_CONVERSION_FALLBACK_FRAMES,
  PROP_CONVERSION_DROPPED_FRAMES,
  PROP_LAYOUT_REJECTIONS,
};

GType
gst_rga_rotation_get_type (void)
{
  static gsize type = 0;
  static const GEnumValue values[] = {
    {GST_RGA_ROTATION_0, "No rotation", "0"},
    {GST_RGA_ROTATION_90, "Rotate clockwise 90 degrees", "90"},
    {GST_RGA_ROTATION_180, "Rotate 180 degrees", "180"},
    {GST_RGA_ROTATION_270, "Rotate clockwise 270 degrees", "270"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType registered = g_enum_register_static ("GstRgaRotation", values);
    g_once_init_leave (&type, registered);
  }
  return type;
}

GType
gst_rga_core_mask_get_type (void)
{
  static gsize type = 0;
  static const GFlagsValue values[] = {
    {GST_RGA_CORE_AUTO, "Automatic core selection", "auto"},
    {GST_RGA_CORE_RGA3_CORE0, "RGA3 core 0", "rga3-core0"},
    {GST_RGA_CORE_RGA3_CORE1, "RGA3 core 1", "rga3-core1"},
    {GST_RGA_CORE_RGA2, "RGA2 core", "rga2"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType registered = g_flags_register_static ("GstRgaCoreMask", values);
    g_once_init_leave (&type, registered);
  }
  return type;
}

static RgaSURF_FORMAT
gst_rga_convert_format_to_rga (GstVideoFormat format)
{
  switch (format) {
    case GST_VIDEO_FORMAT_NV12:
      return RK_FORMAT_YCbCr_420_SP;
    case GST_VIDEO_FORMAT_NV16:
      return RK_FORMAT_YCbCr_422_SP;
    case GST_VIDEO_FORMAT_NV21:
      return RK_FORMAT_YCrCb_420_SP;
    case GST_VIDEO_FORMAT_NV61:
      return RK_FORMAT_YCrCb_422_SP;
    case GST_VIDEO_FORMAT_I420:
      return RK_FORMAT_YCbCr_420_P;
    case GST_VIDEO_FORMAT_YUY2:
      return RK_FORMAT_YUYV_422;
    case GST_VIDEO_FORMAT_UYVY:
      return RK_FORMAT_UYVY_422;
    case GST_VIDEO_FORMAT_BGR:
      return RK_FORMAT_BGR_888;
    case GST_VIDEO_FORMAT_RGB:
      return RK_FORMAT_RGB_888;
    case GST_VIDEO_FORMAT_BGRA:
      return RK_FORMAT_BGRA_8888;
    case GST_VIDEO_FORMAT_RGBA:
      return RK_FORMAT_RGBA_8888;
    case GST_VIDEO_FORMAT_RGB16:
      return RK_FORMAT_BGR_565;
    default:
      return RK_FORMAT_UNKNOWN;
  }
}

static GstRgaMemoryKind
gst_rga_convert_memory_kind (const GstCaps * caps, guint index)
{
#if GST_CHECK_VERSION(1, 24, 0)
  GstCaps *single;
  gboolean dma_drm;

  single = gst_caps_copy_nth (caps, index);
  dma_drm = gst_video_is_dma_drm_caps (single);
  gst_caps_unref (single);
  if (dma_drm)
    return GST_RGA_MEMORY_DMA_DRM;
#endif

  if (gst_caps_features_contains (gst_caps_get_features (caps, index),
          GST_CAPS_FEATURE_MEMORY_DMABUF))
    return GST_RGA_MEMORY_DMABUF;
  return GST_RGA_MEMORY_SYSTEM;
}

static gboolean
gst_rga_convert_info_from_caps (GstCaps * caps, GstVideoInfo * info,
    GstRgaMemoryKind * memory)
{
  *memory = gst_rga_convert_memory_kind (caps, 0);
#if GST_CHECK_VERSION(1, 24, 0)
  if (*memory == GST_RGA_MEMORY_DMA_DRM) {
    GstVideoInfoDmaDrm drm_info;

    gst_video_info_dma_drm_init (&drm_info);
    if (!gst_video_info_dma_drm_from_caps (&drm_info, caps) ||
        drm_info.drm_modifier != 0 ||
        !gst_video_info_dma_drm_to_video_info (&drm_info, info))
      return FALSE;
  } else {
#endif
    if (!gst_video_info_from_caps (info, caps))
      return FALSE;
#if GST_CHECK_VERSION(1, 24, 0)
  }
#endif

  return gst_rga_convert_format_to_rga (GST_VIDEO_INFO_FORMAT (info)) !=
      RK_FORMAT_UNKNOWN;
}

static gboolean
gst_rga_convert_layout_from_buffer (GstBuffer * buffer,
    const GstVideoInfo * info, GstRgaVideoLayout * layout,
    const gchar ** reason)
{
  GstVideoMeta *meta = gst_buffer_get_video_meta (buffer);
  guint pixel_stride;
  guint i;

  layout->format = GST_VIDEO_INFO_FORMAT (info);
  layout->rga_format = gst_rga_convert_format_to_rga (layout->format);
  layout->width = GST_VIDEO_INFO_WIDTH (info);
  layout->height = GST_VIDEO_INFO_HEIGHT (info);
  layout->n_planes = GST_VIDEO_INFO_N_PLANES (info);

  if (meta) {
    if (meta->format != layout->format || meta->width != layout->width ||
        meta->height != layout->height ||
        meta->n_planes != layout->n_planes) {
      *reason = "GstVideoMeta does not match negotiated caps";
      return FALSE;
    }
    for (i = 0; i < layout->n_planes; i++) {
      layout->offset[i] = meta->offset[i];
      layout->stride[i] = meta->stride[i];
    }
  } else {
    for (i = 0; i < layout->n_planes; i++) {
      layout->offset[i] = GST_VIDEO_INFO_PLANE_OFFSET (info, i);
      layout->stride[i] = GST_VIDEO_INFO_PLANE_STRIDE (info, i);
    }
  }

  if (layout->offset[0] != 0 || layout->stride[0] <= 0) {
    *reason = "RGA requires a zero first-plane offset and positive stride";
    return FALSE;
  }

  switch (layout->format) {
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_UYVY:
    case GST_VIDEO_FORMAT_RGB16:
      pixel_stride = 2;
      break;
    case GST_VIDEO_FORMAT_BGR:
    case GST_VIDEO_FORMAT_RGB:
      pixel_stride = 3;
      break;
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_RGBA:
      pixel_stride = 4;
      break;
    default:
      pixel_stride = 1;
      break;
  }

  if ((guint) layout->stride[0] % pixel_stride != 0) {
    *reason = "byte stride is not an integral pixel stride";
    return FALSE;
  }
  layout->wstride = layout->stride[0] / pixel_stride;
  layout->hstride = layout->height;

  switch (layout->format) {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
    case GST_VIDEO_FORMAT_NV16:
    case GST_VIDEO_FORMAT_NV61:
      if (layout->n_planes != 2 || layout->stride[1] != layout->stride[0] ||
          layout->offset[1] == 0 ||
          layout->offset[1] % layout->stride[0] != 0) {
        *reason = "semi-planar offsets or strides are not RGA-linear";
        return FALSE;
      }
      layout->hstride = layout->offset[1] / layout->stride[0];
      if (layout->hstride < layout->height) {
        *reason = "semi-planar vertical stride is smaller than frame height";
        return FALSE;
      }
      if (layout->format == GST_VIDEO_FORMAT_NV12 ||
          layout->format == GST_VIDEO_FORMAT_NV21) {
        if (layout->hstride % 2 != 0) {
          *reason = "4:2:0 vertical stride must be even";
          return FALSE;
        }
        layout->required_size = layout->offset[1] +
            layout->stride[1] * (layout->hstride / 2);
      } else {
        layout->required_size = layout->offset[1] +
            layout->stride[1] * layout->hstride;
      }
      break;
    case GST_VIDEO_FORMAT_I420:
      if (layout->n_planes != 3 || layout->stride[0] % 2 != 0 ||
          layout->stride[1] != layout->stride[0] / 2 ||
          layout->stride[2] != layout->stride[1] ||
          layout->offset[1] == 0 ||
          layout->offset[1] % layout->stride[0] != 0) {
        *reason = "I420 offsets or strides are not RGA-linear";
        return FALSE;
      }
      layout->hstride = layout->offset[1] / layout->stride[0];
      if (layout->hstride < layout->height || layout->hstride % 2 != 0 ||
          layout->offset[2] != layout->offset[1] +
          layout->stride[1] * (layout->hstride / 2)) {
        *reason = "I420 plane offsets do not describe one linear image";
        return FALSE;
      }
      layout->required_size = layout->offset[2] +
          layout->stride[2] * (layout->hstride / 2);
      break;
    default:
      if (layout->n_planes != 1) {
        *reason = "packed format unexpectedly has multiple planes";
        return FALSE;
      }
      layout->required_size = layout->stride[0] * layout->height;
      break;
  }

  if (layout->wstride < layout->width ||
      gst_buffer_get_size (buffer) < layout->required_size) {
    *reason = "buffer is smaller than its negotiated stride layout";
    return FALSE;
  }
  return TRUE;
}

static gboolean
gst_rga_convert_get_dmabuf_fd (GstBuffer * buffer, gint * fd,
    const gchar ** reason)
{
  GstMemory *memory;
  gsize offset;

  if (gst_buffer_n_memory (buffer) != 1) {
    *reason = "RGA requires one DMA-BUF containing every video plane";
    return FALSE;
  }

  memory = gst_buffer_peek_memory (buffer, 0);
  if (!gst_is_dmabuf_memory (memory)) {
    *reason = "buffer memory is not DMA-BUF";
    return FALSE;
  }

  gst_memory_get_sizes (memory, &offset, NULL);
  if (offset != 0) {
    *reason = "DMA-BUF memory has a non-zero base offset";
    return FALSE;
  }

  *fd = gst_dmabuf_memory_get_fd (memory);
  if (*fd < 0) {
    *reason = "DMA-BUF has no importable file descriptor";
    return FALSE;
  }
  return TRUE;
}

static gboolean
gst_rga_convert_crop_valid (const GstRgaVideoLayout * layout, guint x,
    guint y, guint width, guint height, const gchar ** reason)
{
  if (x >= layout->width || y >= layout->height || width == 0 || height == 0 ||
      width > layout->width - x || height > layout->height - y) {
    *reason = "crop rectangle falls outside the input frame";
    return FALSE;
  }

  switch (layout->format) {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
    case GST_VIDEO_FORMAT_I420:
      if ((x | y | width | height) & 1) {
        *reason = "4:2:0 crop coordinates and dimensions must be even";
        return FALSE;
      }
      break;
    case GST_VIDEO_FORMAT_NV16:
    case GST_VIDEO_FORMAT_NV61:
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_UYVY:
      if ((x | width) & 1) {
        *reason = "4:2:2 crop x and width must be even";
        return FALSE;
      }
      break;
    default:
      break;
  }
  return TRUE;
}

static gboolean
gst_rga_convert_output_geometry_valid (const GstRgaVideoLayout * layout,
    const gchar ** reason)
{
  switch (layout->format) {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
    case GST_VIDEO_FORMAT_I420:
      if ((layout->width | layout->height) & 1) {
        *reason = "4:2:0 output dimensions must be even";
        return FALSE;
      }
      break;
    case GST_VIDEO_FORMAT_NV16:
    case GST_VIDEO_FORMAT_NV61:
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_UYVY:
      if (layout->width & 1) {
        *reason = "4:2:2 output width must be even";
        return FALSE;
      }
      break;
    default:
      break;
  }
  return TRUE;
}

static GstAllocator *
gst_rga_convert_get_allocator (GstRgaConvert * self)
{
  GstAllocator *allocator;

  g_mutex_lock (&self->lock);
  if (!self->allocator)
    self->allocator = gst_rga_dma_heap_allocator_new ();
  allocator = self->allocator ? gst_object_ref (self->allocator) : NULL;
  g_mutex_unlock (&self->lock);
  return allocator;
}

static GstBufferPool *
gst_rga_convert_create_pool (GstRgaConvert * self, GstCaps * caps,
    const GstVideoInfo * info, guint * size)
{
  GstAllocator *allocator;
  GstBufferPool *pool;

  allocator = gst_rga_convert_get_allocator (self);
  if (!allocator)
    return NULL;
  pool = gst_rga_dma_heap_pool_new (GST_OBJECT (self), allocator, caps, info,
      size);
  gst_object_unref (allocator);
  return pool;
}

static GstBuffer *
gst_rga_convert_new_staging_buffer (GstRgaConvert * self,
    const GstRgaVideoLayout * layout)
{
  GstAllocator *allocator = gst_rga_convert_get_allocator (self);
  GstBuffer *buffer;

  if (!allocator)
    return NULL;
  buffer = gst_buffer_new_allocate (allocator, layout->required_size, NULL);
  gst_object_unref (allocator);
  if (!buffer)
    return NULL;

  gst_buffer_add_video_meta_full (buffer, GST_VIDEO_FRAME_FLAG_NONE,
      layout->format, layout->width, layout->height, layout->n_planes,
      layout->offset, layout->stride);
  return buffer;
}

static gboolean
gst_rga_convert_copy_to_staging (GstBuffer * input, GstBuffer * staging,
    gsize size)
{
  GstMapInfo map = GST_MAP_INFO_INIT;
  gsize copied;

  if (!gst_buffer_map (staging, &map, GST_MAP_WRITE))
    return FALSE;
  copied = gst_buffer_extract (input, 0, map.data, size);
  gst_buffer_unmap (staging, &map);
  return copied == size;
}

static gboolean
gst_rga_convert_copy_from_staging (GstBuffer * staging, GstBuffer * output,
    gsize size)
{
  GstMapInfo map = GST_MAP_INFO_INIT;
  gsize copied;

  if (!gst_buffer_map (staging, &map, GST_MAP_READ))
    return FALSE;
  copied = gst_buffer_fill (output, 0, map.data, size);
  gst_buffer_unmap (staging, &map);
  return copied == size;
}

static GstFlowReturn
gst_rga_convert_not_negotiated (GstRgaConvert * self, const gchar * reason,
    gboolean layout_rejection)
{
  GstMppConversionStats *stats = gst_mpp_conversion_stats_get (G_OBJECT (self));

  if (layout_rejection)
    gst_mpp_conversion_stats_layout_rejected (stats);
  gst_mpp_conversion_stats_dropped (stats);
  GST_ELEMENT_ERROR (self, CORE, NEGOTIATION,
      ("no 2D converter available for rgaconvert: %s", reason), (NULL));
  return GST_FLOW_NOT_NEGOTIATED;
}

static void
gst_rga_convert_copy_caps_field (GstStructure * output,
    const GstStructure * input, const gchar * field)
{
  const GValue *value = gst_structure_get_value (input, field);

  if (value)
    gst_structure_set_value (output, field, value);
}

static GstCaps *
gst_rga_convert_transform_caps (GstBaseTransform * transform,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstPad *other_pad = direction == GST_PAD_SINK ?
      GST_BASE_TRANSFORM_SRC_PAD (transform) :
      GST_BASE_TRANSFORM_SINK_PAD (transform);
  GstCaps *templates = gst_pad_get_pad_template_caps (other_pad);
  GstCaps *result = gst_caps_new_empty ();
  guint input_index;
  guint pass;

  for (pass = 0; pass < 2; pass++) {
    for (input_index = 0; input_index < gst_caps_get_size (caps);
        input_index++) {
      const GstStructure *input = gst_caps_get_structure (caps, input_index);
      GstRgaMemoryKind input_kind = gst_rga_convert_memory_kind (caps,
          input_index);
      guint template_index;

      for (template_index = 0;
          template_index < gst_caps_get_size (templates); template_index++) {
        GstRgaMemoryKind template_kind = gst_rga_convert_memory_kind (templates,
            template_index);
        GstStructure *output;
        GstCapsFeatures *features;

        if ((pass == 0) != (input_kind == template_kind))
          continue;
        output = gst_structure_copy (gst_caps_get_structure (templates,
                template_index));
        gst_rga_convert_copy_caps_field (output, input, "framerate");
        gst_rga_convert_copy_caps_field (output, input,
            "pixel-aspect-ratio");
        features = gst_caps_features_copy (gst_caps_get_features (templates,
                template_index));
        if (gst_caps_is_subset_structure_full (result, output, features)) {
          gst_structure_free (output);
          gst_caps_features_free (features);
        } else {
          gst_caps_append_structure_full (result, output, features);
        }
      }
    }
  }
  gst_caps_unref (templates);

  if (filter) {
    GstCaps *intersection = gst_caps_intersect_full (filter, result,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (result);
    result = intersection;
  }
  GST_DEBUG_OBJECT (transform, "transformed %" GST_PTR_FORMAT " into %"
      GST_PTR_FORMAT, caps, result);
  return result;
}

static GstCaps *
gst_rga_convert_fixate_caps (GstBaseTransform * transform,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstRgaConvert *self = GST_RGA_CONVERT (transform);
  GstVideoInfo info;
  GstRgaMemoryKind memory;
  GstStructure *output;
  GstRgaRotation rotation;
  guint crop_x;
  guint crop_y;
  guint crop_w;
  guint crop_h;
  guint width;
  guint height;

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);
  output = gst_caps_get_structure (othercaps, 0);

  if (gst_rga_convert_info_from_caps (caps, &info, &memory)) {
    const gchar *format = gst_video_format_to_string
        (GST_VIDEO_INFO_FORMAT (&info));

#if GST_CHECK_VERSION(1, 24, 0)
    if (gst_rga_convert_memory_kind (othercaps, 0) ==
        GST_RGA_MEMORY_DMA_DRM) {
      guint32 fourcc = gst_video_dma_drm_fourcc_from_format
          (GST_VIDEO_INFO_FORMAT (&info));
      gchar *drm_format = gst_video_dma_drm_fourcc_to_string (fourcc, 0);

      if (drm_format) {
        gst_structure_fixate_field_string (output, "drm-format", drm_format);
        g_free (drm_format);
      }
    } else {
#endif
      if (format)
        gst_structure_fixate_field_string (output, "format", format);
#if GST_CHECK_VERSION(1, 24, 0)
    }
#endif

    g_mutex_lock (&self->lock);
    rotation = self->rotation;
    crop_x = self->crop_x;
    crop_y = self->crop_y;
    crop_w = self->crop_w;
    crop_h = self->crop_h;
    g_mutex_unlock (&self->lock);

    width = GST_VIDEO_INFO_WIDTH (&info);
    height = GST_VIDEO_INFO_HEIGHT (&info);
    if (direction == GST_PAD_SINK) {
      if (crop_x < width)
        width = crop_w ? crop_w : width - crop_x;
      if (crop_y < height)
        height = crop_h ? crop_h : height - crop_y;
    }
    if (rotation == GST_RGA_ROTATION_90 ||
        rotation == GST_RGA_ROTATION_270) {
      guint swap = width;
      width = height;
      height = swap;
    }
    gst_structure_fixate_field_nearest_int (output, "width", width);
    gst_structure_fixate_field_nearest_int (output, "height", height);
  }
  return gst_caps_fixate (othercaps);
}

static gboolean
gst_rga_convert_set_caps (GstBaseTransform * transform, GstCaps * input_caps,
    GstCaps * output_caps)
{
  GstRgaConvert *self = GST_RGA_CONVERT (transform);
  GstVideoInfo input_info;
  GstVideoInfo output_info;
  GstRgaMemoryKind input_memory;
  GstRgaMemoryKind output_memory;

  if (!gst_rga_convert_info_from_caps (input_caps, &input_info,
          &input_memory) ||
      !gst_rga_convert_info_from_caps (output_caps, &output_info,
          &output_memory)) {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION,
        ("rgaconvert received unsupported or non-linear caps"),
        ("input=%" GST_PTR_FORMAT " output=%" GST_PTR_FORMAT, input_caps,
            output_caps));
    return FALSE;
  }

  g_mutex_lock (&self->lock);
  self->in_info = input_info;
  self->out_info = output_info;
  self->in_memory = input_memory;
  self->out_memory = output_memory;
  self->have_caps = TRUE;
  g_mutex_unlock (&self->lock);
  gst_base_transform_set_passthrough (transform, FALSE);
  return TRUE;
}

static gboolean
gst_rga_convert_get_unit_size (GstBaseTransform * transform, GstCaps * caps,
    gsize * size)
{
  GstVideoInfo info;
  GstRgaMemoryKind memory;

  (void) transform;
  if (!gst_rga_convert_info_from_caps (caps, &info, &memory))
    return FALSE;
  *size = GST_VIDEO_INFO_SIZE (&info);
  return TRUE;
}

static gboolean
gst_rga_convert_propose_allocation (GstBaseTransform * transform,
    GstQuery * decide_query, GstQuery * query)
{
  GstRgaConvert *self = GST_RGA_CONVERT (transform);
  GstVideoAlignment alignment;
  GstVideoInfo info;
  GstVideoInfo aligned;
  GstRgaMemoryKind memory;
  GstBufferPool *pool;
  GstStructure *params;
  GstCaps *caps;
  gboolean proposed_pool = FALSE;
  gboolean parent_result;
  guint size;

  gst_query_parse_allocation (query, &caps, NULL);
  if (!caps || !gst_rga_convert_info_from_caps (caps, &info, &memory) ||
      !gst_rga_video_info_align (&info, &aligned, &alignment))
    return FALSE;

  params = gst_structure_new ("video-meta",
      "padding-top", G_TYPE_UINT, alignment.padding_top,
      "padding-bottom", G_TYPE_UINT, alignment.padding_bottom,
      "padding-left", G_TYPE_UINT, alignment.padding_left,
      "padding-right", G_TYPE_UINT, alignment.padding_right, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, params);
  gst_structure_free (params);

  pool = gst_rga_convert_create_pool (self, caps, &info, &size);
  if (pool) {
    GstAllocator *allocator = gst_rga_convert_get_allocator (self);

    gst_query_add_allocation_pool (query, pool, size, 2, 0);
    proposed_pool = TRUE;
    if (allocator) {
      gst_query_add_allocation_param (query, allocator, NULL);
      gst_object_unref (allocator);
    }
    gst_object_unref (pool);
  }

  parent_result = GST_BASE_TRANSFORM_CLASS (parent_class)->propose_allocation
      (transform, decide_query, query);
  return proposed_pool || parent_result;
}

static gboolean
gst_rga_convert_decide_allocation (GstBaseTransform * transform,
    GstQuery * query)
{
  GstRgaConvert *self = GST_RGA_CONVERT (transform);
  GstCaps *caps;
  GstVideoInfo info;
  GstRgaMemoryKind memory;
  guint selected = G_MAXUINT;
  guint i;

  gst_query_parse_allocation (query, &caps, NULL);
  if (!caps || !gst_rga_convert_info_from_caps (caps, &info, &memory))
    return FALSE;

  for (i = 0; i < gst_query_get_n_allocation_pools (query); i++) {
    GstBufferPool *pool;
    guint size;
    guint min;
    guint max;

    gst_query_parse_nth_allocation_pool (query, i, &pool, &size, &min, &max);
    if (gst_rga_buffer_pool_uses_dmabuf (pool)) {
      selected = i;
      break;
    }
  }

  if (selected != G_MAXUINT && selected != 0) {
    GstBufferPool *first_pool;
    GstBufferPool *selected_pool;
    guint first_size;
    guint first_min;
    guint first_max;
    guint selected_size;
    guint selected_min;
    guint selected_max;

    gst_query_parse_nth_allocation_pool (query, 0, &first_pool, &first_size,
        &first_min, &first_max);
    gst_query_parse_nth_allocation_pool (query, selected, &selected_pool,
        &selected_size, &selected_min, &selected_max);
    gst_query_set_nth_allocation_pool (query, 0, selected_pool, selected_size,
        selected_min, selected_max);
    gst_query_set_nth_allocation_pool (query, selected, first_pool, first_size,
        first_min, first_max);
  } else if (selected == G_MAXUINT && memory != GST_RGA_MEMORY_SYSTEM) {
    GstBufferPool *pool;
    guint size;

    pool = gst_rga_convert_create_pool (self, caps, &info, &size);
    if (!pool)
      return FALSE;
    if (gst_query_get_n_allocation_pools (query) == 0)
      gst_query_add_allocation_pool (query, pool, size, 2, 0);
    else
      gst_query_set_nth_allocation_pool (query, 0, pool, size, 2, 0);
    gst_object_unref (pool);
  }

  if (memory != GST_RGA_MEMORY_SYSTEM &&
      gst_query_get_n_allocation_params (query) == 0 &&
      gst_query_get_n_allocation_pools (query) > 0) {
    GstAllocationParams params;
    GstAllocator *allocator = NULL;
    GstBufferPool *pool;
    GstStructure *config;

    gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_allocator (config, &allocator, &params);
    if (allocator)
      gst_query_add_allocation_param (query, allocator, &params);
    gst_structure_free (config);
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->decide_allocation (transform,
      query);
}

static gboolean
gst_rga_convert_transform_meta (GstBaseTransform * transform,
    GstBuffer * output, GstMeta * meta, GstBuffer * input)
{
  if (meta->info->api == GST_VIDEO_META_API_TYPE ||
      meta->info->api == GST_VIDEO_CROP_META_API_TYPE)
    return FALSE;
  return GST_BASE_TRANSFORM_CLASS (parent_class)->transform_meta (transform,
      output, meta, input);
}

static gint
gst_rga_convert_usage (GstRgaRotation rotation, gboolean hflip,
    gboolean vflip)
{
  gint usage = 0;

  switch (rotation) {
    case GST_RGA_ROTATION_90:
      usage |= IM_HAL_TRANSFORM_ROT_90;
      break;
    case GST_RGA_ROTATION_180:
      usage |= IM_HAL_TRANSFORM_ROT_180;
      break;
    case GST_RGA_ROTATION_270:
      usage |= IM_HAL_TRANSFORM_ROT_270;
      break;
    default:
      break;
  }
  if (hflip && vflip)
    usage |= IM_HAL_TRANSFORM_FLIP_H_V;
  else if (hflip)
    usage |= IM_HAL_TRANSFORM_FLIP_H;
  else if (vflip)
    usage |= IM_HAL_TRANSFORM_FLIP_V;
  return usage;
}

static GstFlowReturn
gst_rga_convert_transform (GstBaseTransform * transform, GstBuffer * input,
    GstBuffer * output)
{
  GstRgaConvert *self = GST_RGA_CONVERT (transform);
  GstVideoInfo input_info;
  GstVideoInfo output_info;
  GstRgaVideoLayout input_layout = { 0, };
  GstRgaVideoLayout output_layout = { 0, };
  GstMppRgaIm2dRequest request = { 0, };
  GstMppRgaBackend *backend;
  GstBuffer *input_staging = NULL;
  GstBuffer *output_staging = NULL;
  GstBuffer *rga_input = input;
  GstBuffer *rga_output = output;
  GstRgaRotation rotation;
  gboolean hflip;
  gboolean vflip;
  guint core_mask;
  gint priority;
  guint crop_x;
  guint crop_y;
  guint crop_w;
  guint crop_h;
  guint effective_crop_w;
  guint effective_crop_h;
  gboolean allow_cpu_copy;
  gboolean used_cpu_copy = FALSE;
  const gchar *reason = NULL;
  GstMppRgaResult result;
  gint input_fd;
  gint output_fd;

  g_mutex_lock (&self->lock);
  if (!self->have_caps) {
    g_mutex_unlock (&self->lock);
    return gst_rga_convert_not_negotiated (self,
        "input and output caps are not configured", FALSE);
  }
  input_info = self->in_info;
  output_info = self->out_info;
  backend = self->backend;
  rotation = self->rotation;
  hflip = self->hflip;
  vflip = self->vflip;
  core_mask = self->core_mask;
  priority = self->priority;
  crop_x = self->crop_x;
  crop_y = self->crop_y;
  crop_w = self->crop_w;
  crop_h = self->crop_h;
  g_mutex_unlock (&self->lock);

  if (!gst_rga_convert_layout_from_buffer (input, &input_info, &input_layout,
          &reason) ||
      !gst_rga_convert_layout_from_buffer (output, &output_info, &output_layout,
          &reason) ||
      !gst_rga_convert_output_geometry_valid (&output_layout, &reason))
    return gst_rga_convert_not_negotiated (self, reason, TRUE);

  effective_crop_w = crop_w ? crop_w : input_layout.width -
      MIN (crop_x, input_layout.width);
  effective_crop_h = crop_h ? crop_h : input_layout.height -
      MIN (crop_y, input_layout.height);
  if (!gst_rga_convert_crop_valid (&input_layout, crop_x, crop_y,
          effective_crop_w, effective_crop_h, &reason))
    return gst_rga_convert_not_negotiated (self, reason, TRUE);

  allow_cpu_copy = gst_mpp_cpu_copy_allowed ();
  if (!gst_rga_convert_get_dmabuf_fd (input, &input_fd, &reason)) {
    if (!allow_cpu_copy)
      return gst_rga_convert_not_negotiated (self,
          "system-memory input is disabled; set GST_MPP_ALLOW_CPU_COPY=1 for debug staging",
          FALSE);
    input_staging = gst_rga_convert_new_staging_buffer (self, &input_layout);
    if (!input_staging ||
        !gst_rga_convert_copy_to_staging (input, input_staging,
            input_layout.required_size)) {
      reason = "system-memory input could not be copied into a DMA-BUF";
      goto refuse;
    }
    rga_input = input_staging;
    used_cpu_copy = TRUE;
    if (!gst_rga_convert_get_dmabuf_fd (rga_input, &input_fd, &reason))
      goto refuse;
  }

  if (!gst_rga_convert_get_dmabuf_fd (output, &output_fd, &reason)) {
    if (!allow_cpu_copy) {
      reason =
          "system-memory output is disabled; set GST_MPP_ALLOW_CPU_COPY=1 for debug staging";
      goto refuse;
    }
    output_staging = gst_rga_convert_new_staging_buffer (self, &output_layout);
    if (!output_staging) {
      reason = "system-memory output has no DMA-BUF staging allocation";
      goto refuse;
    }
    rga_output = output_staging;
    used_cpu_copy = TRUE;
    if (!gst_rga_convert_get_dmabuf_fd (rga_output, &output_fd, &reason))
      goto refuse;
  }

  if (!gst_buffer_get_video_meta (output))
    gst_buffer_add_video_meta_full (output, GST_VIDEO_FRAME_FLAG_NONE,
        output_layout.format, output_layout.width, output_layout.height,
        output_layout.n_planes, output_layout.offset, output_layout.stride);

  request.src_fd = input_fd;
  request.src_width = input_layout.width;
  request.src_height = input_layout.height;
  request.src_wstride = input_layout.wstride;
  request.src_hstride = input_layout.hstride;
  request.src_format = input_layout.rga_format;
  request.dst_fd = output_fd;
  request.dst_width = output_layout.width;
  request.dst_height = output_layout.height;
  request.dst_wstride = output_layout.wstride;
  request.dst_hstride = output_layout.hstride;
  request.dst_format = output_layout.rga_format;
  request.src_x = crop_x;
  request.src_y = crop_y;
  request.src_rect_width = effective_crop_w;
  request.src_rect_height = effective_crop_h;
  request.dst_rect_width = output_layout.width;
  request.dst_rect_height = output_layout.height;
  request.usage = gst_rga_convert_usage (rotation, hflip, vflip);
  request.core_mask = core_mask;
  request.priority = priority;

  result = gst_mpp_rga_backend_process (backend, GST_MPP_RGA_OP_CONVERT,
      input_layout.format, output_layout.format, &request);
  if (result != GST_MPP_RGA_SUCCESS) {
    reason = result == GST_MPP_RGA_TUPLE_DEMOTED ?
        "the conversion tuple is temporarily demoted" :
        "the trial-verified RGA backend rejected the conversion";
    goto refuse;
  }

  if (output_staging &&
      !gst_rga_convert_copy_from_staging (output_staging, output,
          output_layout.required_size)) {
    reason = "DMA-BUF output could not be copied into system memory";
    goto refuse;
  }
  if (used_cpu_copy)
    gst_mpp_conversion_stats_fallback (gst_mpp_conversion_stats_get
        (G_OBJECT (self)));

  gst_clear_buffer (&input_staging);
  gst_clear_buffer (&output_staging);
  return GST_FLOW_OK;

refuse:
  gst_clear_buffer (&input_staging);
  gst_clear_buffer (&output_staging);
  return gst_rga_convert_not_negotiated (self, reason, FALSE);
}

static void
gst_rga_convert_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRgaConvert *self = GST_RGA_CONVERT (object);
  gboolean reconfigure = FALSE;

  g_mutex_lock (&self->lock);
  switch (prop_id) {
    case PROP_ROTATION:
      self->rotation = g_value_get_enum (value);
      reconfigure = TRUE;
      break;
    case PROP_HFLIP:
      self->hflip = g_value_get_boolean (value);
      break;
    case PROP_VFLIP:
      self->vflip = g_value_get_boolean (value);
      break;
    case PROP_CORE_MASK:
      self->core_mask = g_value_get_flags (value);
      break;
    case PROP_PRIORITY:
      self->priority = g_value_get_int (value);
      break;
    case PROP_CROP_X:
      self->crop_x = g_value_get_uint (value);
      reconfigure = TRUE;
      break;
    case PROP_CROP_Y:
      self->crop_y = g_value_get_uint (value);
      reconfigure = TRUE;
      break;
    case PROP_CROP_W:
      self->crop_w = g_value_get_uint (value);
      reconfigure = TRUE;
      break;
    case PROP_CROP_H:
      self->crop_h = g_value_get_uint (value);
      reconfigure = TRUE;
      break;
    default:
      g_mutex_unlock (&self->lock);
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
  }
  g_mutex_unlock (&self->lock);
  if (reconfigure)
    gst_base_transform_reconfigure_src (GST_BASE_TRANSFORM (self));
}

static void
gst_rga_convert_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstRgaConvert *self = GST_RGA_CONVERT (object);

  if (prop_id == PROP_CONVERSION_FALLBACK_FRAMES ||
      prop_id == PROP_CONVERSION_DROPPED_FRAMES ||
      prop_id == PROP_LAYOUT_REJECTIONS) {
    GstMppConversionStatsSnapshot stats;

    gst_mpp_conversion_stats_snapshot (gst_mpp_conversion_stats_get (object),
        &stats);
    if (prop_id == PROP_CONVERSION_FALLBACK_FRAMES)
      g_value_set_uint64 (value, stats.fallback_frames);
    else if (prop_id == PROP_CONVERSION_DROPPED_FRAMES)
      g_value_set_uint64 (value, stats.dropped_frames);
    else
      g_value_set_uint64 (value, stats.layout_rejections);
    return;
  }

  g_mutex_lock (&self->lock);
  switch (prop_id) {
    case PROP_ROTATION:
      g_value_set_enum (value, self->rotation);
      break;
    case PROP_HFLIP:
      g_value_set_boolean (value, self->hflip);
      break;
    case PROP_VFLIP:
      g_value_set_boolean (value, self->vflip);
      break;
    case PROP_CORE_MASK:
      g_value_set_flags (value, self->core_mask);
      break;
    case PROP_PRIORITY:
      g_value_set_int (value, self->priority);
      break;
    case PROP_CROP_X:
      g_value_set_uint (value, self->crop_x);
      break;
    case PROP_CROP_Y:
      g_value_set_uint (value, self->crop_y);
      break;
    case PROP_CROP_W:
      g_value_set_uint (value, self->crop_w);
      break;
    case PROP_CROP_H:
      g_value_set_uint (value, self->crop_h);
      break;
    default:
      g_mutex_unlock (&self->lock);
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
  }
  g_mutex_unlock (&self->lock);
}

static GstStateChangeReturn
gst_rga_convert_change_state (GstElement * element,
    GstStateChange transition)
{
  GstRgaConvert *self = GST_RGA_CONVERT (element);
  GstMppRgaBackend *backend;

  if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
    g_mutex_lock (&self->lock);
    backend = self->backend;
    g_mutex_unlock (&self->lock);
    if (!gst_mpp_rga_backend_init (backend)) {
      GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
          ("rgaconvert cannot enter READY: trial-verified RGA backend unavailable"),
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
  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_rga_convert_finalize (GObject * object)
{
  GstRgaConvert *self = GST_RGA_CONVERT (object);

  gst_clear_object (&self->allocator);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

void
gst_rga_convert_set_backend_for_test (GstRgaConvert * self,
    GstMppRgaBackend * backend)
{
  g_return_if_fail (GST_IS_RGA_CONVERT (self));
  g_return_if_fail (backend != NULL);
  g_mutex_lock (&self->lock);
  self->backend = backend;
  g_mutex_unlock (&self->lock);
}

void
gst_rga_convert_set_allocator_for_test (GstRgaConvert * self,
    GstAllocator * allocator)
{
  g_return_if_fail (GST_IS_RGA_CONVERT (self));
  g_return_if_fail (allocator == NULL || GST_IS_DMABUF_ALLOCATOR (allocator));
  g_mutex_lock (&self->lock);
  gst_object_replace ((GstObject **) & self->allocator,
      (GstObject *) allocator);
  g_mutex_unlock (&self->lock);
}

static void
gst_rga_convert_init (GstRgaConvert * self)
{
  g_mutex_init (&self->lock);
  gst_video_info_init (&self->in_info);
  gst_video_info_init (&self->out_info);
  self->backend = gst_mpp_rga_backend_get_default ();
  self->rotation = GST_RGA_ROTATION_0;
  self->core_mask = GST_RGA_CORE_AUTO;
  gst_mpp_conversion_stats_attach (G_OBJECT (self));
  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (self), FALSE);
  gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (self), FALSE);
  gst_base_transform_set_qos_enabled (GST_BASE_TRANSFORM (self), TRUE);
}

static void
gst_rga_convert_class_init (GstRgaConvertClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (rga_convert_debug, "rgaconvert", 0,
      "Rockchip RGA video converter");
  gobject_class->set_property = GST_DEBUG_FUNCPTR
      (gst_rga_convert_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR
      (gst_rga_convert_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_rga_convert_finalize);
  element_class->change_state = GST_DEBUG_FUNCPTR
      (gst_rga_convert_change_state);

  g_object_class_install_property (gobject_class, PROP_ROTATION,
      g_param_spec_enum ("rotation", "Rotation",
          "Clockwise rotation applied by RGA", GST_TYPE_RGA_ROTATION,
          GST_RGA_ROTATION_0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_HFLIP,
      g_param_spec_boolean ("hflip", "Horizontal flip",
          "Flip the output horizontally", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_VFLIP,
      g_param_spec_boolean ("vflip", "Vertical flip",
          "Flip the output vertically", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CORE_MASK,
      g_param_spec_flags ("core-mask", "Core mask",
          "RGA3_CORE0, RGA3_CORE1, RGA2, or AUTO scheduler selection",
          GST_TYPE_RGA_CORE_MASK, GST_RGA_CORE_AUTO,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PRIORITY,
      g_param_spec_int ("priority", "Priority", "RGA task priority", 0, 6, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CROP_X,
      g_param_spec_uint ("crop-x", "Crop X", "Input crop left edge", 0,
          G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CROP_Y,
      g_param_spec_uint ("crop-y", "Crop Y", "Input crop top edge", 0,
          G_MAXUINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CROP_W,
      g_param_spec_uint ("crop-w", "Crop width",
          "Input crop width (zero uses the remaining width)", 0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_CROP_H,
      g_param_spec_uint ("crop-h", "Crop height",
          "Input crop height (zero uses the remaining height)", 0, G_MAXUINT,
          0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_CONVERSION_FALLBACK_FRAMES,
      g_param_spec_uint64 ("conversion-fallback-frames",
          "Conversion fallback frames",
          "Frames using the debug CPU-copy staging path", 0, G_MAXUINT64, 0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_CONVERSION_DROPPED_FRAMES,
      g_param_spec_uint64 ("conversion-dropped-frames",
          "Conversion dropped frames",
          "Frames dropped because no 2D converter was available", 0,
          G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_LAYOUT_REJECTIONS,
      g_param_spec_uint64 ("layout-rejections", "Layout rejections",
          "Frames rejected for an unsupported conversion layout", 0,
          G_MAXUINT64, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (element_class,
      "Rockchip RGA video converter", "Filter/Converter/Video/Hardware",
      "Scale, crop, convert, rotate, and flip DMA-BUF video with librga im2d",
      "CERALIVE <contact@ceralive.tv>");
  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  transform_class->passthrough_on_same_caps = FALSE;
  transform_class->transform_caps = GST_DEBUG_FUNCPTR
      (gst_rga_convert_transform_caps);
  transform_class->fixate_caps = GST_DEBUG_FUNCPTR
      (gst_rga_convert_fixate_caps);
  transform_class->set_caps = GST_DEBUG_FUNCPTR (gst_rga_convert_set_caps);
  transform_class->get_unit_size = GST_DEBUG_FUNCPTR
      (gst_rga_convert_get_unit_size);
  transform_class->propose_allocation = GST_DEBUG_FUNCPTR
      (gst_rga_convert_propose_allocation);
  transform_class->decide_allocation = GST_DEBUG_FUNCPTR
      (gst_rga_convert_decide_allocation);
  transform_class->transform_meta = GST_DEBUG_FUNCPTR
      (gst_rga_convert_transform_meta);
  transform_class->transform = GST_DEBUG_FUNCPTR (gst_rga_convert_transform);
}
