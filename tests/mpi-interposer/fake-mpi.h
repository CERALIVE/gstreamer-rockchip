/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CERALIVE_MPI_TEST_FAKE_H
#define CERALIVE_MPI_TEST_FAKE_H

#include <glib.h>
#include <rockchip/rk_mpi.h>

typedef struct {
  guint submissions;
  guint marker;
  gboolean eos;
} FakeFrame;

MppApi *fake_mpi_api(void);
void fake_mpi_put_result(MppCtx ctx, MPP_RET result);
void fake_mpi_packet_result(MppCtx ctx, MPP_RET result, gsize length);
void fake_mpi_block_put(MppCtx ctx);
void fake_mpi_wait_put(MppCtx ctx);
void fake_mpi_release_put(MppCtx ctx);
guint fake_mpi_destroys(void);

#endif
