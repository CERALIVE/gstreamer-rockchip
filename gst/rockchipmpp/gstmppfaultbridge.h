/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef __GST_MPP_FAULT_BRIDGE_H__
#define __GST_MPP_FAULT_BRIDGE_H__

#include <gst/gst.h>

G_BEGIN_DECLS;

/* Cross-layer kernel fault bridge.
 *
 * The RK3588 media island reports an encode-side hardware fault -- an RKVENC
 * job timeout, a reset, or the ~500 ms timeout consequence of an IOMMU page
 * fault -- through its own ftrace events and nowhere else. libmpp's async
 * return set is only MPP_OK / MPP_NOK / MPP_ERR_TIMEOUT, and this plugin
 * deliberately treats the latter two as non-restartable, so a real hardware
 * fault could never reach gst_mpp_enc_restart_context(). This module supplies
 * that missing signal by reading the kernel's own tracepoints.
 *
 * Two layers, split so the correlation rules are testable without a kernel:
 *
 *   * GstMppFaultCorrelator is pure: it consumes trace_pipe TEXT LINES and
 *     answers who a fault belongs to. It performs no I/O at all.
 *   * GstMppFaultBridge owns the tracefs instance, the trace_pipe fd, and the
 *     procfs session-ownership lookup, and feeds the correlator.
 *
 * Every failure path is FAIL-OPEN: an absent tracefs, an absent rockchip_mpp
 * event directory, an unreadable procfs summary or a parse failure disables
 * the bridge and leaves encoding exactly as it was. The bridge is an observer
 * and may never be load-bearing.
 */

/* Bounded LRU depth of the task map. mpp_task_queued is emitted per task, i.e.
 * per frame, so this only has to span the queue-to-error latency of a stalled
 * job (~500 ms), not the session. */
#define GST_MPP_FAULT_TASK_MAP_SIZE 256

/* Distinct owned rkvenc session indices remembered. An encoder acquires one
 * per MPP context, and a context is recreated at most three times per ten
 * seconds by the restart budget. */
#define GST_MPP_FAULT_MAX_OWNED_SESSIONS 32

/* An entry that has seen mpp_task_queued but not yet mpp_core_selected. */
#define GST_MPP_FAULT_CORE_UNASSIGNED (-1)

/* Owned faults required inside GST_MPP_FAULT_WINDOW_US before the bridge asks
 * for a restart. A single recovered task error is not a reason to tear down a
 * healthy stream; a wedged encoder produces them repeatedly. */
#define GST_MPP_FAULT_THRESHOLD 3
#define GST_MPP_FAULT_WINDOW_US (2 * G_USEC_PER_SEC)

typedef enum
{
  /* Not one of the four events we consume, or unparseable. */
  GST_MPP_FAULT_LINE_IGNORED = 0,
  /* trace_pipe reported a ring-buffer overrun. */
  GST_MPP_FAULT_LINE_LOST_EVENTS,
  /* mpp_task_queued: a (session, task_id) pair was recorded. */
  GST_MPP_FAULT_LINE_QUEUED,
  /* mpp_core_selected: a recorded task_id was bound to a core. */
  GST_MPP_FAULT_LINE_CORE_SELECTED,
  /* mpp_core_selected found MORE THAN ONE unbound entry carrying that task_id,
   * so which session the core belongs to cannot be decided. Both candidates
   * are dropped rather than guessed at: a wrong guess would attribute another
   * process's fault to this encoder, and losing detection is the safe failure
   * while a false restart is not. */
  GST_MPP_FAULT_LINE_AMBIGUOUS,
  /* mpp_task_done: a recorded (task_id, core_id) completed and was retired. */
  GST_MPP_FAULT_LINE_RETIRED,
  /* mpp_task_error resolved to a session this encoder owns. */
  GST_MPP_FAULT_LINE_FAULT_OWNED,
  /* mpp_task_error resolved to a session belonging to somebody else. */
  GST_MPP_FAULT_LINE_FAULT_FOREIGN,
  /* mpp_task_error could not be resolved to any session at all. Never acted
   * on: an unjoinable fault is indistinguishable from a foreign one. */
  GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED,
} GstMppFaultLineVerdict;

typedef struct
{
  guint32 task_id;
  /* task_id is atomic_fetch_inc() per TASKQUEUE, not global, so two queues
   * both start at 0 and their ids collide. The join key is therefore
   * (task_id, core_id), which mpp_task_error carries and mpp_task_queued does
   * not -- mpp_core_selected is the hop that supplies it. */
  gint32 core_id;
  guint32 session;
  /* Monotonic insertion order. 0 marks a free slot, so "newest match" is
   * resolvable without a separate occupancy bitmap. */
  guint64 seq;
} GstMppFaultTaskEntry;

typedef struct
{
  GstMppFaultTaskEntry entries[GST_MPP_FAULT_TASK_MAP_SIZE];
  guint next;
  guint64 seq;

  guint32 owned[GST_MPP_FAULT_MAX_OWNED_SESSIONS];
  guint n_owned;
  /* FALSE until ownership has been resolved at least once. While FALSE every
   * fault is UNATTRIBUTED, because "we do not know whose this is" must never
   * read as "ours". */
  gboolean owned_resolved;

  guint64 owned_faults;
  guint64 foreign_faults;
  guint64 unattributed_faults;
  guint64 lost_events;
} GstMppFaultCorrelator;

typedef struct _GstMppFaultBridge GstMppFaultBridge;

void gst_mpp_fault_correlator_init (GstMppFaultCorrelator * corr);
gboolean gst_mpp_fault_correlator_add_session (GstMppFaultCorrelator * corr,
    guint32 session);
gboolean gst_mpp_fault_correlator_owns_session (const GstMppFaultCorrelator *
    corr, guint32 session);
void gst_mpp_fault_correlator_mark_resolved (GstMppFaultCorrelator * corr);
GstMppFaultLineVerdict gst_mpp_fault_correlator_feed_line (GstMppFaultCorrelator
    * corr, const gchar * line);

/* Parses "session: pid=<p> index=<i>" plus its following " device: <name>"
 * line out of /proc/mpp_service/sessions-summary, keeping only rkvenc-core
 * sessions whose pid is in @own_tids. Exposed for testing; the summary's pid
 * field is the CREATING THREAD's TID, not the process pid. */
guint gst_mpp_fault_sessions_from_summary (const gchar * summary,
    GHashTable * own_tids, GstMppFaultCorrelator * corr);

/* Reads the caller's own thread ids from /proc/self/task. Returns a hash set
 * keyed by GUINT_TO_POINTER(tid), or NULL when procfs is unreadable. */
GHashTable *gst_mpp_fault_read_own_tids (void);

/* Returns NULL when the bridge is not enabled or cannot arm. Never fatal. */
GstMppFaultBridge *gst_mpp_fault_bridge_new (GstObject * parent);
void gst_mpp_fault_bridge_free (GstMppFaultBridge * bridge);

/* Re-reads the session summary and UNIONS newly owned indices into the cached
 * set. Called once at start and again after each successful context restart,
 * which creates a new session with a new index. Union rather than replace: the
 * summary's pid is a TID that can die while the session lives, so a later read
 * may legitimately fail to see a session we already know is ours. */
guint gst_mpp_fault_bridge_resolve_sessions (GstMppFaultBridge * bridge,
    GstObject * parent);

/* Drains whatever trace_pipe has. Returns TRUE when the owned-fault threshold
 * was crossed inside the window, at most once per window. */
gboolean gst_mpp_fault_bridge_poll (GstMppFaultBridge * bridge,
    GstObject * parent);

guint64 gst_mpp_fault_bridge_owned_faults (const GstMppFaultBridge * bridge);

G_END_DECLS;

#endif /* __GST_MPP_FAULT_BRIDGE_H__ */
