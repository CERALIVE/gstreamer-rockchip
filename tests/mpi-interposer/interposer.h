/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CERALIVE_MPI_TEST_INTERPOSER_H
#define CERALIVE_MPI_TEST_INTERPOSER_H

#include <glib.h>
#include <rockchip/rk_mpi.h>

typedef enum {
  MPI_TEST_ARMED,
  MPI_TEST_NOT_FOUND,
  MPI_TEST_NOT_READY,
  MPI_TEST_ALREADY_ARMED,
  MPI_TEST_SPENT
} MpiTestArmResult;

typedef struct {
  guint64 id;
  guint64 puts;
  guint64 real_puts;
  guint64 successful_puts;
  guint64 healthy_packets;
  guint64 injections;
  gboolean closing;
} MpiTestStats;

/* IDs are monotonically assigned, never context addresses or creation ordinals
 * guessed by the operator. A destroyed context's ID cannot arm its replacement. */
guint64 mpi_test_context_id(MppCtx ctx);
gboolean mpi_test_snapshot(guint64 id, MpiTestStats *stats);
MpiTestArmResult mpi_test_arm(guint64 id);

#endif
