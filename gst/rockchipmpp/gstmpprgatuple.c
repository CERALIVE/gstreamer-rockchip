/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "gstmpprgatuple.h"

typedef struct
{
  GstMppRgaTupleKey key;
  guint failures;
  guint skipped;
  gboolean demoted;
} GstMppRgaTupleHealth;

struct _GstMppRgaTupleTable
{
  GArray *entries;
};

static GstMppRgaTupleHealth *
gst_mpp_rga_tuple_find (GstMppRgaTupleTable * table,
    const GstMppRgaTupleKey * key, gboolean create)
{
  guint i;

  for (i = 0; i < table->entries->len; i++) {
    GstMppRgaTupleHealth *tuple = &g_array_index (table->entries,
        GstMppRgaTupleHealth, i);
    if (tuple->key.operation == key->operation &&
        tuple->key.in_format == key->in_format &&
        tuple->key.out_format == key->out_format)
      return tuple;
  }

  if (create) {
    GstMppRgaTupleHealth tuple = {.key = *key };
    g_array_append_val (table->entries, tuple);
    return &g_array_index (table->entries, GstMppRgaTupleHealth,
        table->entries->len - 1);
  }

  return NULL;
}

GstMppRgaTupleTable *
gst_mpp_rga_tuple_table_new (void)
{
  GstMppRgaTupleTable *table = g_new0 (GstMppRgaTupleTable, 1);
  table->entries = g_array_new (FALSE, FALSE, sizeof (GstMppRgaTupleHealth));
  return table;
}

void
gst_mpp_rga_tuple_table_free (GstMppRgaTupleTable * table)
{
  g_array_unref (table->entries);
  g_free (table);
}

gboolean
gst_mpp_rga_tuple_should_try (GstMppRgaTupleTable * table,
    const GstMppRgaTupleKey * key)
{
  GstMppRgaTupleHealth *tuple = gst_mpp_rga_tuple_find (table, key, TRUE);

  if (tuple->demoted && ++tuple->skipped <= GST_MPP_RGA_RETRY_INTERVAL)
    return FALSE;

  tuple->skipped = 0;
  return TRUE;
}

void
gst_mpp_rga_tuple_succeeded (GstMppRgaTupleTable * table,
    const GstMppRgaTupleKey * key)
{
  GstMppRgaTupleHealth *tuple = gst_mpp_rga_tuple_find (table, key, TRUE);
  tuple->failures = 0;
  tuple->demoted = FALSE;
}

gboolean
gst_mpp_rga_tuple_failed (GstMppRgaTupleTable * table,
    const GstMppRgaTupleKey * key, guint * failures)
{
  GstMppRgaTupleHealth *tuple = gst_mpp_rga_tuple_find (table, key, TRUE);
  gboolean newly_demoted = FALSE;

  tuple->failures++;
  if (!tuple->demoted && tuple->failures >= GST_MPP_RGA_DEMOTION_THRESHOLD) {
    tuple->failures = GST_MPP_RGA_DEMOTION_THRESHOLD;
    tuple->demoted = TRUE;
    tuple->skipped = 0;
    newly_demoted = TRUE;
  }
  *failures = tuple->failures;
  return newly_demoted;
}

guint
gst_mpp_rga_tuple_failures (GstMppRgaTupleTable * table,
    const GstMppRgaTupleKey * key)
{
  GstMppRgaTupleHealth *tuple = gst_mpp_rga_tuple_find (table, key, FALSE);
  return tuple ? tuple->failures : 0;
}
