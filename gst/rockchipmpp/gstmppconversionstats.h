/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_MPP_CONVERSION_STATS_H__
#define __GST_MPP_CONVERSION_STATS_H__

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS;

typedef struct
{
  GMutex lock;
  guint64 fallback_frames;
  guint64 dropped_frames;
  guint64 layout_rejections;
  guint64 csc_fallback_frames;
  gboolean csc_warned;
} GstMppConversionStats;

typedef struct
{
  guint64 fallback_frames;
  guint64 dropped_frames;
  guint64 layout_rejections;
  guint64 csc_fallback_frames;
} GstMppConversionStatsSnapshot;

void gst_mpp_conversion_stats_init (GstMppConversionStats * stats);
void gst_mpp_conversion_stats_clear (GstMppConversionStats * stats);
void gst_mpp_conversion_stats_attach (GObject * object);
GstMppConversionStats *gst_mpp_conversion_stats_get (GObject * object);
void gst_mpp_conversion_stats_layout_rejected (GstMppConversionStats * stats);
void gst_mpp_conversion_stats_dropped (GstMppConversionStats * stats);
void gst_mpp_conversion_stats_fallback (GstMppConversionStats * stats);
void gst_mpp_conversion_stats_snapshot (GstMppConversionStats * stats,
    GstMppConversionStatsSnapshot * snapshot);
gboolean gst_mpp_cpu_copy_allowed (void);
gboolean gst_mpp_conversion_finish_without_rga (GstMppConversionStats * stats,
    gboolean cpu_compatible, gboolean copy_succeeded);

G_END_DECLS;

#endif
