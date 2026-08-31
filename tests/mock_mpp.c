#define _GNU_SOURCE
#include <rockchip/mpp_log.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_venc_cfg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static MppApi api;
typedef struct {
  char key[96];
  int64_t value;
  void *ptr;
  int kind;
} CfgEntry;
typedef struct {
  CfgEntry entries[128];
  unsigned count;
} MockCfg;
typedef struct {
  int bps_target;
  int bps_min;
  int bps_max;
  int super_mode;
  int64_t super_i_thd;
  int64_t super_p_thd;
} MockEncCfgRecord;
/* Real MPP resolves a config key through a trie and answers MPP_NOK for a name
 * it does not know, writing nothing. Tests arm a name here to reproduce that
 * without needing an MPP build that actually lacks the key. */
#define ENC_CFG_REJECT_CAPACITY 4
static char enc_cfg_rejected_keys[ENC_CFG_REJECT_CAPACITY][96];
static unsigned enc_cfg_rejected_count;
#define ENC_CFG_RECORD_CAPACITY 4096
static int control_counts[512];
static MppEncCfg last_cfg;
static MockEncCfgRecord enc_cfg_records[ENC_CFG_RECORD_CAPACITY];
static unsigned enc_cfg_record_count;
static unsigned enc_cfg_record_dropped;
static atomic_flag enc_cfg_record_lock = ATOMIC_FLAG_INIT;
static atomic_int enc_bps_pause_armed;
static atomic_int enc_bps_pause_entered;
static atomic_int enc_bps_pause_release;
static atomic_int enc_control_pause_armed;
static atomic_int enc_control_pause_entered;
static atomic_int enc_control_pause_release;
/* MPP validates a reference structure inside MPP_ENC_SET_REF_CFG and answers
 * MPP_NOK for one its encoder cannot honour. Tests arm that refusal here. */
static atomic_int enc_reject_ref_cfg;
static atomic_uint enc_ref_cfg_calls;
static atomic_uint enc_ref_cfg_resets;
typedef struct MockPacket {
  MppFrame input_frame;
  MppBuffer buffer;
  size_t length;
  unsigned id;
  int has_intra_meta;
  int intra;
  int heap;
  int encoder_packet;
  int eos;
  int extra_data;
  int alive;
  int sent;
  int has_buffer;
  int mpp_owned;
  struct MockPacket *next;
} MockPacket;
typedef struct {
  int fd;
  size_t size;
  void *data;
  int index;
  unsigned refs;
  size_t map_size;
  int enc_owned;
} MockBuffer;
typedef struct {
  MppFrameFormat format;
  RK_U32 width;
  RK_U32 height;
  RK_U32 horizontal_stride;
  RK_U32 vertical_stride;
  MppBuffer buffer;
  RK_S64 pts;
  RK_U32 info_change;
  MppPacket packet;
} MockFrame;
typedef struct {
  RK_U32 width;
  RK_U32 height;
} MockEncGeometryRecord;
typedef struct {
  MppPacket packet;
  MppFrame frame;
} MockTask;
#define ENC_PACKET_CAPACITY 16
#define ENC_RESET_GENERATIONS 16
/* Per-packet shape, indexed by the 0-based order in which MPP hands packets
 * back. Tests arm the whole plan before pushing a single frame, so the encoder
 * task never races an arming call. Unarmed ordinals keep the default shape. */
#define ENC_PLAN_CAPACITY 32
#define ENC_PLAN_DEFAULT_LENGTH 1
static size_t enc_plan_length[ENC_PLAN_CAPACITY];
static int enc_plan_length_armed[ENC_PLAN_CAPACITY];
/* Tri-state: unarmed (no KEY_OUTPUT_INTRA at all), present and zero, or
 * present and set. The pinned MPP writes the key on every output packet
 * (mpp/codec/mpp_enc_impl.cpp:2481), so the unarmed state is defensive
 * coverage, not the shape MPP normally produces. */
static int enc_plan_intra[ENC_PLAN_CAPACITY];
static int enc_plan_intra_armed[ENC_PLAN_CAPACITY];
/* Page-sized and mmap-backed so the plugin's zero-copy branch can import the
 * dmafd and read the payload back, which the shipped default configuration
 * does. A malloc'd pointer has no fd and silently retires that whole path. */
#define ENC_BUFFER_MAP_SIZE 4096
static MockPacket enc_packets[ENC_PACKET_CAPACITY];
static atomic_uint enc_live_buffers;
static atomic_int buffer_import_failure_armed;
static atomic_uint buffer_import_calls;
static atomic_uint buffer_inc_ref_calls;
/* One-shot: the next buffer MPP hands out carries a dmafd the CPU cannot map. */
static atomic_int buffer_unmappable_armed;
static atomic_uint buffer_unmappable_handed_out;
static atomic_uint enc_head;
static atomic_uint enc_tail;
static atomic_uint frame_set_buffer_count;
static atomic_int enc_test_armed;
static atomic_int enc_reset_seen;
static atomic_uint enc_release_limit;
static atomic_uint enc_queued_packets;
static atomic_uint enc_dequeued_packets;
static atomic_uint enc_duplicate_dequeues;
static atomic_ullong enc_dequeue_mask;
static atomic_uint enc_reset_generation;
static atomic_uint enc_empty_polls_by_generation[ENC_RESET_GENERATIONS];
static atomic_uint enc_packet_deinits;
static atomic_uint enc_packet_double_deinits;
static atomic_uint enc_live_packets;
static atomic_int enc_reject_put;
static atomic_uint enc_put_rejections;
static atomic_uint enc_geometry_record_count;
static MockEncGeometryRecord enc_geometry_records[ENC_PLAN_CAPACITY];
static atomic_uint enc_gap_empty_polls;
static atomic_uint enc_gap_empty_polls_remaining;
static atomic_uint enc_gap_empty_polls_per_packet;

/* Off unless a test arms it, so the encoder harness keeps the MPP behavior it
 * was written against. */
static atomic_int dec_enabled;
static atomic_int dec_info_change_sent;
static atomic_uint dec_queued;
static atomic_uint dec_outputs;
static atomic_uint dec_eos_seen;
static atomic_uint dec_put_calls;
static atomic_uint dec_buffer_full_remaining;
static atomic_int dec_put_terminal;
static atomic_int dec_next_packet_has_buffer;
static atomic_uint dec_packet_buffer_queries;
static atomic_uint dec_packet_post_send_accesses;
static atomic_uint dec_packet_deinits;
static atomic_uint dec_wrong_owner_deinits;
static atomic_uint dec_double_deinits;
static atomic_uint dec_frame_deinits;
static atomic_int dec_output_suppressed;
static atomic_int dec_output_buffers_enabled;
static atomic_uint internal_group_types;
static atomic_uint jpeg_input_timeouts_remaining;
static atomic_uint jpeg_input_poll_calls;
static atomic_int jpeg_last_input_poll_timed_out;
static MockTask jpeg_task;
static MockPacket *packet_allocs;
static atomic_uint dec_arena_packets;
static MppBuffer dec_output_buffer;
static RK_U32 dec_width = 320;
static RK_U32 dec_height = 240;
static atomic_int dec_frame_format;
/* Kept far BELOW every input PTS so the decoder's display-order matcher filters
 * each pending frame out as "future" and settles on no match. Nonzero is
 * load-bearing: gstmppdec.c remaps a zero PTS to GST_CLOCK_TIME_NONE, which
 * matches the oldest frame instead of missing. */
static atomic_llong dec_stale_pts = 1;
static MockCfg *cfg_of(MppEncCfg c) { return (MockCfg *)c; }
static void pause_if_armed(atomic_int *armed, atomic_int *entered,
                           atomic_int *release) {
  if (!atomic_exchange(armed, 0))
    return;

  atomic_store(entered, 1);
  while (!atomic_load(release))
    usleep(100);
}
static MPP_RET cfg_set(MppEncCfg c, const char *name, int64_t v, void *ptr,
                       int kind) {
  MockCfg *m = cfg_of(c);
  CfgEntry *entry = NULL;
  for (unsigned i = 0; i < enc_cfg_rejected_count; i++)
    if (!strcmp(enc_cfg_rejected_keys[i], name))
      return MPP_NOK;
  for (unsigned i = 0; i < m->count; i++) {
    if (!strcmp(m->entries[i].key, name)) {
      entry = &m->entries[i];
      break;
    }
  }
  if (!entry) {
    if (m->count >= 128)
      return MPP_NOK;
    entry = &m->entries[m->count++];
    snprintf(entry->key, sizeof(entry->key), "%s", name);
  }
  entry->value = v;
  entry->ptr = ptr;
  entry->kind = kind;

  if (!strcmp(name, "rc:bps_target"))
    pause_if_armed(&enc_bps_pause_armed, &enc_bps_pause_entered,
                   &enc_bps_pause_release);
  return MPP_OK;
}
static CfgEntry *cfg_find(MppEncCfg c, const char *n) {
  MockCfg *m = cfg_of(c);
  for (unsigned i = 0; i < m->count; i++)
    if (!strcmp(m->entries[i].key, n))
      return &m->entries[i];
  return NULL;
}
static void enc_cfg_record_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&enc_cfg_record_lock,
                                            memory_order_acquire))
    ;
}
static void enc_cfg_record_release(void) {
  atomic_flag_clear_explicit(&enc_cfg_record_lock, memory_order_release);
}
static int cfg_read_s32(MppEncCfg cfg, const char *name) {
  CfgEntry *entry = cfg_find(cfg, name);
  return entry ? (int)entry->value : INT32_MIN;
}
static int64_t cfg_read_value(MppEncCfg cfg, const char *name) {
  CfgEntry *entry = cfg_find(cfg, name);
  return entry ? entry->value : INT64_MIN;
}
static void record_enc_cfg(MppEncCfg cfg) {
  enc_cfg_record_acquire();
  if (enc_cfg_record_count < ENC_CFG_RECORD_CAPACITY) {
    MockEncCfgRecord *record = &enc_cfg_records[enc_cfg_record_count++];
    record->bps_target = cfg_read_s32(cfg, "rc:bps_target");
    record->bps_min = cfg_read_s32(cfg, "rc:bps_min");
    record->bps_max = cfg_read_s32(cfg, "rc:bps_max");
    record->super_mode = cfg_read_s32(cfg, "rc:super_mode");
    record->super_i_thd = cfg_read_value(cfg, "rc:super_i_thd");
    record->super_p_thd = cfg_read_value(cfg, "rc:super_p_thd");
  } else {
    enc_cfg_record_dropped++;
  }
  enc_cfg_record_release();
}
static MPP_RET port_ok(MppCtx c, MppPortType t, MppPollType p) {
  (void)c;
  (void)p;
  if (atomic_load(&dec_enabled) && t == MPP_PORT_INPUT) {
    atomic_fetch_add(&jpeg_input_poll_calls, 1);
    unsigned remaining = atomic_load(&jpeg_input_timeouts_remaining);
    while (remaining && !atomic_compare_exchange_weak(
                            &jpeg_input_timeouts_remaining, &remaining,
                            remaining - 1))
      ;
    if (remaining) {
      atomic_store(&jpeg_last_input_poll_timed_out, 1);
      return MPP_ERR_TIMEOUT;
    }
    atomic_store(&jpeg_last_input_poll_timed_out, 0);
  }
  return MPP_OK;
}
static MPP_RET deq_ok(MppCtx c, MppPortType t, MppTask *p) {
  (void)c;
  if (!p)
    return MPP_NOK;
  *p = NULL;
  if (atomic_load(&dec_enabled) && t == MPP_PORT_INPUT) {
    if (atomic_exchange(&jpeg_last_input_poll_timed_out, 0))
      return MPP_OK;
    memset(&jpeg_task, 0, sizeof(jpeg_task));
    *p = (MppTask)&jpeg_task;
  }
  return MPP_OK;
}
static MPP_RET enq_ok(MppCtx c, MppPortType t, MppTask p) {
  (void)c;
  (void)t;
  (void)p;
  return MPP_OK;
}
static MockFrame *dec_new_frame(RK_S64 pts, RK_U32 info_change) {
  MockFrame *f = calloc(1, sizeof(*f));
  if (!f)
    return NULL;
  f->format = (MppFrameFormat)atomic_load(&dec_frame_format);
  f->width = dec_width;
  f->height = dec_height;
  f->horizontal_stride = dec_width;
  f->vertical_stride = dec_height;
  f->pts = pts;
  f->info_change = info_change;
  if (!info_change && atomic_load(&dec_output_buffers_enabled))
    f->buffer = dec_output_buffer;
  return f;
}
static MPP_RET get_frame_ok(MppCtx c, MppFrame *p) {
  (void)c;
  if (!p)
    return MPP_NOK;
  *p = NULL;
  if (!atomic_load(&dec_enabled))
    return MPP_NOK;

  if (!atomic_load(&dec_info_change_sent)) {
    int unsent = 0;
    if (!atomic_load(&dec_queued))
      return MPP_OK;
    if (atomic_compare_exchange_strong(&dec_info_change_sent, &unsent, 1)) {
      *p = (MppFrame)dec_new_frame(-1, 1);
      return *p ? MPP_OK : MPP_NOK;
    }
  }

  unsigned queued = atomic_load(&dec_queued);
  while (queued &&
         !atomic_compare_exchange_weak(&dec_queued, &queued, queued - 1))
    ;
  if (!queued)
    return MPP_OK;

  /* First data output carries an invalid PTS so the decoder abandons MPP
   * timestamps and matches on the input PTS the test controls. */
  RK_S64 pts = atomic_fetch_add(&dec_outputs, 1)
                   ? (RK_S64)atomic_load(&dec_stale_pts)
                   : -1;
  /* Decoder accounting tests use bufferless outputs by default so the decode
   * loop takes its drop path. Normal-stream tests opt into a shared fd-backed
   * buffer and exercise the real downstream finish path. */
  *p = (MppFrame)dec_new_frame(pts, 0);
  return *p ? MPP_OK : MPP_NOK;
}
static MPP_RET put_packet_ok(MppCtx c, MppPacket p) {
  (void)c;
  MockPacket *packet = (MockPacket *)p;
  if (!atomic_load(&dec_enabled) || !packet)
    return MPP_OK;

  atomic_fetch_add(&dec_put_calls, 1);

  unsigned remaining = atomic_load(&dec_buffer_full_remaining);
  while (remaining && !atomic_compare_exchange_weak(
                           &dec_buffer_full_remaining, &remaining,
                           remaining - 1))
    ;
  if (remaining)
    return MPP_ERR_BUFFER_FULL;

  MPP_RET terminal = (MPP_RET)atomic_load(&dec_put_terminal);
  if (terminal != MPP_OK)
    return terminal;

  packet->sent = 1;
  packet->mpp_owned = packet->has_buffer;

  if (packet->eos)
    atomic_store(&dec_eos_seen, 1);
  else if (!packet->extra_data && !atomic_load(&dec_output_suppressed))
    atomic_fetch_add(&dec_queued, 1);
  return MPP_OK;
}
static MPP_RET control(MppCtx c, MpiCmd cmd, MppParam p) {
  (void)c;
  MPP_RET ret = MPP_OK;
  if (cmd == MPP_ENC_SET_CFG) {
    control_counts[0]++;
    record_enc_cfg((MppEncCfg)p);
    pause_if_armed(&enc_control_pause_armed, &enc_control_pause_entered,
                   &enc_control_pause_release);
  } else if (cmd == MPP_ENC_SET_SEI_CFG)
    control_counts[1]++;
  else if (cmd == MPP_ENC_SET_HEADER_MODE)
    control_counts[2]++;
  else if (cmd == MPP_ENC_SET_REF_CFG) {
    control_counts[3]++;
    atomic_fetch_add(&enc_ref_cfg_calls, 1);
    if (!p)
      atomic_fetch_add(&enc_ref_cfg_resets, 1);
    if (atomic_load(&enc_reject_ref_cfg))
      ret = MPP_NOK;
  }
  FILE *f = fopen(
      getenv("MPP_MOCK_LOG") ? getenv("MPP_MOCK_LOG") : "mpp-mock.log", "a");
  if (f) {
    fprintf(f, "control:%d\n", cmd);
    fclose(f);
  }
  return ret;
}
static MockBuffer *enc_buffer_new(unsigned char payload) {
  MockBuffer *b = calloc(1, sizeof(*b));
  if (!b)
    return NULL;
  b->fd = memfd_create("mppmock-enc", MFD_CLOEXEC);
  if (b->fd < 0 || ftruncate(b->fd, ENC_BUFFER_MAP_SIZE)) {
    if (b->fd >= 0)
      close(b->fd);
    free(b);
    return NULL;
  }
  b->data = mmap(NULL, ENC_BUFFER_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                 b->fd, 0);
  if (b->data == MAP_FAILED) {
    b->data = NULL;
    close(b->fd);
    free(b);
    return NULL;
  }
  ((unsigned char *)b->data)[0] = payload;
  b->map_size = ENC_BUFFER_MAP_SIZE;
  b->enc_owned = 1;
  b->size = 1;
  b->index = -1;
  b->refs = 1;
  atomic_fetch_add(&enc_live_buffers, 1);
  return b;
}
static MPP_RET encode_put(MppCtx c, MppFrame f) {
  (void)c;
  if (atomic_load(&enc_reject_put)) {
    atomic_fetch_add(&enc_put_rejections, 1);
    return MPP_NOK;
  }

  unsigned head = atomic_load(&enc_head);
  unsigned tail = atomic_load(&enc_tail);
  if (tail - head >= ENC_PACKET_CAPACITY)
    return MPP_NOK;

  unsigned slot = tail % ENC_PACKET_CAPACITY;
  MockPacket *packet = &enc_packets[slot];
  MockBuffer *buffer = enc_buffer_new((unsigned char)(tail + 1));
  if (!buffer)
    return MPP_NOK;
  memset(packet, 0, sizeof(*packet));

  packet->input_frame = f;
  packet->buffer = (MppBuffer)buffer;
  packet->length = (tail < ENC_PLAN_CAPACITY && enc_plan_length_armed[tail])
                       ? enc_plan_length[tail]
                       : ENC_PLAN_DEFAULT_LENGTH;
  packet->id = tail + 1;
  packet->has_intra_meta =
      tail < ENC_PLAN_CAPACITY && enc_plan_intra_armed[tail];
  packet->intra = packet->has_intra_meta ? enc_plan_intra[tail] : 0;
  packet->encoder_packet = 1;
  packet->alive = 1;

  unsigned geometry_index = atomic_load(&enc_geometry_record_count);
  if (geometry_index < ENC_PLAN_CAPACITY) {
    MockFrame *frame = (MockFrame *)f;
    enc_geometry_records[geometry_index].width = frame->width;
    enc_geometry_records[geometry_index].height = frame->height;
    atomic_store(&enc_geometry_record_count, geometry_index + 1);
  }

  atomic_store(&enc_tail, tail + 1);
  atomic_fetch_add(&enc_queued_packets, 1);
  atomic_fetch_add(&enc_live_packets, 1);
  return MPP_OK;
}
static MPP_RET encode_get(MppCtx c, MppPacket *p) {
  (void)c;
  if (!p)
    return MPP_NOK;

  unsigned gap_remaining = atomic_load(&enc_gap_empty_polls_remaining);
  while (gap_remaining && !atomic_compare_exchange_weak(
                              &enc_gap_empty_polls_remaining, &gap_remaining,
                              gap_remaining - 1))
    ;
  if (gap_remaining) {
    atomic_fetch_add(&enc_gap_empty_polls, 1);
    *p = NULL;
    return MPP_OK;
  }

  if (atomic_load(&enc_test_armed) && !atomic_load(&enc_reset_seen) &&
      atomic_load(&enc_dequeued_packets) >= atomic_load(&enc_release_limit)) {
    *p = NULL;
    return MPP_OK;
  }

  unsigned head = atomic_load(&enc_head);
  unsigned tail = atomic_load(&enc_tail);
  if (head == tail) {
    unsigned generation = atomic_load(&enc_reset_generation);
    if (generation < ENC_RESET_GENERATIONS)
      atomic_fetch_add(&enc_empty_polls_by_generation[generation], 1);
    *p = NULL;
    return MPP_OK;
  }

  MockPacket *packet = &enc_packets[head % ENC_PACKET_CAPACITY];
  unsigned long long bit = 1ULL << (packet->id - 1);
  unsigned long long prior = atomic_fetch_or(&enc_dequeue_mask, bit);
  if (prior & bit)
    atomic_fetch_add(&enc_duplicate_dequeues, 1);

  atomic_store(&enc_head, head + 1);
  atomic_fetch_add(&enc_dequeued_packets, 1);
  atomic_store(&enc_gap_empty_polls_remaining,
               atomic_load(&enc_gap_empty_polls_per_packet));
  *p = (MppPacket)packet;
  return MPP_OK;
}
static MPP_RET mock_reset(MppCtx c) {
  (void)c;
  atomic_fetch_add(&enc_reset_generation, 1);
  atomic_store(&enc_reset_seen, 1);
  atomic_store(&enc_release_limit, UINT32_MAX);
  return MPP_OK;
}
MPP_RET mpp_buffer_group_get(MppBufferGroup *group, MppBufferType type,
                             MppBufferMode mode, const char *tag,
                             const char *caller) {
  if (mode == MPP_BUFFER_INTERNAL)
    atomic_fetch_or(&internal_group_types, (unsigned)type);
  (void)tag;
  (void)caller;
  *group = calloc(1, 8);
  return *group ? MPP_OK : MPP_NOK;
}
MPP_RET mpp_buffer_group_put(MppBufferGroup group) {
  free(group);
  return MPP_OK;
}
MPP_RET mpp_buffer_group_clear(MppBufferGroup group) {
  (void)group;
  return MPP_OK;
}
/*
 * Reopen a memfd write-only through /proc so the handle MPP publishes cannot be
 * mmap()ed at all: MAP_SHARED refuses both PROT_WRITE and PROT_READ on an
 * O_WRONLY description. That is how a CPU-inaccessible DMA buffer presents to
 * the plugin.
 *
 * Read-only would NOT do. gst_buffer_map() routes through
 * gst_memory_make_mapped(), which silently falls back to copying the memory
 * when the direct map fails, and that copy only needs a READ map -- so a
 * read-only fd maps for writing just fine at the buffer level.
 */
static int reopen_unmappable(int fd) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  return open(path, O_WRONLY | O_CLOEXEC);
}

MPP_RET mpp_buffer_get_with_tag(MppBufferGroup group, MppBuffer *buffer,
                                size_t size, const char *tag,
                                const char *caller) {
  (void)group;
  (void)tag;
  (void)caller;
  if (!buffer)
    return MPP_NOK;
  MockBuffer *b = calloc(1, sizeof(*b));
  if (!b)
    return MPP_NOK;
  b->fd = memfd_create("mppmock", MFD_CLOEXEC);
  if (b->fd < 0 || ftruncate(b->fd, (off_t)size)) {
    if (b->fd >= 0)
      close(b->fd);
    free(b);
    return MPP_NOK;
  }
  if (atomic_exchange(&buffer_unmappable_armed, 0)) {
    int blind_fd = reopen_unmappable(b->fd);
    if (blind_fd < 0) {
      close(b->fd);
      free(b);
      return MPP_NOK;
    }
    close(b->fd);
    b->fd = blind_fd;
    atomic_fetch_add(&buffer_unmappable_handed_out, 1);
  }
  b->size = size;
  b->data = calloc(1, size ? size : 1);
  if (!b->data) {
    close(b->fd);
    free(b);
    return MPP_NOK;
  }
  b->index = -1;
  b->refs = 1;
  *buffer = (MppBuffer)b;
  return MPP_OK;
}
MPP_RET mpp_buffer_import_with_tag(MppBufferGroup group, MppBufferInfo *info,
                                   MppBuffer *buffer, const char *tag,
                                   const char *caller) {
  (void)tag;
  (void)caller;
  if (!info || !buffer)
    return MPP_NOK;
  *buffer = NULL;
  atomic_fetch_add(&buffer_import_calls, 1);
  if (atomic_exchange(&buffer_import_failure_armed, 0))
    return MPP_NOK;
  return mpp_buffer_get_with_tag(group, buffer, info->size, tag, caller);
}
MPP_RET mpp_buffer_put_with_caller(MppBuffer buffer, const char *caller) {
  (void)caller;
  MockBuffer *b = (MockBuffer *)buffer;
  if (!b)
    return MPP_NOK;
  if (--b->refs == 0) {
    close(b->fd);
    if (b->map_size)
      munmap(b->data, b->map_size);
    else
      free(b->data);
    if (b->enc_owned)
      atomic_fetch_sub(&enc_live_buffers, 1);
    free(b);
  }
  return MPP_OK;
}
MPP_RET mpp_buffer_inc_ref_with_caller(MppBuffer buffer, const char *caller) {
  (void)caller;
  MockBuffer *b = (MockBuffer *)buffer;
  if (!b)
    return MPP_NOK;
  b->refs++;
  atomic_fetch_add(&buffer_inc_ref_calls, 1);
  return MPP_OK;
}
int mpp_buffer_get_fd_with_caller(MppBuffer buffer, const char *caller) {
  (void)caller;
  return buffer ? ((MockBuffer *)buffer)->fd : -1;
}
size_t mpp_buffer_get_size_with_caller(MppBuffer buffer, const char *caller) {
  (void)caller;
  return buffer ? ((MockBuffer *)buffer)->size : 0;
}
void *mpp_buffer_get_ptr_with_caller(MppBuffer buffer, const char *caller) {
  (void)caller;
  return buffer ? ((MockBuffer *)buffer)->data : NULL;
}
int mpp_buffer_get_index_with_caller(MppBuffer buffer, const char *caller) {
  (void)caller;
  return buffer ? ((MockBuffer *)buffer)->index : -1;
}
MPP_RET mpp_buffer_set_index_with_caller(MppBuffer buffer, int index,
                                         const char *caller) {
  (void)caller;
  if (!buffer)
    return MPP_NOK;
  ((MockBuffer *)buffer)->index = index;
  return MPP_OK;
}

MPP_RET mpp_frame_init(MppFrame *frame) {
  if (!frame)
    return MPP_NOK;
  *frame = calloc(1, sizeof(MockFrame));
  return *frame ? MPP_OK : MPP_NOK;
}
MPP_RET mpp_frame_deinit(MppFrame *frame) {
  if (!frame || !*frame)
    return MPP_NOK;
  free(*frame);
  *frame = NULL;
  atomic_fetch_add(&dec_frame_deinits, 1);
  return MPP_OK;
}
MppFrameFormat mpp_frame_get_fmt(MppFrame frame) {
  return ((MockFrame *)frame)->format;
}
void mpp_frame_set_fmt(MppFrame frame, MppFrameFormat format) {
  ((MockFrame *)frame)->format = format;
}
RK_U32 mpp_frame_get_width(const MppFrame frame) {
  return ((MockFrame *)frame)->width;
}
void mpp_frame_set_width(MppFrame frame, RK_U32 width) {
  ((MockFrame *)frame)->width = width;
}
RK_U32 mpp_frame_get_height(const MppFrame frame) {
  return ((MockFrame *)frame)->height;
}
void mpp_frame_set_height(MppFrame frame, RK_U32 height) {
  ((MockFrame *)frame)->height = height;
}
RK_U32 mpp_frame_get_hor_stride(const MppFrame frame) {
  return ((MockFrame *)frame)->horizontal_stride;
}
void mpp_frame_set_hor_stride(MppFrame frame, RK_U32 stride) {
  ((MockFrame *)frame)->horizontal_stride = stride;
}
RK_U32 mpp_frame_get_ver_stride(const MppFrame frame) {
  return ((MockFrame *)frame)->vertical_stride;
}
void mpp_frame_set_ver_stride(MppFrame frame, RK_U32 stride) {
  ((MockFrame *)frame)->vertical_stride = stride;
}
MppBuffer mpp_frame_get_buffer(const MppFrame frame) {
  return ((MockFrame *)frame)->buffer;
}
void mpp_frame_set_buffer(MppFrame frame, MppBuffer buffer) {
  ((MockFrame *)frame)->buffer = buffer;
  atomic_fetch_add(&frame_set_buffer_count, 1);
}

RK_S64 mpp_frame_get_pts(const MppFrame frame) {
  return ((MockFrame *)frame)->pts;
}
void mpp_frame_set_pts(MppFrame frame, RK_S64 pts) {
  ((MockFrame *)frame)->pts = pts;
}
RK_U32 mpp_frame_get_info_change(const MppFrame frame) {
  return ((MockFrame *)frame)->info_change;
}
RK_U32 mpp_frame_get_eos(const MppFrame frame) {
  (void)frame;
  return 0;
}
RK_U32 mpp_frame_get_discard(const MppFrame frame) {
  (void)frame;
  return 0;
}
RK_U32 mpp_frame_get_errinfo(const MppFrame frame) {
  (void)frame;
  return 0;
}
RK_U32 mpp_frame_get_mode(const MppFrame frame) {
  (void)frame;
  return 0;
}
RK_U32 mpp_frame_get_offset_x(const MppFrame frame) {
  (void)frame;
  return 0;
}
RK_U32 mpp_frame_get_offset_y(const MppFrame frame) {
  (void)frame;
  return 0;
}

MppMeta mpp_packet_get_meta(const MppPacket packet) { return (MppMeta)packet; }
size_t mpp_packet_get_length(const MppPacket packet) {
  return packet ? ((MockPacket *)packet)->length : 0;
}
MppBuffer mpp_packet_get_buffer(const MppPacket packet) {
  MockPacket *p = (MockPacket *)packet;
  if (!p)
    return NULL;
  if (p->encoder_packet)
    return p->buffer;
  atomic_fetch_add(&dec_packet_buffer_queries, 1);
  if (p->sent)
    atomic_fetch_add(&dec_packet_post_send_accesses, 1);
  return p->has_buffer ? (MppBuffer)p : NULL;
}
MPP_RET mpp_packet_init(MppPacket *packet, void *data, size_t size) {
  (void)data;
  (void)size;
  if (!packet)
    return MPP_NOK;
  MockPacket *p = calloc(1, sizeof(*p));
  if (!p)
    return MPP_NOK;
  p->heap = 1;
  p->alive = 1;
  if (atomic_load(&dec_enabled))
    p->has_buffer = atomic_exchange(&dec_next_packet_has_buffer, 0);
  p->next = packet_allocs;
  packet_allocs = p;
  atomic_fetch_add(&dec_arena_packets, 1);
  *packet = (MppPacket)p;
  return MPP_OK;
}
MPP_RET mpp_packet_init_with_buffer(MppPacket *packet, MppBuffer buffer) {
  MPP_RET ret = mpp_packet_init(packet, NULL, 0);
  if (!ret && *packet)
    ((MockPacket *)*packet)->has_buffer = (buffer != NULL);
  return ret;
}
void mpp_packet_set_pts(MppPacket packet, RK_S64 pts) {
  (void)packet;
  (void)pts;
}
void mpp_packet_set_size(MppPacket packet, size_t size) {
  (void)packet;
  (void)size;
}
void mpp_packet_set_length(MppPacket packet, size_t size) {
  (void)packet;
  (void)size;
}
MPP_RET mpp_packet_set_eos(MppPacket packet) {
  if (!packet)
    return MPP_NOK;
  ((MockPacket *)packet)->eos = 1;
  return MPP_OK;
}
MPP_RET mpp_packet_set_extra_data(MppPacket packet) {
  if (!packet)
    return MPP_NOK;
  ((MockPacket *)packet)->extra_data = 1;
  return MPP_OK;
}
MPP_RET mpp_packet_deinit(MppPacket *packet) {
  if (!packet)
    return MPP_NOK;
  MockPacket *p = (MockPacket *)*packet;
  if (p && p->encoder_packet) {
    if (!p->alive) {
      atomic_fetch_add(&enc_packet_double_deinits, 1);
    } else {
      p->alive = 0;
      atomic_fetch_add(&enc_packet_deinits, 1);
      atomic_fetch_sub(&enc_live_packets, 1);
      /* Real MPP returns the packet's buffer to its group here; any reference
       * the plugin took for a zero-copy push has to outlive that. */
      if (p->buffer) {
        mpp_buffer_put_with_caller(p->buffer, "mpp_packet_deinit");
        p->buffer = NULL;
      }
    }
    *packet = NULL;
    return MPP_OK;
  }
  if (p && p->heap) {
    if (!p->alive)
      atomic_fetch_add(&dec_double_deinits, 1);
    else if (p->mpp_owned)
      atomic_fetch_add(&dec_wrong_owner_deinits, 1);
    else
      atomic_fetch_add(&dec_packet_deinits, 1);
    p->alive = 0;
  }
  *packet = NULL;
  return MPP_OK;
}
/* Pointer-range identification: mpp_frame_get_meta() and mpp_packet_get_meta()
 * both hand back the object itself, so the key must not be read out of a
 * MockFrame as though it were a MockPacket. */
static MockPacket *enc_packet_from_meta(MppMeta meta) {
  MockPacket *p = (MockPacket *)meta;
  if (p >= enc_packets && p < enc_packets + ENC_PACKET_CAPACITY)
    return p;
  return NULL;
}
MPP_RET mpp_meta_get_s32(MppMeta meta, MppMetaKey key, RK_S32 *val) {
  MockPacket *p = enc_packet_from_meta(meta);
  if (!p || !val || key != KEY_OUTPUT_INTRA || !p->has_intra_meta)
    return MPP_NOK;
  *val = (RK_S32)p->intra;
  return MPP_OK;
}
MPP_RET mpp_meta_get_frame(MppMeta meta, MppMetaKey key, MppFrame *frame) {
  (void)key;
  if (!meta || !frame)
    return MPP_NOK;
  *frame = ((MockPacket *)meta)->input_frame;
  return MPP_OK;
}
MppMeta mpp_frame_get_meta(const MppFrame frame) { return (MppMeta)frame; }
MPP_RET mpp_meta_set_packet(MppMeta meta, MppMetaKey key, MppPacket packet) {
  (void)key;
  if (!meta)
    return MPP_NOK;
  ((MockFrame *)meta)->packet = packet;
  return MPP_OK;
}
MPP_RET mpp_task_meta_set_packet(MppTask task, MppMetaKey key,
                                 MppPacket packet) {
  (void)key;
  if (!task)
    return MPP_NOK;
  ((MockTask *)task)->packet = packet;
  return MPP_OK;
}
MPP_RET mpp_task_meta_set_frame(MppTask task, MppMetaKey key, MppFrame frame) {
  (void)key;
  if (!task)
    return MPP_NOK;
  ((MockTask *)task)->frame = frame;
  return MPP_OK;
}
MPP_RET mpp_task_meta_get_frame(MppTask task, MppMetaKey key, MppFrame *frame) {
  (void)key;
  if (!task || !frame)
    return MPP_NOK;
  *frame = ((MockTask *)task)->frame;
  return MPP_OK;
}

MPP_RET mpp_create(MppCtx *ctx, MppApi **mpi) {
  static int x;
  *ctx = (MppCtx)&x;
  memset(&api, 0, sizeof(api));
  api.size = sizeof(api);
  api.encode_put_frame = encode_put;
  api.encode_get_packet = encode_get;
  api.control = control;
  api.reset = mock_reset;
  api.poll = port_ok;
  api.dequeue = deq_ok;
  api.enqueue = enq_ok;
  api.decode_put_packet = put_packet_ok;
  api.decode_get_frame = get_frame_ok;
  *mpi = &api;
  FILE *f = fopen(
      getenv("MPP_MOCK_LOG") ? getenv("MPP_MOCK_LOG") : "mpp-mock.log", "a");
  if (f) {
    fputs("mpp_create\n", f);
    fclose(f);
  }
  return MPP_OK;
}
MPP_RET mpp_init(MppCtx c, MppCtxType t, MppCodingType coding) {
  (void)c;
  (void)t;
  FILE *f = fopen(
      getenv("MPP_MOCK_LOG") ? getenv("MPP_MOCK_LOG") : "mpp-mock.log", "a");
  if (f) {
    fprintf(f, "mpp_init:%d\n", coding);
    fclose(f);
  }
  return MPP_OK;
}
MPP_RET mpp_destroy(MppCtx c) {
  (void)c;
  return MPP_OK;
}
void mpp_set_log_level(int l) { (void)l; }

MPP_RET mpp_enc_cfg_init(MppEncCfg *c) {
  *c = calloc(1, sizeof(MockCfg));
  last_cfg = *c;
  return *c ? MPP_OK : MPP_NOK;
}
MPP_RET mpp_enc_cfg_init_k(MppEncCfg *c) { return mpp_enc_cfg_init(c); }
MPP_RET mpp_enc_cfg_create(MppEncCfg *c, RK_U32 mode) {
  (void)mode;
  return mpp_enc_cfg_init(c);
}
MPP_RET mpp_enc_cfg_deinit(MppEncCfg c) {
  if (c == last_cfg)
    last_cfg = NULL;
  free(c);
  return MPP_OK;
}
#define SET(n, t, k)                                                           \
  MPP_RET n(MppEncCfg c, const char *n, RK_##t v) {                            \
    return cfg_set(c, n, (int64_t)v, NULL, k);                                 \
  }
SET(mpp_enc_cfg_set_s8, S8, 1)
SET(mpp_enc_cfg_set_u8, U8, 2)
SET(mpp_enc_cfg_set_s16, S16, 1) SET(mpp_enc_cfg_set_u16, U16, 2)
    SET(mpp_enc_cfg_set_s32, S32, 1) SET(mpp_enc_cfg_set_u32, U32, 2)
        SET(mpp_enc_cfg_set_s64, S64, 1)
            SET(mpp_enc_cfg_set_u64, U64, 2) MPP_RET
    mpp_enc_cfg_set_ptr(MppEncCfg c, const char *n, void *v) {
  return cfg_set(c, n, 0, v, 3);
}
MPP_RET mpp_enc_cfg_set_st(MppEncCfg c, const char *n, void *v) {
  return mpp_enc_cfg_set_ptr(c, n, v);
}
#define GET(n, t)                                                              \
  MPP_RET n(MppEncCfg c, const char *n, RK_##t *v) {                           \
    CfgEntry *e = cfg_find(c, n);                                              \
    if (!e || !v)                                                              \
      return MPP_NOK;                                                          \
    *v = (RK_##t)e->value;                                                     \
    return MPP_OK;                                                             \
  }
GET(mpp_enc_cfg_get_s8, S8)
GET(mpp_enc_cfg_get_u8, U8)
GET(mpp_enc_cfg_get_s16, S16) GET(mpp_enc_cfg_get_u16, U16)
    GET(mpp_enc_cfg_get_s32, S32) GET(mpp_enc_cfg_get_u32, U32)
        GET(mpp_enc_cfg_get_s64, S64) GET(mpp_enc_cfg_get_u64, U64) MPP_RET
    mpp_enc_cfg_get_ptr(MppEncCfg c, const char *n, void **v) {
  CfgEntry *e = cfg_find(c, n);
  if (!e || !v)
    return MPP_NOK;
  *v = e->ptr;
  return MPP_OK;
}
MPP_RET mpp_enc_cfg_get_st(MppEncCfg c, const char *n, void *v) {
  (void)v;
  return cfg_find(c, n) ? MPP_OK : MPP_NOK;
}
int mpp_mock_cfg_get_s32(MppEncCfg c, const char *n) {
  CfgEntry *e = cfg_find(c, n);
  return e ? (int)e->value : INT32_MIN;
}
int mpp_mock_last_cfg_s32(const char *n) {
  return last_cfg ? mpp_mock_cfg_get_s32(last_cfg, n) : INT32_MIN;
}

unsigned mpp_mock_enc_cfg_record_count(void) {
  enc_cfg_record_acquire();
  unsigned count = enc_cfg_record_count;
  enc_cfg_record_release();
  return count;
}
unsigned mpp_mock_enc_cfg_record_dropped(void) {
  enc_cfg_record_acquire();
  unsigned dropped = enc_cfg_record_dropped;
  enc_cfg_record_release();
  return dropped;
}
int mpp_mock_enc_cfg_record_s32(unsigned index, const char *name) {
  int value = INT32_MIN;
  enc_cfg_record_acquire();
  if (index < enc_cfg_record_count) {
    MockEncCfgRecord *record = &enc_cfg_records[index];
    if (!strcmp(name, "rc:bps_target"))
      value = record->bps_target;
    else if (!strcmp(name, "rc:bps_min"))
      value = record->bps_min;
    else if (!strcmp(name, "rc:bps_max"))
      value = record->bps_max;
    else if (!strcmp(name, "rc:super_mode"))
      value = record->super_mode;
  }
  enc_cfg_record_release();
  return value;
}
/* Same snapshots, full width: a super-frame threshold of 0xFFFFFFFF does not
 * survive the s32 accessor above. */
int64_t mpp_mock_enc_cfg_record_value(unsigned index, const char *name) {
  int64_t value = INT64_MIN;
  enc_cfg_record_acquire();
  if (index < enc_cfg_record_count) {
    MockEncCfgRecord *record = &enc_cfg_records[index];
    if (!strcmp(name, "rc:super_i_thd"))
      value = record->super_i_thd;
    else if (!strcmp(name, "rc:super_p_thd"))
      value = record->super_p_thd;
  }
  enc_cfg_record_release();
  return value;
}
void mpp_mock_enc_cfg_reject_key(const char *key) {
  if (enc_cfg_rejected_count >= ENC_CFG_REJECT_CAPACITY)
    return;
  snprintf(enc_cfg_rejected_keys[enc_cfg_rejected_count++],
           sizeof(enc_cfg_rejected_keys[0]), "%s", key);
}
void mpp_mock_enc_pause_next_bps_target(void) {
  atomic_store(&enc_bps_pause_entered, 0);
  atomic_store(&enc_bps_pause_release, 0);
  atomic_store(&enc_bps_pause_armed, 1);
}
unsigned mpp_mock_enc_bps_target_paused(void) {
  return (unsigned)atomic_load(&enc_bps_pause_entered);
}
void mpp_mock_enc_resume_bps_target(void) {
  atomic_store(&enc_bps_pause_release, 1);
}
void mpp_mock_enc_pause_next_set_cfg(void) {
  atomic_store(&enc_control_pause_entered, 0);
  atomic_store(&enc_control_pause_release, 0);
  atomic_store(&enc_control_pause_armed, 1);
}
unsigned mpp_mock_enc_set_cfg_paused(void) {
  return (unsigned)atomic_load(&enc_control_pause_entered);
}
void mpp_mock_enc_resume_set_cfg(void) {
  atomic_store(&enc_control_pause_release, 1);
}
unsigned mpp_mock_control_count(int cmd) {
  return cmd == MPP_ENC_SET_CFG           ? (unsigned)control_counts[0]
         : cmd == MPP_ENC_SET_SEI_CFG     ? (unsigned)control_counts[1]
         : cmd == MPP_ENC_SET_HEADER_MODE ? (unsigned)control_counts[2]
         : cmd == MPP_ENC_SET_REF_CFG     ? (unsigned)control_counts[3]
                                          : 0;
}
void mpp_mock_enc_reject_ref_cfg(int reject) {
  atomic_store(&enc_reject_ref_cfg, !!reject);
}
unsigned mpp_mock_enc_ref_cfg_calls(void) {
  return atomic_load(&enc_ref_cfg_calls);
}
unsigned mpp_mock_enc_ref_cfg_resets(void) {
  return atomic_load(&enc_ref_cfg_resets);
}
unsigned mpp_mock_frame_set_buffer_count(void) {
  return atomic_load(&frame_set_buffer_count);
}
void mpp_mock_enc_arm_reset_drain(void) {
  atomic_store(&enc_test_armed, 1);
  atomic_store(&enc_reset_seen, 0);
  atomic_store(&enc_release_limit, 0);
}
void mpp_mock_enc_release_packets(unsigned count) {
  atomic_store(&enc_release_limit, count);
}
void mpp_mock_enc_reject_input(void) { atomic_store(&enc_reject_put, 1); }
unsigned mpp_mock_enc_put_rejections(void) {
  return atomic_load(&enc_put_rejections);
}
void mpp_mock_enc_accept_input(void) { atomic_store(&enc_reject_put, 0); }
unsigned mpp_mock_enc_geometry_record_count(void) {
  return atomic_load(&enc_geometry_record_count);
}
unsigned mpp_mock_enc_geometry_width(unsigned index) {
  return index < atomic_load(&enc_geometry_record_count)
             ? enc_geometry_records[index].width
             : 0;
}
unsigned mpp_mock_enc_geometry_height(unsigned index) {
  return index < atomic_load(&enc_geometry_record_count)
             ? enc_geometry_records[index].height
             : 0;
}
void mpp_mock_enc_set_empty_polls_between_packets(unsigned count) {
  atomic_store(&enc_gap_empty_polls_per_packet, count);
}
unsigned mpp_mock_enc_gap_empty_polls(void) {
  return atomic_load(&enc_gap_empty_polls);
}
/* An MPP rate-controller drop is a zero-length packet that still carries its
 * MppBuffer, so the backing buffer is deliberately left intact here. */
void mpp_mock_enc_set_packet_length(unsigned ordinal, size_t length) {
  if (ordinal >= ENC_PLAN_CAPACITY)
    return;
  enc_plan_length[ordinal] = length;
  enc_plan_length_armed[ordinal] = 1;
}
/* An unarmed ordinal omits KEY_OUTPUT_INTRA entirely. The pinned MPP always
 * writes it, so this models a malformed or future producer rather than MPP's
 * own behaviour -- it exists so an absent key cannot silently read as a zero. */
void mpp_mock_enc_set_packet_intra(unsigned ordinal, int intra) {
  if (ordinal >= ENC_PLAN_CAPACITY)
    return;
  enc_plan_intra[ordinal] = intra;
  enc_plan_intra_armed[ordinal] = 1;
}
unsigned mpp_mock_enc_queued_packets(void) {
  return atomic_load(&enc_queued_packets);
}
unsigned mpp_mock_enc_dequeued_packets(void) {
  return atomic_load(&enc_dequeued_packets);
}
unsigned mpp_mock_enc_queue_depth(void) {
  return atomic_load(&enc_tail) - atomic_load(&enc_head);
}
unsigned mpp_mock_enc_duplicate_dequeues(void) {
  return atomic_load(&enc_duplicate_dequeues);
}
unsigned mpp_mock_enc_reset_generation(void) {
  return atomic_load(&enc_reset_generation);
}
unsigned mpp_mock_enc_empty_polls_for_generation(unsigned generation) {
  if (generation >= ENC_RESET_GENERATIONS)
    return 0;
  return atomic_load(&enc_empty_polls_by_generation[generation]);
}
unsigned mpp_mock_enc_packet_deinits(void) {
  return atomic_load(&enc_packet_deinits);
}
unsigned mpp_mock_enc_packet_double_deinits(void) {
  return atomic_load(&enc_packet_double_deinits);
}
unsigned mpp_mock_enc_live_packets(void) {
  return atomic_load(&enc_live_packets);
}
unsigned mpp_mock_enc_live_buffers(void) {
  return atomic_load(&enc_live_buffers);
}
void mpp_mock_arm_unmappable_buffer(void) {
  atomic_store(&buffer_unmappable_armed, 1);
}
unsigned mpp_mock_unmappable_buffers(void) {
  return atomic_load(&buffer_unmappable_handed_out);
}
void mpp_mock_fail_next_buffer_import(void) {
  atomic_store(&buffer_import_failure_armed, 1);
}
unsigned mpp_mock_buffer_import_calls(void) {
  return atomic_load(&buffer_import_calls);
}
unsigned mpp_mock_buffer_inc_ref_calls(void) {
  return atomic_load(&buffer_inc_ref_calls);
}
unsigned mpp_mock_buffer_ref_count(MppBuffer buffer) {
  return buffer ? ((MockBuffer *)buffer)->refs : 0;
}
/* Safe only once no element can still own a packet, i.e. after the harness has
 * been torn down. Callers reclaim explicitly; arming reclaims again so a test
 * that forgets cannot carry an arena into the next one. */
void mpp_mock_dec_release_packets(void) {
  atomic_store(&dec_output_buffers_enabled, 0);
  if (dec_output_buffer) {
    mpp_buffer_put_with_caller(dec_output_buffer, __func__);
    dec_output_buffer = NULL;
  }
  while (packet_allocs) {
    MockPacket *packet = packet_allocs;
    packet_allocs = packet->next;
    free(packet);
    atomic_fetch_sub(&dec_arena_packets, 1);
  }
}
/* Lets the seam assert the arena is empty without a leak checker, which is
 * what CI can actually run: LeakSanitizer cannot start under qemu-user. */
unsigned mpp_mock_dec_live_packets(void) {
  return atomic_load(&dec_arena_packets);
}
void mpp_mock_dec_arm(unsigned width, unsigned height) {
  mpp_mock_dec_release_packets();
  dec_width = (RK_U32)width;
  dec_height = (RK_U32)height;
  atomic_store(&dec_frame_format, MPP_FMT_YUV420SP);
  atomic_store(&dec_info_change_sent, 0);
  atomic_store(&dec_queued, 0);
  atomic_store(&dec_outputs, 0);
  atomic_store(&dec_eos_seen, 0);
  atomic_store(&dec_put_calls, 0);
  atomic_store(&dec_buffer_full_remaining, 0);
  atomic_store(&dec_put_terminal, MPP_OK);
  atomic_store(&dec_next_packet_has_buffer, 0);
  atomic_store(&dec_packet_buffer_queries, 0);
  atomic_store(&dec_packet_post_send_accesses, 0);
  atomic_store(&dec_packet_deinits, 0);
  atomic_store(&dec_wrong_owner_deinits, 0);
  atomic_store(&dec_double_deinits, 0);
  atomic_store(&dec_frame_deinits, 0);
  atomic_store(&dec_output_suppressed, 0);
  atomic_store(&dec_output_buffers_enabled, 0);
  atomic_store(&dec_stale_pts, 1);
  atomic_store(&internal_group_types, 0);
  atomic_store(&jpeg_input_timeouts_remaining, 0);
  atomic_store(&jpeg_input_poll_calls, 0);
  atomic_store(&jpeg_last_input_poll_timed_out, 0);
  atomic_store(&dec_enabled, 1);
}
/* Stops the mock producing so a harness teardown can drain, but deliberately
 * keeps the packet arena alive for the element that is still stopping. */
void mpp_mock_dec_disarm(void) { atomic_store(&dec_enabled, 0); }
unsigned mpp_mock_dec_queued(void) { return atomic_load(&dec_queued); }
unsigned mpp_mock_dec_outputs(void) { return atomic_load(&dec_outputs); }
void mpp_mock_dec_set_output_suppressed(int suppressed) {
  atomic_store(&dec_output_suppressed, !!suppressed);
}
void mpp_mock_dec_set_output_pts(RK_S64 pts) {
  atomic_store(&dec_stale_pts, pts);
}
void mpp_mock_dec_set_output_buffers(int enabled) {
  if (enabled && !dec_output_buffer) {
    size_t size = (size_t)dec_width * dec_height * 3 / 2;
    size = (size + 4095) & ~(size_t)4095;
    if (mpp_buffer_get_with_tag(NULL, &dec_output_buffer, size, NULL,
                                __func__) != MPP_OK)
      abort();
  }
  atomic_store(&dec_output_buffers_enabled, !!enabled);
}
void mpp_mock_dec_set_frame_format(MppFrameFormat format) {
  atomic_store(&dec_frame_format, format);
}
void mpp_mock_dec_set_put_result(unsigned buffer_full_count,
                                 MPP_RET terminal) {
  atomic_store(&dec_buffer_full_remaining, buffer_full_count);
  atomic_store(&dec_put_terminal, terminal);
}
unsigned mpp_mock_dec_put_calls(void) { return atomic_load(&dec_put_calls); }
void mpp_mock_dec_set_next_packet_has_buffer(int has_buffer) {
  atomic_store(&dec_next_packet_has_buffer, !!has_buffer);
}
unsigned mpp_mock_dec_packet_buffer_queries(void) {
  return atomic_load(&dec_packet_buffer_queries);
}
unsigned mpp_mock_dec_packet_post_send_accesses(void) {
  return atomic_load(&dec_packet_post_send_accesses);
}
unsigned mpp_mock_dec_packet_deinits(void) {
  return atomic_load(&dec_packet_deinits);
}
unsigned mpp_mock_dec_wrong_owner_deinits(void) {
  return atomic_load(&dec_wrong_owner_deinits);
}
unsigned mpp_mock_dec_double_deinits(void) {
  return atomic_load(&dec_double_deinits);
}
unsigned mpp_mock_dec_frame_deinits(void) {
  return atomic_load(&dec_frame_deinits);
}
unsigned mpp_mock_internal_group_types(void) {
  return atomic_load(&internal_group_types);
}
void mpp_mock_jpeg_set_input_timeouts(unsigned count) {
  atomic_store(&jpeg_input_timeouts_remaining, count);
  atomic_store(&jpeg_input_poll_calls, 0);
  atomic_store(&jpeg_last_input_poll_timed_out, 0);
}
unsigned mpp_mock_jpeg_input_poll_calls(void) {
  return atomic_load(&jpeg_input_poll_calls);
}
void mpp_mock_reset(void) {
  memset(control_counts, 0, sizeof(control_counts));
  last_cfg = NULL;
  enc_cfg_record_acquire();
  memset(enc_cfg_records, 0, sizeof(enc_cfg_records));
  enc_cfg_record_count = 0;
  enc_cfg_record_dropped = 0;
  enc_cfg_record_release();
  memset(enc_cfg_rejected_keys, 0, sizeof(enc_cfg_rejected_keys));
  enc_cfg_rejected_count = 0;
  atomic_store(&enc_bps_pause_armed, 0);
  atomic_store(&enc_bps_pause_entered, 0);
  atomic_store(&enc_bps_pause_release, 1);
  atomic_store(&enc_control_pause_armed, 0);
  atomic_store(&enc_control_pause_entered, 0);
  atomic_store(&enc_control_pause_release, 1);
  atomic_store(&enc_reject_ref_cfg, 0);
  atomic_store(&enc_ref_cfg_calls, 0);
  atomic_store(&enc_ref_cfg_resets, 0);
  atomic_store(&enc_head, 0);
  atomic_store(&enc_tail, 0);
  atomic_store(&frame_set_buffer_count, 0);
  atomic_store(&enc_test_armed, 0);
  atomic_store(&enc_reset_seen, 0);
  atomic_store(&enc_release_limit, UINT32_MAX);
  atomic_store(&enc_queued_packets, 0);
  atomic_store(&enc_dequeued_packets, 0);
  atomic_store(&enc_duplicate_dequeues, 0);
  atomic_store(&enc_dequeue_mask, 0);
  atomic_store(&enc_reset_generation, 0);
  for (unsigned i = 0; i < ENC_RESET_GENERATIONS; i++)
    atomic_store(&enc_empty_polls_by_generation[i], 0);
  atomic_store(&enc_packet_deinits, 0);
  atomic_store(&enc_packet_double_deinits, 0);
  atomic_store(&enc_live_packets, 0);
  atomic_store(&enc_reject_put, 0);
  atomic_store(&enc_put_rejections, 0);
  atomic_store(&enc_geometry_record_count, 0);
  memset(enc_geometry_records, 0, sizeof(enc_geometry_records));
  atomic_store(&enc_gap_empty_polls, 0);
  atomic_store(&enc_gap_empty_polls_remaining, 0);
  atomic_store(&enc_gap_empty_polls_per_packet, 0);
  memset(enc_packets, 0, sizeof(enc_packets));
  atomic_store(&enc_live_buffers, 0);
  atomic_store(&buffer_import_failure_armed, 0);
  atomic_store(&buffer_import_calls, 0);
  atomic_store(&buffer_inc_ref_calls, 0);
  atomic_store(&buffer_unmappable_armed, 0);
  atomic_store(&buffer_unmappable_handed_out, 0);
  memset(enc_plan_length, 0, sizeof(enc_plan_length));
  memset(enc_plan_length_armed, 0, sizeof(enc_plan_length_armed));
  memset(enc_plan_intra, 0, sizeof(enc_plan_intra));
  memset(enc_plan_intra_armed, 0, sizeof(enc_plan_intra_armed));
  mpp_mock_dec_disarm();
  mpp_mock_dec_release_packets();
}

static atomic_int mock_rga_enabled;

int c_RkRgaInit(void) { return 0; }
int c_RkRgaBlit(void *src, void *dst, void *src1) {
  (void)src;
  (void)dst;
  (void)src1;
  return atomic_load(&mock_rga_enabled) ? 0 : -1;
}
void mpp_mock_rga_set_enabled(int enabled) {
  atomic_store(&mock_rga_enabled, !!enabled);
}
