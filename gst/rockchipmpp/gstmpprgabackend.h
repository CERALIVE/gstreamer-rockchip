/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_MPP_RGA_BACKEND_H__
#define __GST_MPP_RGA_BACKEND_H__

#include <gst/video/video.h>
#include "gstmppconversionstats.h"      // IWYU pragma: export
#ifdef HAVE_RGA
#include <rga/RgaApi.h>
#endif

G_BEGIN_DECLS;
#define GST_MPP_RGA_DEMOTION_THRESHOLD 8
#define GST_MPP_RGA_RETRY_INTERVAL 8
typedef enum
{
  GST_MPP_RGA_OP_ENCODE_CONVERT,
  GST_MPP_RGA_OP_DECODE_CONVERT,
  GST_MPP_RGA_OP_JPEG_CONVERT,
  GST_MPP_RGA_OP_CONVERT,
  GST_MPP_RGA_OP_COMPOSITOR_COPY,
  GST_MPP_RGA_OP_COMPOSITE,
} GstMppRgaOperation;

typedef enum
{
  GST_MPP_RGA_SUCCESS,
  GST_MPP_RGA_UNAVAILABLE,
  GST_MPP_RGA_TUPLE_DEMOTED,
  GST_MPP_RGA_BLIT_FAILED,
  GST_MPP_RGA_DEVICE_LOST,
  GST_MPP_RGA_LAYOUT_REJECTED,
  GST_MPP_RGA_NOT_SUPPORTED,
} GstMppRgaResult;

const gchar *gst_mpp_rga_operation_name (GstMppRgaOperation operation);
GstFlowReturn gst_mpp_rga_result_to_flow (GstMppRgaResult result);

#ifdef HAVE_RGA
typedef struct
{
  guint32 major;
  guint32 minor;
  guint32 revision;
  gchar string[16];
} GstMppRgaDriverVersion;

typedef struct
{
  gint src_fd;
  gint src_width;
  gint src_height;
  gint src_wstride;
  gint src_hstride;
  RgaSURF_FORMAT src_format;
  gint dst_fd;
  gint dst_width;
  gint dst_height;
  gint dst_wstride;
  gint dst_hstride;
  RgaSURF_FORMAT dst_format;
  gint src_x;
  gint src_y;
  gint src_rect_width;
  gint src_rect_height;
  gint dst_x;
  gint dst_y;
  gint dst_rect_width;
  gint dst_rect_height;
  gint usage;
  guint core_mask;
  gint priority;
  gint src_color_space_mode;
  gint dst_color_space_mode;
  GstVideoColorimetry colorspace_in;
  GstVideoColorimetry colorspace_out;
  gboolean csc_fallback;
  gint interp;
} GstMppRgaIm2dRequest;

typedef struct
{
  GstMppRgaIm2dRequest transform;
  gint pat_fd;
  gint pat_width;
  gint pat_height;
  gint pat_wstride;
  gint pat_hstride;
  RgaSURF_FORMAT pat_format;
  gint pat_x;
  gint pat_y;
  gint pat_rect_width;
  gint pat_rect_height;
  guint8 src_alpha;
  guint8 pat_alpha;
} GstMppRgaIm2dCompositeRequest;

typedef struct
{
  gboolean (*probe) (gpointer user_data, GstMppRgaDriverVersion * version,
      gint * error_number);
  gint (*init) (gpointer user_data);
  gint (*blit) (rga_info_t * src, rga_info_t * dst, gpointer user_data);
  gint (*process) (const GstMppRgaIm2dRequest * request, gpointer user_data);
  gint (*composite) (const GstMppRgaIm2dCompositeRequest * request,
      gpointer user_data);
  /* Capability is reported SEPARATELY from the entry point's presence: the
   * real backend always compiles process_async in, but an older runtime
   * resolves no improcessOpt and has nowhere to return a release fence. */
  gboolean (*async_supported) (gpointer user_data);
  gint (*process_async) (const GstMppRgaIm2dRequest * request,
      gint * release_fence_fd, gpointer user_data);
} GstMppRgaBackendOps;

typedef struct _GstMppRgaBackend GstMppRgaBackend;

gboolean gst_mpp_rga_request_set_colorimetry (GstMppRgaIm2dRequest * request,
    const GstVideoInfo * input, const GstVideoInfo * output);
gboolean gst_mpp_rga_composite_set_colorimetry (GstMppRgaIm2dCompositeRequest *
    request, const GstVideoInfo * accumulator, const GstVideoInfo * overlay);
void gst_mpp_rga_count_csc_fallback (const GstMppRgaIm2dRequest * request,
    GstMppConversionStats * stats);

GstMppRgaBackend *gst_mpp_rga_backend_new (const GstMppRgaBackendOps * ops,
    gpointer user_data);
void gst_mpp_rga_backend_free (GstMppRgaBackend * backend);
gboolean gst_mpp_rga_backend_init (GstMppRgaBackend * backend);
GstMppRgaBackend *gst_mpp_rga_backend_get_default (void);
GstMppRgaResult gst_mpp_rga_backend_blit (GstMppRgaBackend * backend,
    GstMppRgaOperation operation, GstVideoFormat in_format,
    GstVideoFormat out_format, rga_info_t * src, rga_info_t * dst);
GstMppRgaResult gst_mpp_rga_backend_process (GstMppRgaBackend * backend,
    GstMppRgaOperation operation, GstVideoFormat in_format,
    GstVideoFormat out_format, const GstMppRgaIm2dRequest * request);
gboolean gst_mpp_rga_backend_supports_async (GstMppRgaBackend * backend);
GstMppRgaResult gst_mpp_rga_backend_process_async (GstMppRgaBackend * backend,
    GstMppRgaOperation operation, GstVideoFormat in_format,
    GstVideoFormat out_format, const GstMppRgaIm2dRequest * request,
    gint * release_fence_fd);
gboolean gst_mpp_rga_fence_wait (gint release_fence_fd, gint timeout_ms);
GstMppRgaResult gst_mpp_rga_backend_composite (GstMppRgaBackend * backend,
    GstVideoFormat in_format, GstVideoFormat out_format,
    const GstMppRgaIm2dCompositeRequest * request);
guint gst_mpp_rga_backend_tuple_failures (GstMppRgaBackend * backend,
    GstMppRgaOperation operation, GstVideoFormat in_format,
    GstVideoFormat out_format);
#endif

G_END_DECLS;
#endif
