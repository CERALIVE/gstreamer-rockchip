/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_RGA_CONVERT_H__
#define __GST_RGA_CONVERT_H__

#include <gst/base/gstbasetransform.h>

#include "gstmpprgabackend.h"

G_BEGIN_DECLS;

typedef enum
{
  GST_RGA_ROTATION_0 = 0,
  GST_RGA_ROTATION_90 = 90,
  GST_RGA_ROTATION_180 = 180,
  GST_RGA_ROTATION_270 = 270,
} GstRgaRotation;

#define GST_TYPE_RGA_ROTATION (gst_rga_rotation_get_type ())
GType gst_rga_rotation_get_type (void);

typedef enum
{
  GST_RGA_CORE_AUTO = 0,
  GST_RGA_CORE_RGA3_CORE0 = 1 << 0,
  GST_RGA_CORE_RGA3_CORE1 = 1 << 1,
  GST_RGA_CORE_RGA2 = 1 << 2,
} GstRgaCoreMask;

#define GST_TYPE_RGA_CORE_MASK (gst_rga_core_mask_get_type ())
GType gst_rga_core_mask_get_type (void);

#define GST_TYPE_RGA_CONVERT (gst_rga_convert_get_type ())
G_DECLARE_FINAL_TYPE (GstRgaConvert, gst_rga_convert, GST, RGA_CONVERT,
    GstBaseTransform);

GstFlowReturn gst_rga_convert_release_pipelined (GstRgaConvert * self,
    GstBuffer * produced, gint release_fence_fd, GstBuffer ** outbuf);

void gst_rga_convert_set_backend_for_test (GstRgaConvert * self,
    GstMppRgaBackend * backend);
void gst_rga_convert_set_allocator_for_test (GstRgaConvert * self,
    GstAllocator * allocator);

G_END_DECLS;

#endif
