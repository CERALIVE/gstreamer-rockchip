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
  if (request->core_mask != 0) {
    status = imconfig (IM_CONFIG_SCHEDULER_CORE, request->core_mask);
    if (status <= IM_STATUS_FAILED)
      return status;
  }

  status = imconfig (IM_CONFIG_PRIORITY, request->priority);
  if (status <= IM_STATUS_FAILED)
    return status;

  src = wrapbuffer_fd (request->src_fd, request->src_width,
      request->src_height, request->src_format, request->src_wstride,
      request->src_hstride);
  dst = wrapbuffer_fd (request->dst_fd, request->dst_width,
      request->dst_height, request->dst_format, request->dst_wstride,
      request->dst_hstride);

  return improcess (src, dst, pat, src_rect, dst_rect, pat_rect,
      request->usage | IM_SYNC);
}
#endif

static const GstMppRgaBackendOps gst_mpp_rga_real_ops = {
  .probe = gst_mpp_rga_real_probe,
  .init = gst_mpp_rga_real_init,
  .blit = gst_mpp_rga_real_blit,
#ifdef GST_MPP_RGA_ENABLE_IM2D
  .process = gst_mpp_rga_real_process,
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
    default:
      return "unknown";
  }
}
