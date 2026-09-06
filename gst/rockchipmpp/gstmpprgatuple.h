/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_MPP_RGA_TUPLE_H__
#define __GST_MPP_RGA_TUPLE_H__

#include "gstmpprgabackend.h"

G_BEGIN_DECLS;

typedef struct
{
  GstMppRgaOperation operation;
  GstVideoFormat in_format;
  GstVideoFormat out_format;
} GstMppRgaTupleKey;

typedef struct _GstMppRgaTupleTable GstMppRgaTupleTable;

GstMppRgaTupleTable *gst_mpp_rga_tuple_table_new (void);
void gst_mpp_rga_tuple_table_free (GstMppRgaTupleTable * table);
gboolean gst_mpp_rga_tuple_should_try (GstMppRgaTupleTable * table,
    const GstMppRgaTupleKey * key);
void gst_mpp_rga_tuple_succeeded (GstMppRgaTupleTable * table,
    const GstMppRgaTupleKey * key);
gboolean gst_mpp_rga_tuple_failed (GstMppRgaTupleTable * table,
    const GstMppRgaTupleKey * key, guint * failures);
guint gst_mpp_rga_tuple_failures (GstMppRgaTupleTable * table,
    const GstMppRgaTupleKey * key);

G_END_DECLS;

#endif
