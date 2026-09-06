/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "gstrgautil.h"

#define GST_ALLOCATOR_RGA_DMA_HEAP "rga-dma-heap"

GST_DEBUG_CATEGORY_STATIC (rga_util_debug);
#define GST_CAT_DEFAULT rga_util_debug

typedef struct _GstRgaDmaHeapAllocator GstRgaDmaHeapAllocator;
typedef struct _GstRgaDmaHeapAllocatorClass GstRgaDmaHeapAllocatorClass;

struct _GstRgaDmaHeapAllocator
{
  GstDmaBufAllocator parent;
  gint heap_fd;
};

struct _GstRgaDmaHeapAllocatorClass
{
  GstDmaBufAllocatorClass parent_class;
};

#define GST_TYPE_RGA_DMA_HEAP_ALLOCATOR \
  (gst_rga_dma_heap_allocator_get_type ())
#define GST_RGA_DMA_HEAP_ALLOCATOR(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_RGA_DMA_HEAP_ALLOCATOR, \
      GstRgaDmaHeapAllocator))

G_DEFINE_TYPE (GstRgaDmaHeapAllocator, gst_rga_dma_heap_allocator,
    GST_TYPE_DMABUF_ALLOCATOR);

static GstMemory *
gst_rga_dma_heap_allocator_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  GstRgaDmaHeapAllocator *self = GST_RGA_DMA_HEAP_ALLOCATOR (allocator);
  struct dma_heap_allocation_data data = { 0, };
  GstMemory *memory;
  gsize prefix = params ? params->prefix : 0;
  gsize padding = params ? params->padding : 0;
  gsize total;
  glong page_size;

  if (G_MAXSIZE - prefix < size || G_MAXSIZE - prefix - size < padding)
    return NULL;

  total = prefix + size + padding;
  page_size = sysconf (_SC_PAGESIZE);
  if (page_size <= 0)
    page_size = 4096;
  data.len = GST_ROUND_UP_N (total, (gsize) page_size);
  data.fd_flags = O_RDWR | O_CLOEXEC;

  if (ioctl (self->heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
    GST_ERROR_OBJECT (self, "dma-heap allocation of %" G_GSIZE_FORMAT
        " bytes failed: %s", total, g_strerror (errno));
    return NULL;
  }

  memory = gst_dmabuf_allocator_alloc (allocator, data.fd, data.len);
  if (!memory) {
    close (data.fd);
    return NULL;
  }

  gst_memory_resize (memory, prefix, size);
  return memory;
}

static void
gst_rga_dma_heap_allocator_finalize (GObject * object)
{
  GstRgaDmaHeapAllocator *self = GST_RGA_DMA_HEAP_ALLOCATOR (object);

  if (self->heap_fd >= 0)
    close (self->heap_fd);
  G_OBJECT_CLASS (gst_rga_dma_heap_allocator_parent_class)->finalize (object);
}

static void
gst_rga_dma_heap_allocator_class_init (GstRgaDmaHeapAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class = GST_ALLOCATOR_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (rga_util_debug, "rgautil", 0,
      "Rockchip RGA allocation helpers");
  allocator_class->alloc = GST_DEBUG_FUNCPTR
      (gst_rga_dma_heap_allocator_alloc);
  gobject_class->finalize = GST_DEBUG_FUNCPTR
      (gst_rga_dma_heap_allocator_finalize);
}

static void
gst_rga_dma_heap_allocator_init (GstRgaDmaHeapAllocator * self)
{
  GstAllocator *allocator = GST_ALLOCATOR_CAST (self);

  self->heap_fd = -1;
  allocator->mem_type = GST_ALLOCATOR_RGA_DMA_HEAP;
  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

GstAllocator *
gst_rga_dma_heap_allocator_new (void)
{
  static const gchar *heap_paths[] = {
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/system",
  };
  GstRgaDmaHeapAllocator *allocator;
  guint i;

  allocator = g_object_new (GST_TYPE_RGA_DMA_HEAP_ALLOCATOR, NULL);
  gst_object_ref_sink (allocator);
  for (i = 0; i < G_N_ELEMENTS (heap_paths); i++) {
    allocator->heap_fd = open (heap_paths[i], O_RDWR | O_CLOEXEC);
    if (allocator->heap_fd >= 0) {
      GST_INFO_OBJECT (allocator, "using dma-heap %s", heap_paths[i]);
      return GST_ALLOCATOR_CAST (allocator);
    }
  }

  GST_WARNING_OBJECT (allocator, "no usable system dma-heap: %s",
      g_strerror (errno));
  gst_object_unref (allocator);
  return NULL;
}

gboolean
gst_rga_video_info_align (const GstVideoInfo * input, GstVideoInfo * aligned,
    GstVideoAlignment * alignment)
{
  *aligned = *input;
  gst_video_alignment_reset (alignment);
  alignment->padding_right = GST_ROUND_UP_N (GST_VIDEO_INFO_WIDTH (input),
      GST_RGA_DMA_HEAP_ALIGNMENT) - GST_VIDEO_INFO_WIDTH (input);
  alignment->padding_bottom = GST_ROUND_UP_N (GST_VIDEO_INFO_HEIGHT (input),
      GST_RGA_DMA_HEAP_ALIGNMENT) - GST_VIDEO_INFO_HEIGHT (input);
  return gst_video_info_align (aligned, alignment);
}

GstBufferPool *
gst_rga_dma_heap_pool_new (GstObject * owner, GstAllocator * allocator,
    GstCaps * caps, const GstVideoInfo * info, guint * size)
{
  GstVideoAlignment alignment;
  GstVideoInfo aligned;
  GstBufferPool *pool;
  GstStructure *config;
  GstCaps *pool_caps;

  g_return_val_if_fail (GST_IS_OBJECT (owner), NULL);
  g_return_val_if_fail (GST_IS_DMABUF_ALLOCATOR (allocator), NULL);

  if (!gst_rga_video_info_align (info, &aligned, &alignment))
    return NULL;

  pool_caps = gst_video_info_to_caps (&aligned);
  gst_caps_set_features (pool_caps, 0,
      gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_DMABUF, NULL));
  pool = gst_video_buffer_pool_new ();
  config = gst_buffer_pool_get_config (pool);
  *size = GST_VIDEO_INFO_SIZE (&aligned);
  gst_buffer_pool_config_set_params (config, pool_caps, *size, 2, 0);
  gst_buffer_pool_config_set_allocator (config, allocator, NULL);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gst_buffer_pool_config_add_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
  gst_buffer_pool_config_set_video_alignment (config, &alignment);
  gst_caps_unref (pool_caps);

  if (!gst_buffer_pool_set_config (pool, config)) {
    GST_ERROR_OBJECT (owner, "failed to configure DMA-BUF pool for %"
        GST_PTR_FORMAT, caps);
    gst_object_unref (pool);
    return NULL;
  }
  return pool;
}

gboolean
gst_rga_buffer_pool_uses_dmabuf (GstBufferPool * pool)
{
  GstStructure *config;
  GstAllocator *allocator = NULL;
  GstAllocationParams params;
  gboolean uses_dmabuf;

  if (!pool)
    return FALSE;
  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_get_allocator (config, &allocator, &params);
  uses_dmabuf = allocator && GST_IS_DMABUF_ALLOCATOR (allocator);
  gst_structure_free (config);
  return uses_dmabuf;
}
