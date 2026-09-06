/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_RGA_COMPOSITOR_H__
#define __GST_RGA_COMPOSITOR_H__

#include <gst/video/gstvideoaggregator.h>

#include "gstmpprgabackend.h"

G_BEGIN_DECLS;

typedef enum
{
  GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_RIGHT,
  GST_RGA_COMPOSITOR_LAYOUT_PIP_TOP_LEFT,
  GST_RGA_COMPOSITOR_LAYOUT_PIP_BOTTOM_RIGHT,
  GST_RGA_COMPOSITOR_LAYOUT_PIP_BOTTOM_LEFT,
  GST_RGA_COMPOSITOR_LAYOUT_PBP_LEFT_RIGHT,
  GST_RGA_COMPOSITOR_LAYOUT_PBP_TOP_BOTTOM,
  GST_RGA_COMPOSITOR_LAYOUT_CUSTOM,
} GstRgaCompositorLayout;

#define GST_TYPE_RGA_COMPOSITOR_LAYOUT \
  (gst_rga_compositor_layout_get_type ())
GType gst_rga_compositor_layout_get_type (void);

#define GST_TYPE_RGA_COMPOSITOR_PAD (gst_rga_compositor_pad_get_type ())
G_DECLARE_FINAL_TYPE (GstRgaCompositorPad, gst_rga_compositor_pad, GST,
    RGA_COMPOSITOR_PAD, GstVideoAggregatorPad);

#define GST_TYPE_RGA_COMPOSITOR (gst_rga_compositor_get_type ())
G_DECLARE_FINAL_TYPE (GstRgaCompositor, gst_rga_compositor, GST,
    RGA_COMPOSITOR, GstVideoAggregator);

void gst_rga_compositor_set_backend_for_test (GstRgaCompositor * self,
    GstMppRgaBackend * backend);
void gst_rga_compositor_set_allocator_for_test (GstRgaCompositor * self,
    GstAllocator * allocator);

G_END_DECLS;

#endif
