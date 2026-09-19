/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <string.h>

#include "gstmppconversionstats.h"

static GQuark
gst_mpp_conversion_stats_quark (void)
{
  return g_quark_from_static_string ("gst-mpp-conversion-stats");
}

static void
gst_mpp_conversion_stats_free (gpointer data)
{
  GstMppConversionStats *stats = data;
  gst_mpp_conversion_stats_clear (stats);
  g_free (stats);
}

void
gst_mpp_conversion_stats_init (GstMppConversionStats * stats)
{
  memset (stats, 0, sizeof (*stats));
  g_mutex_init (&stats->lock);
}

void
gst_mpp_conversion_stats_clear (GstMppConversionStats * stats)
{
  g_mutex_clear (&stats->lock);
}

void
gst_mpp_conversion_stats_attach (GObject * object)
{
  GstMppConversionStats *stats = g_new0 (GstMppConversionStats, 1);
  gst_mpp_conversion_stats_init (stats);
  g_object_set_qdata_full (object, gst_mpp_conversion_stats_quark (), stats,
      gst_mpp_conversion_stats_free);
}

GstMppConversionStats *
gst_mpp_conversion_stats_get (GObject * object)
{
  return g_object_get_qdata (object, gst_mpp_conversion_stats_quark ());
}

void
gst_mpp_conversion_stats_layout_rejected (GstMppConversionStats * stats)
{
  g_mutex_lock (&stats->lock);
  stats->layout_rejections++;
  g_mutex_unlock (&stats->lock);
}

void
gst_mpp_conversion_stats_dropped (GstMppConversionStats * stats)
{
  g_mutex_lock (&stats->lock);
  stats->dropped_frames++;
  g_mutex_unlock (&stats->lock);
}

void
gst_mpp_conversion_stats_fallback (GstMppConversionStats * stats)
{
  g_mutex_lock (&stats->lock);
  stats->fallback_frames++;
  g_mutex_unlock (&stats->lock);
}

void
gst_mpp_conversion_stats_snapshot (GstMppConversionStats * stats,
    GstMppConversionStatsSnapshot * snapshot)
{
  g_mutex_lock (&stats->lock);
  snapshot->fallback_frames = stats->fallback_frames;
  snapshot->dropped_frames = stats->dropped_frames;
  snapshot->layout_rejections = stats->layout_rejections;
  snapshot->csc_fallback_frames = stats->csc_fallback_frames;
  g_mutex_unlock (&stats->lock);
}

gboolean
gst_mpp_cpu_copy_allowed (void)
{
  return g_strcmp0 (g_getenv ("GST_MPP_ALLOW_CPU_COPY"), "1") == 0;
}

gboolean
gst_mpp_conversion_finish_without_rga (GstMppConversionStats * stats,
    gboolean cpu_compatible, gboolean copy_succeeded)
{
  if (cpu_compatible && gst_mpp_cpu_copy_allowed () && copy_succeeded) {
    gst_mpp_conversion_stats_fallback (stats);
    return TRUE;
  }

  gst_mpp_conversion_stats_dropped (stats);
  return FALSE;
}
