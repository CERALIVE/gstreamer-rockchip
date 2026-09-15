/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#ifndef CERALIVE_MPI_TEST_ONLY
#error "Encoded fixtures must never replace the production MPP provider"
#endif

#include <rockchip/rk_mpi.h>
#define mpp_create fixture_base_create
#include "../mock_mpp.c"
#undef mpp_create

static GMutex fixture_mutex;
static GBytes *fixture;

void mpi_fixture_payload(const guint8 *data, gsize size) {
  g_assert_cmpuint(size, <=, ENC_BUFFER_MAP_SIZE);
  g_mutex_lock(&fixture_mutex);
  g_clear_pointer(&fixture, g_bytes_unref);
  if (data)
    fixture = g_bytes_new(data, size);
  g_mutex_unlock(&fixture_mutex);
}

static MPP_RET fixture_put(MppCtx ctx, MppFrame frame) {
  g_mutex_lock(&fixture_mutex);
  g_assert_nonnull(fixture);
  MPP_RET ret = encode_put(ctx, frame);
  if (ret == MPP_OK) {
    unsigned tail = atomic_load(&enc_tail);
    MockPacket *packet = &enc_packets[(tail - 1) % ENC_PACKET_CAPACITY];
    MockBuffer *buffer = (MockBuffer *)packet->buffer;
    gsize length;
    const guint8 *data = g_bytes_get_data(fixture, &length);
    memcpy(buffer->data, data, length);
    buffer->size = length;
    packet->length = length;
    packet->has_intra_meta = 1;
    packet->intra = 1;
  }
  g_mutex_unlock(&fixture_mutex);
  return ret;
}

MPP_RET mpp_create(MppCtx *ctx, MppApi **mpi) {
  MPP_RET ret = fixture_base_create(ctx, mpi);
  if (ret == MPP_OK)
    (*mpi)->encode_put_frame = fixture_put;
  return ret;
}
