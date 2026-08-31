/*
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

#include "gstmpph265enc.h"

/* FIXME: Not all chips support NV24 and Y444. */
#define MPP_H265_ENC_FORMATS MPP_ENC_FORMATS ", NV24, Y444"

#define GST_MPP_H265_ENC(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
    GST_TYPE_MPP_H265_ENC, GstMppH265Enc))

#define GST_CAT_DEFAULT mpp_h265_enc_debug
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

struct _GstMppH265Enc
{
  GstMppEnc parent;

  guint qp_init;
  guint qp_min;
  guint qp_max;
  guint qp_min_i;
  guint qp_max_i;
  gint qp_ip;

  gint profile;            /* MPP_PROFILE_HEVC_*: 1=main, 2=main10, 3=main-still */
  gint tier;               /* 0=main tier, 1=high tier */
  gint level;              /* general_level_idc (level * 30, e.g. 120 = 4.0) */
  gboolean sao;            /* Sample Adaptive Offset filter */
};

typedef struct
{
  guint qp_init;
  guint qp_min;
  guint qp_max;
  guint qp_min_i;
  guint qp_max_i;
  gint qp_ip;
  gint profile;
  gint tier;
  gint level;
  gboolean sao;
  MppEncRcMode rc_mode;
} GstMppH265EncPropertiesSnapshot;

#define parent_class gst_mpp_h265_enc_parent_class
G_DEFINE_TYPE (GstMppH265Enc, gst_mpp_h265_enc, GST_TYPE_MPP_ENC);

#define DEFAULT_PROP_QP_INIT 26
#define DEFAULT_PROP_QP_MIN 0   /* Auto */
#define DEFAULT_PROP_QP_MAX 0   /* Auto */
#define DEFAULT_PROP_QP_MIN_I 0 /* Auto */
#define DEFAULT_PROP_QP_MAX_I 0 /* Auto */
#define DEFAULT_PROP_QP_IP -1   /* Auto */
#define DEFAULT_PROP_PROFILE 1  /* MPP_PROFILE_HEVC_MAIN */
#define DEFAULT_PROP_TIER 0     /* Main tier */
#define DEFAULT_PROP_LEVEL 120  /* Level 4.0 */
#define DEFAULT_PROP_SAO TRUE

enum
{
  PROP_0,
  PROP_QP_INIT,
  PROP_QP_MIN,
  PROP_QP_MAX,
  PROP_QP_MIN_I,
  PROP_QP_MAX_I,
  PROP_QP_IP,
  PROP_PROFILE,
  PROP_TIER,
  PROP_LEVEL,
  PROP_SAO,
  PROP_LAST,
};

#define GST_TYPE_MPP_H265_ENC_PROFILE (gst_mpp_h265_enc_profile_get_type ())
static GType
gst_mpp_h265_enc_profile_get_type (void)
{
  static GType profile = 0;

  if (!profile) {
    static const GEnumValue profiles[] = {
      {1, "Main", "main"},
      {2, "Main 10", "main10"},
      {3, "Main Still Picture", "main-still"},
      {0, NULL, NULL}
    };
    profile = g_enum_register_static ("GstMppH265Profile", profiles);
  }
  return profile;
}

#define GST_TYPE_MPP_H265_ENC_TIER (gst_mpp_h265_enc_tier_get_type ())
static GType
gst_mpp_h265_enc_tier_get_type (void)
{
  static GType tier = 0;

  if (!tier) {
    static const GEnumValue tiers[] = {
      {0, "Main tier", "main"},
      {1, "High tier", "high"},
      {0, NULL, NULL}
    };
    tier = g_enum_register_static ("GstMppH265Tier", tiers);
  }
  return tier;
}

#define GST_TYPE_MPP_H265_ENC_LEVEL (gst_mpp_h265_enc_level_get_type ())
static GType
gst_mpp_h265_enc_level_get_type (void)
{
  static GType level = 0;

  if (!level) {
    /* value is general_level_idc (level number * 30) */
    static const GEnumValue levels[] = {
      {90, "Level 3", "3"},
      {93, "Level 3.1", "3.1"},
      {120, "Level 4", "4"},
      {123, "Level 4.1", "4.1"},
      {150, "Level 5", "5"},
      {153, "Level 5.1", "5.1"},
      {156, "Level 5.2", "5.2"},
      {180, "Level 6", "6"},
      {183, "Level 6.1", "6.1"},
      {186, "Level 6.2", "6.2"},
      {0, NULL, NULL}
    };
    level = g_enum_register_static ("GstMppH265Level", levels);
  }
  return level;
}

#define GST_MPP_H265_ENC_SIZE_CAPS \
    "width  = (int) [ 96, MAX ], height = (int) [ 64, MAX ]"

static GstStaticPadTemplate gst_mpp_h265_enc_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265, "
        GST_MPP_H265_ENC_SIZE_CAPS ","
        "stream-format = (string) { byte-stream }, "
        "alignment = (string) { au }")
    );

static GstStaticPadTemplate gst_mpp_h265_enc_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw,"
        "format = (string) { " MPP_H265_ENC_FORMATS " }, "
        GST_MPP_H265_ENC_SIZE_CAPS ";"));

/*
 * H.265 level limits, Tables A.6/A.8/A.9 of ITU-T H.265, keyed by
 * general_level_idc. Identical values to MPP's own `levels[]`
 * (mpp/codec/enc/h265/h265e_ps.c at the pinned 1.5.0-1), which is deliberate:
 * MPP consults only max_luma_ps there, so this table extends the same numbers
 * to the two axes it leaves unchecked while agreeing with it exactly on the one
 * it does check. Level 8.5 is omitted -- it is the unbounded still-picture
 * level, not a streaming target, and the level property does not offer it.
 *
 * max_br_* is in units of CpbBrVclFactor bits/s, 1000 for every profile this
 * element offers (Main, Main 10, Main Still Picture -- Table A.2), not bits/s.
 * G_MAXUINT in max_br_high marks a level at which the high tier is not defined.
 */
typedef struct
{
  gint level;
  const gchar *name;
  guint max_luma_ps;
  guint64 max_luma_sr;
  guint max_br_main;
  guint max_br_high;
} GstMppH265LevelLimits;

static const GstMppH265LevelLimits gst_mpp_h265_enc_level_limits[] = {
  {30, "1", 36864, 552960, 128, G_MAXUINT},
  {60, "2", 122880, 3686400, 1500, G_MAXUINT},
  {63, "2.1", 245760, 7372800, 3000, G_MAXUINT},
  {90, "3", 552960, 16588800, 6000, G_MAXUINT},
  {93, "3.1", 983040, 33177600, 10000, G_MAXUINT},
  {120, "4", 2228224, 66846720, 12000, 30000},
  {123, "4.1", 2228224, 133693440, 20000, 50000},
  {150, "5", 8912896, 267386880, 25000, 100000},
  {153, "5.1", 8912896, 534773760, 40000, 160000},
  {156, "5.2", 8912896, 1069547520, 60000, 240000},
  {180, "6", 35651584, 1069547520, 60000, 240000},
  {183, "6.1", 35651584, 2139095040, 120000, 480000},
  {186, "6.2", 35651584, 4278190080ULL, 240000, 800000},
};

#define GST_MPP_H265_CPB_BR_VCL_FACTOR 1000

static guint
gst_mpp_h265_enc_level_index (gint level)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (gst_mpp_h265_enc_level_limits); i++) {
    if (gst_mpp_h265_enc_level_limits[i].level == level)
      return i;
  }
  return 0;
}

static guint
gst_mpp_h265_enc_required_level_index (gint tier,
    const GstMppEncRateInfo * rate, gboolean * conforming)
{
  guint64 luma_ps;
  guint64 luma_ticks;
  guint i;

  luma_ps = (guint64) rate->width * rate->height;

  /* Cross-multiplied for the same reason as the H.264 macroblock rate: the
   * exact sample rate is a rational, and rounding it hides violations narrower
   * than one frame per second. */
  luma_ticks = luma_ps * (guint64) rate->fps_n;

  *conforming = TRUE;

  for (i = 0; i < G_N_ELEMENTS (gst_mpp_h265_enc_level_limits); i++) {
    const GstMppH265LevelLimits *limits = &gst_mpp_h265_enc_level_limits[i];
    guint max_br = tier ? limits->max_br_high : limits->max_br_main;

    if (tier && max_br == G_MAXUINT)
      continue;
    if (luma_ps > (guint64) limits->max_luma_ps)
      continue;
    if (luma_ticks > limits->max_luma_sr * (guint64) rate->fps_d)
      continue;
    if (rate->bitrate && (guint64) rate->bitrate >
        (guint64) max_br * GST_MPP_H265_CPB_BR_VCL_FACTOR)
      continue;
    return i;
  }

  *conforming = FALSE;
  return G_N_ELEMENTS (gst_mpp_h265_enc_level_limits) - 1;
}

/*
 * The level the bitstream actually conforms to, which is what the VPS/SPS and
 * the src caps must both carry.
 *
 * MPP raises an under-declared level for picture size alone (h265e_ps.c only
 * tests maxLumaSamples), so a 1080p60 stream declared at level 4 keeps emitting
 * a VPS claiming level 4 while exceeding its luma sample rate by 2x.
 *
 * Raising rather than rejecting is deliberate: `level` defaults to 4 and
 * cerastream never sets it, so every shipped 1080p50/60, 1440p and 2160p
 * profile would fail negotiation under a hard rejection.
 */
static gint
gst_mpp_h265_enc_effective_level (GstVideoEncoder * encoder, gint tier,
    gint declared, const GstMppEncRateInfo * rate)
{
  gboolean conforming;
  guint declared_index;
  guint required_index;
  gint required;

  if (rate->width <= 0 || rate->height <= 0 || rate->fps_n <= 0
      || rate->fps_d <= 0)
    return declared;

  declared_index = gst_mpp_h265_enc_level_index (declared);
  required_index =
      gst_mpp_h265_enc_required_level_index (tier, rate, &conforming);
  required = gst_mpp_h265_enc_level_limits[required_index].level;

  if (!conforming) {
    GST_WARNING_OBJECT (encoder, "%dx%d@%d/%d fps at %u bps exceeds every H.265 "
        "level; encoding at the highest (%d), which the stream may still "
        "exceed", rate->width, rate->height, rate->fps_n, rate->fps_d,
        rate->bitrate, required);
  } else if (required_index <= declared_index) {
    return declared;
  } else {
    GST_WARNING_OBJECT (encoder, "declared H.265 level_idc %d cannot carry "
        "%dx%d@%d/%d fps at %u bps; raising to level_idc %d so the bitstream "
        "and its VPS agree", declared, rate->width, rate->height, rate->fps_n,
        rate->fps_d, rate->bitrate, required);
  }

  return required;
}

static void
gst_mpp_h265_enc_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (object);
  GstMppH265Enc *self = GST_MPP_H265_ENC (encoder);
  GstMppEnc *mppenc = GST_MPP_ENC (encoder);
  gboolean invalid = FALSE;

  GST_MPP_ENC_PROP_LOCK (encoder);
  switch (prop_id) {
    case PROP_QP_INIT:{
      guint qp_init = g_value_get_uint (value);
      if (self->qp_init == qp_init)
        goto out;

      self->qp_init = qp_init;
      break;
    }
    case PROP_QP_MIN:{
      guint qp_min = g_value_get_uint (value);
      if (self->qp_min == qp_min)
        goto out;

      self->qp_min = qp_min;
      break;
    }
    case PROP_QP_MAX:{
      guint qp_max = g_value_get_uint (value);
      if (self->qp_max == qp_max)
        goto out;

      self->qp_max = qp_max;
      break;
    }
    case PROP_QP_MIN_I:{
      guint qp_min_i = g_value_get_uint (value);
      if (self->qp_min_i == qp_min_i)
        goto out;

      self->qp_min_i = qp_min_i;
      break;
    }
    case PROP_QP_MAX_I:{
      guint qp_max_i = g_value_get_uint (value);
      if (self->qp_max_i == qp_max_i)
        goto out;

      self->qp_max_i = qp_max_i;
      break;
    }
    case PROP_QP_IP:{
      gint qp_ip = g_value_get_int (value);
      if (self->qp_ip == qp_ip)
        goto out;

      self->qp_ip = qp_ip;
      break;
    }
    case PROP_PROFILE:{
      gint profile = g_value_get_enum (value);
      if (self->profile == profile)
        goto out;

      self->profile = profile;
      break;
    }
    case PROP_TIER:{
      gint tier = g_value_get_enum (value);
      if (self->tier == tier)
        goto out;

      self->tier = tier;
      break;
    }
    case PROP_LEVEL:{
      gint level = g_value_get_enum (value);
      if (self->level == level)
        goto out;

      self->level = level;
      break;
    }
    case PROP_SAO:{
      gboolean sao = g_value_get_boolean (value);
      if (self->sao == sao)
        goto out;

      self->sao = sao;
      break;
    }
    default:
      invalid = TRUE;
      goto out;
  }

  mppenc->prop_dirty = TRUE;

out:
  GST_MPP_ENC_PROP_UNLOCK (encoder);
  if (invalid)
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
gst_mpp_h265_enc_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (object);
  GstMppH265Enc *self = GST_MPP_H265_ENC (encoder);
  gboolean invalid = FALSE;

  GST_MPP_ENC_PROP_LOCK (encoder);
  switch (prop_id) {
    case PROP_QP_INIT:
      g_value_set_uint (value, self->qp_init);
      break;
    case PROP_QP_MIN:
      g_value_set_uint (value, self->qp_min);
      break;
    case PROP_QP_MAX:
      g_value_set_uint (value, self->qp_max);
      break;
    case PROP_QP_MIN_I:
      g_value_set_uint (value, self->qp_min_i);
      break;
    case PROP_QP_MAX_I:
      g_value_set_uint (value, self->qp_max_i);
      break;
    case PROP_QP_IP:
      g_value_set_int (value, self->qp_ip);
      break;
    case PROP_PROFILE:
      g_value_set_enum (value, self->profile);
      break;
    case PROP_TIER:
      g_value_set_enum (value, self->tier);
      break;
    case PROP_LEVEL:
      g_value_set_enum (value, self->level);
      break;
    case PROP_SAO:
      g_value_set_boolean (value, self->sao);
      break;
    default:
      invalid = TRUE;
      break;
  }
  GST_MPP_ENC_PROP_UNLOCK (encoder);

  if (invalid)
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

/*
 * The GStreamer spellings, which are not the property nicks: `main10` is
 * `main-10` and `main-still` is `main-still-picture` in video/x-h265 caps, and
 * the level field carries the level number rather than general_level_idc.
 * Publishing the nick or the idc would put a value downstream cannot parse into
 * a field h265parse and the muxers read.
 */
static const gchar *
gst_mpp_h265_enc_profile_string (gint profile)
{
  switch (profile) {
    case 2:
      return "main-10";
    case 3:
      return "main-still-picture";
    default:
      return "main";
  }
}

static const gchar *
gst_mpp_h265_enc_level_string (gint level)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (gst_mpp_h265_enc_level_limits); i++) {
    if (gst_mpp_h265_enc_level_limits[i].level == level)
      return gst_mpp_h265_enc_level_limits[i].name;
  }
  return NULL;
}

static gboolean
gst_mpp_h265_enc_set_src_caps (GstVideoEncoder * encoder, gint profile,
    gint tier, gint level)
{
  GstStructure *structure;
  GstCaps *caps;
  const gchar *level_string;

  caps = gst_caps_new_empty_simple ("video/x-h265");

  structure = gst_caps_get_structure (caps, 0);
  gst_structure_set (structure, "stream-format",
      G_TYPE_STRING, "byte-stream", NULL);
  gst_structure_set (structure, "alignment", G_TYPE_STRING, "au", NULL);

  gst_structure_set (structure, "profile", G_TYPE_STRING,
      gst_mpp_h265_enc_profile_string (profile), NULL);
  gst_structure_set (structure, "tier", G_TYPE_STRING,
      tier ? "high" : "main", NULL);

  /* Tier and level are one constraint, so an unnameable level leaves both out
   * rather than publishing a tier for a level nobody can read. */
  level_string = gst_mpp_h265_enc_level_string (level);
  if (level_string)
    gst_structure_set (structure, "level", G_TYPE_STRING, level_string, NULL);
  else
    gst_structure_remove_field (structure, "tier");

  return gst_mpp_enc_set_src_caps (encoder, caps);
}

static void
gst_mpp_h265_enc_snapshot_properties (GstVideoEncoder * encoder,
    gpointer snapshot)
{
  GstMppH265Enc *self = GST_MPP_H265_ENC (encoder);
  GstMppEnc *mppenc = GST_MPP_ENC (encoder);
  GstMppH265EncPropertiesSnapshot *properties = snapshot;
  GstMppEncRateInfo rate;

  gst_mpp_enc_snapshot_rate_info (encoder, &rate);

  properties->qp_init = self->qp_init;
  properties->qp_min = self->qp_min;
  properties->qp_max = self->qp_max;
  properties->qp_min_i = self->qp_min_i;
  properties->qp_max_i = self->qp_max_i;
  properties->qp_ip = self->qp_ip;
  properties->profile = self->profile;
  properties->tier = self->tier;
  /* Declared level stays on the property; the effective one is what MPP is
   * configured with and what the caps publish. */
  properties->level = gst_mpp_h265_enc_effective_level (encoder, self->tier,
      self->level, &rate);
  properties->sao = self->sao;
  properties->rc_mode = mppenc->rc_mode;
}

static void
gst_mpp_h265_enc_configure_properties (GstVideoEncoder * encoder,
    gconstpointer snapshot)
{
  GstMppEnc *mppenc = GST_MPP_ENC (encoder);
  const GstMppH265EncPropertiesSnapshot *properties = snapshot;

  gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_init", properties->qp_init);

  if (properties->rc_mode == MPP_ENC_RC_MODE_FIXQP) {
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_min", properties->qp_init);
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_max", properties->qp_init);
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_min_i", properties->qp_init);
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_max_i", properties->qp_init);
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_ip", 0);
  } else {
    /* MPP_ENC_RC_MODE_CBR/MPP_ENC_RC_MODE_VBR/MPP_ENC_RC_MODE_AVBR */
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_min",
        properties->qp_min ? properties->qp_min : 10);
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_max",
        properties->qp_max ? properties->qp_max : 51);
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_min_i",
        properties->qp_min_i ? properties->qp_min_i : 10);
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_max_i",
        properties->qp_max_i ? properties->qp_max_i : 51);
    gst_mpp_enc_cfg_set_s32 (mppenc, "rc:qp_ip",
        properties->qp_ip >= 0 ? properties->qp_ip : 2);
  }

  gst_mpp_enc_cfg_set_s32 (mppenc, "h265:profile", properties->profile);
  gst_mpp_enc_cfg_set_s32 (mppenc, "h265:tier", properties->tier);
  gst_mpp_enc_cfg_set_s32 (mppenc, "h265:level", properties->level);
  gst_mpp_enc_cfg_set_s32 (mppenc, "h265:sao_luma_disable",
      properties->sao ? 0 : 1);
  gst_mpp_enc_cfg_set_s32 (mppenc, "h265:sao_chroma_disable",
      properties->sao ? 0 : 1);
}

static gboolean
gst_mpp_h265_enc_apply_properties (GstVideoEncoder * encoder)
{
  GstMppH265EncPropertiesSnapshot properties;
  gboolean applied;

  if (!gst_mpp_enc_apply_properties_full (encoder,
          gst_mpp_h265_enc_snapshot_properties,
          gst_mpp_h265_enc_configure_properties, &properties, &applied))
    return FALSE;
  if (!applied)
    return TRUE;

  return gst_mpp_h265_enc_set_src_caps (encoder, properties.profile,
      properties.tier, properties.level);
}

static gboolean
gst_mpp_h265_enc_set_format (GstVideoEncoder * encoder,
    GstVideoCodecState * state)
{
  GstVideoEncoderClass *pclass = GST_VIDEO_ENCODER_CLASS (parent_class);

  if (!pclass->set_format (encoder, state))
    return FALSE;

  return gst_mpp_h265_enc_apply_properties (encoder);
}

static GstFlowReturn
gst_mpp_h265_enc_handle_frame (GstVideoEncoder * encoder,
    GstVideoCodecFrame * frame)
{
  GstVideoEncoderClass *pclass = GST_VIDEO_ENCODER_CLASS (parent_class);

  if (G_UNLIKELY (!gst_mpp_h265_enc_apply_properties (encoder))) {
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_NOT_NEGOTIATED;
  }

  return pclass->handle_frame (encoder, frame);
}

static void
gst_mpp_h265_enc_init (GstMppH265Enc * self)
{
  self->parent.mpp_type = MPP_VIDEO_CodingHEVC;

  self->qp_init = DEFAULT_PROP_QP_INIT;
  self->qp_min = DEFAULT_PROP_QP_MIN;
  self->qp_max = DEFAULT_PROP_QP_MAX;
  self->qp_min_i = DEFAULT_PROP_QP_MIN_I;
  self->qp_max_i = DEFAULT_PROP_QP_MAX_I;
  self->qp_ip = DEFAULT_PROP_QP_IP;
  self->profile = DEFAULT_PROP_PROFILE;
  self->tier = DEFAULT_PROP_TIER;
  self->level = DEFAULT_PROP_LEVEL;
  self->sao = DEFAULT_PROP_SAO;
}

static void
gst_mpp_h265_enc_class_init (GstMppH265EncClass * klass)
{
  GstVideoEncoderClass *encoder_class = GST_VIDEO_ENCODER_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "mpph265enc", 0,
      "MPP H265 encoder");

  encoder_class->set_format = GST_DEBUG_FUNCPTR (gst_mpp_h265_enc_set_format);
  encoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_mpp_h265_enc_handle_frame);

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_mpp_h265_enc_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_mpp_h265_enc_get_property);

  g_object_class_install_property (gobject_class, PROP_QP_INIT,
      g_param_spec_uint ("qp-init", "Initial QP",
          "Initial QP (lower value means higher quality)",
          0, 51, DEFAULT_PROP_QP_INIT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_QP_MIN,
      g_param_spec_uint ("qp-min", "Min QP",
          "Min QP (0 = default)", 0, 51, DEFAULT_PROP_QP_MIN,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_QP_MAX,
      g_param_spec_uint ("qp-max", "Max QP",
          "Max QP (0 = default)", 0, 51, DEFAULT_PROP_QP_MAX,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_QP_MIN_I,
      g_param_spec_uint ("qp-min-i", "Min Intra QP",
          "Min Intra QP (0 = default)", 0, 51, DEFAULT_PROP_QP_MIN_I,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_QP_MAX_I,
      g_param_spec_uint ("qp-max-i", "Max Intra QP",
          "Max Intra QP (0 = default)", 0, 51, DEFAULT_PROP_QP_MAX_I,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_QP_IP,
      g_param_spec_int ("qp-delta-ip", "Delta QP between I and P",
          "Delta QP between I and P (-1 = default)", -1, 8, DEFAULT_PROP_QP_IP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PROFILE,
      g_param_spec_enum ("profile", "H265 profile",
          "H265 profile",
          GST_TYPE_MPP_H265_ENC_PROFILE, DEFAULT_PROP_PROFILE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_TIER,
      g_param_spec_enum ("tier", "H265 tier",
          "H265 tier",
          GST_TYPE_MPP_H265_ENC_TIER, DEFAULT_PROP_TIER,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_LEVEL,
      g_param_spec_enum ("level", "H265 level",
          "H265 level (4~4.1 = 1080p@30fps, 4.1 = 1080p@60fps, 5~5.2 = 4K)",
          GST_TYPE_MPP_H265_ENC_LEVEL, DEFAULT_PROP_LEVEL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SAO,
      g_param_spec_boolean ("sao", "Sample Adaptive Offset",
          "Enable the SAO in-loop filter", DEFAULT_PROP_SAO,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_h265_enc_src_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_h265_enc_sink_template));

  gst_element_class_set_static_metadata (element_class,
      "Rockchip Mpp H265 Encoder", "Codec/Encoder/Video",
      "Encode video streams via Rockchip Mpp",
      "Jeffy Chen <jeffy.chen@rock-chips.com>");
}

gboolean
gst_mpp_h265_enc_register (GstPlugin * plugin, guint rank)
{
  if (!gst_mpp_enc_supported (MPP_VIDEO_CodingHEVC))
    return FALSE;

  return gst_element_register (plugin, "mpph265enc", rank,
      gst_mpp_h265_enc_get_type ());
}
