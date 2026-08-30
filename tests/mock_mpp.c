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
typedef struct {
  MppFrame input_frame;
} MockPacket;
typedef struct {
  int fd;
  size_t size;
  int index;
  unsigned refs;
} MockBuffer;
static _Atomic(MppFrame) queued_frame;
static MockPacket output_packet;
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
static MPP_RET get_frame_ok(MppCtx c, MppFrame *p) {
  (void)c;
  if (p)
    *p = NULL;
  return MPP_NOK;
}
static MPP_RET put_packet_ok(MppCtx c, MppPacket p) {
  (void)c;
  (void)p;
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

MppMeta mpp_packet_get_meta(const MppPacket packet) { return (MppMeta)packet; }
size_t mpp_packet_get_length(const MppPacket packet) {
  (void)packet;
  return 0;
}
MppBuffer mpp_packet_get_buffer(const MppPacket packet) {
  (void)packet;
  return NULL;
}
MPP_RET mpp_packet_deinit(MppPacket *packet) {
  if (packet)
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
void mpp_mock_reset(void) {
  memset(control_counts, 0, sizeof(control_counts));
  last_cfg = NULL;
  atomic_store(&queued_frame, NULL);
  memset(&output_packet, 0, sizeof(output_packet));
}
