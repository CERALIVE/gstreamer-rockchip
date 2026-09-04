/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_RGA_UTIL_H__
#define __GST_RGA_UTIL_H__

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <gst/video/gstvideopool.h>

G_BEGIN_DECLS;

#define GST_RGA_DMA_HEAP_ALIGNMENT 16

GstAllocator *gst_rga_dma_heap_allocator_new (void);
gboolean gst_rga_video_info_align (const GstVideoInfo * input,
    GstVideoInfo * aligned, GstVideoAlignment * alignment);
GstBufferPool *gst_rga_dma_heap_pool_new (GstObject * owner,
    GstAllocator * allocator, GstCaps * caps, const GstVideoInfo * info,
    guint * size);
gboolean gst_rga_buffer_pool_uses_dmabuf (GstBufferPool * pool);

G_END_DECLS;

#endif
