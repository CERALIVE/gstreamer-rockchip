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

#ifndef  __GST_MPP_ENC_H__
#define  __GST_MPP_ENC_H__

#include <gst/video/gstvideoencoder.h>

#include "gstmpp.h"

G_BEGIN_DECLS;

#define GST_TYPE_MPP_ENC (gst_mpp_enc_get_type())
G_DECLARE_FINAL_TYPE (GstMppEnc, gst_mpp_enc, GST, MPP_ENC, GstVideoEncoder);

#define GST_MPP_ENC(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
    GST_TYPE_MPP_ENC, GstMppEnc))

struct _GstMppEnc
{
  GstVideoEncoder parent;

  GMutex mutex;
  /* Runtime properties use a dedicated mutex rather than mutex above because
   * the frame mutex spans MPP control calls. A separate lock lets the encode
   * path acknowledge one coherent property snapshot, then release it before
   * calling MPP, so property setters cannot deadlock behind the streaming path. */
  GMutex prop_mutex;
  GstAllocator *allocator;
  GstVideoCodecState *input_state;

  /* final input video info */
  GstVideoInfo info;

  /* stop handling new frame when flushing */
  gint flushing;

  /* drop frames when flushing but not draining */
  gboolean draining;

  /* frame system numbers that are ready for sending to MPP */
  GList *frames;

  /* Max number of pending frames */
  guint32 max_pending;

  guint pending_frames;
  GMutex event_mutex;
  GCond event_cond;

  /* flow return from pad task */
  GstFlowReturn task_ret;

  MppEncHeaderMode header_mode;
  MppEncRcMode rc_mode;
  MppEncSeiMode sei_mode;

  gint rotation;
  gint width;
  gint height;

  gint gop;
  guint max_reenc;

  guint bps;
  guint bps_min;
  guint bps_max;

  gint fps_out;           /* output framerate numerator (0 = same as input) */
  gint drop_mode;         /* MppEncRcDropFrmMode: 0=disabled, 1=normal, 2=pskip */
  guint drop_threshold;   /* % over bps_max that triggers drop (default 50) */

  /* Rolling intra refresh: spreads I-macroblocks across frames instead of
   * periodic IDR spikes. 0 = disabled, else number of MB rows refreshed per
   * frame. Tuned for lossy low-latency streaming. */
  guint intra_refresh;

  /* Super-frame handling: bound the size of a single coded frame so a scene
   * cut / keyframe cannot spike the send buffer. */
  gint super_mode;        /* MppEncRcSuperFrameMode: 0=none, 1=drop, 2=reenc */
  guint super_i_thd;      /* I-frame size threshold in bytes (0 = auto) */
  guint super_p_thd;      /* P-frame size threshold in bytes (0 = auto) */

  /* De-breathing: smooths the GOP bitrate "breathing" oscillation. */
  gboolean debreath;
  guint debreath_strength; /* [0, 35] */

  /* Content-adaptive tuning. */
  gint scene_mode;        /* 0=default, 1=ipc, 2=ipc-ptz */
  guint anti_flicker;     /* temporal anti-flicker strength [0, 3], 0 = off */

  gboolean zero_copy_pkt;

  gboolean arm_afbc;

  gboolean prop_dirty;

  /* Set when width/height changes while the encoder is already running, so the
   * next frame reconfigures the scaler in place instead of rejecting the change. */
  gboolean res_dirty;

  /* Temporal SVC. 0/1 = off (flat IPPP), 2/3/4 = tsvc2/tsvc3/tsvc4
   * hierarchical-P. Higher temporal layers are non-reference, so a downstream
   * scheduler can drop them to shed framerate under congestion without breaking
   * decode. ref_cfg owns the MPP reference-structure handle (NULL while off);
   * ref_dirty requests a rebuild on the next apply. */
  guint num_temporal_layers;
  MppEncRefCfg ref_cfg;
  gboolean ref_dirty;

  MppEncCfg mpp_cfg;

  /* Sticky for the life of mpp_cfg: set when MPP refuses a config key, which
   * means the plugin's idea of the MPP config surface disagrees with the MPP it
   * is running against. The key set is compile-time constant, so a refusal will
   * repeat on every apply; latching it keeps a rejection from being lost in the
   * window between the write and the MPP_ENC_SET_CFG that would report it. */
  gboolean cfg_error;

  MppFrame mpp_frame;

  MppCodingType mpp_type;
  MppCtx mpp_ctx;
  MppApi *mpi;
};

#define GST_MPP_ENC_PROP_LOCK(encoder) \
  g_mutex_lock (&GST_MPP_ENC (encoder)->prop_mutex)
#define GST_MPP_ENC_PROP_UNLOCK(encoder) \
  g_mutex_unlock (&GST_MPP_ENC (encoder)->prop_mutex)

typedef void (*GstMppEncSnapshotPropertiesFunc) (GstVideoEncoder * encoder,
    gpointer snapshot);
typedef void (*GstMppEncConfigurePropertiesFunc) (GstVideoEncoder * encoder,
    gconstpointer snapshot);

#define MPP_ENC_IN_FORMATS \
    "NV12, I420, YUY2, UYVY, " \
    "BGR16, RGB16, BGR, RGB, " \
    "ABGR, ARGB, BGRA, RGBA, xBGR, xRGB, BGRx, RGBx"

#ifdef HAVE_RGA
#define MPP_ENC_FORMATS MPP_ENC_IN_FORMATS "," GST_RGA_FORMATS
#else
#define MPP_ENC_FORMATS MPP_ENC_IN_FORMATS
#endif

/* Checked wrappers around mpp_enc_cfg_set_*. MPP answers MPP_NOK for a key it
 * does not know and writes nothing, so an unchecked setter turns a misspelt or
 * version-drifted key into a tunable that silently never takes effect. Every
 * config write in this plugin goes through these. */
gboolean gst_mpp_enc_cfg_set_s32 (GstMppEnc * self, const gchar * key,
    gint value);
gboolean gst_mpp_enc_cfg_set_u32 (GstMppEnc * self, const gchar * key,
    guint value);

gboolean gst_mpp_enc_apply_properties (GstVideoEncoder * encoder);
gboolean gst_mpp_enc_apply_properties_full (GstVideoEncoder * encoder,
    GstMppEncSnapshotPropertiesFunc snapshot_codec_properties,
    GstMppEncConfigurePropertiesFunc configure_codec_properties,
    gpointer codec_snapshot, gboolean * applied);
gboolean gst_mpp_enc_set_src_caps (GstVideoEncoder * encoder, GstCaps * caps);

gboolean gst_mpp_enc_supported (MppCodingType mpp_type);

G_END_DECLS;

#endif /* __GST_MPP_ENC_H__ */
