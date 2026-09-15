/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CERALIVE_MPI_TEST_PROGRAM_TARGET_H
#define CERALIVE_MPI_TEST_PROGRAM_TARGET_H

#include "interposer.h"
#include <gst/gst.h>

guint64 mpi_test_program_id(GstElement *program);
MpiTestArmResult mpi_test_arm_program(GstElement *program, guint64 expected_id);

#endif
