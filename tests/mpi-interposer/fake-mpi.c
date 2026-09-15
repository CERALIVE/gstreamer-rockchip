/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fake-mpi.h"

typedef struct {
  MPP_RET put_result;
  MPP_RET get_result;
  gsize length;
  GMutex mutex;
  GCond cond;
  gboolean block;
  gboolean entered;
} FakeContext;

static gint destroys;

static MPP_RET put(MppCtx ctx, MppFrame frame) {
  FakeContext *self = ctx;
  g_mutex_lock(&self->mutex);
  self->entered = TRUE;
  g_cond_broadcast(&self->cond);
  while (self->block)
    g_cond_wait(&self->cond, &self->mutex);
  MPP_RET ret = self->put_result;
  if (ret == MPP_OK && frame)
    ((FakeFrame *)frame)->submissions++;
  g_mutex_unlock(&self->mutex);
  return ret;
}

static MPP_RET get(MppCtx ctx, MppPacket *packet) {
  FakeContext *self = ctx;
  *packet = self->get_result == MPP_OK ? (MppPacket)&self->length : NULL;
  return self->get_result;
}

static MPP_RET control(MppCtx ctx, MpiCmd cmd, MppParam param) {
  (void)ctx;
  (void)cmd;
  (void)param;
  return MPP_OK;
}

static MPP_RET reset(MppCtx ctx) {
  (void)ctx;
  return MPP_OK;
}

static MPP_RET poll_port(MppCtx ctx, MppPortType port, MppPollType timeout) {
  (void)ctx;
  (void)port;
  (void)timeout;
  return MPP_ERR_TIMEOUT;
}

static const MppApi api = {
    .size = sizeof(MppApi), .version = 42,
    .encode_put_frame = put, .encode_get_packet = get,
    .control = control, .reset = reset, .poll = poll_port,
    .reserv = {1234}};

MppApi *fake_mpi_api(void) { return (MppApi *)&api; }

MPP_RET mpp_create(MppCtx *ctx, MppApi **mpi) {
  if (!ctx || !mpi)
    return MPP_ERR_NULL_PTR;
  FakeContext *self = g_new0(FakeContext, 1);
  self->length = 1;
  *ctx = self;
  *mpi = (MppApi *)&api;
  return MPP_OK;
}

MPP_RET mpp_init(MppCtx ctx, MppCtxType type, MppCodingType coding) {
  (void)ctx;
  (void)type;
  (void)coding;
  return MPP_OK;
}

MPP_RET mpp_destroy(MppCtx ctx) {
  FakeContext *self = ctx;
  g_atomic_int_inc(&destroys);
  g_mutex_clear(&self->mutex);
  g_cond_clear(&self->cond);
  g_free(self);
  return MPP_OK;
}

size_t mpp_packet_get_length(const MppPacket packet) {
  return *(const gsize *)packet;
}

RK_U32 mpp_frame_get_eos(const MppFrame frame) {
  return ((const FakeFrame *)frame)->eos;
}

void fake_mpi_put_result(MppCtx ctx, MPP_RET result) {
  ((FakeContext *)ctx)->put_result = result;
}

void fake_mpi_packet_result(MppCtx ctx, MPP_RET result, gsize length) {
  FakeContext *self = ctx;
  self->get_result = result;
  self->length = length;
}

void fake_mpi_block_put(MppCtx ctx) {
  ((FakeContext *)ctx)->block = TRUE;
}

void fake_mpi_wait_put(MppCtx ctx) {
  FakeContext *self = ctx;
  g_mutex_lock(&self->mutex);
  while (!self->entered)
    g_cond_wait(&self->cond, &self->mutex);
  g_mutex_unlock(&self->mutex);
}

void fake_mpi_release_put(MppCtx ctx) {
  FakeContext *self = ctx;
  g_mutex_lock(&self->mutex);
  self->block = FALSE;
  g_cond_broadcast(&self->cond);
  g_mutex_unlock(&self->mutex);
}

guint fake_mpi_destroys(void) { return (guint)g_atomic_int_get(&destroys); }
