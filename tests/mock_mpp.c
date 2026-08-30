#define _GNU_SOURCE
#include <rockchip/mpp_log.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_venc_cfg.h>
#include <stdatomic.h>
#include <stdint.h>
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
static int control_counts[512];
static MppEncCfg last_cfg;
typedef struct MockPacket {
  MppFrame input_frame;
  int heap;
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
  int index;
  unsigned refs;
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
} MockFrame;
static _Atomic(MppFrame) queued_frame;
static atomic_uint frame_set_buffer_count;
static MockPacket output_packet;

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
static MockPacket *packet_allocs;
static RK_U32 dec_width = 320;
static RK_U32 dec_height = 240;
/* Kept far BELOW every input PTS so the decoder's display-order matcher filters
 * each pending frame out as "future" and settles on no match. Nonzero is
 * load-bearing: gstmppdec.c remaps a zero PTS to GST_CLOCK_TIME_NONE, which
 * matches the oldest frame instead of missing. */
static RK_S64 dec_stale_pts = 1;
static MockCfg *cfg_of(MppEncCfg c) { return (MockCfg *)c; }
static MPP_RET cfg_set(MppEncCfg c, const char *name, int64_t v, void *ptr,
                       int kind) {
  MockCfg *m = cfg_of(c);
  for (unsigned i = 0; i < m->count; i++)
    if (!strcmp(m->entries[i].key, name)) {
      m->entries[i].value = v;
      m->entries[i].ptr = ptr;
      m->entries[i].kind = kind;
      return MPP_OK;
    }
  if (m->count >= 128)
    return MPP_NOK;
  CfgEntry *e = &m->entries[m->count++];
  snprintf(e->key, sizeof(e->key), "%s", name);
  e->value = v;
  e->ptr = ptr;
  e->kind = kind;
  return MPP_OK;
}
static CfgEntry *cfg_find(MppEncCfg c, const char *n) {
  MockCfg *m = cfg_of(c);
  for (unsigned i = 0; i < m->count; i++)
    if (!strcmp(m->entries[i].key, n))
      return &m->entries[i];
  return NULL;
}
static MPP_RET ok(void *c) {
  (void)c;
  return MPP_OK;
}
static MPP_RET port_ok(MppCtx c, MppPortType t, MppPollType p) {
  (void)c;
  (void)t;
  (void)p;
  return MPP_OK;
}
static MPP_RET deq_ok(MppCtx c, MppPortType t, MppTask *p) {
  (void)c;
  (void)t;
  if (p)
    *p = NULL;
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
  f->format = MPP_FMT_YUV420SP;
  f->width = dec_width;
  f->height = dec_height;
  f->horizontal_stride = dec_width;
  f->vertical_stride = dec_height;
  f->pts = pts;
  f->info_change = info_change;
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
  RK_S64 pts = atomic_fetch_add(&dec_outputs, 1) ? dec_stale_pts : -1;
  /* No MppBuffer: the decode loop rejects the frame and takes its drop path,
   * which is what releases the pending frame this output was matched to. */
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
  else if (!packet->extra_data)
    atomic_fetch_add(&dec_queued, 1);
  return MPP_OK;
}
static MPP_RET control(MppCtx c, MpiCmd cmd, MppParam p) {
  (void)c;
  (void)p;
  if (cmd == MPP_ENC_SET_CFG)
    control_counts[0]++;
  else if (cmd == MPP_ENC_SET_SEI_CFG)
    control_counts[1]++;
  else if (cmd == MPP_ENC_SET_HEADER_MODE)
    control_counts[2]++;
  FILE *f = fopen(
      getenv("MPP_MOCK_LOG") ? getenv("MPP_MOCK_LOG") : "mpp-mock.log", "a");
  if (f) {
    fprintf(f, "control:%d\n", cmd);
    fclose(f);
  }
  return MPP_OK;
}
static MPP_RET encode_put(MppCtx c, MppFrame f) {
  (void)c;
  atomic_store(&queued_frame, f);
  return MPP_OK;
}
static MPP_RET encode_get(MppCtx c, MppPacket *p) {
  (void)c;
  MppFrame f = atomic_exchange(&queued_frame, NULL);
  if (!p)
    return MPP_NOK;
  if (!f) {
    *p = NULL;
    return MPP_OK;
  }
  output_packet.input_frame = f;
  *p = (MppPacket)&output_packet;
  return MPP_OK;
}
MPP_RET mpp_buffer_group_get(MppBufferGroup *group, MppBufferType type,
                             MppBufferMode mode, const char *tag,
                             const char *caller) {
  (void)type;
  (void)mode;
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
  b->size = size;
  b->index = -1;
  b->refs = 1;
  *buffer = (MppBuffer)b;
  return MPP_OK;
}
MPP_RET mpp_buffer_put_with_caller(MppBuffer buffer, const char *caller) {
  (void)caller;
  MockBuffer *b = (MockBuffer *)buffer;
  if (!b)
    return MPP_NOK;
  if (--b->refs == 0) {
    close(b->fd);
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
  (void)packet;
  return 0;
}
MppBuffer mpp_packet_get_buffer(const MppPacket packet) {
  MockPacket *p = (MockPacket *)packet;
  if (!p)
    return NULL;
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
  *packet = (MppPacket)p;
  return MPP_OK;
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
MPP_RET mpp_meta_get_frame(MppMeta meta, MppMetaKey key, MppFrame *frame) {
  (void)key;
  if (!meta || !frame)
    return MPP_NOK;
  *frame = ((MockPacket *)meta)->input_frame;
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
  api.reset = ok;
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
unsigned mpp_mock_control_count(int cmd) {
  return cmd == MPP_ENC_SET_CFG           ? (unsigned)control_counts[0]
         : cmd == MPP_ENC_SET_SEI_CFG     ? (unsigned)control_counts[1]
         : cmd == MPP_ENC_SET_HEADER_MODE ? (unsigned)control_counts[2]
                                          : 0;
}
unsigned mpp_mock_frame_set_buffer_count(void) {
  return atomic_load(&frame_set_buffer_count);
}
void mpp_mock_dec_arm(unsigned width, unsigned height) {
  dec_width = (RK_U32)width;
  dec_height = (RK_U32)height;
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
  atomic_store(&dec_enabled, 1);
}
void mpp_mock_dec_disarm(void) {
  atomic_store(&dec_enabled, 0);
  while (packet_allocs) {
    MockPacket *packet = packet_allocs;
    packet_allocs = packet->next;
    free(packet);
  }
}
unsigned mpp_mock_dec_queued(void) { return atomic_load(&dec_queued); }
unsigned mpp_mock_dec_outputs(void) { return atomic_load(&dec_outputs); }
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
void mpp_mock_reset(void) {
  memset(control_counts, 0, sizeof(control_counts));
  last_cfg = NULL;
  atomic_store(&queued_frame, NULL);
  atomic_store(&frame_set_buffer_count, 0);
  memset(&output_packet, 0, sizeof(output_packet));
  mpp_mock_dec_disarm();
}
