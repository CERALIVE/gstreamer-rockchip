/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CERALIVE_MPI_TEST_ONLY
#error "Program-encoder targeting belongs only in the standalone test build"
#endif

#include "program-target.h"
#include "../../gst/rockchipmpp/gstmppenc.h"

static gboolean is_program(GstElement *element) {
  GType type = g_type_from_name("GstMppEnc");
  if (!element || !type || !G_TYPE_CHECK_INSTANCE_TYPE(element, type))
    return FALSE;
  GTypeQuery query;
  g_type_query(type, &query);
  if (query.instance_size != sizeof(GstMppEnc))
    return FALSE;
  gchar *name = gst_object_get_name(GST_OBJECT(element));
  gboolean matches = g_strcmp0(name, "venc_bps") == 0;
  g_free(name);
  return matches;
}

guint64 mpi_test_program_id(GstElement *program) {
  if (!is_program(program))
    return 0;
  GstMppEnc *encoder = (GstMppEnc *)program;
  GST_VIDEO_ENCODER_STREAM_LOCK(encoder);
  guint64 id = mpi_test_context_id(encoder->mpp_ctx);
  GST_VIDEO_ENCODER_STREAM_UNLOCK(encoder);
  return id;
}

MpiTestArmResult mpi_test_arm_program(GstElement *program, guint64 expected_id) {
  if (!is_program(program) || !expected_id)
    return MPI_TEST_NOT_FOUND;
  GstMppEnc *encoder = (GstMppEnc *)program;
  GST_VIDEO_ENCODER_STREAM_LOCK(encoder);
  guint64 actual = mpi_test_context_id(encoder->mpp_ctx);
  MpiTestArmResult result = actual == expected_id
                               ? mpi_test_arm(actual)
                               : MPI_TEST_NOT_FOUND;
  GST_VIDEO_ENCODER_STREAM_UNLOCK(encoder);
  return result;
}
