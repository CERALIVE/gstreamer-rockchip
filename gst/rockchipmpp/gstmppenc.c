/*
 * Copyright 2017 Rockchip Electronics Co., Ltd
 *     Author: Randy Li <randy.li@rock-chips.com>
 *
 * Copyright 2021 Rockchip Electronics Co., Ltd
 *     Author: Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstmppallocator.h"
#include "gstmppenc.h"

#define GST_CAT_DEFAULT mpp_enc_debug
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

#define parent_class gst_mpp_enc_parent_class
G_DEFINE_ABSTRACT_TYPE (GstMppEnc, gst_mpp_enc, GST_TYPE_VIDEO_ENCODER);

#define GST_MPP_ENC_TASK_STARTED(encoder) \
    (gst_pad_get_task_state ((encoder)->srcpad) == GST_TASK_STARTED)

#define GST_MPP_ENC_MUTEX(encoder) (&GST_MPP_ENC (encoder)->mutex)

#define GST_MPP_ENC_LOCK(encoder) \
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder); \
  g_mutex_lock (GST_MPP_ENC_MUTEX (encoder)); \
  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);

#define GST_MPP_ENC_UNLOCK(encoder) \
  g_mutex_unlock (GST_MPP_ENC_MUTEX (encoder));

#define GST_MPP_ENC_EVENT_MUTEX(encoder) (&GST_MPP_ENC (encoder)->event_mutex)
#define GST_MPP_ENC_EVENT_COND(encoder) (&GST_MPP_ENC (encoder)->event_cond)

#define GST_MPP_ENC_BROADCAST(encoder) \
  g_mutex_lock (GST_MPP_ENC_EVENT_MUTEX (encoder)); \
  g_cond_broadcast (GST_MPP_ENC_EVENT_COND (encoder)); \
  g_mutex_unlock (GST_MPP_ENC_EVENT_MUTEX (encoder));

#define GST_MPP_ENC_FLUSHING(encoder) \
  g_atomic_int_get (&GST_MPP_ENC (encoder)->flushing)

#define GST_MPP_ENC_PENDING(encoder) \
  g_atomic_int_get (&GST_MPP_ENC (encoder)->pending_frames)
#define GST_MPP_ENC_SET_PENDING(encoder, value) \
  g_atomic_int_set (&GST_MPP_ENC (encoder)->pending_frames, value)

#define GST_MPP_ENC_WAIT(encoder, condition) \
  g_mutex_lock (GST_MPP_ENC_EVENT_MUTEX (encoder)); \
  while (!(condition)) \
    g_cond_wait (GST_MPP_ENC_EVENT_COND (encoder), \
        GST_MPP_ENC_EVENT_MUTEX (encoder)); \
  g_mutex_unlock (GST_MPP_ENC_EVENT_MUTEX (encoder));

#define DEFAULT_PROP_HEADER_MODE MPP_ENC_HEADER_MODE_DEFAULT    /* First frame */
#define DEFAULT_PROP_SEI_MODE MPP_ENC_SEI_MODE_DISABLE
#define DEFAULT_PROP_RC_MODE MPP_ENC_RC_MODE_CBR
#define DEFAULT_PROP_ROTATION 0
#define DEFAULT_PROP_GOP -1     /* Same as FPS */
#define DEFAULT_PROP_MAX_REENC 1
#define DEFAULT_PROP_BPS 0      /* Auto */
#define DEFAULT_PROP_BPS_MIN 0  /* Auto */
#define DEFAULT_PROP_BPS_MAX 0  /* Auto */
#define DEFAULT_PROP_WIDTH 0    /* Original */
#define DEFAULT_PROP_HEIGHT 0   /* Original */
#define DEFAULT_PROP_FPS_OUT 0          /* Same as input */
#define DEFAULT_PROP_DROP_MODE 0       /* Disabled */
#define DEFAULT_PROP_DROP_THRESHOLD 50 /* 50% over bps_max */
#define DEFAULT_PROP_INTRA_REFRESH 0   /* Disabled */
#define DEFAULT_PROP_SUPER_MODE MPP_ENC_RC_SUPER_FRM_NONE
#define DEFAULT_PROP_SUPER_I_THD 0     /* Auto */
#define DEFAULT_PROP_SUPER_P_THD 0     /* Auto */
#define DEFAULT_PROP_DEBREATH FALSE
#define DEFAULT_PROP_DEBREATH_STRENGTH 16
#define DEFAULT_PROP_SCENE_MODE 0      /* Default */
#define DEFAULT_PROP_ANTI_FLICKER 0    /* Disabled */
#define DEFAULT_PROP_NUM_TEMPORAL_LAYERS 0 /* Off (flat IPPP) */
#define DEFAULT_PROP_ZERO_COPY_PKT TRUE

/* Input isn't ARM AFBC by default */
static GstVideoFormat DEFAULT_PROP_ARM_AFBC = FALSE;

#define MPP_MAX_PENDING 16      /* Max number of MPP pending frames */
static guint32 DEFAULT_PROP_MAX_PENDING = MPP_MAX_PENDING;

#define DEFAULT_FPS 30

enum
{
  PROP_0,
  PROP_MAX_PENDING,
  PROP_HEADER_MODE,
  PROP_RC_MODE,
  PROP_ROTATION,
  PROP_SEI_MODE,
  PROP_GOP,
  PROP_MAX_REENC,
  PROP_BPS,
  PROP_BPS_MIN,
  PROP_BPS_MAX,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_ZERO_COPY_PKT,
  PROP_ARM_AFBC,
  PROP_FPS_OUT,
  PROP_DROP_MODE,
  PROP_DROP_THRESHOLD,
  PROP_INTRA_REFRESH,
  PROP_SUPER_MODE,
  PROP_SUPER_I_THD,
  PROP_SUPER_P_THD,
  PROP_DEBREATH,
  PROP_DEBREATH_STRENGTH,
  PROP_SCENE_MODE,
  PROP_ANTI_FLICKER,
  PROP_NUM_TEMPORAL_LAYERS,
  PROP_LAST,
};

gboolean
gst_mpp_enc_supported (MppCodingType mpp_type)
{
  MppCtx mpp_ctx;
  MppApi *mpi;

  mpp_set_log_level(MPP_LOG_WARN);

  if (mpp_create (&mpp_ctx, &mpi))
    return FALSE;

  if (mpp_init (mpp_ctx, MPP_CTX_ENC, mpp_type)) {
    mpp_destroy (mpp_ctx);
    return FALSE;
  }

  mpp_destroy (mpp_ctx);
  return TRUE;
}

gboolean
gst_mpp_enc_video_info_align (GstVideoInfo * info)
{
  gint vstride;

  /* Allow skipping vstride aligning for RKVENC */
  if (g_getenv ("GST_MPP_ENC_ALIGNED_VSTRIDE")) {
    vstride = 0;
  } else {
    vstride = GST_MPP_VIDEO_INFO_VSTRIDE (info);
  }

  return gst_mpp_video_info_align (info, 0, vstride);
}

gboolean
gst_mpp_enc_cfg_set_s32 (GstMppEnc * self, const gchar * key, gint value)
{
  if (mpp_enc_cfg_set_s32 (self->mpp_cfg, key, value)) {
    GST_ERROR_OBJECT (self, "MPP rejected config key '%s' (= %d)", key, value);
    self->cfg_error = TRUE;
    return FALSE;
  }
  return TRUE;
}

gboolean
gst_mpp_enc_cfg_set_u32 (GstMppEnc * self, const gchar * key, guint value)
{
  if (mpp_enc_cfg_set_u32 (self->mpp_cfg, key, value)) {
    GST_ERROR_OBJECT (self, "MPP rejected config key '%s' (= %u)", key, value);
    self->cfg_error = TRUE;
    return FALSE;
  }
  return TRUE;
}

static void
gst_mpp_enc_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (object);
  GstMppEnc *self = GST_MPP_ENC (encoder);
  gboolean broadcast = FALSE;
  gboolean invalid = FALSE;

  GST_MPP_ENC_PROP_LOCK (encoder);
  switch (prop_id) {
    case PROP_MAX_PENDING:{
      self->max_pending = g_value_get_uint (value);
      broadcast = TRUE;
      goto out;
    }
    case PROP_HEADER_MODE:{
      MppEncHeaderMode header_mode = g_value_get_enum (value);
      if (self->header_mode == header_mode)
        goto out;

      self->header_mode = header_mode;
      break;
    }
    case PROP_SEI_MODE:{
      MppEncSeiMode sei_mode = g_value_get_enum (value);
      if (self->sei_mode == sei_mode)
        goto out;

      self->sei_mode = sei_mode;
      break;
    }
    case PROP_RC_MODE:{
      MppEncRcMode rc_mode = g_value_get_enum (value);
      if (self->rc_mode == rc_mode)
        goto out;

      self->rc_mode = rc_mode;
      break;
    }
    case PROP_GOP:{
      gint gop = g_value_get_int (value);
      if (self->gop == gop)
        goto out;

      self->gop = gop;
      break;
    }
    case PROP_MAX_REENC:{
      guint max_reenc = g_value_get_uint (value);
      if (self->max_reenc == max_reenc)
        goto out;

      self->max_reenc = max_reenc;
      break;
    }
    case PROP_BPS:{
      guint bps = g_value_get_uint (value);
      if (self->bps == bps)
        goto out;

      self->bps = bps;
      break;
    }
    case PROP_BPS_MIN:{
      guint bps_min = g_value_get_uint (value);
      if (self->bps_min == bps_min)
        goto out;

      self->bps_min = bps_min;
      break;
    }
    case PROP_BPS_MAX:{
      guint bps_max = g_value_get_uint (value);
      if (self->bps_max == bps_max)
        goto out;

      self->bps_max = bps_max;
      break;
    }
    case PROP_FPS_OUT:{
      gint fps_out = g_value_get_int (value);
      if (self->fps_out == fps_out)
        goto out;

      self->fps_out = fps_out;
      break;
    }
    case PROP_DROP_MODE:{
      gint drop_mode = g_value_get_int (value);
      if (self->drop_mode == drop_mode)
        goto out;

      self->drop_mode = drop_mode;
      break;
    }
    case PROP_DROP_THRESHOLD:{
      guint drop_threshold = g_value_get_uint (value);
      if (self->drop_threshold == drop_threshold)
        goto out;

      self->drop_threshold = drop_threshold;
      break;
    }
    case PROP_INTRA_REFRESH:{
      guint intra_refresh = g_value_get_uint (value);
      if (self->intra_refresh == intra_refresh)
        goto out;

      self->intra_refresh = intra_refresh;
      break;
    }
    case PROP_SUPER_MODE:{
      gint super_mode = g_value_get_enum (value);
      if (self->super_mode == super_mode)
        goto out;

      self->super_mode = super_mode;
      break;
    }
    case PROP_SUPER_I_THD:{
      guint super_i_thd = g_value_get_uint (value);
      if (self->super_i_thd == super_i_thd)
        goto out;

      self->super_i_thd = super_i_thd;
      break;
    }
    case PROP_SUPER_P_THD:{
      guint super_p_thd = g_value_get_uint (value);
      if (self->super_p_thd == super_p_thd)
        goto out;

      self->super_p_thd = super_p_thd;
      break;
    }
    case PROP_DEBREATH:{
      gboolean debreath = g_value_get_boolean (value);
      if (self->debreath == debreath)
        goto out;

      self->debreath = debreath;
      break;
    }
    case PROP_DEBREATH_STRENGTH:{
      guint debreath_strength = g_value_get_uint (value);
      if (self->debreath_strength == debreath_strength)
        goto out;

      self->debreath_strength = debreath_strength;
      break;
    }
    case PROP_SCENE_MODE:{
      gint scene_mode = g_value_get_enum (value);
      if (self->scene_mode == scene_mode)
        goto out;

      self->scene_mode = scene_mode;
      break;
    }
    case PROP_ANTI_FLICKER:{
      guint anti_flicker = g_value_get_uint (value);
      if (self->anti_flicker == anti_flicker)
        goto out;

      self->anti_flicker = anti_flicker;
      break;
    }
    case PROP_ROTATION:{
      if (self->input_state)
        GST_WARNING_OBJECT (encoder, "unable to change rotation");
      else
        self->rotation = g_value_get_enum (value);
      goto out;
    }
    case PROP_WIDTH:{
      guint width = g_value_get_uint (value);
      if (width != (guint) self->width) {
        self->width = width;
        /* Rescale in place on the next frame when already streaming. */
        if (self->input_state)
          self->res_dirty = TRUE;
      }
      goto out;
    }
    case PROP_HEIGHT:{
      guint height = g_value_get_uint (value);
      if (height != (guint) self->height) {
        self->height = height;
        if (self->input_state)
          self->res_dirty = TRUE;
      }
      goto out;
    }
    case PROP_ZERO_COPY_PKT:{
      self->zero_copy_pkt = g_value_get_boolean (value);
      goto out;
    }
    case PROP_ARM_AFBC:{
      if (self->input_state)
        GST_WARNING_OBJECT (encoder, "unable to change ARM AFBC");
      else
        self->arm_afbc = g_value_get_boolean (value);
      goto out;
    }
    case PROP_NUM_TEMPORAL_LAYERS:{
      guint layers = g_value_get_uint (value);
      if (layers != self->num_temporal_layers) {
        self->num_temporal_layers = layers;
        /* Rebuild the reference structure on the next apply. */
        self->ref_dirty = TRUE;
      }
      break;
    }
    default:
      invalid = TRUE;
      goto out;
  }

  self->prop_dirty = TRUE;

out:
  GST_MPP_ENC_PROP_UNLOCK (encoder);
  if (broadcast) {
    GST_MPP_ENC_BROADCAST (encoder);
  }
  if (invalid)
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
gst_mpp_enc_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (object);
  GstMppEnc *self = GST_MPP_ENC (encoder);
  gboolean invalid = FALSE;

  GST_MPP_ENC_PROP_LOCK (encoder);
  switch (prop_id) {
    case PROP_MAX_PENDING:
      g_value_set_uint (value, self->max_pending);
      break;
    case PROP_HEADER_MODE:
      g_value_set_enum (value, self->header_mode);
      break;
    case PROP_SEI_MODE:
      g_value_set_enum (value, self->sei_mode);
      break;
    case PROP_RC_MODE:
      g_value_set_enum (value, self->rc_mode);
      break;
    case PROP_ROTATION:
      g_value_set_enum (value, self->rotation);
      break;
    case PROP_GOP:
      g_value_set_int (value, self->gop);
      break;
    case PROP_MAX_REENC:
      g_value_set_uint (value, self->max_reenc);
      break;
    case PROP_BPS:
      g_value_set_uint (value, self->bps);
      break;
    case PROP_BPS_MIN:
      g_value_set_uint (value, self->bps_min);
      break;
    case PROP_BPS_MAX:
      g_value_set_uint (value, self->bps_max);
      break;
    case PROP_WIDTH:
      g_value_set_uint (value, self->width);
      break;
    case PROP_HEIGHT:
      g_value_set_uint (value, self->height);
      break;
    case PROP_ZERO_COPY_PKT:
      g_value_set_boolean (value, self->zero_copy_pkt);
      break;
    case PROP_ARM_AFBC:
      g_value_set_boolean (value, self->arm_afbc);
      break;
    case PROP_FPS_OUT:
      g_value_set_int (value, self->fps_out);
      break;
    case PROP_DROP_MODE:
      g_value_set_int (value, self->drop_mode);
      break;
    case PROP_DROP_THRESHOLD:
      g_value_set_uint (value, self->drop_threshold);
      break;
    case PROP_INTRA_REFRESH:
      g_value_set_uint (value, self->intra_refresh);
      break;
    case PROP_SUPER_MODE:
      g_value_set_enum (value, self->super_mode);
      break;
    case PROP_SUPER_I_THD:
      g_value_set_uint (value, self->super_i_thd);
      break;
    case PROP_SUPER_P_THD:
      g_value_set_uint (value, self->super_p_thd);
      break;
    case PROP_DEBREATH:
      g_value_set_boolean (value, self->debreath);
      break;
    case PROP_DEBREATH_STRENGTH:
      g_value_set_uint (value, self->debreath_strength);
      break;
    case PROP_SCENE_MODE:
      g_value_set_enum (value, self->scene_mode);
      break;
    case PROP_ANTI_FLICKER:
      g_value_set_uint (value, self->anti_flicker);
      break;
    case PROP_NUM_TEMPORAL_LAYERS:
      g_value_set_uint (value, self->num_temporal_layers);
      break;
    default:
      invalid = TRUE;
      break;
  }
  GST_MPP_ENC_PROP_UNLOCK (encoder);

  if (invalid)
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

typedef struct
{
  MppEncHeaderMode header_mode;
  MppEncRcMode rc_mode;
  MppEncSeiMode sei_mode;
  gint gop;
  guint max_reenc;
  guint bps;
  guint bps_min;
  guint bps_max;
  gint fps_out;
  gint fps_n;
  gint fps_d;
  gint width;
  gint height;
  gint drop_mode;
  guint drop_threshold;
  guint intra_refresh;
  gint super_mode;
  guint super_i_thd;
  guint super_p_thd;
  gboolean debreath;
  guint debreath_strength;
  gint scene_mode;
  guint anti_flicker;
  guint num_temporal_layers;
  gboolean ref_dirty;
} GstMppEncPropertiesSnapshot;

/*
 * MPP classifies a coded frame as "super" with `(RK_U32) bit_real >= bits_thr`
 * (mpp/codec/rc/rc_model_v2.c:1276 at the pinned 1.5.0-1). A threshold of 0
 * therefore matches EVERY frame the moment super-frame mode is enabled, and in
 * MPP_ENC_RC_SUPER_FRM_DROP that branch also rewrites the rate controller's own
 * drop_mode/drop_gap. 0 is this element's documented "unset" value, so it maps
 * to a threshold no real frame can reach rather than one every frame reaches.
 */
static guint
gst_mpp_enc_super_threshold (guint threshold)
{
  return threshold ? threshold : G_MAXUINT;
}

/*
 * The framerate the encoder actually emits, which is what both the automatic
 * bitrate and the default GOP must be sized against. fps-out decimates output
 * independently of the input caps, so a 60-in/30-out pipeline that sizes either
 * one off the input rate gets twice the bitrate and a two-second GOP.
 */
static gint
gst_mpp_enc_effective_fps_out (const GstMppEncPropertiesSnapshot * properties)
{
  gint fps;

  if (properties->fps_out > 0)
    return properties->fps_out;

  if (properties->fps_n <= 0 || properties->fps_d <= 0)
    return DEFAULT_FPS;

  fps = properties->fps_n / properties->fps_d;
  return fps > 0 ? fps : DEFAULT_FPS;
}

/*
 * The bitrate=0 "auto" target: an eighth of a raw 8-bit luma plane per output
 * frame, saturated at G_MAXINT because MPP's rc:bps_* fields are RK_S32.
 *
 * The frame term is bounded BEFORE it is scaled by the framerate rather than
 * after. width and height are each only bounded by G_MAXINT, so the product can
 * reach 2^59 after the shift, and multiplying that by a framerate first would
 * wrap guint64 itself -- leaving a post-hoc `> G_MAXINT` test looking at a
 * small wrapped value and silently returning it as if it were in range.
 */
static guint
gst_mpp_enc_auto_bitrate (gint width, gint height, gint fps)
{
  guint64 frame_bits;
  guint64 limit = G_MAXINT;

  if (width <= 0 || height <= 0 || fps <= 0)
    return 0;

  frame_bits = width;
  frame_bits = frame_bits * height / 8;

  if (frame_bits > limit / (guint64) fps)
    return (guint) G_MAXINT;

  return (guint) (frame_bits * fps);
}

/*
 * Scale a bitrate by a small ratio, saturating at G_MAXINT. Same rule as above:
 * the operand is bounded against the numerator before the multiply, so no
 * intermediate can wrap.
 */
static gint
gst_mpp_enc_scale_bitrate (guint bps, guint numerator, guint denominator)
{
  guint64 scaled = bps;
  guint64 limit = G_MAXINT;

  if (scaled > G_MAXUINT64 / (guint64) numerator)
    return G_MAXINT;

  scaled = scaled * numerator / denominator;

  return scaled > limit ? G_MAXINT : (gint) scaled;
}

/*
 * Build and apply the temporal-SVC reference structure for
 * num_temporal_layers (2/3/4 -> tsvc2/3/4). Frames in higher temporal
 * layers are non-reference, so a downstream scheduler can drop the top layer(s)
 * to halve/quarter framerate without breaking decode. The layer tables are a
 * direct port of MPP's own mpi_enc_gen_ref_cfg reference implementation.
 *
 * num_temporal_layers < 2 resets to MPP's default flat IPPP structure. The
 * reference config change restarts the GOP (a fresh IDR), which is the intended
 * behaviour for a hierarchical-P transition.
 */
static gboolean
gst_mpp_enc_apply_ref_cfg (GstVideoEncoder * encoder,
    guint num_temporal_layers)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  MppEncRefLtFrmCfg lt_ref[4];
  MppEncRefStFrmCfg st_ref[16];
  RK_S32 lt_cnt = 0;
  RK_S32 st_cnt = 0;

  if (num_temporal_layers < 2) {
    /* NULL ref cfg restores MPP's default (flat IPPP) references. */
    if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_REF_CFG, NULL)) {
      GST_WARNING_OBJECT (self, "failed to reset reference cfg");
      return FALSE;
    }
    GST_INFO_OBJECT (self, "temporal layers disabled (flat IPPP)");
    return TRUE;
  }

  if (!self->ref_cfg && mpp_enc_ref_cfg_init (&self->ref_cfg))
    return FALSE;
  mpp_enc_ref_cfg_reset (self->ref_cfg);

  memset (lt_ref, 0, sizeof (lt_ref));
  memset (st_ref, 0, sizeof (st_ref));

  switch (num_temporal_layers) {
    case 4:{
      /* tsvc4: 4 temporal layers, one long-term ref anchoring the base. */
      lt_cnt = 1;
      lt_ref[0].lt_idx = 0;
      lt_ref[0].temporal_id = 0;
      lt_ref[0].ref_mode = REF_TO_PREV_LT_REF;
      lt_ref[0].lt_gap = 8;
      lt_ref[0].lt_delay = 0;

      st_cnt = 9;
      st_ref[0].is_non_ref = 0;
      st_ref[0].temporal_id = 0;
      st_ref[0].ref_mode = REF_TO_TEMPORAL_LAYER;
      st_ref[1].is_non_ref = 1;
      st_ref[1].temporal_id = 3;
      st_ref[1].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[2].is_non_ref = 0;
      st_ref[2].temporal_id = 2;
      st_ref[2].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[3].is_non_ref = 1;
      st_ref[3].temporal_id = 3;
      st_ref[3].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[4].is_non_ref = 0;
      st_ref[4].temporal_id = 1;
      st_ref[4].ref_mode = REF_TO_PREV_LT_REF;
      st_ref[5].is_non_ref = 1;
      st_ref[5].temporal_id = 3;
      st_ref[5].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[6].is_non_ref = 0;
      st_ref[6].temporal_id = 2;
      st_ref[6].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[7].is_non_ref = 1;
      st_ref[7].temporal_id = 3;
      st_ref[7].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[8].is_non_ref = 0;
      st_ref[8].temporal_id = 0;
      st_ref[8].ref_mode = REF_TO_TEMPORAL_LAYER;
    } break;
    case 3:{
      /* tsvc3: 3 temporal layers. */
      st_cnt = 5;
      st_ref[0].is_non_ref = 0;
      st_ref[0].temporal_id = 0;
      st_ref[0].ref_mode = REF_TO_TEMPORAL_LAYER;
      st_ref[1].is_non_ref = 1;
      st_ref[1].temporal_id = 2;
      st_ref[1].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[2].is_non_ref = 0;
      st_ref[2].temporal_id = 1;
      st_ref[2].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[3].is_non_ref = 1;
      st_ref[3].temporal_id = 2;
      st_ref[3].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[4].is_non_ref = 0;
      st_ref[4].temporal_id = 0;
      st_ref[4].ref_mode = REF_TO_TEMPORAL_LAYER;
    } break;
    default:{                   /* 2 */
      /* tsvc2: 2 temporal layers. */
      st_cnt = 3;
      st_ref[0].is_non_ref = 0;
      st_ref[0].temporal_id = 0;
      st_ref[0].ref_mode = REF_TO_TEMPORAL_LAYER;
      st_ref[1].is_non_ref = 1;
      st_ref[1].temporal_id = 1;
      st_ref[1].ref_mode = REF_TO_PREV_REF_FRM;
      st_ref[2].is_non_ref = 0;
      st_ref[2].temporal_id = 0;
      st_ref[2].ref_mode = REF_TO_PREV_REF_FRM;
    } break;
  }

  mpp_enc_ref_cfg_set_cfg_cnt (self->ref_cfg, lt_cnt, st_cnt);
  if (lt_cnt)
    mpp_enc_ref_cfg_add_lt_cfg (self->ref_cfg, lt_cnt, lt_ref);
  if (st_cnt)
    mpp_enc_ref_cfg_add_st_cfg (self->ref_cfg, st_cnt, st_ref);

  if (mpp_enc_ref_cfg_check (self->ref_cfg)) {
    GST_WARNING_OBJECT (self, "temporal-layer ref cfg check failed");
    return FALSE;
  }

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_REF_CFG, self->ref_cfg)) {
    GST_WARNING_OBJECT (self, "failed to set temporal-layer ref cfg");
    return FALSE;
  }

  GST_INFO_OBJECT (self, "temporal layers: tsvc%u enabled",
      num_temporal_layers);
  return TRUE;
}

gboolean
gst_mpp_enc_apply_properties_full (GstVideoEncoder * encoder,
    GstMppEncSnapshotPropertiesFunc snapshot_codec_properties,
    GstMppEncConfigurePropertiesFunc configure_codec_properties,
    gpointer codec_snapshot, gboolean * applied)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstMppEncPropertiesSnapshot properties;
  gint fps_out;

  if (applied)
    *applied = FALSE;

  /* Checked before the prop_dirty early-out on purpose: a pass that latched
   * cfg_error already cleared prop_dirty, so a later quiescent apply would
   * otherwise report success for a config MPP never accepted. */
  if (self->cfg_error) {
    GST_ERROR_OBJECT (self, "refusing to apply a config MPP partly rejected");
    return FALSE;
  }

  GST_MPP_ENC_PROP_LOCK (encoder);
  if (!self->prop_dirty) {
    GST_MPP_ENC_PROP_UNLOCK (encoder);
    return TRUE;
  }

  properties.header_mode = self->header_mode;
  properties.rc_mode = self->rc_mode;
  properties.sei_mode = self->sei_mode;
  properties.gop = self->gop;
  properties.max_reenc = self->max_reenc;
  properties.bps = self->bps;
  properties.bps_min = self->bps_min;
  properties.bps_max = self->bps_max;
  properties.fps_out = self->fps_out;
  properties.fps_n = GST_VIDEO_INFO_FPS_N (&self->info);
  properties.fps_d = GST_VIDEO_INFO_FPS_D (&self->info);
  properties.width = GST_VIDEO_INFO_WIDTH (&self->info);
  properties.height = GST_VIDEO_INFO_HEIGHT (&self->info);
  properties.drop_mode = self->drop_mode;
  properties.drop_threshold = self->drop_threshold;
  properties.intra_refresh = self->intra_refresh;
  properties.super_mode = self->super_mode;
  properties.super_i_thd = self->super_i_thd;
  properties.super_p_thd = self->super_p_thd;
  properties.debreath = self->debreath;
  properties.debreath_strength = self->debreath_strength;
  properties.scene_mode = self->scene_mode;
  properties.anti_flicker = self->anti_flicker;
  properties.num_temporal_layers = self->num_temporal_layers;
  properties.ref_dirty = self->ref_dirty;
  if (snapshot_codec_properties)
    snapshot_codec_properties (encoder, codec_snapshot);
  self->prop_dirty = FALSE;
  self->ref_dirty = FALSE;
  GST_MPP_ENC_PROP_UNLOCK (encoder);

  if (applied)
    *applied = TRUE;

  /* Resolve the auto sentinel into the snapshot only. self->bps must keep its
   * 0, or the first geometry the encoder sees becomes the permanent target. */
  fps_out = gst_mpp_enc_effective_fps_out (&properties);
  if (!properties.bps)
    properties.bps = gst_mpp_enc_auto_bitrate (properties.width,
        properties.height, fps_out);

  if (configure_codec_properties)
    configure_codec_properties (encoder, codec_snapshot);

  /* Apply a pending temporal-layer reference-structure change before the rest
   * of the config; the ref cfg governs the GOP structure the RC then sizes to. */
  if (properties.ref_dirty)
    gst_mpp_enc_apply_ref_cfg (encoder, properties.num_temporal_layers);

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_SEI_CFG,
          &properties.sei_mode))
    GST_WARNING_OBJECT (self, "failed to set sei mode");

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_HEADER_MODE,
          &properties.header_mode))
    GST_WARNING_OBJECT (self, "failed to set header mode");

  gst_mpp_enc_cfg_set_s32 (self, "rc:gop",
      properties.gop < 0 ? fps_out : properties.gop);
  gst_mpp_enc_cfg_set_u32 (self, "rc:max_reenc_times", properties.max_reenc);
  gst_mpp_enc_cfg_set_s32 (self, "rc:mode", properties.rc_mode);

  if (!properties.bps || properties.rc_mode == MPP_ENC_RC_MODE_FIXQP) {
    /* BPS settings are ignored */
  } else if (properties.rc_mode == MPP_ENC_RC_MODE_CBR) {
    /* CBR mode has narrow bound */
    gst_mpp_enc_cfg_set_s32 (self, "rc:bps_target", properties.bps);
    gst_mpp_enc_cfg_set_s32 (self, "rc:bps_max", properties.bps_max ?
        (gint) properties.bps_max :
        gst_mpp_enc_scale_bitrate (properties.bps, 17, 16));
    gst_mpp_enc_cfg_set_s32 (self, "rc:bps_min", properties.bps_min ?
        (gint) properties.bps_min :
        gst_mpp_enc_scale_bitrate (properties.bps, 15, 16));
  } else {
    /* MPP_ENC_RC_MODE_VBR/MPP_ENC_RC_MODE_AVBR */
    /* VBR mode has wide bound */
    gst_mpp_enc_cfg_set_s32 (self, "rc:bps_target", properties.bps);
    gst_mpp_enc_cfg_set_s32 (self, "rc:bps_max", properties.bps_max ?
        (gint) properties.bps_max :
        gst_mpp_enc_scale_bitrate (properties.bps, 17, 16));
    gst_mpp_enc_cfg_set_s32 (self, "rc:bps_min", properties.bps_min ?
        (gint) properties.bps_min :
        gst_mpp_enc_scale_bitrate (properties.bps, 1, 16));
  }

  /* Output framerate (for runtime decimation) */
  if (properties.fps_out > 0) {
    gst_mpp_enc_cfg_set_s32 (self, "rc:fps_out_num", properties.fps_out);
    gst_mpp_enc_cfg_set_s32 (self, "rc:fps_out_denorm", 1);
  } else {
    gst_mpp_enc_cfg_set_s32 (self, "rc:fps_out_num", properties.fps_n);
    gst_mpp_enc_cfg_set_s32 (self, "rc:fps_out_denorm", properties.fps_d);
  }

  /* Frame drop mode */
  gst_mpp_enc_cfg_set_u32 (self, "rc:drop_mode", properties.drop_mode);
  gst_mpp_enc_cfg_set_u32 (self, "rc:drop_thd", properties.drop_threshold);

  /* Rolling intra refresh (per-row). Spreads I-blocks across frames so there
   * are no periodic IDR bitrate spikes, and gives continuous loss recovery on
   * a lossy link. Only enabled when a non-zero row count is requested. */
  if (properties.intra_refresh) {
    gst_mpp_enc_cfg_set_u32 (self, "rc:refresh_en", 1);
    gst_mpp_enc_cfg_set_u32 (self, "rc:refresh_mode",
        MPP_ENC_RC_INTRA_REFRESH_ROW);
    gst_mpp_enc_cfg_set_u32 (self, "rc:refresh_num", properties.intra_refresh);
  } else {
    gst_mpp_enc_cfg_set_u32 (self, "rc:refresh_en", 0);
  }

  /* Super-frame: bound a single coded frame's size so a scene cut / keyframe
   * cannot spike the send buffer. Written unconditionally, including when the
   * mode is off, because MPP keeps the last value it was given and a stale
   * threshold would otherwise resurrect when the mode is switched back on. */
  gst_mpp_enc_cfg_set_u32 (self, "rc:super_mode", properties.super_mode);
  gst_mpp_enc_cfg_set_u32 (self, "rc:super_i_thd",
      gst_mpp_enc_super_threshold (properties.super_i_thd));
  gst_mpp_enc_cfg_set_u32 (self, "rc:super_p_thd",
      gst_mpp_enc_super_threshold (properties.super_p_thd));

  /* De-breathing: smooths the GOP bitrate breathing oscillation. */
  gst_mpp_enc_cfg_set_u32 (self, "rc:debreath_en", properties.debreath ? 1 : 0);
  gst_mpp_enc_cfg_set_u32 (self, "rc:debreath_strength",
      properties.debreath_strength);

  /* Content-adaptive tuning. Written on every apply, including at 0: these are
   * the values an operator steps back down to turn the tuning off, and MPP has
   * no notion of "unset" -- it keeps whatever it was last told. The keys are
   * part of the pinned MPP's config surface, and gst_mpp_enc_cfg_set_* now
   * reports a build that disagrees rather than letting the write vanish. */
  gst_mpp_enc_cfg_set_s32 (self, "tune:scene_mode", properties.scene_mode);
  gst_mpp_enc_cfg_set_s32 (self, "tune:anti_flicker_str",
      properties.anti_flicker);

  if (self->cfg_error) {
    GST_ERROR_OBJECT (self, "refusing to apply a config MPP partly rejected");
    return FALSE;
  }

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_CFG, self->mpp_cfg)) {
    GST_WARNING_OBJECT (self, "failed to set enc cfg");
    return FALSE;
  }

  return TRUE;
}

gboolean
gst_mpp_enc_apply_properties (GstVideoEncoder * encoder)
{
  return gst_mpp_enc_apply_properties_full (encoder, NULL, NULL, NULL, NULL);
}

gboolean
gst_mpp_enc_set_src_caps (GstVideoEncoder * encoder, GstCaps * caps)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo *info = &self->info;
  GstVideoCodecState *output_state;

  gst_caps_set_simple (caps,
      "width", G_TYPE_INT, GST_VIDEO_INFO_WIDTH (info),
      "height", G_TYPE_INT, GST_VIDEO_INFO_HEIGHT (info), NULL);

  GST_DEBUG_OBJECT (self, "output caps: %" GST_PTR_FORMAT, caps);

  output_state = gst_video_encoder_set_output_state (encoder,
      caps, self->input_state);

  GST_VIDEO_INFO_WIDTH (&output_state->info) = GST_VIDEO_INFO_WIDTH (info);
  GST_VIDEO_INFO_HEIGHT (&output_state->info) = GST_VIDEO_INFO_HEIGHT (info);
  gst_video_codec_state_unref (output_state);

  return gst_video_encoder_negotiate (encoder);
}

static gboolean gst_mpp_enc_poll_packet_locked (GstVideoEncoder * encoder);

#define MPP_ENC_DRAIN_NO_PROGRESS_US (100 * 1000)

static void
gst_mpp_enc_stop_task (GstVideoEncoder * encoder, gboolean drain)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstTask *task = encoder->srcpad->task;

  if (!GST_MPP_ENC_TASK_STARTED (encoder))
    return;

  GST_DEBUG_OBJECT (self, "stopping encoding thread");

  /* Discard pending frames */
  if (!drain)
    GST_MPP_ENC_SET_PENDING (encoder, 0);

  GST_MPP_ENC_BROADCAST (encoder);

  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
  /* Wait for task thread to pause */
  if (task) {
    GST_OBJECT_LOCK (task);
    while (GST_TASK_STATE (task) == GST_TASK_STARTED)
      GST_TASK_WAIT (task);
    GST_OBJECT_UNLOCK (task);
  }

  gst_pad_stop_task (encoder->srcpad);
  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
}

static GstFlowReturn
gst_mpp_enc_reset (GstVideoEncoder * encoder, gboolean drain, gboolean final)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstFlowReturn result;
  gint64 no_progress_deadline;
  gboolean fully_drained;

  GST_DEBUG_OBJECT (self, "resetting");

  /* Publish before taking mutex: handle_frame may be waiting for capacity. */
  g_atomic_int_set (&self->flushing, TRUE);
  GST_MPP_ENC_BROADCAST (encoder);
  GST_MPP_ENC_LOCK (encoder);
  self->draining = drain;

  /* HACK: The MPP is not capable of handling resets properly. */
  self->draining = TRUE;

  gst_mpp_enc_stop_task (encoder, self->draining);

  g_atomic_int_set (&self->flushing, final);
  self->draining = FALSE;

  self->task_ret = GST_FLOW_OK;
  self->mpi->reset (self->mpp_ctx);

  /* MPP leaves encoder output queued across reset. Drain through the normal
   * output path so stale packets cannot be assigned to the next session. */
  no_progress_deadline =
      g_get_monotonic_time () + MPP_ENC_DRAIN_NO_PROGRESS_US;
  while (g_get_monotonic_time () < no_progress_deadline) {
    if (gst_mpp_enc_poll_packet_locked (encoder)) {
      no_progress_deadline =
          g_get_monotonic_time () + MPP_ENC_DRAIN_NO_PROGRESS_US;
      continue;
    }
    g_usleep (1000);
  }

  result = self->task_ret;
  fully_drained =
      GST_MPP_ENC_PENDING (encoder) == 0 && self->frames == NULL;
  if (drain && !fully_drained) {
    GST_ERROR_OBJECT (self,
        "EOS drain timed out with %d pending and %u unsubmitted frames",
        GST_MPP_ENC_PENDING (encoder), g_list_length (self->frames));
    result = GST_FLOW_ERROR;
  }

  self->task_ret = GST_FLOW_OK;
  GST_MPP_ENC_SET_PENDING (encoder, 0);

  if (self->frames) {
    g_list_free (self->frames);
    self->frames = NULL;
  }

  /* Force re-apply prop */
  GST_MPP_ENC_PROP_LOCK (encoder);
  self->prop_dirty = TRUE;
  GST_MPP_ENC_PROP_UNLOCK (encoder);

  GST_MPP_ENC_UNLOCK (encoder);
  return result;
}

static gboolean
gst_mpp_enc_start (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  MppPollType timeout;

  GST_DEBUG_OBJECT (self, "starting");

  gst_video_info_init (&self->info);

  self->allocator = gst_mpp_allocator_new ();
  if (!self->allocator)
    return FALSE;

  gst_mpp_allocator_set_cacheable (self->allocator, FALSE);

  if (mpp_create (&self->mpp_ctx, &self->mpi))
    goto err_unref_alloc;

  timeout = MPP_POLL_NON_BLOCK;
  if (self->mpi->control (self->mpp_ctx, MPP_SET_INPUT_TIMEOUT, &timeout))
    goto err_destroy_mpp;

  /* 1ms timeout for polling */
  timeout = 1;
  if (self->mpi->control (self->mpp_ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout))
    goto err_destroy_mpp;

  if (mpp_init (self->mpp_ctx, MPP_CTX_ENC, self->mpp_type))
    goto err_destroy_mpp;

  if (mpp_frame_init (&self->mpp_frame))
    goto err_destroy_mpp;

  if (mpp_enc_cfg_init (&self->mpp_cfg))
    goto err_deinit_frame;

  self->cfg_error = FALSE;

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_GET_CFG, self->mpp_cfg))
    goto err_deinit_cfg;

  self->task_ret = GST_FLOW_OK;
  GST_MPP_ENC_PROP_LOCK (encoder);
  self->input_state = NULL;
  GST_MPP_ENC_PROP_UNLOCK (encoder);
  g_atomic_int_set (&self->flushing, FALSE);
  GST_MPP_ENC_SET_PENDING (encoder, 0);
  self->frames = NULL;

  g_mutex_init (&self->mutex);

  g_mutex_init (&self->event_mutex);
  g_cond_init (&self->event_cond);

  GST_DEBUG_OBJECT (self, "started");

  return TRUE;

err_deinit_cfg:
  mpp_enc_cfg_deinit (self->mpp_cfg);
err_deinit_frame:
  mpp_frame_deinit (&self->mpp_frame);
err_destroy_mpp:
  mpp_destroy (self->mpp_ctx);
err_unref_alloc:
  gst_object_unref (self->allocator);
  return FALSE;
}

static gboolean
gst_mpp_enc_stop (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoCodecState *input_state;

  GST_DEBUG_OBJECT (self, "stopping");

  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
  gst_mpp_enc_reset (encoder, FALSE, TRUE);
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);

  g_cond_clear (&self->event_cond);
  g_mutex_clear (&self->event_mutex);

  g_mutex_clear (&self->mutex);

  mpp_enc_cfg_deinit (self->mpp_cfg);
  if (self->ref_cfg) {
    mpp_enc_ref_cfg_deinit (&self->ref_cfg);
    self->ref_cfg = NULL;
  }
  mpp_frame_set_buffer (self->mpp_frame, NULL);
  mpp_frame_deinit (&self->mpp_frame);
  mpp_destroy (self->mpp_ctx);

  gst_object_unref (self->allocator);

  GST_MPP_ENC_PROP_LOCK (encoder);
  input_state = self->input_state;
  self->input_state = NULL;
  GST_MPP_ENC_PROP_UNLOCK (encoder);
  if (input_state)
    gst_video_codec_state_unref (input_state);

  GST_DEBUG_OBJECT (self, "stopped");

  return TRUE;
}

static gboolean
gst_mpp_enc_flush (GstVideoEncoder * encoder)
{
  GST_DEBUG_OBJECT (encoder, "flushing");
  return gst_mpp_enc_reset (encoder, FALSE, FALSE) == GST_FLOW_OK;
}

static GstFlowReturn
gst_mpp_enc_finish (GstVideoEncoder * encoder)
{
  GST_DEBUG_OBJECT (encoder, "finishing");
  return gst_mpp_enc_reset (encoder, TRUE, FALSE);
}

static gboolean
gst_mpp_enc_apply_strides (GstVideoEncoder * encoder, gint hstride,
    gint vstride)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo *info = &self->info;

  mpp_frame_set_hor_stride (self->mpp_frame, hstride);
  mpp_frame_set_ver_stride (self->mpp_frame, vstride);
  gst_mpp_enc_cfg_set_s32 (self, "prep:hor_stride", hstride);
  gst_mpp_enc_cfg_set_s32 (self, "prep:ver_stride", vstride);

  if (hstride == GST_MPP_VIDEO_INFO_HSTRIDE (info) &&
      vstride == GST_MPP_VIDEO_INFO_VSTRIDE (info))
    return TRUE;

  GST_INFO_OBJECT (self, "strides updated to (%dx%d)", hstride, vstride);

  GST_MPP_ENC_PROP_LOCK (encoder);
  self->prop_dirty = TRUE;
  GST_MPP_ENC_PROP_UNLOCK (encoder);
  return gst_mpp_video_info_align (info, hstride, vstride);
}

static gboolean
gst_mpp_enc_set_format (GstVideoEncoder * encoder, GstVideoCodecState * state)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo *info = &self->info;
  GstVideoCodecState *old_state;
  MppFrameFormat format;
  gint width, height, hstride, vstride, rotation, prop_width, prop_height;
  gboolean arm_afbc;

  GST_DEBUG_OBJECT (self, "setting format: %" GST_PTR_FORMAT, state->caps);

  GST_MPP_ENC_PROP_LOCK (encoder);
  old_state = self->input_state;
  if (old_state && gst_caps_is_strictly_equal (old_state->caps, state->caps)) {
    GST_MPP_ENC_PROP_UNLOCK (encoder);
    return TRUE;
  }
  GST_MPP_ENC_PROP_UNLOCK (encoder);

  if (old_state) {
    if (gst_mpp_enc_reset (encoder, TRUE, FALSE) != GST_FLOW_OK)
      return FALSE;

    GST_MPP_ENC_PROP_LOCK (encoder);
    self->input_state = NULL;
    GST_MPP_ENC_PROP_UNLOCK (encoder);
    gst_video_codec_state_unref (old_state);
  }

  GST_MPP_ENC_PROP_LOCK (encoder);
  self->input_state = gst_video_codec_state_ref (state);
  rotation = self->rotation;
  prop_width = self->width;
  prop_height = self->height;
  arm_afbc = self->arm_afbc;
  GST_MPP_ENC_PROP_UNLOCK (encoder);

  *info = state->info;

  if (!gst_mpp_enc_video_info_align (info))
    return FALSE;

  format = gst_mpp_gst_format_to_mpp_format (GST_VIDEO_INFO_FORMAT (info));
  width = GST_VIDEO_INFO_WIDTH (info);
  height = GST_VIDEO_INFO_HEIGHT (info);

  if (rotation % 180)
    SWAP (width, height);

  width = prop_width ? : width;
  height = prop_height ? : height;

  /*
   * Check for conversion
   * NOTE: Not checking the strides here, since they might not be the actual
   * ones (could be overrided by video-meta)
   */
  if (rotation || format == MPP_FMT_BUTT ||
      width != GST_VIDEO_INFO_WIDTH (info) ||
      height != GST_VIDEO_INFO_HEIGHT (info)) {
    if (!gst_mpp_use_rga ()) {
      GST_ERROR_OBJECT (self, "unable to convert without RGA");
      return FALSE;
    }

    /* Prefer NV12 when using RGA conversion */
    format = MPP_FMT_YUV420SP;

    gst_mpp_video_info_update_format (info,
        gst_mpp_mpp_format_to_gst_format (format), width, height);

    if (!gst_mpp_enc_video_info_align (info))
      return FALSE;

    GST_INFO_OBJECT (self, "converting to aligned %s",
        gst_mpp_video_format_to_string (GST_VIDEO_INFO_FORMAT (info)));
  }

  hstride = GST_MPP_VIDEO_INFO_HSTRIDE (info);
  vstride = GST_MPP_VIDEO_INFO_VSTRIDE (info);

  GST_INFO_OBJECT (self, "applying %s%s %dx%d (%dx%d)",
      gst_mpp_video_format_to_string (GST_VIDEO_INFO_FORMAT (info)),
      arm_afbc ? "(AFBC)" : "", width, height, hstride, vstride);

  if (arm_afbc) {
    if (self->mpp_type != MPP_VIDEO_CodingAVC &&
        self->mpp_type != MPP_VIDEO_CodingHEVC) {
      GST_WARNING_OBJECT (self, "Only H.264 and H.265 support ARM AFBC");
      GST_MPP_ENC_PROP_LOCK (encoder);
      self->arm_afbc = FALSE;
      GST_MPP_ENC_PROP_UNLOCK (encoder);
    } else {
      format |= MPP_FRAME_FBC_AFBC_V2;
    }
  }

  mpp_frame_set_fmt (self->mpp_frame, format);
  mpp_frame_set_width (self->mpp_frame, width);
  mpp_frame_set_height (self->mpp_frame, height);

  if (!GST_VIDEO_INFO_FPS_N (info) ||
      GST_VIDEO_INFO_FPS_N (info) / GST_VIDEO_INFO_FPS_D (info) > 256) {
    GST_WARNING_OBJECT (self, "framerate (%d/%d) is insane!",
        GST_VIDEO_INFO_FPS_N (info), GST_VIDEO_INFO_FPS_D (info));
    GST_VIDEO_INFO_FPS_N (info) = DEFAULT_FPS;
    GST_VIDEO_INFO_FPS_D (info) = 1;
  }

  gst_mpp_enc_cfg_set_s32 (self, "prep:format", format);
  gst_mpp_enc_cfg_set_s32 (self, "prep:width", width);
  gst_mpp_enc_cfg_set_s32 (self, "prep:height", height);

  gst_mpp_enc_cfg_set_s32 (self, "rc:fps_in_flex", 0);
  gst_mpp_enc_cfg_set_s32 (self, "rc:fps_in_num", GST_VIDEO_INFO_FPS_N (info));
  gst_mpp_enc_cfg_set_s32 (self, "rc:fps_in_denorm",
      GST_VIDEO_INFO_FPS_D (info));
  gst_mpp_enc_cfg_set_s32 (self, "rc:fps_out_flex", 0);
  gst_mpp_enc_cfg_set_s32 (self, "rc:fps_out_num", GST_VIDEO_INFO_FPS_N (info));
  gst_mpp_enc_cfg_set_s32 (self, "rc:fps_out_denorm",
      GST_VIDEO_INFO_FPS_D (info));

  return gst_mpp_enc_apply_strides (encoder, hstride, vstride);
}

/*
 * Re-derive the scaler target and encoder config from the current input caps
 * plus the (possibly just-changed) width/height properties, without tearing the
 * pipeline down. This mirrors the resolution-relevant half of set_format and is
 * driven off self->input_state->info rather than a fresh caps event, so the
 * output resolution can be stepped at runtime (the degradation ladder).
 *
 * ASSUMPTION (validate on hardware): MPP honours a prep:width/height change
 * pushed via MPP_ENC_SET_CFG on a running context by reconfiguring the vepu
 * pre-scaler and emitting a fresh IDR. If MPP instead needs a full ctx reinit,
 * this reconfigure will misbehave and the change should be reverted in favour
 * of a caps-reneg restart.
 */
static gboolean
gst_mpp_enc_apply_pending_resolution (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo *info = &self->info;
  GstVideoCodecState *input_state;
  MppFrameFormat format;
  gint width, height, hstride, vstride, rotation, prop_width, prop_height;
  gboolean arm_afbc;
  GstCaps *caps;

  GST_MPP_ENC_PROP_LOCK (encoder);
  if (!self->res_dirty || !self->input_state) {
    GST_MPP_ENC_PROP_UNLOCK (encoder);
    return TRUE;
  }

  self->res_dirty = FALSE;
  /* New geometry invalidates the automatic bitrate derived from the old one. */
  self->prop_dirty = TRUE;
  input_state = self->input_state;
  rotation = self->rotation;
  prop_width = self->width;
  prop_height = self->height;
  arm_afbc = self->arm_afbc;
  GST_MPP_ENC_PROP_UNLOCK (encoder);

  *info = input_state->info;

  if (!gst_mpp_enc_video_info_align (info))
    return FALSE;

  format = gst_mpp_gst_format_to_mpp_format (GST_VIDEO_INFO_FORMAT (info));
  width = GST_VIDEO_INFO_WIDTH (info);
  height = GST_VIDEO_INFO_HEIGHT (info);

  if (rotation % 180)
    SWAP (width, height);

  width = prop_width ? : width;
  height = prop_height ? : height;

  if (rotation || format == MPP_FMT_BUTT ||
      width != GST_VIDEO_INFO_WIDTH (info) ||
      height != GST_VIDEO_INFO_HEIGHT (info)) {
    if (!gst_mpp_use_rga ()) {
      GST_ERROR_OBJECT (self, "unable to rescale without RGA");
      return FALSE;
    }

    format = MPP_FMT_YUV420SP;

    gst_mpp_video_info_update_format (info,
        gst_mpp_mpp_format_to_gst_format (format), width, height);

    if (!gst_mpp_enc_video_info_align (info))
      return FALSE;
  }

  hstride = GST_MPP_VIDEO_INFO_HSTRIDE (info);
  vstride = GST_MPP_VIDEO_INFO_VSTRIDE (info);

  if (arm_afbc && (self->mpp_type == MPP_VIDEO_CodingAVC ||
          self->mpp_type == MPP_VIDEO_CodingHEVC))
    format |= MPP_FRAME_FBC_AFBC_V2;

  GST_INFO_OBJECT (self, "rescaling to %dx%d (%dx%d)", width, height,
      hstride, vstride);

  mpp_frame_set_fmt (self->mpp_frame, format);
  mpp_frame_set_width (self->mpp_frame, width);
  mpp_frame_set_height (self->mpp_frame, height);

  gst_mpp_enc_cfg_set_s32 (self, "prep:format", format);
  gst_mpp_enc_cfg_set_s32 (self, "prep:width", width);
  gst_mpp_enc_cfg_set_s32 (self, "prep:height", height);

  if (!gst_mpp_enc_apply_strides (encoder, hstride, vstride))
    return FALSE;

  if (self->cfg_error) {
    GST_ERROR_OBJECT (self, "refusing to apply a config MPP partly rejected");
    return FALSE;
  }

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_CFG, self->mpp_cfg))
    return FALSE;

  /* Renegotiate the encoded caps so downstream sees the new resolution.
   * set_src_caps() overwrites width/height from self->info and takes ownership
   * of the caps (via gst_video_encoder_set_output_state), so hand it a writable
   * ref and do not unref afterwards. */
  caps = gst_pad_get_current_caps (encoder->srcpad);
  if (caps) {
    caps = gst_caps_make_writable (caps);
    if (!gst_mpp_enc_set_src_caps (encoder, caps))
      return FALSE;
  }

  return TRUE;
}

static gboolean
gst_mpp_enc_propose_allocation (GstVideoEncoder * encoder, GstQuery * query)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstStructure *config, *params;
  GstVideoAlignment align;
  GstBufferPool *pool;
  GstVideoInfo info;
  GstCaps *caps;
  guint size;

  GST_DEBUG_OBJECT (self, "propose allocation");

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps == NULL)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  gst_mpp_enc_video_info_align (&info);
  size = GST_VIDEO_INFO_SIZE (&info);

  gst_video_alignment_reset (&align);
  align.padding_right = gst_mpp_get_pixel_stride (&info) -
      GST_VIDEO_INFO_WIDTH (&info);
  align.padding_bottom = GST_MPP_VIDEO_INFO_VSTRIDE (&info) -
      GST_VIDEO_INFO_HEIGHT (&info);

  /* Expose alignment to video-meta */
  params = gst_structure_new ("video-meta",
      "padding-top", G_TYPE_UINT, align.padding_top,
      "padding-bottom", G_TYPE_UINT, align.padding_bottom,
      "padding-left", G_TYPE_UINT, align.padding_left,
      "padding-right", G_TYPE_UINT, align.padding_right, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, params);
  gst_structure_free (params);

  pool = gst_video_buffer_pool_new ();

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, 0, 0);
  gst_buffer_pool_config_set_allocator (config, self->allocator, NULL);

  /* Expose alignment to pool */
  gst_buffer_pool_config_add_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
  gst_buffer_pool_config_set_video_alignment (config, &align);

  gst_buffer_pool_set_config (pool, config);

  gst_query_add_allocation_pool (query, pool, size, 2, 0);
  gst_query_add_allocation_param (query, self->allocator, NULL);

  gst_object_unref (pool);

  return GST_VIDEO_ENCODER_CLASS (parent_class)->propose_allocation (encoder,
      query);
}

static GstBuffer *
gst_mpp_enc_convert (GstVideoEncoder * encoder, GstVideoCodecFrame * frame)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo src_info;
  GstVideoInfo dst_info = self->info;
  GstVideoFrame src_frame, dst_frame;
  GstBuffer *outbuf, *inbuf;
  GstMemory *in_mem, *out_mem = NULL;
  GstVideoMeta *meta;
  gsize size, maxsize, offset;
  gint src_hstride, src_vstride, rotation;
  gboolean strides_changed;
  guint i;

  GST_MPP_ENC_PROP_LOCK (encoder);
  src_info = self->input_state->info;
  rotation = self->rotation;
  GST_MPP_ENC_PROP_UNLOCK (encoder);

  inbuf = frame->input_buffer;

  meta = gst_buffer_get_video_meta (inbuf);
  if (meta) {
    for (i = 0; i < meta->n_planes; i++) {
      GST_VIDEO_INFO_PLANE_STRIDE (&src_info, i) = meta->stride[i];
      GST_VIDEO_INFO_PLANE_OFFSET (&src_info, i) = meta->offset[i];
    }
  }

  size = gst_buffer_get_sizes (inbuf, &offset, &maxsize);
  if (size < GST_VIDEO_INFO_SIZE (&src_info)) {
    GST_ERROR_OBJECT (self, "input buffer too small (%" G_GSIZE_FORMAT
        " < %" G_GSIZE_FORMAT ")", size, GST_VIDEO_INFO_SIZE (&src_info));
    return NULL;
  }

  outbuf = gst_buffer_new ();
  if (!outbuf)
    goto err;

  if (rotation)
    goto convert;

  if (gst_buffer_n_memory (inbuf) != 1)
    goto convert;

  in_mem = gst_buffer_peek_memory (inbuf, 0);

  out_mem = gst_mpp_allocator_import_gst_memory (self->allocator, in_mem);
  if (!out_mem)
    goto convert;

  src_hstride = GST_MPP_VIDEO_INFO_HSTRIDE (&src_info);
  src_vstride = GST_MPP_VIDEO_INFO_VSTRIDE (&src_info);

  /**
   * Update the strides of the dst video info temporarily to test if we
   * can use the src buffer directly.
   */
  if (!gst_mpp_video_info_align (&dst_info, src_hstride, src_vstride) ||
      !gst_mpp_enc_video_info_align (&dst_info) ||
      !gst_mpp_video_info_matched (&src_info, &dst_info)) {
    /* Reset the temporarily modified dst video info. */
    dst_info = self->info;
    goto convert;
  }

  strides_changed = src_hstride != GST_MPP_VIDEO_INFO_HSTRIDE (&self->info) ||
      src_vstride != GST_MPP_VIDEO_INFO_VSTRIDE (&self->info);
  gst_mpp_enc_apply_strides (encoder, src_hstride, src_vstride);
  if (strides_changed && self->mpi->control (self->mpp_ctx, MPP_ENC_SET_CFG,
          self->mpp_cfg))
    goto err;

  /*
   * Invariant: out_mem names a reference this function still owns. Appending
   * transfers it to outbuf, so clearing it is what lets convert: and err:
   * release it without knowing which path reached them.
   */
  gst_buffer_append_memory (outbuf, out_mem);
  out_mem = NULL;

  /* Keep a ref of the original memory */
  gst_buffer_append_memory (outbuf, gst_memory_ref (in_mem));

  GST_DEBUG_OBJECT (self, "using imported buffer");
  goto out;

convert:
  if (out_mem)
    gst_memory_unref (out_mem);

  out_mem = gst_allocator_alloc (self->allocator,
      GST_VIDEO_INFO_SIZE (&dst_info), NULL);
  if (!out_mem)
    goto err;

  gst_buffer_append_memory (outbuf, out_mem);
  out_mem = NULL;

#ifdef HAVE_RGA
  /* outbuf owns the memory now, so the blit borrows it back rather than
   * keeping a second name for it alive across the error paths below. */
  if (gst_mpp_use_rga () &&
      gst_mpp_rga_convert (inbuf, &src_info,
          gst_buffer_peek_memory (outbuf, 0), &dst_info, rotation)) {
    GST_DEBUG_OBJECT (self, "using RGA converted buffer");
    goto out;
  }
#endif

  if (rotation ||
      GST_VIDEO_INFO_FORMAT (&src_info) != GST_VIDEO_INFO_FORMAT (&dst_info))
    goto err;

  /*
   * Nothing below writes the MPP buffer, so a skipped copy leaves it holding
   * whatever the allocator last put there. Both maps must therefore fail the
   * conversion rather than fall through to the success path.
   */
  if (!gst_video_frame_map (&src_frame, &src_info, inbuf, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "failed to map input frame");
    goto err;
  }

  if (!gst_video_frame_map (&dst_frame, &dst_info, outbuf, GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (self, "failed to map converted frame");
    gst_video_frame_unmap (&src_frame);
    goto err;
  }

  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
  if (!gst_video_frame_copy (&dst_frame, &src_frame)) {
    GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
    gst_video_frame_unmap (&dst_frame);
    gst_video_frame_unmap (&src_frame);
    goto err;
  }
  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
  gst_video_frame_unmap (&dst_frame);
  gst_video_frame_unmap (&src_frame);

  GST_DEBUG_OBJECT (self, "using software converted buffer");

out:
  gst_buffer_copy_into (outbuf, inbuf,
      GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0, 0);

  gst_buffer_add_video_meta_full (outbuf, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (&dst_info),
      GST_VIDEO_INFO_WIDTH (&dst_info), GST_VIDEO_INFO_HEIGHT (&dst_info),
      GST_VIDEO_INFO_N_PLANES (&dst_info), dst_info.offset, dst_info.stride);

  return outbuf;
err:
  if (out_mem)
    gst_memory_unref (out_mem);

  if (outbuf)
    gst_buffer_unref (outbuf);

  GST_ERROR_OBJECT (self, "failed to convert frame");
  return NULL;
}

static gboolean
gst_mpp_enc_send_frame_locked (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoCodecFrame *frame;
  GstMemory *mem;
  MppFrame mframe;
  MppBuffer mbuf;
  gboolean keyframe;
  guint32 frame_number;

  if (!self->frames)
    return FALSE;

  if (mpp_frame_init (&mframe))
    return FALSE;

  mpp_frame_set_fmt (mframe, mpp_frame_get_fmt (self->mpp_frame));
  mpp_frame_set_width (mframe, mpp_frame_get_width (self->mpp_frame));
  mpp_frame_set_height (mframe, mpp_frame_get_height (self->mpp_frame));
  mpp_frame_set_hor_stride (mframe, mpp_frame_get_hor_stride (self->mpp_frame));
  mpp_frame_set_ver_stride (mframe, mpp_frame_get_ver_stride (self->mpp_frame));

  frame_number = GPOINTER_TO_UINT (g_list_nth_data (self->frames, 0));
  frame = gst_video_encoder_get_frame (encoder, frame_number);

  keyframe = GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (frame);
  if (keyframe) {
    GST_INFO_OBJECT (self, "forcing keyframe");

    if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_IDR_FRAME, NULL))
      GST_WARNING_OBJECT (self, "failed to set keyframe request");
  }

  /* HACK: Get the converted input buffer from frame->output_buffer */
  mem = gst_buffer_peek_memory (frame->output_buffer, 0);
  mbuf = gst_mpp_mpp_buffer_from_gst_memory (mem);
  mpp_frame_set_buffer (mframe, mbuf);

  gst_video_codec_frame_unref (frame);

  if (self->mpi->encode_put_frame (self->mpp_ctx, mframe)) {
    mpp_frame_deinit (&mframe);
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "encoding frame %d", frame_number);
  self->frames = g_list_delete_link (self->frames, self->frames);
  return TRUE;
}

static gboolean
gst_mpp_enc_poll_packet_locked (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoCodecFrame *frame;
  GstBuffer *buffer;
  GstMemory *mem;
  MppFrame mframe;
  MppPacket mpkt;
  MppMeta meta;
  MppBuffer mbuf;
  RK_S32 output_intra;
  gint pkt_size;
  gboolean zero_copy_pkt;

  GST_MPP_ENC_PROP_LOCK (encoder);
  zero_copy_pkt = self->zero_copy_pkt;
  GST_MPP_ENC_PROP_UNLOCK (encoder);

  self->mpi->encode_get_packet (self->mpp_ctx, &mpkt);
  if (!mpkt)
    return FALSE;

  /* Deinit input frame */
  meta = mpp_packet_get_meta (mpkt);
  if (!mpp_meta_get_frame (meta, KEY_INPUT_FRAME, &mframe))
    mpp_frame_deinit (&mframe);

  /* MPP reports whether it coded this output as an intra/IDR picture here. */
  if (mpp_meta_get_s32 (meta, KEY_OUTPUT_INTRA, &output_intra))
    output_intra = 0;

  /* Wake up the frame producer */
  g_atomic_int_add (&self->pending_frames, -1);
  GST_MPP_ENC_BROADCAST (encoder);

  /* This encoded frame must be the oldest one */
  frame = gst_video_encoder_get_oldest_frame (encoder);
  if (!frame) {
    GST_WARNING_OBJECT (self, "dropping packet without an oldest frame");
    goto out;
  }

  if (GST_MPP_ENC_FLUSHING (encoder) && !self->draining)
    goto drop;

  pkt_size = mpp_packet_get_length (mpkt);
  mbuf = mpp_packet_get_buffer (mpkt);
  if (!mbuf)
    goto error;

  /* An MPP rate-controller drop still returns a packet, with its buffer
   * attached and zero length. Wrapping that in a GstBuffer feeds an empty
   * access unit to the muxer instead of reporting the frame as dropped. */
  if (pkt_size <= 0)
    goto rc_drop;

  if (zero_copy_pkt) {
    buffer = gst_buffer_new ();
    if (!buffer)
      goto error;

    /* Allocated from the same DRM allocator in MPP */
    mpp_buffer_set_index (mbuf, gst_mpp_allocator_get_index (self->allocator));

    mem = gst_mpp_allocator_import_mppbuf (self->allocator, mbuf);
    if (!mem) {
      gst_buffer_unref (buffer);
      goto error;
    }

    gst_memory_resize (mem, 0, pkt_size);
    gst_buffer_append_memory (buffer, mem);
  } else {
    buffer = gst_video_encoder_allocate_output_buffer (encoder, pkt_size);
    if (!buffer)
      goto error;

    gst_buffer_fill (buffer, 0, mpp_buffer_get_ptr (mbuf), pkt_size);
  }

  gst_buffer_replace (&frame->output_buffer, buffer);
  gst_buffer_unref (buffer);

  /* Must precede finish_frame(): the base class turns the sync-point flag into
   * the downstream DELTA_UNIT flag and its force-key-unit bookkeeping. */
  if (output_intra)
    GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (frame);
  else
    GST_VIDEO_CODEC_FRAME_UNSET_SYNC_POINT (frame);

  GST_DEBUG_OBJECT (self, "finish frame ts=%" GST_TIME_FORMAT,
      GST_TIME_ARGS (frame->pts));

  self->task_ret = gst_video_encoder_finish_frame (encoder, frame);

out:
  mpp_packet_deinit (&mpkt);
  return TRUE;
error:
  GST_WARNING_OBJECT (self, "can't process this frame");
  goto drop;
rc_drop:
  GST_DEBUG_OBJECT (self, "rate controller dropped this frame");
drop:
  GST_DEBUG_OBJECT (self, "drop frame");
  gst_buffer_replace (&frame->output_buffer, NULL);
  self->task_ret = gst_video_encoder_finish_frame (encoder, frame);
  goto out;
}

static void
gst_mpp_enc_loop (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  gboolean packet_progress = FALSE;

  GST_MPP_ENC_WAIT (encoder,
      GST_MPP_ENC_PENDING (encoder) || GST_MPP_ENC_FLUSHING (encoder));

  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);

  if (GST_MPP_ENC_FLUSHING (encoder) && !GST_MPP_ENC_PENDING (encoder)) {
    GST_INFO_OBJECT (self, "flushing");
    self->task_ret = GST_FLOW_FLUSHING;
    goto out;
  }

  /* Try sending ready frames to MPP (non-block) */
  while (gst_mpp_enc_send_frame_locked (encoder));

  /* Try polling encoded packets from MPP (1ms timeout) */
  while (gst_mpp_enc_poll_packet_locked (encoder))
    packet_progress = TRUE;

  if (GST_MPP_ENC_FLUSHING (encoder) && GST_MPP_ENC_PENDING (encoder) &&
      !packet_progress) {
    gint64 deadline = g_get_monotonic_time () + MPP_ENC_DRAIN_NO_PROGRESS_US;
    do {
      g_usleep (1000);
      packet_progress = gst_mpp_enc_poll_packet_locked (encoder);
    } while (!packet_progress && g_get_monotonic_time () < deadline);

    if (!packet_progress) {
      GST_WARNING_OBJECT (self,
          "pausing encoder drain with %d pending frames after no output progress",
          GST_MPP_ENC_PENDING (encoder));
      self->task_ret = GST_FLOW_FLUSHING;
    }
  }

out:
  if (self->task_ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (self, "leaving output thread: %s",
        gst_flow_get_name (self->task_ret));

    gst_pad_pause_task (encoder->srcpad);
  }

  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
}

static guint
gst_mpp_enc_get_max_pending (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  guint max_pending;

  GST_MPP_ENC_PROP_LOCK (encoder);
  max_pending = self->max_pending;
  GST_MPP_ENC_PROP_UNLOCK (encoder);
  return max_pending;
}

static GstFlowReturn
gst_mpp_enc_handle_frame (GstVideoEncoder * encoder, GstVideoCodecFrame * frame)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstBuffer *buffer;
  GstFlowReturn ret = GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "handling frame %d", frame->system_frame_number);

  GST_MPP_ENC_LOCK (encoder);

  if (G_UNLIKELY (GST_MPP_ENC_FLUSHING (encoder)))
    goto flushing;

  if (G_UNLIKELY (!GST_MPP_ENC_TASK_STARTED (encoder))) {
    GST_DEBUG_OBJECT (self, "starting encoding thread");

    gst_pad_start_task (encoder->srcpad,
        (GstTaskFunction) gst_mpp_enc_loop, encoder, NULL);
  }

  /* Apply a pending runtime resolution change before converting this frame. */
  if (G_UNLIKELY (!gst_mpp_enc_apply_pending_resolution (encoder)))
    goto not_negotiated;

  buffer = gst_mpp_enc_convert (encoder, frame);
  if (G_UNLIKELY (!buffer))
    goto not_negotiated;

  /* HACK: store the converted input buffer in frame->output_buffer */
  frame->output_buffer = buffer;

  /* Avoid holding too many frames */
  if (G_UNLIKELY (GST_MPP_ENC_PENDING (encoder) >=
          (gint) gst_mpp_enc_get_max_pending (encoder))) {
    GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
    GST_MPP_ENC_WAIT (encoder,
        GST_MPP_ENC_PENDING (encoder) <
        (gint) gst_mpp_enc_get_max_pending (encoder) ||
        GST_MPP_ENC_FLUSHING (encoder));
    GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
  }

  if (G_UNLIKELY (GST_MPP_ENC_FLUSHING (encoder)))
    goto flushing;

  g_atomic_int_inc (&self->pending_frames);
  self->frames =
      g_list_append (self->frames,
      GUINT_TO_POINTER (frame->system_frame_number));

  GST_MPP_ENC_BROADCAST (encoder);

  gst_video_codec_frame_unref (frame);

  GST_MPP_ENC_UNLOCK (encoder);

  return self->task_ret;

flushing:
  GST_WARNING_OBJECT (self, "flushing");
  ret = GST_FLOW_FLUSHING;
  goto drop;
not_negotiated:
  GST_ERROR_OBJECT (self, "not negotiated");
  ret = GST_FLOW_NOT_NEGOTIATED;
  goto drop;
drop:
  GST_WARNING_OBJECT (self, "can't handle this frame");
  gst_video_encoder_finish_frame (encoder, frame);

  GST_MPP_ENC_UNLOCK (encoder);

  return ret;
}

static GstStateChangeReturn
gst_mpp_enc_change_state (GstElement * element, GstStateChange transition)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (element);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
    gst_mpp_enc_reset (encoder, FALSE, TRUE);
    GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_mpp_enc_finalize (GObject * object)
{
  GstMppEnc *self = GST_MPP_ENC (object);

  g_mutex_clear (&self->prop_mutex);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_mpp_enc_init (GstMppEnc * self)
{
  g_mutex_init (&self->prop_mutex);

  self->mpp_type = MPP_VIDEO_CodingUnused;

  self->max_pending = DEFAULT_PROP_MAX_PENDING;

  self->header_mode = DEFAULT_PROP_HEADER_MODE;
  self->sei_mode = DEFAULT_PROP_SEI_MODE;
  self->rc_mode = DEFAULT_PROP_RC_MODE;
  self->rotation = DEFAULT_PROP_ROTATION;
  self->gop = DEFAULT_PROP_GOP;
  self->max_reenc = DEFAULT_PROP_MAX_REENC;
  self->bps = DEFAULT_PROP_BPS;
  self->bps_min = DEFAULT_PROP_BPS_MIN;
  self->bps_max = DEFAULT_PROP_BPS_MAX;
  self->zero_copy_pkt = DEFAULT_PROP_ZERO_COPY_PKT;
  self->arm_afbc = DEFAULT_PROP_ARM_AFBC;
  self->fps_out = DEFAULT_PROP_FPS_OUT;
  self->drop_mode = DEFAULT_PROP_DROP_MODE;
  self->drop_threshold = DEFAULT_PROP_DROP_THRESHOLD;
  self->intra_refresh = DEFAULT_PROP_INTRA_REFRESH;
  self->super_mode = DEFAULT_PROP_SUPER_MODE;
  self->super_i_thd = DEFAULT_PROP_SUPER_I_THD;
  self->super_p_thd = DEFAULT_PROP_SUPER_P_THD;
  self->debreath = DEFAULT_PROP_DEBREATH;
  self->debreath_strength = DEFAULT_PROP_DEBREATH_STRENGTH;
  self->scene_mode = DEFAULT_PROP_SCENE_MODE;
  self->anti_flicker = DEFAULT_PROP_ANTI_FLICKER;
  self->prop_dirty = TRUE;
}

#define GST_TYPE_MPP_ENC_HEADER_MODE (gst_mpp_enc_header_mode_get_type ())
static GType
gst_mpp_enc_header_mode_get_type (void)
{
  static GType header_mode = 0;

  if (!header_mode) {
    static const GEnumValue modes[] = {
      {MPP_ENC_HEADER_MODE_DEFAULT, "Only in the first frame", "first-frame"},
      {MPP_ENC_HEADER_MODE_EACH_IDR, "In every IDR frames", "each-idr"},
      {0, NULL, NULL}
    };
    header_mode = g_enum_register_static ("MppEncHeaderMode", modes);
  }
  return header_mode;
}

#define GST_TYPE_MPP_ENC_SEI_MODE (gst_mpp_enc_sei_mode_get_type ())
static GType
gst_mpp_enc_sei_mode_get_type (void)
{
  static GType sei_mode = 0;

  if (!sei_mode) {
    static const GEnumValue modes[] = {
      {MPP_ENC_SEI_MODE_DISABLE, "SEI disabled", "disable"},
      {MPP_ENC_SEI_MODE_ONE_SEQ, "One SEI per sequence", "one-seq"},
      {MPP_ENC_SEI_MODE_ONE_FRAME, "One SEI per frame(if changed)",
          "one-frame"},
      {0, NULL, NULL}
    };
    sei_mode = g_enum_register_static ("GstMppEncSeiMode", modes);
  }
  return sei_mode;
}

#define GST_TYPE_MPP_ENC_RC_MODE (gst_mpp_enc_rc_mode_get_type ())
static GType
gst_mpp_enc_rc_mode_get_type (void)
{
  static GType rc_mode = 0;

  if (!rc_mode) {
    static const GEnumValue modes[] = {
      {MPP_ENC_RC_MODE_VBR, "Variable bitrate", "vbr"},
      {MPP_ENC_RC_MODE_CBR, "Constant bitrate", "cbr"},
      {MPP_ENC_RC_MODE_FIXQP, "Fixed QP", "fixqp"},
      {0, NULL, NULL}
    };
    rc_mode = g_enum_register_static ("GstMppEncRcMode", modes);
  }
  return rc_mode;
}

#define GST_TYPE_MPP_ENC_SUPER_MODE (gst_mpp_enc_super_mode_get_type ())
static GType
gst_mpp_enc_super_mode_get_type (void)
{
  static GType super_mode = 0;

  if (!super_mode) {
    static const GEnumValue modes[] = {
      {MPP_ENC_RC_SUPER_FRM_NONE, "Disabled", "none"},
      {MPP_ENC_RC_SUPER_FRM_DROP, "Drop oversized frames", "drop"},
      {MPP_ENC_RC_SUPER_FRM_REENC, "Re-encode oversized frames", "reenc"},
      {0, NULL, NULL}
    };
    super_mode = g_enum_register_static ("GstMppEncSuperMode", modes);
  }
  return super_mode;
}

#define GST_TYPE_MPP_ENC_SCENE_MODE (gst_mpp_enc_scene_mode_get_type ())
static GType
gst_mpp_enc_scene_mode_get_type (void)
{
  static GType scene_mode = 0;

  if (!scene_mode) {
    static const GEnumValue modes[] = {
      {0, "Default", "default"},
      {1, "IP camera", "ipc"},
      {2, "IP camera with pan/tilt/zoom", "ipc-ptz"},
      {0, NULL, NULL}
    };
    scene_mode = g_enum_register_static ("GstMppEncSceneMode", modes);
  }
  return scene_mode;
}

#ifdef HAVE_RGA
#define GST_TYPE_MPP_ENC_ROTATION (gst_mpp_enc_rotation_get_type ())
static GType
gst_mpp_enc_rotation_get_type (void)
{
  static GType rotation = 0;

  if (!rotation) {
    static const GEnumValue rotations[] = {
      {0, "Rotate 0", "0"},
      {90, "Rotate 90", "90"},
      {180, "Rotate 180", "180"},
      {270, "Rotate 270", "270"},
      {0, NULL, NULL}
    };
    rotation = g_enum_register_static ("GstMppEncRotation", rotations);
  }
  return rotation;
}
#endif

static void
gst_mpp_enc_class_init (GstMppEncClass * klass)
{
  GstVideoEncoderClass *encoder_class = GST_VIDEO_ENCODER_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  const gchar *env;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "mppenc", 0, "MPP encoder");

  encoder_class->start = GST_DEBUG_FUNCPTR (gst_mpp_enc_start);
  encoder_class->stop = GST_DEBUG_FUNCPTR (gst_mpp_enc_stop);
  encoder_class->flush = GST_DEBUG_FUNCPTR (gst_mpp_enc_flush);
  encoder_class->finish = GST_DEBUG_FUNCPTR (gst_mpp_enc_finish);
  encoder_class->set_format = GST_DEBUG_FUNCPTR (gst_mpp_enc_set_format);
  encoder_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_mpp_enc_propose_allocation);
  encoder_class->handle_frame = GST_DEBUG_FUNCPTR (gst_mpp_enc_handle_frame);

  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_mpp_enc_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_mpp_enc_get_property);
  gobject_class->finalize = gst_mpp_enc_finalize;

  env = g_getenv ("GST_MPP_ENC_MAX_PENDING");
  if (env)
    DEFAULT_PROP_MAX_PENDING = MAX (MIN (atoi (env), MPP_MAX_PENDING), 1);

  g_object_class_install_property (gobject_class, PROP_MAX_PENDING,
      g_param_spec_uint ("max-pending", "Max pending frames",
          "Max pending frames",
          1, MPP_MAX_PENDING, DEFAULT_PROP_MAX_PENDING,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_HEADER_MODE,
      g_param_spec_enum ("header-mode", "Header mode",
          "Header mode",
          GST_TYPE_MPP_ENC_HEADER_MODE, DEFAULT_PROP_HEADER_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SEI_MODE,
      g_param_spec_enum ("sei-mode", "SEI mode",
          "SEI mode",
          GST_TYPE_MPP_ENC_SEI_MODE, DEFAULT_PROP_SEI_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_RC_MODE,
      g_param_spec_enum ("rc-mode", "RC mode",
          "RC mode",
          GST_TYPE_MPP_ENC_RC_MODE, DEFAULT_PROP_RC_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

#ifdef HAVE_RGA
  if (!gst_mpp_use_rga ())
    goto no_rga;

  g_object_class_install_property (gobject_class, PROP_ROTATION,
      g_param_spec_enum ("rotation", "Rotation",
          "Rotation",
          GST_TYPE_MPP_ENC_ROTATION, DEFAULT_PROP_ROTATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_WIDTH,
      g_param_spec_uint ("width", "Width",
          "Width (0 = original)",
          0, G_MAXINT, DEFAULT_PROP_WIDTH,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_HEIGHT,
      g_param_spec_uint ("height", "Height",
          "Height (0 = original)",
          0, G_MAXINT, DEFAULT_PROP_HEIGHT,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));

no_rga:
#endif

  g_object_class_install_property (gobject_class, PROP_GOP,
      g_param_spec_int ("gop", "Group of pictures",
          "Group of pictures starting with I frame (-1 = FPS, 1 = all I frames)",
          -1, G_MAXINT, DEFAULT_PROP_GOP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAX_REENC,
      g_param_spec_uint ("max-reenc", "Max re-encode times",
          "Max re-encode times for one frame",
          0, 3, DEFAULT_PROP_MAX_REENC,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BPS,
      g_param_spec_uint ("bitrate", "Target BPS",
          "Target BPS (0 = auto calculate)",
          0, G_MAXINT, DEFAULT_PROP_BPS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BPS_MIN,
      g_param_spec_uint ("bitrate-min", "Min BPS",
          "Min BPS (0 = auto calculate)",
          0, G_MAXINT, DEFAULT_PROP_BPS_MIN,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BPS_MAX,
      g_param_spec_uint ("bitrate-max", "Max BPS",
          "Max BPS (0 = auto calculate)",
          0, G_MAXINT, DEFAULT_PROP_BPS_MAX,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FPS_OUT,
      g_param_spec_int ("fps-out", "Output framerate",
          "Output framerate numerator (0 = same as input)",
          0, 256, DEFAULT_PROP_FPS_OUT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DROP_MODE,
      g_param_spec_int ("drop-mode", "Frame drop mode",
          "Frame drop mode (0=disabled, 1=normal skip, 2=pskip empty frame)",
          0, 2, DEFAULT_PROP_DROP_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DROP_THRESHOLD,
      g_param_spec_uint ("drop-threshold", "Drop threshold",
          "Percentage over bps_max that triggers frame drop",
          0, 100, DEFAULT_PROP_DROP_THRESHOLD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_INTRA_REFRESH,
      g_param_spec_uint ("intra-refresh", "Intra refresh",
          "Rolling intra refresh: MB rows refreshed per frame "
          "(0 = disabled, uses periodic IDR instead)",
          0, G_MAXINT, DEFAULT_PROP_INTRA_REFRESH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SUPER_MODE,
      g_param_spec_enum ("super-mode", "Super-frame mode",
          "Bound a single coded frame's size to protect the send buffer",
          GST_TYPE_MPP_ENC_SUPER_MODE, DEFAULT_PROP_SUPER_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SUPER_I_THD,
      g_param_spec_uint ("super-i-thd", "Super-frame I threshold",
          "I-frame size threshold in bytes (0 = auto)",
          0, G_MAXINT, DEFAULT_PROP_SUPER_I_THD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SUPER_P_THD,
      g_param_spec_uint ("super-p-thd", "Super-frame P threshold",
          "P-frame size threshold in bytes (0 = auto)",
          0, G_MAXINT, DEFAULT_PROP_SUPER_P_THD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DEBREATH,
      g_param_spec_boolean ("debreath", "De-breathing",
          "Smooth the GOP bitrate breathing oscillation",
          DEFAULT_PROP_DEBREATH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DEBREATH_STRENGTH,
      g_param_spec_uint ("debreath-strength", "De-breathing strength",
          "De-breathing strength (only used when debreath=true)",
          0, 35, DEFAULT_PROP_DEBREATH_STRENGTH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SCENE_MODE,
      g_param_spec_enum ("scene-mode", "Scene mode",
          "Content-adaptive scene tuning",
          GST_TYPE_MPP_ENC_SCENE_MODE, DEFAULT_PROP_SCENE_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ANTI_FLICKER,
      g_param_spec_uint ("anti-flicker", "Anti-flicker strength",
          "Temporal anti-flicker strength (0 = disabled)",
          0, 3, DEFAULT_PROP_ANTI_FLICKER,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NUM_TEMPORAL_LAYERS,
      g_param_spec_uint ("num-temporal-layers", "Temporal layers",
          "Temporal SVC layers (0/1 = off, 2/3/4 = tsvc2/3/4 hierarchical-P; "
          "higher layers are droppable to reduce framerate under congestion)",
          0, 4, DEFAULT_PROP_NUM_TEMPORAL_LAYERS,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ZERO_COPY_PKT,
      g_param_spec_boolean ("zero-copy-pkt", "Zero-copy encoded packet",
          "Zero-copy encoded packet", DEFAULT_PROP_ZERO_COPY_PKT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  env = g_getenv ("GST_MPP_ENC_DEFAULT_ARM_AFBC");
  if (env && env[0] == '1')
    DEFAULT_PROP_ARM_AFBC = TRUE;

  g_object_class_install_property (gobject_class, PROP_ARM_AFBC,
      g_param_spec_boolean ("arm-afbc", "ARM AFBC",
          "Input is ARM AFBC compressed format", DEFAULT_PROP_ARM_AFBC,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_mpp_enc_change_state);
}
