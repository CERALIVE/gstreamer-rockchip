/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "gstmpprgabackend.h"
#include "gstmpprgatuple.h"

#ifdef HAVE_RGA
#ifdef GST_MPP_RGA_ENABLE_IM2D
#include <rga/im2d.h>
#endif

#define GST_MPP_RGA_DEVICE "/dev/rga"
#ifndef RGA_IOC_GET_DRVIER_VERSION
#define RGA_IOC_GET_DRVIER_VERSION _IOR ('r', 1, GstMppRgaDriverVersion)
#endif

GST_DEBUG_CATEGORY_STATIC (mpp_rga_backend_debug);
#define GST_CAT_DEFAULT mpp_rga_backend_debug

struct _GstMppRgaBackend
{
  GMutex lock;
  GstMppRgaBackendOps ops;
  gpointer user_data;
  GstMppRgaTupleTable *tuples;
  GstMppRgaDriverVersion version;
  gboolean initialized;
  gboolean available;
};

static gboolean
gst_mpp_rga_real_probe (gpointer user_data, GstMppRgaDriverVersion * version,
    gint * error_number)
{
  gint fd;
  gint ret;

  (void) user_data;
  fd = open (GST_MPP_RGA_DEVICE, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    *error_number = errno;
    return FALSE;
  }

  memset (version, 0, sizeof (*version));
  ret = ioctl (fd, RGA_IOC_GET_DRVIER_VERSION, version);
  *error_number = ret < 0 ? errno : 0;
  close (fd);
  return ret >= 0;
}

static gint
gst_mpp_rga_real_init (gpointer user_data)
{
  (void) user_data;
  return c_RkRgaInit ();
}

static gint
gst_mpp_rga_real_blit (rga_info_t * src, rga_info_t * dst, gpointer user_data)
{
  (void) user_data;
  return c_RkRgaBlit (src, dst, NULL);
}

#ifdef GST_MPP_RGA_ENABLE_IM2D
static gint
gst_mpp_rga_color_space (const GstVideoInfo * info)
{
  gboolean full = info->colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;

  if (!full && info->colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235)
    return -1;
  if (GST_VIDEO_INFO_IS_RGB (info))
    return full ? IM_RGB_FULL : -1;
  if (!GST_VIDEO_INFO_IS_YUV (info))
    return -1;

  switch (info->colorimetry.matrix) {
    case GST_VIDEO_COLOR_MATRIX_BT601:
      return full ? IM_YUV_BT601_FULL_RANGE : IM_YUV_BT601_LIMIT_RANGE;
    case GST_VIDEO_COLOR_MATRIX_BT709:
      return full ? IM_YUV_BT709_FULL_RANGE : IM_YUV_BT709_LIMIT_RANGE;
    default:
      return -1;
  }
}

gboolean
gst_mpp_rga_request_set_colorimetry (GstMppRgaIm2dRequest * request,
    const GstVideoInfo * input, const GstVideoInfo * output)
{
  gint src_mode;
  gint dst_mode;

  request->src_color_space_mode = IM_COLOR_SPACE_DEFAULT;
  request->dst_color_space_mode = IM_COLOR_SPACE_DEFAULT;
  if (GST_VIDEO_INFO_IS_RGB (input) == GST_VIDEO_INFO_IS_RGB (output) &&
      input->colorimetry.matrix == output->colorimetry.matrix &&
      input->colorimetry.range == output->colorimetry.range)
    return TRUE;

  src_mode = gst_mpp_rga_color_space (input);
  dst_mode = gst_mpp_rga_color_space (output);
  if (src_mode < 0 || dst_mode < 0)
    return FALSE;

  /* Directional modes belong on dst, even for YUV input. Avoid requiring
   * full-CSC hardware for the matrices the ordinary CSC units implement. */
  if (src_mode == IM_RGB_FULL) {
    switch (dst_mode) {
      case IM_YUV_BT601_LIMIT_RANGE:
        request->dst_color_space_mode = IM_RGB_TO_YUV_BT601_LIMIT;
        return TRUE;
      case IM_YUV_BT601_FULL_RANGE:
        request->dst_color_space_mode = IM_RGB_TO_YUV_BT601_FULL;
        return TRUE;
      case IM_YUV_BT709_LIMIT_RANGE:
        request->dst_color_space_mode = IM_RGB_TO_YUV_BT709_LIMIT;
        return TRUE;
      default:
        break;
    }
  } else if (dst_mode == IM_RGB_FULL) {
    switch (src_mode) {
      case IM_YUV_BT601_LIMIT_RANGE:
        request->dst_color_space_mode = IM_YUV_TO_RGB_BT601_LIMIT;
        return TRUE;
      case IM_YUV_BT601_FULL_RANGE:
        request->dst_color_space_mode = IM_YUV_TO_RGB_BT601_FULL;
        return TRUE;
      case IM_YUV_BT709_LIMIT_RANGE:
        request->dst_color_space_mode = IM_YUV_TO_RGB_BT709_LIMIT;
        return TRUE;
      default:
        break;
    }
  }

  request->src_color_space_mode = src_mode;
  request->dst_color_space_mode = dst_mode;
  return TRUE;
}

gboolean
gst_mpp_rga_composite_set_colorimetry (GstMppRgaIm2dCompositeRequest * request,
    const GstVideoInfo * accumulator, const GstVideoInfo * overlay)
{
  GstMppRgaIm2dRequest y2r = { 0, };
  GstMppRgaIm2dRequest r2y = { 0, };

  /* The YUV accumulator crosses RGB for blending, then returns to YUV.
   * Full-CSC endpoint modes cannot express this two-direction blend. */
  if (!gst_mpp_rga_request_set_colorimetry (&y2r, accumulator, overlay) ||
      !gst_mpp_rga_request_set_colorimetry (&r2y, overlay, accumulator) ||
      !(y2r.dst_color_space_mode & IM_YUV_TO_RGB_MASK) ||
      !(r2y.dst_color_space_mode & IM_RGB_TO_YUV_MASK))
    return FALSE;

  request->transform.src_color_space_mode = IM_COLOR_SPACE_DEFAULT;
  request->transform.dst_color_space_mode = y2r.dst_color_space_mode |
      r2y.dst_color_space_mode;
  return TRUE;
}

static gint
gst_mpp_rga_real_configure_im2d (guint core_mask, gint priority)
{
  IM_STATUS status;
  gint saved_errno;

  if (core_mask != 0) {
    errno = 0;
    status = imconfig (IM_CONFIG_SCHEDULER_CORE, core_mask);
    saved_errno = errno;
    if (status <= IM_STATUS_FAILED) {
      GST_WARNING
          ("imconfig scheduler-core=%u returned status=%d errno=%d (%s)",
          core_mask, status, saved_errno, g_strerror (saved_errno));
      errno = saved_errno;
      return status;
    }
  }

  errno = 0;
  status = imconfig (IM_CONFIG_PRIORITY, priority);
  saved_errno = errno;
  if (status <= IM_STATUS_FAILED)
    GST_WARNING ("imconfig priority=%d returned status=%d errno=%d (%s)",
        priority, status, saved_errno, g_strerror (saved_errno));
  errno = saved_errno;
  return status;
}

static gint
gst_mpp_rga_real_process (const GstMppRgaIm2dRequest * request,
    gpointer user_data)
{
  rga_buffer_t src;
  rga_buffer_t dst;
  rga_buffer_t pat = { 0, };
  im_rect src_rect = {
    request->src_x,
    request->src_y,
    request->src_rect_width,
    request->src_rect_height,
  };
  im_rect dst_rect = {
    request->dst_x,
    request->dst_y,
    request->dst_rect_width,
    request->dst_rect_height,
  };
  im_rect pat_rect = { 0, };
  IM_STATUS status;

  (void) user_data;
  status = gst_mpp_rga_real_configure_im2d (request->core_mask,
      request->priority);
  if (status <= IM_STATUS_FAILED)
    return status;

  src = wrapbuffer_fd (request->src_fd, request->src_width,
      request->src_height, request->src_format, request->src_wstride,
      request->src_hstride);
  dst = wrapbuffer_fd (request->dst_fd, request->dst_width,
      request->dst_height, request->dst_format, request->dst_wstride,
      request->dst_hstride);
  src.color_space_mode = request->src_color_space_mode;
  dst.color_space_mode = request->dst_color_space_mode;

  return improcess (src, dst, pat, src_rect, dst_rect, pat_rect,
      request->usage | IM_SYNC);
}

static gint
gst_mpp_rga_real_composite (const GstMppRgaIm2dCompositeRequest * request,
    gpointer user_data)
{
  const GstMppRgaIm2dRequest *transform = &request->transform;
  rga_buffer_t source;
  rga_buffer_t output;
  rga_buffer_t pat;
  im_rect source_rect = {
    transform->src_x,
    transform->src_y,
    transform->src_rect_width,
    transform->src_rect_height,
  };
  im_rect output_rect = {
    transform->dst_x,
    transform->dst_y,
    transform->dst_rect_width,
    transform->dst_rect_height,
  };
  im_rect pat_rect = {
    request->pat_x,
    request->pat_y,
    request->pat_rect_width,
    request->pat_rect_height,
  };
  IM_STATUS status;
  gint saved_errno;

  (void) user_data;
  status = gst_mpp_rga_real_configure_im2d (transform->core_mask,
      transform->priority);
  if (status <= IM_STATUS_FAILED)
    return status;

  source = wrapbuffer_fd (transform->src_fd, transform->src_width,
      transform->src_height, transform->src_format, transform->src_wstride,
      transform->src_hstride);
  source.global_alpha = request->src_alpha;
  output = wrapbuffer_fd (transform->dst_fd, transform->dst_width,
      transform->dst_height, transform->dst_format, transform->dst_wstride,
      transform->dst_hstride);
  pat = wrapbuffer_fd (request->pat_fd, request->pat_width,
      request->pat_height, request->pat_format, request->pat_wstride,
      request->pat_hstride);
  pat.global_alpha = request->pat_alpha;
  source.color_space_mode = transform->src_color_space_mode;
  output.color_space_mode = transform->dst_color_space_mode;

  errno = 0;
  status = improcess (source, output, pat, source_rect, output_rect, pat_rect,
      transform->usage | IM_SYNC);
  saved_errno = errno;
  if (status <= IM_STATUS_FAILED)
    GST_WARNING ("improcess composite returned status=%d errno=%d (%s): %s; "
        "src={fd=%d fmt=%#x size=%dx%d stride=%dx%d rect=%d,%d,%d,%d} "
        "dst={fd=%d fmt=%#x size=%dx%d stride=%dx%d rect=%d,%d,%d,%d} "
        "pat={fd=%d fmt=%#x size=%dx%d stride=%dx%d rect=%d,%d,%d,%d} "
        "usage=%#x csc=%#x/%#x alpha=%u/%u",
        status, saved_errno, g_strerror (saved_errno), imStrError (status),
        source.fd, source.format, source.width, source.height,
        source.wstride, source.hstride, source_rect.x, source_rect.y,
        source_rect.width, source_rect.height,
        output.fd, output.format, output.width, output.height,
        output.wstride, output.hstride, output_rect.x, output_rect.y,
        output_rect.width, output_rect.height,
        pat.fd, pat.format, pat.width, pat.height, pat.wstride, pat.hstride,
        pat_rect.x, pat_rect.y, pat_rect.width, pat_rect.height,
        transform->usage | IM_SYNC, source.color_space_mode,
        output.color_space_mode, request->src_alpha, request->pat_alpha);
  /* Logging must not change the errno used by tuple/device-loss accounting. */
  errno = saved_errno;
  return status;
}
#endif

static const GstMppRgaBackendOps gst_mpp_rga_real_ops = {
  .probe = gst_mpp_rga_real_probe,
  .init = gst_mpp_rga_real_init,
  .blit = gst_mpp_rga_real_blit,
#ifdef GST_MPP_RGA_ENABLE_IM2D
  .process = gst_mpp_rga_real_process,
  .composite = gst_mpp_rga_real_composite,
#endif
};

static gboolean
gst_mpp_rga_version_supported (const GstMppRgaDriverVersion * version)
{
  if (version->major != 1)
    return version->major > 1;
  if (version->minor != 2)
    return version->minor > 2;
  return version->revision >= 4;
}

GstMppRgaBackend *
gst_mpp_rga_backend_new (const GstMppRgaBackendOps * ops, gpointer user_data)
{
  GstMppRgaBackend *backend;

  g_return_val_if_fail (ops != NULL, NULL);
  g_return_val_if_fail (ops->probe != NULL, NULL);
  g_return_val_if_fail (ops->init != NULL, NULL);
  g_return_val_if_fail (ops->blit != NULL, NULL);

  backend = g_new0 (GstMppRgaBackend, 1);
  g_mutex_init (&backend->lock);
  backend->ops = *ops;
  backend->user_data = user_data;
  backend->tuples = gst_mpp_rga_tuple_table_new ();
  return backend;
}

void
gst_mpp_rga_backend_free (GstMppRgaBackend * backend)
{
  if (!backend)
    return;

  gst_mpp_rga_tuple_table_free (backend->tuples);
  g_mutex_clear (&backend->lock);
  g_free (backend);
}

gboolean
gst_mpp_rga_backend_init (GstMppRgaBackend * backend)
{
  gint error_number = 0;
  gint init_result;
  gboolean available;

  g_return_val_if_fail (backend != NULL, FALSE);

  g_mutex_lock (&backend->lock);
  if (backend->initialized)
    goto out;

  backend->initialized = TRUE;
  if (!backend->ops.probe (backend->user_data, &backend->version,
          &error_number)) {
    GST_WARNING ("RGA probe failed for %s: %s", GST_MPP_RGA_DEVICE,
        g_strerror (error_number));
    goto out;
  }

  backend->version.string[sizeof (backend->version.string) - 1] = '\0';
  if (!gst_mpp_rga_version_supported (&backend->version)) {
    GST_WARNING ("RGA driver version %u.%u.%u is below required 1.2.4",
        backend->version.major, backend->version.minor,
        backend->version.revision);
    goto out;
  }

  backend->available = TRUE;
  GST_INFO ("RGA driver probe succeeded: %s (%u.%u.%u)",
      backend->version.string, backend->version.major, backend->version.minor,
      backend->version.revision);

  init_result = backend->ops.init (backend->user_data);
  if (init_result < 0)
    GST_WARNING ("c_RkRgaInit returned %d after successful driver probe",
        init_result);

out:
  available = backend->available;
  g_mutex_unlock (&backend->lock);
  return available;
}

static gpointer
gst_mpp_rga_create_default (gpointer user_data)
{
  GstMppRgaBackend *backend;

  (void) user_data;
  GST_DEBUG_CATEGORY_INIT (mpp_rga_backend_debug, "mpprgabackend", 0,
      "MPP RGA backend");
  backend = gst_mpp_rga_backend_new (&gst_mpp_rga_real_ops, NULL);
  gst_mpp_rga_backend_init (backend);
  return backend;
}

GstMppRgaBackend *
gst_mpp_rga_backend_get_default (void)
{
  static GOnce once = G_ONCE_INIT;

  return g_once (&once, gst_mpp_rga_create_default, NULL);
}

static GstMppRgaResult
gst_mpp_rga_backend_begin (GstMppRgaBackend * backend,
    GstMppRgaOperation operation, GstVideoFormat in_format,
    GstVideoFormat out_format, GstMppRgaTupleKey * key)
{
  key->operation = operation;
  key->in_format = in_format;
  key->out_format = out_format;

  g_return_val_if_fail (backend != NULL, GST_MPP_RGA_UNAVAILABLE);

  if (!gst_mpp_rga_backend_init (backend))
    return GST_MPP_RGA_UNAVAILABLE;

  g_mutex_lock (&backend->lock);
  if (!backend->available) {
    g_mutex_unlock (&backend->lock);
    return GST_MPP_RGA_UNAVAILABLE;
  }

  if (!gst_mpp_rga_tuple_should_try (backend->tuples, key)) {
    g_mutex_unlock (&backend->lock);
    return GST_MPP_RGA_TUPLE_DEMOTED;
  }
  g_mutex_unlock (&backend->lock);
  return GST_MPP_RGA_SUCCESS;
}

static GstMppRgaResult
gst_mpp_rga_backend_finish (GstMppRgaBackend * backend,
    const GstMppRgaTupleKey * key, gint ret, gint blit_errno,
    gboolean succeeded)
{
  guint failures;

  g_mutex_lock (&backend->lock);
  if (succeeded) {
    gst_mpp_rga_tuple_succeeded (backend->tuples, key);
    g_mutex_unlock (&backend->lock);
    return GST_MPP_RGA_SUCCESS;
  }

  if (ret == -ENODEV || blit_errno == ENODEV) {
    if (backend->available)
      GST_WARNING ("RGA device disappeared during %s",
          gst_mpp_rga_operation_name (key->operation));
    backend->available = FALSE;
    g_mutex_unlock (&backend->lock);
    return GST_MPP_RGA_DEVICE_LOST;
  }

  if (gst_mpp_rga_tuple_failed (backend->tuples, key, &failures)) {
    GST_WARNING ("RGA tuple demoted after %u consecutive failures: %s %s to %s",
        failures, gst_mpp_rga_operation_name (key->operation),
        gst_video_format_to_string (key->in_format),
        gst_video_format_to_string (key->out_format));
  }
  g_mutex_unlock (&backend->lock);
  return GST_MPP_RGA_BLIT_FAILED;
}

GstMppRgaResult
gst_mpp_rga_backend_blit (GstMppRgaBackend * backend,
    GstMppRgaOperation operation, GstVideoFormat in_format,
    GstVideoFormat out_format, rga_info_t * src, rga_info_t * dst)
{
  GstMppRgaTupleKey key;
  GstMppRgaResult result;
  gint ret;
  gint blit_errno;

  result = gst_mpp_rga_backend_begin (backend, operation, in_format,
      out_format, &key);
  if (result != GST_MPP_RGA_SUCCESS)
    return result;

  errno = 0;
  ret = backend->ops.blit (src, dst, backend->user_data);
  blit_errno = errno;
  return gst_mpp_rga_backend_finish (backend, &key, ret, blit_errno, ret >= 0);
}

GstMppRgaResult
gst_mpp_rga_backend_process (GstMppRgaBackend * backend,
    GstMppRgaOperation operation, GstVideoFormat in_format,
    GstVideoFormat out_format, const GstMppRgaIm2dRequest * request)
{
  GstMppRgaTupleKey key;
  GstMppRgaResult result;
  gint ret;
  gint blit_errno;

  g_return_val_if_fail (backend != NULL, GST_MPP_RGA_UNAVAILABLE);
  g_return_val_if_fail (request != NULL, GST_MPP_RGA_LAYOUT_REJECTED);
  if (!backend->ops.process)
    return GST_MPP_RGA_UNAVAILABLE;

  result = gst_mpp_rga_backend_begin (backend, operation, in_format,
      out_format, &key);
  if (result != GST_MPP_RGA_SUCCESS)
    return result;

  errno = 0;
  ret = backend->ops.process (request, backend->user_data);
  blit_errno = errno;
  return gst_mpp_rga_backend_finish (backend, &key, ret, blit_errno, ret > 0);
}

GstMppRgaResult
gst_mpp_rga_backend_composite (GstMppRgaBackend * backend,
    GstVideoFormat in_format, GstVideoFormat out_format,
    const GstMppRgaIm2dCompositeRequest * request)
{
  GstMppRgaTupleKey key;
  GstMppRgaResult result;
  gint ret;
  gint blit_errno;

  g_return_val_if_fail (backend != NULL, GST_MPP_RGA_UNAVAILABLE);
  g_return_val_if_fail (request != NULL, GST_MPP_RGA_LAYOUT_REJECTED);
  if (!backend->ops.composite)
    return GST_MPP_RGA_UNAVAILABLE;

  result = gst_mpp_rga_backend_begin (backend, GST_MPP_RGA_OP_COMPOSITE,
      in_format, out_format, &key);
  if (result != GST_MPP_RGA_SUCCESS)
    return result;

  errno = 0;
  ret = backend->ops.composite (request, backend->user_data);
  blit_errno = errno;
  return gst_mpp_rga_backend_finish (backend, &key, ret, blit_errno, ret > 0);
}

guint
gst_mpp_rga_backend_tuple_failures (GstMppRgaBackend * backend,
    GstMppRgaOperation operation, GstVideoFormat in_format,
    GstVideoFormat out_format)
{
  GstMppRgaTupleKey key = {
    .operation = operation,
    .in_format = in_format,
    .out_format = out_format,
  };
  guint failures;

  g_return_val_if_fail (backend != NULL, 0);
  g_mutex_lock (&backend->lock);
  failures = gst_mpp_rga_tuple_failures (backend->tuples, &key);
  g_mutex_unlock (&backend->lock);
  return failures;
}

#endif

const gchar *
gst_mpp_rga_operation_name (GstMppRgaOperation operation)
{
  switch (operation) {
    case GST_MPP_RGA_OP_ENCODE_CONVERT:
      return "encode-convert";
    case GST_MPP_RGA_OP_DECODE_CONVERT:
      return "decode-convert";
    case GST_MPP_RGA_OP_JPEG_CONVERT:
      return "jpeg-convert";
    case GST_MPP_RGA_OP_CONVERT:
      return "rgaconvert";
    case GST_MPP_RGA_OP_COMPOSITOR_COPY:
      return "rgacompositor-copy";
    case GST_MPP_RGA_OP_COMPOSITE:
      return "rgacompositor";
    default:
      return "unknown";
  }
}
