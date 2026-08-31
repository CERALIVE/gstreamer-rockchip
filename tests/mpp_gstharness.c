#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <gst/video/video.h>
#include <limits.h>
#include <rockchip/rk_mpi_cmd.h>
#include <stdatomic.h>
extern int mpp_mock_last_cfg_s32(const char *name);
extern int64_t mpp_mock_enc_cfg_record_value(unsigned index, const char *name);
extern unsigned mpp_mock_enc_cfg_record_count(void);
extern unsigned mpp_mock_enc_cfg_record_dropped(void);
extern int mpp_mock_enc_cfg_record_s32(unsigned index, const char *name);
extern void mpp_mock_enc_cfg_reject_key(const char *key);
extern void mpp_mock_enc_pause_next_bps_target(void);
extern unsigned mpp_mock_enc_bps_target_paused(void);
extern void mpp_mock_enc_resume_bps_target(void);
extern void mpp_mock_enc_pause_next_set_cfg(void);
extern unsigned mpp_mock_enc_set_cfg_paused(void);
extern void mpp_mock_enc_resume_set_cfg(void);
extern unsigned mpp_mock_control_count(int cmd);
extern unsigned mpp_mock_frame_set_buffer_count(void);
extern void mpp_mock_enc_arm_reset_drain(void);
extern void mpp_mock_enc_release_packets(unsigned count);
extern void mpp_mock_enc_set_packet_length(unsigned ordinal, size_t length);
extern void mpp_mock_enc_set_packet_intra(unsigned ordinal, int intra);
extern unsigned mpp_mock_enc_queued_packets(void);
extern unsigned mpp_mock_enc_dequeued_packets(void);
extern unsigned mpp_mock_enc_queue_depth(void);
extern unsigned mpp_mock_enc_duplicate_dequeues(void);
extern unsigned mpp_mock_enc_reset_generation(void);
extern unsigned mpp_mock_enc_empty_polls_for_generation(unsigned generation);
extern unsigned mpp_mock_enc_packet_deinits(void);
extern unsigned mpp_mock_enc_packet_double_deinits(void);
extern unsigned mpp_mock_enc_live_packets(void);
extern unsigned mpp_mock_enc_live_buffers(void);
extern void mpp_mock_reset(void);

static atomic_uint reset_output_count;
static guint8 reset_output_ids[8];
static GstClockTime reset_output_pts[8];
static atomic_int fail_first_reset_output;

typedef struct {
  GstElement *element;
  guint bitrate;
  atomic_int failed;
} PausedPropertySet;

typedef struct {
  GstElement *element;
  guint final_bitrate;
  atomic_int failed;
} PropertyHammer;

/* gst_mpp_enc_handle_frame() only queues the frame and broadcasts; the
 * mpp_frame_set_buffer() call happens later on the encoder's srcpad task
 * thread. Sampling the counter straight after gst_harness_push() therefore
 * races the encode task, so wait for it instead. */
static gboolean wait_for_encoder_frame(void) {
  gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (!mpp_mock_frame_set_buffer_count()) {
    if (g_get_monotonic_time() > deadline)
      return FALSE;
    g_usleep(1000);
  }
  return TRUE;
}

static gboolean wait_for_uint(unsigned (*read_value)(void), unsigned want) {
  gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (read_value() < want) {
    if (g_get_monotonic_time() > deadline)
      return FALSE;
    g_usleep(1000);
  }
  return TRUE;
}

static gpointer set_bitrate_at_bps_pause(gpointer data) {
  PausedPropertySet *set = data;
  if (!wait_for_uint(mpp_mock_enc_bps_target_paused, 1)) {
    atomic_store(&set->failed, 1);
    mpp_mock_enc_resume_bps_target();
    return NULL;
  }

  g_object_set(set->element, "bitrate", set->bitrate, NULL);
  mpp_mock_enc_resume_bps_target();
  return NULL;
}

static gpointer hammer_bitrate_until_set_cfg_pause(gpointer data) {
  PropertyHammer *hammer = data;

  for (guint i = 0; i < 4096; i++) {
    guint bitrate = 800000 + (i % 128) * 16000;
    guint observed = 0;
    g_object_set(hammer->element, "bitrate", bitrate, NULL);
    g_object_get(hammer->element, "bitrate", &observed, NULL);
    if (observed != bitrate) {
      atomic_store(&hammer->failed, 1);
      break;
    }
    if (!(i % 64))
      g_thread_yield();
  }

  if (!wait_for_uint(mpp_mock_enc_set_cfg_paused, 1)) {
    atomic_store(&hammer->failed, 1);
    mpp_mock_enc_resume_set_cfg();
    return NULL;
  }

  g_object_set(hammer->element, "bitrate", hammer->final_bitrate, NULL);
  mpp_mock_enc_resume_set_cfg();
  return NULL;
}

static void push_runtime_property_frame(GstHarness *h, guint index) {
  GstBuffer *frame = gst_buffer_new_allocate(NULL, 115200, NULL);
  fail_unless(frame != NULL);
  GST_BUFFER_PTS(frame) = index * (GST_SECOND / 30);
  GST_BUFFER_DURATION(frame) = GST_SECOND / 30;
  fail_unless_equals_int(gst_harness_push(h, frame), GST_FLOW_OK);
}

static gboolean recorded_bitrate(guint bitrate) {
  unsigned count = mpp_mock_enc_cfg_record_count();
  for (unsigned i = 0; i < count; i++)
    if (mpp_mock_enc_cfg_record_s32(i, "rc:bps_target") == (int)bitrate)
      return TRUE;
  return FALSE;
}

static void assert_coherent_bitrate_records(unsigned first) {
  unsigned count = mpp_mock_enc_cfg_record_count();
  fail_unless(count > first, "no encoder config was recorded");
  fail_unless_equals_int(mpp_mock_enc_cfg_record_dropped(), 0);

  for (unsigned i = first; i < count; i++) {
    int target = mpp_mock_enc_cfg_record_s32(i, "rc:bps_target");
    int minimum = mpp_mock_enc_cfg_record_s32(i, "rc:bps_min");
    int maximum = mpp_mock_enc_cfg_record_s32(i, "rc:bps_max");
    fail_unless(target != INT32_MIN && minimum != INT32_MIN &&
                    maximum != INT32_MIN,
                "config %u omitted a bitrate field", i);
    fail_unless_equals_int(minimum, (int)((gint64)target * 15 / 16));
    fail_unless_equals_int(maximum, (int)((gint64)target * 17 / 16));
  }
}

static unsigned read_reset_output_count(void) {
  return atomic_load(&reset_output_count);
}

static GstFlowReturn capture_reset_output(GstPad *pad, GstObject *parent,
                                          GstBuffer *buffer) {
  (void)pad;
  (void)parent;
  unsigned index = atomic_load(&reset_output_count);
  guint8 id = 0;
  fail_unless(index < G_N_ELEMENTS(reset_output_ids));
  fail_unless_equals_int(gst_buffer_extract(buffer, 0, &id, 1), 1);
  reset_output_ids[index] = id;
  reset_output_pts[index] = GST_BUFFER_PTS(buffer);
  atomic_store(&reset_output_count, index + 1);
  gst_buffer_unref(buffer);
  if (atomic_exchange(&fail_first_reset_output, 0))
    return GST_FLOW_ERROR;
  return GST_FLOW_OK;
}

static void push_reset_frame(GstHarness *h, guint pts_index) {
  GstBuffer *frame = gst_buffer_new_allocate(NULL, 115200, NULL);
  fail_unless(frame != NULL);
  GST_BUFFER_PTS(frame) = pts_index * (GST_SECOND / 30);
  GST_BUFFER_DURATION(frame) = GST_SECOND / 30;
  fail_unless_equals_int(gst_harness_push(h, frame), GST_FLOW_OK);
}

/*
 * Everything below is sampled where the encoder actually delivers -- inside the
 * peer chain function, and from the QoS message GstVideoEncoder posts while it
 * drops a frame. Reading element state after the fact would let a mutant that
 * merely reorders the delivery path keep passing.
 */
#define OUTPUT_CAPTURE_CAPACITY 8
typedef struct {
  gsize size;
  GstClockTime pts;
  gboolean delta_unit;
  guint8 payload;
} CapturedOutput;
static atomic_uint output_capture_count;
static CapturedOutput output_capture[OUTPUT_CAPTURE_CAPACITY];

static unsigned read_output_capture_count(void) {
  return atomic_load(&output_capture_count);
}

static GstFlowReturn capture_output_buffer(GstPad *pad, GstObject *parent,
                                           GstBuffer *buffer) {
  (void)pad;
  (void)parent;
  unsigned index = atomic_load(&output_capture_count);
  fail_unless(index < G_N_ELEMENTS(output_capture));
  CapturedOutput *out = &output_capture[index];
  out->size = gst_buffer_get_size(buffer);
  out->pts = GST_BUFFER_PTS(buffer);
  out->delta_unit = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
  out->payload = 0;
  if (out->size)
    gst_buffer_extract(buffer, 0, &out->payload, 1);
  atomic_store(&output_capture_count, index + 1);
  gst_buffer_unref(buffer);
  return GST_FLOW_OK;
}

static void reset_output_capture(void) {
  atomic_store(&output_capture_count, 0);
  memset(output_capture, 0, sizeof(output_capture));
}

static void read_qos_frame_counts(GstBus *bus, guint64 *processed,
                                  guint64 *dropped) {
  GstMessage *message;
  *processed = 0;
  *dropped = 0;
  while ((message = gst_bus_pop_filtered(bus, GST_MESSAGE_QOS))) {
    GstFormat format = GST_FORMAT_UNDEFINED;
    guint64 seen_processed = 0;
    guint64 seen_dropped = 0;
    gst_message_parse_qos_stats(message, &format, &seen_processed,
                                &seen_dropped);
    if (format == GST_FORMAT_BUFFERS) {
      *processed = MAX(*processed, seen_processed);
      *dropped = MAX(*dropped, seen_dropped);
    }
    gst_message_unref(message);
  }
}

static void assert_no_frames_left_pending(GstElement *element) {
  GList *frames = gst_video_encoder_get_frames(GST_VIDEO_ENCODER(element));
  guint pending = g_list_length(frames);
  g_list_free_full(frames, (GDestroyNotify)gst_video_codec_frame_unref);
  fail_unless_equals_int(pending, 0);
}

/*
 * MPP signals a rate-controller drop with a packet that still carries its
 * MppBuffer but reports zero length. The frame produced no access unit, so
 * nothing may reach the muxer and the encoder must account it as dropped.
 *
 * Both packet-delivery modes are exercised because they fail differently: the
 * zero-copy branch (the property default) wraps the drop as a genuine zero-byte
 * GstBuffer, while the copy branch asks for a zero-sized output buffer, which
 * is a GLib precondition violation.
 */
static void check_rc_drop_is_not_pushed(gboolean zero_copy_pkt) {
  const GstClockTime frame_duration = GST_SECOND / 30;
  mpp_mock_reset();
  reset_output_capture();
  mpp_mock_enc_set_packet_length(1, 0);

  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, "zero-copy-pkt",
               zero_copy_pkt, NULL);
  GstBus *bus = gst_bus_new();
  gst_element_set_bus(h->element, bus);
  gst_pad_set_chain_function(h->sinkpad,
                             GST_DEBUG_FUNCPTR(capture_output_buffer));
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_play(h);

  for (guint i = 0; i < 3; i++)
    push_reset_frame(h, i);

  /* Deinit happens after finish_frame, so three deinits means all three
   * packets are fully processed and no further output can appear. */
  fail_unless(wait_for_uint(mpp_mock_enc_packet_deinits, 3),
              "encoder did not finish processing every packet");

  for (unsigned i = 0; i < read_output_capture_count(); i++)
    fail_unless(output_capture[i].size > 0,
                "output %u reached the peer as a %" G_GSIZE_FORMAT
                "-byte buffer",
                i, output_capture[i].size);
  fail_unless_equals_int(read_output_capture_count(), 2);
  fail_unless_equals_uint64(output_capture[0].pts, 0);
  fail_unless_equals_uint64(output_capture[1].pts, 2 * frame_duration);
  fail_unless_equals_int(output_capture[0].payload, 1);
  fail_unless_equals_int(output_capture[1].payload, 3);

  guint64 processed = 0;
  guint64 dropped = 0;
  read_qos_frame_counts(bus, &processed, &dropped);
  /* The QoS message carries the encoder's totals as of the drop itself, so
   * exactly one frame had been processed when the second one was dropped. */
  fail_unless_equals_uint64(dropped, 1);
  fail_unless_equals_uint64(processed, 1);
  assert_no_frames_left_pending(h->element);
  g_print("rc drop (zero-copy-pkt=%s): outputs=%u payloads=%u,%u "
          "processed=%" G_GUINT64_FORMAT " dropped=%" G_GUINT64_FORMAT
          " deinits=%u live-packets=%u\n",
          zero_copy_pkt ? "true" : "false", read_output_capture_count(),
          output_capture[0].payload, output_capture[1].payload, processed,
          dropped, mpp_mock_enc_packet_deinits(),
          mpp_mock_enc_live_packets());

  gst_element_set_bus(h->element, NULL);
  gst_harness_teardown(h);
  gst_object_unref(bus);
  fail_unless_equals_int(mpp_mock_enc_live_buffers(), 0);
}

GST_START_TEST(test_zero_length_rc_drop_is_not_pushed_downstream) {
  check_rc_drop_is_not_pushed(TRUE);
}
GST_END_TEST

GST_START_TEST(test_zero_length_rc_drop_is_not_pushed_when_copying) {
  check_rc_drop_is_not_pushed(FALSE);
}
GST_END_TEST

/*
 * pre_push() runs inside gst_video_encoder_finish_frame(), holding the very
 * frame that is about to be pushed, so the sync-point flag is read where the
 * encoder actually delivers rather than from element state afterwards.
 */
#define SYNC_CAPTURE_CAPACITY 8
static atomic_uint sync_capture_count;
static gboolean sync_capture_flag[SYNC_CAPTURE_CAPACITY];
static guint32 sync_capture_frame[SYNC_CAPTURE_CAPACITY];

static GstFlowReturn capture_sync_point(GstVideoEncoder *encoder,
                                        GstVideoCodecFrame *frame) {
  (void)encoder;
  unsigned index = atomic_load(&sync_capture_count);
  fail_unless(index < G_N_ELEMENTS(sync_capture_flag));
  sync_capture_flag[index] = GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT(frame);
  sync_capture_frame[index] = frame->system_frame_number;
  atomic_store(&sync_capture_count, index + 1);
  return GST_FLOW_OK;
}

/*
 * MPP flags an intra/IDR output with KEY_OUTPUT_INTRA. A sync point must reach
 * the peer without DELTA_UNIT, and a predicted frame must carry it.
 *
 * The pinned MPP writes the key on every output packet, IDR or not
 * (mpp/codec/mpp_enc_impl.cpp:2481), so ordinals 0 and 2 -- present-and-set and
 * present-and-zero -- are the two real shapes. Ordinal 1 omits the key entirely,
 * which MPP does not currently do; it is defensive coverage proving an absent
 * key cannot read as a set one.
 */
GST_START_TEST(test_intra_output_is_marked_as_a_sync_point) {
  mpp_mock_reset();
  reset_output_capture();
  atomic_store(&sync_capture_count, 0);
  memset(sync_capture_flag, 0, sizeof(sync_capture_flag));
  memset(sync_capture_frame, 0, sizeof(sync_capture_frame));
  mpp_mock_enc_set_packet_intra(0, 1);
  mpp_mock_enc_set_packet_intra(2, 0);

  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, NULL);
  GstVideoEncoderClass *klass = GST_VIDEO_ENCODER_GET_CLASS(h->element);
  GstFlowReturn (*saved_pre_push)(GstVideoEncoder *, GstVideoCodecFrame *) =
      klass->pre_push;
  klass->pre_push = capture_sync_point;
  gst_pad_set_chain_function(h->sinkpad,
                             GST_DEBUG_FUNCPTR(capture_output_buffer));
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_play(h);

  for (guint i = 0; i < 3; i++)
    push_reset_frame(h, i);
  fail_unless(wait_for_uint(mpp_mock_enc_packet_deinits, 3),
              "encoder did not finish processing every packet");

  fail_unless_equals_int(read_output_capture_count(), 3);
  fail_unless_equals_int(atomic_load(&sync_capture_count), 3);
  for (unsigned i = 0; i < 3; i++) {
    gboolean want_sync = (i == 0);
    fail_unless_equals_int(sync_capture_frame[i], i);
    fail_unless(sync_capture_flag[i] == want_sync,
                "frame %u reached the push path with sync-point=%d, expected %d",
                i, sync_capture_flag[i], want_sync);
    fail_unless(output_capture[i].delta_unit == !want_sync,
                "output %u carried DELTA_UNIT=%d, expected %d", i,
                output_capture[i].delta_unit, !want_sync);
  }
  g_print("intra sync points: sync=%d,%d,%d delta-unit=%d,%d,%d\n",
          sync_capture_flag[0], sync_capture_flag[1], sync_capture_flag[2],
          output_capture[0].delta_unit, output_capture[1].delta_unit,
          output_capture[2].delta_unit);

  klass->pre_push = saved_pre_push;
  gst_harness_teardown(h);
}
GST_END_TEST

#define CONVERT_FRAME_WIDTH 320
#define CONVERT_FRAME_HEIGHT 240
#define CONVERT_FRAME_SIZE (CONVERT_FRAME_WIDTH * CONVERT_FRAME_HEIGHT * 3 / 2)

static GstHarness *start_conversion_harness(gint rotation) {
  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, "zero-copy-pkt", FALSE,
               NULL);
  if (rotation)
    g_object_set(h->element, "rotation", rotation, NULL);
  gst_harness_set_src_caps_str(h, "video/x-raw,format=NV12,width=320,"
                                  "height=240,framerate=30/1");
  gst_harness_set_drop_buffers(h, TRUE);
  gst_harness_play(h);
  return h;
}

static GstFlowReturn push_conversion_frame(GstHarness *h, GstBuffer *frame) {
  GST_BUFFER_PTS(frame) = 0;
  GST_BUFFER_DURATION(frame) = GST_SECOND / 30;
  return gst_harness_push(h, frame);
}

/*
 * gst_mpp_enc_convert() appends the freshly allocated MPP memory to the output
 * buffer before it knows the conversion will work, so the buffer owns that
 * memory from then on. A rotation the RGA blit cannot service -- RGA is
 * compiled in, so set_format accepts the property, but no RGA hardware exists
 * here so the blit fails -- lands on the error path with the append already
 * done. Releasing the local reference there is a second release of memory the
 * buffer is about to release itself.
 *
 * The flow return is an error either way, so what marks the defect is not an
 * assertion here but GStreamer's own parent tracking: gst_buffer_append_memory()
 * registers the buffer as a parent of the memory, and finalising the memory
 * while that registration stands raises "object finalizing but still has 1
 * parents", which gst_check turns into a failure.
 */
GST_START_TEST(test_failed_rotation_leaves_appended_memory_singly_owned) {
  mpp_mock_reset();
  GstHarness *h = start_conversion_harness(90);

  GstFlowReturn ret = push_conversion_frame(
      h, gst_buffer_new_allocate(NULL, CONVERT_FRAME_SIZE, NULL));

  fail_unless_equals_int(ret, GST_FLOW_NOT_NEGOTIATED);
  assert_no_frames_left_pending(h->element);
  g_print("failed rotation: flow=%s\n", gst_flow_get_name(ret));

  gst_harness_teardown(h);
  fail_unless_equals_int(mpp_mock_enc_queued_packets(), 0);
  fail_unless_equals_int(mpp_mock_enc_live_buffers(), 0);
}
GST_END_TEST

static void check_factory(const char *name, const char *property) {
  GstElementFactory *f = gst_element_factory_find(name);
  fail_unless(f != NULL, "missing %s", name);
  GstElement *e = gst_element_factory_create(f, NULL);
  fail_unless(e != NULL);
  guint n = 0;
  GParamSpec **ps = g_object_class_list_properties(G_OBJECT_GET_CLASS(e), &n);
  fail_unless(n > 0);
  fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(e), property) !=
              NULL);
  g_free(ps);
  gst_object_unref(e);
  gst_object_unref(f);
}

GST_START_TEST(test_factories_properties) {
  check_factory("mpph264enc", "bitrate");
  check_factory("mpph265enc", "bitrate");
  check_factory("mppvideodec", "fbc");
  check_factory("mppjpegdec", "format");
}
GST_END_TEST
GST_START_TEST(test_runtime_property_snapshot_is_coherent_and_eventually_applied) {
  const guint before_pause_bitrate = 1600000;
  const guint during_pause_bitrate = 3200000;
  const guint final_bitrate = 6400000;
  mpp_mock_reset();

  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 1000000, "rc-mode", 1,
               "zero-copy-pkt", FALSE, NULL);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_set_drop_buffers(h, TRUE);
  gst_harness_play(h);

  unsigned first_record = mpp_mock_enc_cfg_record_count();
  g_object_set(h->element, "bitrate", before_pause_bitrate, NULL);
  mpp_mock_enc_pause_next_bps_target();
  PausedPropertySet paused_set = {
      .element = h->element,
      .bitrate = during_pause_bitrate,
  };
  atomic_init(&paused_set.failed, 0);
  GThread *setter =
      g_thread_new("mpp-bitrate-setter", set_bitrate_at_bps_pause, &paused_set);
  push_runtime_property_frame(h, 0);
  g_thread_join(setter);
  fail_unless_equals_int(atomic_load(&paused_set.failed), 0);
  assert_coherent_bitrate_records(first_record);

  g_object_set(h->element, "bitrate", 960000, NULL);
  mpp_mock_enc_pause_next_set_cfg();
  PropertyHammer hammer = {
      .element = h->element,
      .final_bitrate = final_bitrate,
  };
  atomic_init(&hammer.failed, 0);
  setter = g_thread_new("mpp-bitrate-hammer",
                        hammer_bitrate_until_set_cfg_pause, &hammer);
  push_runtime_property_frame(h, 1);
  g_thread_join(setter);
  fail_unless_equals_int(atomic_load(&hammer.failed), 0);

  for (guint i = 2; i < 128 && !recorded_bitrate(final_bitrate); i++)
    push_runtime_property_frame(h, i);
  fail_unless(recorded_bitrate(final_bitrate),
              "the final quiescent bitrate was never applied");
  assert_coherent_bitrate_records(first_record);

  gst_harness_teardown(h);
}
GST_END_TEST
GST_START_TEST(test_encoder_reset_drains_old_packets_before_new_session) {
  mpp_mock_reset();
  mpp_mock_enc_arm_reset_drain();
  atomic_store(&reset_output_count, 0);
  atomic_store(&fail_first_reset_output, 1);
  memset(reset_output_ids, 0, sizeof(reset_output_ids));
  memset(reset_output_pts, 0, sizeof(reset_output_pts));

  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, "zero-copy-pkt", FALSE,
               "max-pending", 16, NULL);
  gst_pad_set_chain_function(h->sinkpad,
                             GST_DEBUG_FUNCPTR(capture_reset_output));
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_play(h);

  for (guint i = 0; i < 16; i++)
    push_reset_frame(h, i);
  fail_unless(wait_for_uint(mpp_mock_enc_queued_packets, 16),
              "encoder did not queue the maximum old-session backlog");

  mpp_mock_enc_release_packets(1);
  fail_unless(wait_for_uint(read_reset_output_count, 1),
              "first packet did not pause the encoder task");
  fail_unless_equals_int(mpp_mock_enc_dequeued_packets(), 1);
  fail_unless_equals_int(mpp_mock_enc_queue_depth(), 15);

  unsigned reset_generation = mpp_mock_enc_reset_generation() + 1;
  fail_unless(gst_element_set_state(h->element, GST_STATE_READY) !=
              GST_STATE_CHANGE_FAILURE);
  fail_unless(mpp_mock_enc_reset_generation() >= reset_generation);
  fail_unless_equals_int(mpp_mock_enc_dequeued_packets(), 16);
  fail_unless_equals_int(mpp_mock_enc_queue_depth(), 0);
  fail_unless_equals_int(
      mpp_mock_enc_empty_polls_for_generation(reset_generation), 1);
  fail_unless_equals_int(mpp_mock_enc_duplicate_dequeues(), 0);
  fail_unless_equals_int(mpp_mock_enc_packet_deinits(), 16);
  fail_unless_equals_int(mpp_mock_enc_packet_double_deinits(), 0);
  fail_unless_equals_int(mpp_mock_enc_live_packets(), 0);
  g_print("reset generation %u drained old packets: dequeued=%u depth=%u "
          "empty-polls=%u deinits=%u double-deinits=%u live=%u duplicates=%u\n",
          reset_generation,
          mpp_mock_enc_dequeued_packets(), mpp_mock_enc_queue_depth(),
          mpp_mock_enc_empty_polls_for_generation(reset_generation),
          mpp_mock_enc_packet_deinits(),
          mpp_mock_enc_packet_double_deinits(), mpp_mock_enc_live_packets(),
          mpp_mock_enc_duplicate_dequeues());

  fail_unless_equals_int(reset_output_ids[0], 1);
  fail_unless_equals_uint64(reset_output_pts[0], 0);

  gst_harness_play(h);
  push_reset_frame(h, 100);
  fail_unless(wait_for_uint(read_reset_output_count, 2),
              "new-session packet was not produced");
  fail_unless_equals_int(reset_output_ids[1], 17);
  fail_unless_equals_uint64(reset_output_pts[1], 100 * (GST_SECOND / 30));
  fail_unless(wait_for_uint(mpp_mock_enc_packet_deinits, 17),
              "new-session packet was not released");
  fail_unless_equals_int(mpp_mock_enc_dequeued_packets(), 17);
  fail_unless_equals_int(mpp_mock_enc_queue_depth(), 0);
  fail_unless_equals_int(mpp_mock_enc_duplicate_dequeues(), 0);
  fail_unless_equals_int(mpp_mock_enc_packet_deinits(), 17);
  fail_unless_equals_int(mpp_mock_enc_packet_double_deinits(), 0);
  fail_unless_equals_int(mpp_mock_enc_live_packets(), 0);
  g_print("new session output: packet=%u pts=%" G_GUINT64_FORMAT
          " dequeued=%u depth=%u deinits=%u live=%u\n",
          reset_output_ids[1], reset_output_pts[1],
          mpp_mock_enc_dequeued_packets(), mpp_mock_enc_queue_depth(),
          mpp_mock_enc_packet_deinits(), mpp_mock_enc_live_packets());

  mpp_mock_enc_arm_reset_drain();
  atomic_store(&fail_first_reset_output, 1);
  push_reset_frame(h, 200);
  push_reset_frame(h, 201);
  fail_unless(wait_for_uint(mpp_mock_enc_queued_packets, 19),
              "encoder did not queue the variable-backlog reset packets");

  mpp_mock_enc_release_packets(18);
  fail_unless(wait_for_uint(read_reset_output_count, 3),
              "packet 18 did not pause the encoder task");
  fail_unless_equals_int(reset_output_ids[2], 18);
  fail_unless_equals_uint64(reset_output_pts[2], 200 * (GST_SECOND / 30));
  fail_unless_equals_int(mpp_mock_enc_dequeued_packets(), 18);
  fail_unless_equals_int(mpp_mock_enc_queue_depth(), 1);

  reset_generation = mpp_mock_enc_reset_generation() + 1;
  fail_unless(gst_element_set_state(h->element, GST_STATE_READY) !=
              GST_STATE_CHANGE_FAILURE);
  fail_unless_equals_int(mpp_mock_enc_dequeued_packets(), 19);
  fail_unless_equals_int(mpp_mock_enc_queue_depth(), 0);
  fail_unless_equals_int(
      mpp_mock_enc_empty_polls_for_generation(reset_generation), 1);
  fail_unless_equals_int(mpp_mock_enc_packet_deinits(), 19);
  fail_unless_equals_int(mpp_mock_enc_packet_double_deinits(), 0);
  fail_unless_equals_int(mpp_mock_enc_live_packets(), 0);
  fail_unless_equals_int(mpp_mock_enc_duplicate_dequeues(), 0);
  g_print("reset generation %u variable backlog: dequeued=%u depth=%u "
          "empty-polls=%u deinits=%u live=%u\n",
          reset_generation, mpp_mock_enc_dequeued_packets(),
          mpp_mock_enc_queue_depth(),
          mpp_mock_enc_empty_polls_for_generation(reset_generation),
          mpp_mock_enc_packet_deinits(), mpp_mock_enc_live_packets());

  gst_harness_teardown(h);
}
GST_END_TEST
GST_START_TEST(test_jpeg_caps_with_harness) {
  GstHarness *h = gst_harness_new_empty();
  fail_unless(h != NULL);
  GstElement *e = gst_element_factory_make("mppjpegdec", NULL);
  fail_unless(e != NULL);
  GstPad *src = gst_element_get_static_pad(e, "src");
  fail_unless(src != NULL);
  gst_harness_add_element_src_pad(h, src);
  GstCaps *caps = gst_pad_get_pad_template_caps(src);
  GstCaps *nv12 = gst_caps_from_string("video/x-raw,format=NV12");
  GstCaps *nv12_range = gst_caps_from_string(
      "video/x-raw,format=NV12,width=[1,MAX],height=[1,MAX]");
  fail_unless(gst_caps_can_intersect(caps, nv12));
  fail_unless(gst_caps_can_intersect(caps, nv12_range));
  fail_unless(gst_caps_is_fixed(caps) == FALSE);
  gst_caps_unref(nv12);
  gst_caps_unref(nv12_range);
  gst_caps_unref(caps);
  gst_object_unref(src);
  gst_harness_teardown(h);
}
GST_END_TEST
GST_START_TEST(test_video_decoder_caps_truth) {
  GstElementFactory *factory = gst_element_factory_find("mppvideodec");
  fail_unless(factory != NULL);
  const GList *templates =
      gst_element_factory_get_static_pad_templates(factory);
  GstCaps *sink_caps = NULL;
  GstCaps *src_caps = NULL;
  for (const GList *item = templates; item != NULL; item = item->next) {
    GstStaticPadTemplate *templ = item->data;
    GstCaps *caps = gst_static_caps_get(&templ->static_caps);
    if (templ->direction == GST_PAD_SINK)
      sink_caps = caps;
    else if (templ->direction == GST_PAD_SRC)
      src_caps = caps;
    else
      gst_caps_unref(caps);
  }
  fail_unless(sink_caps != NULL);
  fail_unless(src_caps != NULL);
  GstCaps *h264 = gst_caps_from_string("video/x-h264,parsed=true,alignment=au");
  GstCaps *h265 = gst_caps_from_string("video/x-h265,parsed=true,alignment=au");
  GstCaps *av1 = gst_caps_from_string("video/x-av1,parsed=true");
  GstCaps *nv12 = gst_caps_from_string("video/x-raw,format=NV12");
  fail_unless(gst_caps_can_intersect(sink_caps, h264));
  fail_unless(gst_caps_can_intersect(sink_caps, h265));
  fail_unless(gst_caps_can_intersect(sink_caps, av1));
  fail_unless(gst_caps_can_intersect(src_caps, nv12));
  gst_caps_unref(h264);
  gst_caps_unref(h265);
  gst_caps_unref(av1);
  gst_caps_unref(nv12);
  gst_caps_unref(sink_caps);
  gst_caps_unref(src_caps);
  gst_object_unref(factory);
}
GST_END_TEST
static void check_encoder_lifecycle(const char *factory) {
  GstHarness *h = gst_harness_new(factory);
  fail_unless(h != NULL, "could not create %s", factory);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, "zero-copy-pkt", FALSE,
               NULL);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_play(h);
  GstBuffer *frame = gst_buffer_new_allocate(NULL, 115200, NULL);
  fail_unless(frame != NULL);
  GST_BUFFER_PTS(frame) = 0;
  GST_BUFFER_DURATION(frame) = GST_SECOND / 30;
  gst_harness_set_drop_buffers(h, TRUE);
  fail_unless_equals_int(gst_harness_push(h, frame), GST_FLOW_OK);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:width"), 320);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:height"), 240);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:mode"), 1);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_target"), 500);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:fps_in_num"), 30);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:fps_in_denorm"), 1);
  fail_unless(mpp_mock_control_count(MPP_ENC_SET_CFG) > 0);
  fail_unless(mpp_mock_control_count(MPP_ENC_SET_SEI_CFG) > 0);
  fail_unless(mpp_mock_control_count(MPP_ENC_SET_HEADER_MODE) > 0);
  fail_unless(wait_for_encoder_frame(),
              "encoder frame must stay inside the mock MPP ABI");
  gst_harness_teardown(h);
}
static GstHarness *start_encoder_harness(const char *caps) {
  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "rc-mode", 1, "zero-copy-pkt", FALSE, NULL);
  gst_harness_set_src_caps_str(h, caps);
  gst_harness_set_drop_buffers(h, TRUE);
  gst_harness_play(h);
  return h;
}

GST_START_TEST(test_drop_threshold_uses_the_key_mpp_actually_registers) {
  mpp_mock_reset();
  GstHarness *h =
      start_encoder_harness("video/x-raw,format=NV12,width=320,height=240,"
                            "framerate=30/1");
  g_object_set(h->element, "bitrate", 500, "drop-mode", 1, "drop-threshold", 42,
               NULL);
  push_runtime_property_frame(h, 0);

  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:drop_thd"), 42);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:drop_threshold"), INT32_MIN);
  gst_harness_teardown(h);
}
GST_END_TEST

GST_START_TEST(test_rejected_config_key_fails_the_apply) {
  mpp_mock_reset();
  mpp_mock_enc_cfg_reject_key("rc:drop_thd");

  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "bitrate", 500, "rc-mode", 1, "zero-copy-pkt", FALSE,
               NULL);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1");
  gst_harness_set_drop_buffers(h, TRUE);
  gst_harness_play(h);

  GstBuffer *frame = gst_buffer_new_allocate(NULL, 115200, NULL);
  fail_unless(frame != NULL);
  GST_BUFFER_PTS(frame) = 0;
  GST_BUFFER_DURATION(frame) = GST_SECOND / 30;
  fail_unless_equals_int(gst_harness_push(h, frame), GST_FLOW_NOT_NEGOTIATED);
  fail_unless_equals_int(mpp_mock_control_count(MPP_ENC_SET_CFG), 0);
  gst_harness_teardown(h);
}
GST_END_TEST

GST_START_TEST(test_zero_valued_tuning_resets_reach_mpp) {
  mpp_mock_reset();
  GstHarness *h =
      start_encoder_harness("video/x-raw,format=NV12,width=320,height=240,"
                            "framerate=30/1");
  g_object_set(h->element, "bitrate", 500, "scene-mode", 1, "anti-flicker", 3,
               "super-mode", 1, "super-i-thd", 90000, "super-p-thd", 30000,
               "debreath", TRUE, "debreath-strength", 20, NULL);
  push_runtime_property_frame(h, 0);

  fail_unless_equals_int(mpp_mock_last_cfg_s32("tune:scene_mode"), 1);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("tune:anti_flicker_str"), 3);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:super_i_thd"), 90000);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:super_p_thd"), 30000);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:debreath_strength"), 20);

  g_object_set(h->element, "scene-mode", 0, "anti-flicker", 0,
               "debreath-strength", 0, NULL);
  push_runtime_property_frame(h, 1);

  fail_unless_equals_int(mpp_mock_last_cfg_s32("tune:scene_mode"), 0);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("tune:anti_flicker_str"), 0);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:debreath_strength"), 0);
  gst_harness_teardown(h);
}
GST_END_TEST

/*
 * The super-frame thresholds are NOT plain zero-resettable fields, so they are
 * asserted apart from the four above. MPP classifies a frame as super with
 * `(RK_U32) bit_real >= bits_thr` (mpp/codec/rc/rc_model_v2.c:1276), so writing
 * a raw 0 while the mode is on marks every frame super -- and under
 * SUPER_FRM_DROP that same branch rewrites the rate controller's drop_mode.
 * "Unset" therefore has to reach MPP as a threshold nothing attains.
 */
static unsigned delivered_enc_cfg(unsigned before, const char *phase) {
  unsigned now = mpp_mock_enc_cfg_record_count();
  fail_unless(now > before, "%s: nothing was delivered to MPP_ENC_SET_CFG",
              phase);
  return now - 1;
}

static void check_super_thresholds(unsigned record, const char *phase,
                                   gint mode, gint64 i_thd, gint64 p_thd) {
  fail_unless_equals_int(mpp_mock_enc_cfg_record_s32(record, "rc:super_mode"),
                         mode);
  fail_unless(mpp_mock_enc_cfg_record_value(record, "rc:super_i_thd") == i_thd,
              "%s: I threshold reached MPP as %" G_GINT64_FORMAT
              ", expected %" G_GINT64_FORMAT,
              phase, mpp_mock_enc_cfg_record_value(record, "rc:super_i_thd"),
              i_thd);
  fail_unless(mpp_mock_enc_cfg_record_value(record, "rc:super_p_thd") == p_thd,
              "%s: P threshold reached MPP as %" G_GINT64_FORMAT
              ", expected %" G_GINT64_FORMAT,
              phase, mpp_mock_enc_cfg_record_value(record, "rc:super_p_thd"),
              p_thd);
}

GST_START_TEST(test_super_frame_thresholds_reset_to_unreachable_not_zero) {
  mpp_mock_reset();
  GstHarness *h =
      start_encoder_harness("video/x-raw,format=NV12,width=320,height=240,"
                            "framerate=30/1");

  unsigned before = mpp_mock_enc_cfg_record_count();
  g_object_set(h->element, "bitrate", 500, "super-mode", 1, "super-i-thd",
               90000, "super-p-thd", 30000, NULL);
  push_runtime_property_frame(h, 0);
  check_super_thresholds(delivered_enc_cfg(before, "enable"), "enable", 1,
                         90000, 30000);

  /* Thresholds cleared while the mode is OFF must still reach MPP, or they
   * resurrect at the next enable. */
  before = mpp_mock_enc_cfg_record_count();
  g_object_set(h->element, "super-mode", 0, "super-i-thd", 0, "super-p-thd", 0,
               NULL);
  push_runtime_property_frame(h, 1);
  check_super_thresholds(delivered_enc_cfg(before, "disabled clear"),
                         "disabled clear", 0, 0xFFFFFFFFLL, 0xFFFFFFFFLL);

  /* Re-enabling must neither restore the old thresholds nor hand MPP a 0 that
   * classifies every frame as super. */
  before = mpp_mock_enc_cfg_record_count();
  g_object_set(h->element, "super-mode", 1, NULL);
  push_runtime_property_frame(h, 2);
  check_super_thresholds(delivered_enc_cfg(before, "re-enable"), "re-enable", 1,
                         0xFFFFFFFFLL, 0xFFFFFFFFLL);
  gst_harness_teardown(h);
}
GST_END_TEST

static void push_sized_frame(GstHarness *h, guint index, gsize size,
                             gint fps) {
  GstBuffer *frame = gst_buffer_new_allocate(NULL, size, NULL);
  fail_unless(frame != NULL);
  GST_BUFFER_PTS(frame) = index * (GST_SECOND / fps);
  GST_BUFFER_DURATION(frame) = GST_SECOND / fps;
  fail_unless_equals_int(gst_harness_push(h, frame), GST_FLOW_OK);
}

GST_START_TEST(test_auto_bitrate_recomputes_for_new_output_geometry) {
  mpp_mock_reset();
  GstHarness *h =
      start_encoder_harness("video/x-raw,format=NV12,width=320,height=240,"
                            "framerate=30/1");
  push_sized_frame(h, 0, 115200, 30);

  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:width"), 320);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_target"),
                         320 * 240 / 8 * 30);

  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=640,height=480,framerate=30/1");
  push_sized_frame(h, 1, 460800, 30);

  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:width"), 640);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_target"),
                         640 * 480 / 8 * 30);
  gst_harness_teardown(h);
}
GST_END_TEST

GST_START_TEST(test_auto_bitrate_keeps_the_zero_sentinel) {
  mpp_mock_reset();
  GstHarness *h =
      start_encoder_harness("video/x-raw,format=NV12,width=320,height=240,"
                            "framerate=30/1");
  push_sized_frame(h, 0, 115200, 30);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_target"),
                         320 * 240 / 8 * 30);

  guint reported = 1;
  g_object_get(h->element, "bitrate", &reported, NULL);
  fail_unless_equals_int(reported, 0);
  gst_harness_teardown(h);
}
GST_END_TEST

/*
 * The automatic target is derived while the caps event is handled, so these
 * saturation cases need no buffer -- which is what makes them affordable, since
 * a real frame at these geometries is 100MB and 1.6GB respectively.
 *
 * 8192x8192 at the 256 fps-out ceiling puts width*height/8*fps at exactly 2^31,
 * the boundary where 32-bit arithmetic turns negative. 32768x32768 is simply a
 * large geometry GStreamer accepts for NV12; it is not a universal ceiling,
 * because gst_video_info_from_caps() bounds the frame by format
 * (round_up_128(width) * height against G_MAXUINT / bpp), so the limit differs
 * per format and is not a property of this element.
 */
static void check_auto_bitrate_saturates(const char *caps) {
  mpp_mock_reset();
  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "rc-mode", 1, "zero-copy-pkt", FALSE, "fps-out", 256,
               NULL);
  gst_harness_set_src_caps_str(h, caps);

  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_target"), G_MAXINT);
  /* The derived bounds saturate too. In CBR they are target*17/16 and
   * target*15/16, which overflow a signed 32-bit int at this target. */
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_max"), G_MAXINT);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_min"),
                         (gint)((gint64)G_MAXINT * 15 / 16));
  gst_harness_teardown(h);
}

GST_START_TEST(test_auto_bitrate_clamps_instead_of_overflowing) {
  check_auto_bitrate_saturates(
      "video/x-raw,format=NV12,width=8192,height=8192,framerate=30/1");
}
GST_END_TEST

GST_START_TEST(test_auto_bitrate_saturates_at_a_large_nv12_geometry) {
  check_auto_bitrate_saturates(
      "video/x-raw,format=NV12,width=32768,height=32768,framerate=30/1");
}
GST_END_TEST

/*
 * The runtime width/height ladder reaches the encoder through
 * gst_mpp_enc_apply_pending_resolution(), a different path from a caps change:
 * apply_strides() cannot mark properties dirty here because it is handed the
 * strides it just read back out of the same info, so its equality check always
 * short-circuits. Without an explicit prop_dirty on that path the automatic
 * bitrate keeps the geometry it was first negotiated with.
 */
GST_START_TEST(test_auto_bitrate_recomputes_for_runtime_resolution_property) {
  mpp_mock_reset();
  GstHarness *h =
      start_encoder_harness("video/x-raw,format=NV12,width=320,height=240,"
                            "framerate=30/1");
  push_sized_frame(h, 0, 115200, 30);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_target"),
                         320 * 240 / 8 * 30);

  g_object_set(h->element, "width", 160, "height", 120, NULL);

  /* The rescale itself needs real RGA hardware, so these pushes are expected to
   * fail downstream of the config work; the assertion is on what reached MPP,
   * not on the flow return. The first frame runs apply_pending_resolution, the
   * second is when the re-derived config is applied. */
  for (guint i = 1; i < 4; i++) {
    GstBuffer *frame = gst_buffer_new_allocate(NULL, 115200, NULL);
    fail_unless(frame != NULL);
    GST_BUFFER_PTS(frame) = i * (GST_SECOND / 30);
    GST_BUFFER_DURATION(frame) = GST_SECOND / 30;
    gst_harness_push(h, frame);
  }

  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:width"), 160);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("prep:height"), 120);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_target"),
                         160 * 120 / 8 * 30);
  gst_harness_teardown(h);
}
GST_END_TEST

GST_START_TEST(test_gop_and_auto_bitrate_follow_fps_out) {
  mpp_mock_reset();
  GstHarness *h = gst_harness_new("mpph264enc");
  fail_unless(h != NULL);
  g_object_set(h->element, "rc-mode", 1, "zero-copy-pkt", FALSE, "fps-out", 30,
               NULL);
  gst_harness_set_src_caps_str(
      h, "video/x-raw,format=NV12,width=320,height=240,framerate=60/1");
  gst_harness_set_drop_buffers(h, TRUE);
  gst_harness_play(h);
  push_sized_frame(h, 0, 115200, 60);

  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:fps_in_num"), 60);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:fps_out_num"), 30);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:gop"), 30);
  fail_unless_equals_int(mpp_mock_last_cfg_s32("rc:bps_target"),
                         320 * 240 / 8 * 30);
  gst_harness_teardown(h);
}
GST_END_TEST

GST_START_TEST(test_h264_encoder_lifecycle) {
  mpp_mock_reset();
  check_encoder_lifecycle("mpph264enc");
}
GST_END_TEST
GST_START_TEST(test_h265_encoder_lifecycle) {
  mpp_mock_reset();
  check_encoder_lifecycle("mpph265enc");
}
GST_END_TEST

Suite *mpp_gstharness_suite(void) {
  Suite *s = suite_create("rockchipmpp");
  TCase *tc = tcase_create("caps");
  tcase_add_test(tc, test_factories_properties);
  tcase_add_test(tc, test_jpeg_caps_with_harness);
  tcase_add_test(tc, test_video_decoder_caps_truth);
  tcase_add_test(tc, test_drop_threshold_uses_the_key_mpp_actually_registers);
  tcase_add_test(tc, test_rejected_config_key_fails_the_apply);
  tcase_add_test(tc, test_zero_valued_tuning_resets_reach_mpp);
  tcase_add_test(tc, test_super_frame_thresholds_reset_to_unreachable_not_zero);
  tcase_add_test(tc, test_auto_bitrate_recomputes_for_new_output_geometry);
  tcase_add_test(tc,
                 test_auto_bitrate_recomputes_for_runtime_resolution_property);
  tcase_add_test(tc, test_auto_bitrate_keeps_the_zero_sentinel);
  tcase_add_test(tc, test_auto_bitrate_clamps_instead_of_overflowing);
  tcase_add_test(tc, test_auto_bitrate_saturates_at_a_large_nv12_geometry);
  tcase_add_test(tc, test_gop_and_auto_bitrate_follow_fps_out);
  tcase_add_test(tc, test_h264_encoder_lifecycle);
  tcase_add_test(tc, test_h265_encoder_lifecycle);
  tcase_add_test(tc, test_zero_length_rc_drop_is_not_pushed_downstream);
  tcase_add_test(tc, test_zero_length_rc_drop_is_not_pushed_when_copying);
  tcase_add_test(tc, test_intra_output_is_marked_as_a_sync_point);
  tcase_add_test(tc, test_failed_rotation_leaves_appended_memory_singly_owned);
  tcase_add_test(
      tc, test_runtime_property_snapshot_is_coherent_and_eventually_applied);
  tcase_add_test(tc, test_encoder_reset_drains_old_packets_before_new_session);
  suite_add_tcase(s, tc);
  return s;
}
GST_CHECK_MAIN(mpp_gstharness)
