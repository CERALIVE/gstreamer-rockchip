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
#include <linux/sync_file.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "gstmpprgabackend.h"
#include "gstmpprgatuple.h"

#ifdef HAVE_RGA
#ifdef GST_MPP_RGA_ENABLE_IM2D
#include <gmodule.h>
#include <rga/im2d.h>
#if RGA_CURRENT_API_HEADER_VERSION < ((1 << 24) | (10 << 16) | (5 << 8))
#error "C6b requires librga im2d headers >= 1.10.5 (R1); older runtimes remain supported"
#endif
#endif

#define GST_MPP_RGA_DEVICE "/dev/rga"
#ifndef RGA_IOC_GET_DRVIER_VERSION
#define RGA_IOC_GET_DRVIER_VERSION _IOR ('r', 1, GstMppRgaDriverVersion)
#endif

GST_DEBUG_CATEGORY_STATIC (mpp_rga_backend_debug);
#define GST_CAT_DEFAULT mpp_rga_backend_debug

#ifdef GST_MPP_RGA_ENABLE_IM2D
typedef IM_STATUS (*GstMppRgaProcessOpt) (rga_buffer_t, rga_buffer_t,
    rga_buffer_t, im_rect, im_rect, im_rect, int, int *, im_opt_t *, int);

/* The explicit module reference must outlive every streaming thread. Never
 * close it: a plugin dependency need not be visible in the global namespace. */
static GModule *rga_module;
static GstMppRgaProcessOpt rga_process_opt;

static gpointer
gst_mpp_rga_resolve_api (gpointer unused)
{
  (void) unused;
  rga_module = g_module_open ("librga.so.2",
      G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if (rga_module && g_module_symbol (rga_module, "improcessOpt",
          (gpointer *) & rga_process_opt))
    GST_INFO ("improcessOpt resolved");
  else
    GST_WARNING ("improcessOpt unavailable; using seven-argument improcess; "
        "interpolation and async disabled");
  return rga_module;
}

static void
gst_mpp_rga_ensure_api (void)
{
  static GOnce once = G_ONCE_INIT;
  g_once (&once, gst_mpp_rga_resolve_api, NULL);
}

static gboolean
gst_mpp_rga_status_ok (gint status)
{
  return status == IM_STATUS_SUCCESS || status == IM_STATUS_NOERROR;
}

static IM_STATUS
gst_mpp_rga_submit_full (rga_buffer_t src, rga_buffer_t dst, rga_buffer_t pat,
    im_rect srect, im_rect drect, im_rect prect, gint interp, gint usage,
    gint * release_fence_fd)
{
  im_opt_t opt = { 0, };

  gst_mpp_rga_ensure_api ();
  opt.version = RGA_CURRENT_API_HEADER_VERSION;
  opt.interp = interp;
  if (release_fence_fd) {
    *release_fence_fd = -1;
    GST_LOG ("submitting asynchronous improcessOpt interp=%d", interp);
    return rga_process_opt (src, dst, pat, srect, drect, prect,
        -1, release_fence_fd, &opt, usage | IM_ASYNC);
  }
  if (rga_process_opt) {
    GST_LOG ("submitting synchronous improcessOpt interp=%d", interp);
    return rga_process_opt (src, dst, pat, srect, drect, prect,
        -1, NULL, &opt, usage | IM_SYNC);
  }
  return improcess (src, dst, pat, srect, drect, prect, usage | IM_SYNC);
}

static IM_STATUS
gst_mpp_rga_submit (rga_buffer_t src, rga_buffer_t dst, rga_buffer_t pat,
    im_rect srect, im_rect drect, im_rect prect, gint interp, gint usage)
{
  return gst_mpp_rga_submit_full (src, dst, pat, srect, drect, prect, interp,
      usage, NULL);
}

static gboolean
gst_mpp_rga_real_async_supported (gpointer user_data)
{
  (void) user_data;
  gst_mpp_rga_ensure_api ();
  return rga_process_opt != NULL;
}
#endif

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
#ifdef GST_MPP_RGA_ENABLE_IM2D
  if (g_strcmp0 (g_getenv ("GST_MPP_RGA_LEGACY_BLIT"), "1") != 0) {
    rga_buffer_t source;
    rga_buffer_t output;
    rga_buffer_t pat = { 0, };
    im_rect srect = { src->rect.xoffset, src->rect.yoffset,
      src->rect.width, src->rect.height
    };
    im_rect drect = { dst->rect.xoffset, dst->rect.yoffset,
      dst->rect.width, dst->rect.height
    };
    im_rect prect = { 0, };
    gint usage = 0;
    IM_STATUS status;

    switch (src->rotation) {
      case 0:
        break;
      case HAL_TRANSFORM_ROT_90:
        usage = IM_HAL_TRANSFORM_ROT_90;
        break;
      case HAL_TRANSFORM_ROT_180:
        usage = IM_HAL_TRANSFORM_ROT_180;
        break;
      case HAL_TRANSFORM_ROT_270:
        usage = IM_HAL_TRANSFORM_ROT_270;
        break;
      default:
        return -EINVAL;
    }
    source = src->virAddr ?
        wrapbuffer_virtualaddr (src->virAddr,
        src->rect.width + src->rect.xoffset,
        src->rect.height + src->rect.yoffset, src->rect.format,
        src->rect.wstride, src->rect.hstride) : wrapbuffer_fd (src->fd,
        src->rect.width + src->rect.xoffset,
        src->rect.height + src->rect.yoffset, src->rect.format,
        src->rect.wstride, src->rect.hstride);
    output =
        wrapbuffer_fd (dst->fd, dst->rect.width, dst->rect.height,
        dst->rect.format, dst->rect.wstride, dst->rect.hstride);
    imsetColorSpace (&source, IM_COLOR_SPACE_DEFAULT);
    imsetColorSpace (&output, dst->color_space_mode);
    status = gst_mpp_rga_submit (source, output, pat, srect, drect, prect,
        IM_INTERP_DEFAULT, usage);
    /* The legacy ops.blit contract is errno-style, not IM_STATUS. */
    return gst_mpp_rga_status_ok (status) ? 0 : -EIO;
  }
  {
    static gsize warned;
    if (g_once_init_enter (&warned)) {
      GST_WARNING
          ("GST_MPP_RGA_LEGACY_BLIT=1: using legacy c_RkRgaBlit rollback");
      g_once_init_leave (&warned, 1);
    }
  }
#endif
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
  request->colorspace_in = input->colorimetry;
  request->colorspace_out = output->colorimetry;
  request->csc_fallback = FALSE;
  if (GST_VIDEO_INFO_IS_RGB (input) && GST_VIDEO_INFO_IS_RGB (output))
    return TRUE;
  if (GST_VIDEO_INFO_IS_YUV (input) && GST_VIDEO_INFO_IS_YUV (output)) {
    if (gst_video_colorimetry_is_equal (&input->colorimetry,
            &output->colorimetry))
      return TRUE;
    goto fallback;
  }

  src_mode = gst_mpp_rga_color_space (input);
  dst_mode = gst_mpp_rga_color_space (output);
  if (src_mode < 0 || dst_mode < 0)
    goto fallback;

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

fallback:
  /* D29: preserve the default-matrix route for unexpressible requests, but
   * never describe it as an explicit CSC. The submitter counts each frame. */
  request->csc_fallback = TRUE;
  return FALSE;
}

void
gst_mpp_rga_count_csc_fallback (const GstMppRgaIm2dRequest * request,
    GstMppConversionStats * stats)
{
  gboolean warn;

  if (!request->csc_fallback)
    return;
  g_mutex_lock (&stats->lock);
  stats->csc_fallback_frames++;
  warn = !stats->csc_warned;
  stats->csc_warned = TRUE;
  g_mutex_unlock (&stats->lock);
  if (warn) {
    gchar *input = gst_video_colorimetry_to_string (&request->colorspace_in);
    gchar *output = gst_video_colorimetry_to_string (&request->colorspace_out);
    GST_WARNING ("CSC fallback: caps colorimetry %s -> %s cannot be expressed; "
        "using librga default matrix (csc-fallback-frames)",
        input ? input : "unknown", output ? output : "unknown");
    g_free (input);
    g_free (output);
  }
}

gboolean
gst_mpp_rga_composite_set_colorimetry (GstMppRgaIm2dCompositeRequest * request,
    const GstVideoInfo * accumulator, const GstVideoInfo * overlay)
{
  GstMppRgaIm2dRequest y2r = { 0, };
  GstMppRgaIm2dRequest r2y = { 0, };

  /* The YUV accumulator crosses RGB for blending, then returns to YUV.
   * Full-CSC endpoint modes cannot express this two-direction blend. */
  gst_mpp_rga_request_set_colorimetry (&y2r, accumulator, overlay);
  gst_mpp_rga_request_set_colorimetry (&r2y, overlay, accumulator);
  request->transform.colorspace_in = accumulator->colorimetry;
  request->transform.colorspace_out = overlay->colorimetry;
  request->transform.csc_fallback = y2r.csc_fallback || r2y.csc_fallback;
  if (request->transform.csc_fallback ||
      !(y2r.dst_color_space_mode & IM_YUV_TO_RGB_MASK) ||
      !(r2y.dst_color_space_mode & IM_RGB_TO_YUV_MASK)) {
    request->transform.src_color_space_mode = IM_COLOR_SPACE_DEFAULT;
    request->transform.dst_color_space_mode = IM_COLOR_SPACE_DEFAULT;
    return FALSE;
  }

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
    if (!gst_mpp_rga_status_ok (status)) {
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
  if (!gst_mpp_rga_status_ok (status))
    GST_WARNING ("imconfig priority=%d returned status=%d errno=%d (%s)",
        priority, status, saved_errno, g_strerror (saved_errno));
  errno = saved_errno;
  return status;
}

static gint
gst_mpp_rga_real_process_full (const GstMppRgaIm2dRequest * request,
    gint * release_fence_fd)
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

  status = gst_mpp_rga_real_configure_im2d (request->core_mask,
      request->priority);
  if (!gst_mpp_rga_status_ok (status))
    return status;

  src = wrapbuffer_fd (request->src_fd, request->src_width,
      request->src_height, request->src_format, request->src_wstride,
      request->src_hstride);
  dst = wrapbuffer_fd (request->dst_fd, request->dst_width,
      request->dst_height, request->dst_format, request->dst_wstride,
      request->dst_hstride);
  imsetColorSpace (&src, request->src_color_space_mode);
  imsetColorSpace (&dst, request->dst_color_space_mode);

  return gst_mpp_rga_submit_full (src, dst, pat, src_rect, dst_rect, pat_rect,
      request->interp, request->usage, release_fence_fd);
}

static gint
gst_mpp_rga_real_process (const GstMppRgaIm2dRequest * request,
    gpointer user_data)
{
  (void) user_data;
  return gst_mpp_rga_real_process_full (request, NULL);
}

static gint
gst_mpp_rga_real_process_async (const GstMppRgaIm2dRequest * request,
    gint * release_fence_fd, gpointer user_data)
{
  (void) user_data;
  return gst_mpp_rga_real_process_full (request, release_fence_fd);
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
  if (!gst_mpp_rga_status_ok (status))
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
  imsetColorSpace (&source, transform->src_color_space_mode);
  imsetColorSpace (&output, transform->dst_color_space_mode);

  errno = 0;
  status = gst_mpp_rga_submit (source, output, pat, source_rect, output_rect,
      pat_rect, transform->interp, transform->usage);
  saved_errno = errno;
  if (!gst_mpp_rga_status_ok (status))
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
  .async_supported = gst_mpp_rga_real_async_supported,
  .process_async = gst_mpp_rga_real_process_async,
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
#ifdef GST_MPP_RGA_ENABLE_IM2D
  gst_mpp_rga_ensure_api ();
#endif
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
  result = gst_mpp_rga_backend_finish (backend, &key, ret, blit_errno,
      ret == IM_STATUS_SUCCESS || ret == IM_STATUS_NOERROR);
  if (result == GST_MPP_RGA_BLIT_FAILED &&
      (ret == IM_STATUS_NOT_SUPPORTED || ret == IM_STATUS_INVALID_PARAM ||
          ret == IM_STATUS_ILLEGAL_PARAM || ret == IM_STATUS_ERROR_VERSION))
    return GST_MPP_RGA_NOT_SUPPORTED;
  return result;
}

gboolean
gst_mpp_rga_backend_supports_async (GstMppRgaBackend * backend)
{
  g_return_val_if_fail (backend != NULL, FALSE);
  if (!backend->ops.process_async || !backend->ops.async_supported)
    return FALSE;
  return backend->ops.async_supported (backend->user_data);
}

GstMppRgaFenceStatus
gst_mpp_rga_fence_status (gint release_fence_fd, gint timeout_ms)
{
  struct pollfd pfd = { release_fence_fd, POLLIN, 0 };
  struct sync_file_info info = { 0, };
  gint rc;

  if (release_fence_fd == GST_MPP_RGA_FENCE_MISSING)
    return GST_MPP_RGA_FENCE_PENDING;
  if (release_fence_fd == -1)
    return GST_MPP_RGA_FENCE_COMPLETE;

  do {
    rc = poll (&pfd, 1, timeout_ms);
  } while (rc < 0 && errno == EINTR);

  /* Broken descriptors do not establish that DMA has stopped. */
  if (rc <= 0 || (pfd.revents & (POLLNVAL | POLLERR | POLLHUP)) ||
      !(pfd.revents & POLLIN))
    return GST_MPP_RGA_FENCE_PENDING;

  /* sync_file reports POLLIN for both successful and error-signalled fences.
   * num_fences=0 requests aggregate status without allocating a fence array. */
  do {
    rc = ioctl (release_fence_fd, SYNC_IOC_FILE_INFO, &info);
  } while (rc < 0 && errno == EINTR);

  if (rc < 0) {
    GST_WARNING ("Cannot query sync_file fence %d: %s; retaining ownership",
        release_fence_fd, g_strerror (errno));
    return GST_MPP_RGA_FENCE_PENDING;
  }
  if (info.status > 0)
    return GST_MPP_RGA_FENCE_COMPLETE;
  if (info.status < 0)
    return GST_MPP_RGA_FENCE_ERROR;
  GST_WARNING
      ("Readable sync_file fence %d is still active; retaining ownership",
      release_fence_fd);
  return GST_MPP_RGA_FENCE_PENDING;
}

gboolean
gst_mpp_rga_fence_wait (gint release_fence_fd, gint timeout_ms)
{
  GstMppRgaFenceStatus status =
      gst_mpp_rga_fence_status (release_fence_fd, timeout_ms);

  if (status != GST_MPP_RGA_FENCE_PENDING && release_fence_fd >= 0)
    close (release_fence_fd);
  return status == GST_MPP_RGA_FENCE_COMPLETE;
}

GstMppRgaResult
gst_mpp_rga_backend_process_async (GstMppRgaBackend * backend,
    GstMppRgaOperation operation, GstVideoFormat in_format,
    GstVideoFormat out_format, const GstMppRgaIm2dRequest * request,
    gint * release_fence_fd)
{
  GstMppRgaTupleKey key;
  GstMppRgaResult result;
  gint ret;
  gint blit_errno;

  g_return_val_if_fail (backend != NULL, GST_MPP_RGA_UNAVAILABLE);
  g_return_val_if_fail (request != NULL, GST_MPP_RGA_LAYOUT_REJECTED);
  g_return_val_if_fail (release_fence_fd != NULL, GST_MPP_RGA_LAYOUT_REJECTED);

  *release_fence_fd = -1;
  if (!gst_mpp_rga_backend_supports_async (backend))
    return GST_MPP_RGA_UNAVAILABLE;

  result = gst_mpp_rga_backend_begin (backend, operation, in_format,
      out_format, &key);
  if (result != GST_MPP_RGA_SUCCESS)
    return result;

  errno = 0;
  ret = backend->ops.process_async (request, release_fence_fd,
      backend->user_data);
  blit_errno = errno;
  if ((ret == IM_STATUS_SUCCESS || ret == IM_STATUS_NOERROR) &&
      *release_fence_fd < 0) {
    *release_fence_fd = GST_MPP_RGA_FENCE_MISSING;
    ret = IM_STATUS_FAILED;
    GST_ERROR ("Async submission succeeded without a completion fence");
  }
  result = gst_mpp_rga_backend_finish (backend, &key, ret, blit_errno,
      ret == IM_STATUS_SUCCESS || ret == IM_STATUS_NOERROR);
  if (result == GST_MPP_RGA_BLIT_FAILED &&
      (ret == IM_STATUS_NOT_SUPPORTED || ret == IM_STATUS_INVALID_PARAM ||
          ret == IM_STATUS_ILLEGAL_PARAM || ret == IM_STATUS_ERROR_VERSION))
    return GST_MPP_RGA_NOT_SUPPORTED;
  return result;
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
  result = gst_mpp_rga_backend_finish (backend, &key, ret, blit_errno,
      ret == IM_STATUS_SUCCESS || ret == IM_STATUS_NOERROR);
  if (result == GST_MPP_RGA_BLIT_FAILED &&
      (ret == IM_STATUS_NOT_SUPPORTED || ret == IM_STATUS_INVALID_PARAM ||
          ret == IM_STATUS_ILLEGAL_PARAM || ret == IM_STATUS_ERROR_VERSION))
    return GST_MPP_RGA_NOT_SUPPORTED;
  return result;
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

GstFlowReturn
gst_mpp_rga_result_to_flow (GstMppRgaResult result)
{
  switch (result) {
    case GST_MPP_RGA_SUCCESS:
      return GST_FLOW_OK;
    case GST_MPP_RGA_UNAVAILABLE:
    case GST_MPP_RGA_TUPLE_DEMOTED:
    case GST_MPP_RGA_LAYOUT_REJECTED:
    case GST_MPP_RGA_NOT_SUPPORTED:
      return GST_FLOW_NOT_NEGOTIATED;
    case GST_MPP_RGA_BLIT_FAILED:
    case GST_MPP_RGA_DEVICE_LOST:
    default:
      return GST_FLOW_ERROR;
  }
}

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
