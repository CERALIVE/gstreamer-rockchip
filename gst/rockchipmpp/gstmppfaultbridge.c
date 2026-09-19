/*
 * Copyright 2026 CERALIVE <contact@ceralive.tv>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "gstmppfaultbridge.h"

GST_DEBUG_CATEGORY_STATIC (mppfaultbridge_debug);
#define GST_CAT_DEFAULT mppfaultbridge_debug

/* Default tracefs mount, with the historical debugfs location as a fallback. */
#define TRACEFS_PRIMARY "/sys/kernel/tracing"
#define TRACEFS_FALLBACK "/sys/kernel/debug/tracing"
#define MPP_EVENT_DIR "events/rockchip_mpp"
#define SESSIONS_SUMMARY "/proc/mpp_service/sessions-summary"

/* Per-CPU ring size for the private instance. mpp_task_queued is per-frame, so
 * this only has to cover one poll interval; an overrun loses a join key, which
 * degrades a fault to UNATTRIBUTED rather than misattributing it. */
#define TRACE_BUFFER_SIZE_KB "256"

/* Bytes pulled from trace_pipe per poll, and the read chunk. The cap bounds
 * the time this spends holding the encoder's stream lock. */
#define TRACE_READ_CHUNK 4096
#define TRACE_READS_PER_POLL 16

/* trace_pipe is drained at most this often. The encoder task loop spins at
 * roughly 1 ms while frames are outstanding, and a fault does not need
 * millisecond latency. */
#define POLL_INTERVAL_US (G_USEC_PER_SEC / 100)

struct _GstMppFaultBridge
{
  GstMppFaultCorrelator corr;

  /* Directory we created and therefore must remove, or NULL when the instance
   * was supplied externally. */
  gchar *created_instance;
  gchar *instance_dir;
  gchar *sessions_path;
  gint pipe_fd;

  /* Partial trailing line carried across reads. trace_pipe has no alignment
   * guarantee, so a record can straddle a read boundary. */
  GString *partial;

  gint64 last_poll;
  gint64 window_started;
  guint window_faults;
  guint threshold;
};

/* ------------------------------------------------------------------ parsing */

/* Returns the payload after "<name>: " when @line carries that trace event.
 *
 * The event name is matched at a token boundary rather than with a bare
 * strstr, because a trace_pipe record starts with the task comm and a comm can
 * itself contain both spaces and colons ("kworker/0:1-22"). */
static const gchar *
gst_mpp_fault_event_payload (const gchar * line, const gchar * name)
{
  gsize name_len = strlen (name);
  const gchar *at = line;

  while ((at = strstr (at, name)) != NULL) {
    const gchar *after = at + name_len;

    if ((at == line || at[-1] == ' ') && after[0] == ':' && after[1] == ' ')
      return after + 2;

    at = after;
  }
  return NULL;
}

/* Reads "<key>=<number>" out of a trace payload. The key must start the token,
 * so "task=" cannot match inside "core_task=". Accepts the 0x form because
 * irq_status is printed as hex. */
static gboolean
gst_mpp_fault_parse_field (const gchar * payload, const gchar * key,
    gint64 * out)
{
  gsize key_len = strlen (key);
  const gchar *at = payload;

  while ((at = strstr (at, key)) != NULL) {
    const gchar *value = at + key_len;

    if (at != payload && at[-1] != ' ') {
      at = value;
      continue;
    }

    {
      gchar *end = NULL;
      gint64 parsed;

      errno = 0;
      parsed = g_ascii_strtoll (value, &end, 0);
      if (end == value || errno != 0)
        return FALSE;

      *out = parsed;
      return TRUE;
    }
  }
  return FALSE;
}

/* ---------------------------------------------------------------- correlator */

void
gst_mpp_fault_correlator_init (GstMppFaultCorrelator * corr)
{
  memset (corr, 0, sizeof (*corr));
}

gboolean
gst_mpp_fault_correlator_owns_session (const GstMppFaultCorrelator * corr,
    guint32 session)
{
  guint i;

  for (i = 0; i < corr->n_owned; i++) {
    if (corr->owned[i] == session)
      return TRUE;
  }
  return FALSE;
}

void
gst_mpp_fault_correlator_mark_resolved (GstMppFaultCorrelator * corr)
{
  corr->owned_resolved = TRUE;
}

gboolean
gst_mpp_fault_correlator_add_session (GstMppFaultCorrelator * corr,
    guint32 session)
{
  if (gst_mpp_fault_correlator_owns_session (corr, session))
    return FALSE;

  if (corr->n_owned == GST_MPP_FAULT_MAX_OWNED_SESSIONS) {
    /* Drop the oldest. A session index is only retired by the kernel when the
     * session closes, so the oldest is the one least likely to still exist. */
    memmove (&corr->owned[0], &corr->owned[1],
        (GST_MPP_FAULT_MAX_OWNED_SESSIONS - 1) * sizeof (corr->owned[0]));
    corr->n_owned--;
  }

  corr->owned[corr->n_owned++] = session;
  return TRUE;
}

/* Newest live entry matching @task_id, optionally constrained to a core.
 * @core_id of GST_MPP_FAULT_CORE_UNASSIGNED selects entries that have NOT yet
 * been bound to a core. */
static GstMppFaultTaskEntry *
gst_mpp_fault_lookup (GstMppFaultCorrelator * corr, guint32 task_id,
    gint32 core_id)
{
  GstMppFaultTaskEntry *best = NULL;
  guint i;

  for (i = 0; i < GST_MPP_FAULT_TASK_MAP_SIZE; i++) {
    GstMppFaultTaskEntry *entry = &corr->entries[i];

    if (entry->seq == 0 || entry->task_id != task_id)
      continue;
    if (entry->core_id != core_id)
      continue;
    if (best == NULL || entry->seq > best->seq)
      best = entry;
  }
  return best;
}

/* Oldest entry carrying @task_id that has not been bound to a core yet, plus
 * how many such entries exist. Oldest, because a taskqueue pops tasks in the
 * order it queued them; the count is what lets the caller refuse to decide
 * when two queues are holding the same id. */
static GstMppFaultTaskEntry *
gst_mpp_fault_unbound (GstMppFaultCorrelator * corr, guint32 task_id,
    guint * candidates)
{
  GstMppFaultTaskEntry *best = NULL;
  guint i;

  *candidates = 0;
  for (i = 0; i < GST_MPP_FAULT_TASK_MAP_SIZE; i++) {
    GstMppFaultTaskEntry *entry = &corr->entries[i];

    if (entry->seq == 0 || entry->task_id != task_id)
      continue;
    if (entry->core_id != GST_MPP_FAULT_CORE_UNASSIGNED)
      continue;

    (*candidates)++;
    if (best == NULL || entry->seq < best->seq)
      best = entry;
  }
  return best;
}

static void
gst_mpp_fault_insert (GstMppFaultCorrelator * corr, guint32 task_id,
    guint32 session)
{
  GstMppFaultTaskEntry *entry = &corr->entries[corr->next];

  corr->next = (corr->next + 1) % GST_MPP_FAULT_TASK_MAP_SIZE;
  entry->task_id = task_id;
  entry->core_id = GST_MPP_FAULT_CORE_UNASSIGNED;
  entry->session = session;
  entry->seq = ++corr->seq;
}

static void
gst_mpp_fault_retire (GstMppFaultTaskEntry * entry)
{
  entry->seq = 0;
  entry->core_id = GST_MPP_FAULT_CORE_UNASSIGNED;
}

static void
gst_mpp_fault_drop_unbound (GstMppFaultCorrelator * corr, guint32 task_id)
{
  guint i;

  for (i = 0; i < GST_MPP_FAULT_TASK_MAP_SIZE; i++) {
    GstMppFaultTaskEntry *entry = &corr->entries[i];

    if (entry->seq == 0 || entry->task_id != task_id)
      continue;
    if (entry->core_id == GST_MPP_FAULT_CORE_UNASSIGNED)
      gst_mpp_fault_retire (entry);
  }
}

GstMppFaultLineVerdict
gst_mpp_fault_correlator_feed_line (GstMppFaultCorrelator * corr,
    const gchar * line)
{
  const gchar *payload;
  gint64 task_id = 0;
  gint64 core_id = 0;
  gint64 session = 0;

  if (line == NULL || line[0] == '\0')
    return GST_MPP_FAULT_LINE_IGNORED;

  /* trace_pipe reports a dropped range inline. Losing a queued event can only
   * downgrade a later fault to UNATTRIBUTED, so this is a health counter
   * rather than a correctness problem. */
  if (strstr (line, "LOST ") != NULL && strstr (line, "EVENTS") != NULL) {
    corr->lost_events++;
    return GST_MPP_FAULT_LINE_LOST_EVENTS;
  }

  if (line[0] == '#')
    return GST_MPP_FAULT_LINE_IGNORED;

  payload = gst_mpp_fault_event_payload (line, "mpp_task_queued");
  if (payload != NULL) {
    if (!gst_mpp_fault_parse_field (payload, "session=", &session) ||
        !gst_mpp_fault_parse_field (payload, "task=", &task_id))
      return GST_MPP_FAULT_LINE_IGNORED;

    gst_mpp_fault_insert (corr, (guint32) task_id, (guint32) session);
    return GST_MPP_FAULT_LINE_QUEUED;
  }

  payload = gst_mpp_fault_event_payload (line, "mpp_core_selected");
  if (payload != NULL) {
    GstMppFaultTaskEntry *entry;
    guint candidates;

    if (!gst_mpp_fault_parse_field (payload, "task=", &task_id) ||
        !gst_mpp_fault_parse_field (payload, "core=", &core_id))
      return GST_MPP_FAULT_LINE_IGNORED;

    entry = gst_mpp_fault_unbound (corr, (guint32) task_id, &candidates);
    if (entry == NULL)
      return GST_MPP_FAULT_LINE_IGNORED;

    /* Two taskqueues can both be holding this id at once, and mpp_core_selected
     * says nothing about which queue it came from. Binding either candidate
     * would be a coin flip whose wrong side attributes another process's fault
     * to this encoder, so the id is abandoned instead. */
    if (candidates > 1) {
      gst_mpp_fault_drop_unbound (corr, (guint32) task_id);
      return GST_MPP_FAULT_LINE_AMBIGUOUS;
    }

    entry->core_id = (gint32) core_id;
    return GST_MPP_FAULT_LINE_CORE_SELECTED;
  }

  payload = gst_mpp_fault_event_payload (line, "mpp_task_done");
  if (payload != NULL) {
    GstMppFaultTaskEntry *entry;

    if (!gst_mpp_fault_parse_field (payload, "task=", &task_id) ||
        !gst_mpp_fault_parse_field (payload, "core=", &core_id))
      return GST_MPP_FAULT_LINE_IGNORED;

    entry = gst_mpp_fault_lookup (corr, (guint32) task_id, (gint32) core_id);
    if (entry == NULL)
      return GST_MPP_FAULT_LINE_IGNORED;

    gst_mpp_fault_retire (entry);
    return GST_MPP_FAULT_LINE_RETIRED;
  }

  payload = gst_mpp_fault_event_payload (line, "mpp_task_error");
  if (payload != NULL) {
    GstMppFaultTaskEntry *entry;
    guint32 faulted_session;

    if (!gst_mpp_fault_parse_field (payload, "task=", &task_id) ||
        !gst_mpp_fault_parse_field (payload, "core=", &core_id))
      return GST_MPP_FAULT_LINE_IGNORED;

    entry = gst_mpp_fault_lookup (corr, (guint32) task_id, (gint32) core_id);
    if (entry == NULL) {
      /* Either the join key was queued before tracing started, or the ring
       * dropped it, or the core never matched. Attributing this to ourselves
       * would restart a healthy encoder on somebody else's fault. */
      corr->unattributed_faults++;
      return GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED;
    }

    faulted_session = entry->session;
    gst_mpp_fault_retire (entry);

    if (!corr->owned_resolved) {
      corr->unattributed_faults++;
      return GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED;
    }

    if (!gst_mpp_fault_correlator_owns_session (corr, faulted_session)) {
      corr->foreign_faults++;
      return GST_MPP_FAULT_LINE_FAULT_FOREIGN;
    }

    corr->owned_faults++;
    return GST_MPP_FAULT_LINE_FAULT_OWNED;
  }

  return GST_MPP_FAULT_LINE_IGNORED;
}

/* ------------------------------------------------------------ own sessions */

GHashTable *
gst_mpp_fault_read_own_tids (void)
{
  GHashTable *tids;
  GError *error = NULL;
  GDir *dir;
  const gchar *name;

  dir = g_dir_open ("/proc/self/task", 0, &error);
  if (dir == NULL) {
    g_clear_error (&error);
    return NULL;
  }

  tids = g_hash_table_new (g_direct_hash, g_direct_equal);
  while ((name = g_dir_read_name (dir)) != NULL) {
    gchar *end = NULL;
    guint64 tid = g_ascii_strtoull (name, &end, 10);

    if (end == name || *end != '\0' || tid == 0 || tid > G_MAXUINT)
      continue;
    g_hash_table_add (tids, GUINT_TO_POINTER ((guint) tid));
  }
  g_dir_close (dir);
  return tids;
}

guint
gst_mpp_fault_sessions_from_summary (const gchar * summary,
    GHashTable * own_tids, GstMppFaultCorrelator * corr)
{
  gchar **lines;
  guint added = 0;
  guint i;
  gboolean have_pending = FALSE;
  guint pending_pid = 0;
  guint pending_index = 0;

  if (summary == NULL || own_tids == NULL)
    return 0;

  /* A real board's summary carries IOVA dumps and encoder tables the
   * documented abbreviated form does not, so this scans for the two lines it
   * needs and ignores everything else rather than assuming a record layout. */
  lines = g_strsplit (summary, "\n", -1);
  for (i = 0; lines[i] != NULL; i++) {
    const gchar *line = lines[i];
    const gchar *trimmed = line;
    gint64 pid = 0;
    gint64 index = 0;

    while (*trimmed == ' ' || *trimmed == '\t')
      trimmed++;

    if (g_str_has_prefix (trimmed, "session:")) {
      have_pending = FALSE;
      if (gst_mpp_fault_parse_field (trimmed, "pid=", &pid) &&
          gst_mpp_fault_parse_field (trimmed, "index=", &index) &&
          pid > 0 && index >= 0) {
        have_pending = TRUE;
        pending_pid = (guint) pid;
        pending_index = (guint) index;
      }
      continue;
    }

    if (!have_pending || !g_str_has_prefix (trimmed, "device:"))
      continue;

    have_pending = FALSE;

    /* Only encode sessions. A decode or RGA session sharing an index space
     * would otherwise let an unrelated fault reach the encoder. */
    if (strstr (trimmed, "rkvenc") == NULL)
      continue;

    if (!g_hash_table_contains (own_tids, GUINT_TO_POINTER (pending_pid)))
      continue;

    if (gst_mpp_fault_correlator_add_session (corr, pending_index))
      added++;
  }
  g_strfreev (lines);
  return added;
}

/* -------------------------------------------------------------- tracefs I/O */

static gboolean
gst_mpp_fault_write_file (const gchar * path, const gchar * value)
{
  gint fd = open (path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  gssize written;

  if (fd < 0)
    return FALSE;

  written = write (fd, value, strlen (value));
  close (fd);
  return written == (gssize) strlen (value);
}

static gboolean
gst_mpp_fault_enable_event (const gchar * instance, const gchar * event)
{
  gchar *path = g_build_filename (instance, MPP_EVENT_DIR, event, "enable",
      NULL);
  gboolean ok = gst_mpp_fault_write_file (path, "1");

  g_free (path);
  return ok;
}

static gchar *
gst_mpp_fault_tracefs_root (void)
{
  const gchar *env = g_getenv ("GST_MPP_FAULT_TRACEFS");

  if (env != NULL && env[0] != '\0')
    return g_strdup (env);
  if (g_file_test (TRACEFS_PRIMARY, G_FILE_TEST_IS_DIR))
    return g_strdup (TRACEFS_PRIMARY);
  if (g_file_test (TRACEFS_FALLBACK, G_FILE_TEST_IS_DIR))
    return g_strdup (TRACEFS_FALLBACK);
  return NULL;
}

static gboolean
gst_mpp_fault_enabled (void)
{
  const gchar *env = g_getenv ("GST_MPP_FAULT_BRIDGE");

  /* Opt-in. The mechanism and its correlation rules are covered by host tests,
   * but no board has yet produced a real injected fault through this path, so
   * arming it by default would ship an untested recovery trigger. */
  return env != NULL && g_str_equal (env, "1");
}

static guint
gst_mpp_fault_threshold (void)
{
  const gchar *env = g_getenv ("GST_MPP_FAULT_BRIDGE_THRESHOLD");
  gint64 value;

  if (env == NULL || env[0] == '\0')
    return GST_MPP_FAULT_THRESHOLD;

  value = g_ascii_strtoll (env, NULL, 10);
  if (value < 1 || value > 1000)
    return GST_MPP_FAULT_THRESHOLD;
  return (guint) value;
}

static gchar *
gst_mpp_fault_create_instance (const gchar * root, GstObject * parent)
{
  static gint counter = 0;
  gchar *instances = g_build_filename (root, "instances", NULL);
  gchar *name;
  gchar *path;

  if (!g_file_test (instances, G_FILE_TEST_IS_DIR)) {
    GST_INFO_OBJECT (parent, "no tracefs instances directory at %s", instances);
    g_free (instances);
    return NULL;
  }

  name = g_strdup_printf ("ceralive-mppenc-%d-%d", (gint) getpid (),
      g_atomic_int_add (&counter, 1));
  path = g_build_filename (instances, name, NULL);
  g_free (instances);
  g_free (name);

  /* A private instance owns its own ring buffer and its own per-event enable
   * state, so an operator or another tool touching the global buffer cannot
   * disturb this reader, and this reader cannot disturb them. */
  if (g_mkdir_with_parents (path, 0755) != 0) {
    GST_INFO_OBJECT (parent, "cannot create trace instance %s: %s", path,
        g_strerror (errno));
    g_free (path);
    return NULL;
  }
  return path;
}

GstMppFaultBridge *
gst_mpp_fault_bridge_new (GstObject * parent)
{
  GstMppFaultBridge *bridge;
  const gchar *supplied;
  const gchar *sessions;
  gchar *instance = NULL;
  gchar *created = NULL;
  gchar *events;
  gchar *pipe_path;
  gchar *buffer_path;
  gint fd;

  static gsize once = 0;
  if (g_once_init_enter (&once)) {
    GST_DEBUG_CATEGORY_INIT (mppfaultbridge_debug, "mppfaultbridge", 0,
        "Rockchip MPP kernel fault bridge");
    g_once_init_leave (&once, 1);
  }

  if (!gst_mpp_fault_enabled ())
    return NULL;

  supplied = g_getenv ("GST_MPP_FAULT_INSTANCE");
  if (supplied != NULL && supplied[0] != '\0') {
    instance = g_strdup (supplied);
  } else {
    gchar *root = gst_mpp_fault_tracefs_root ();

    if (root == NULL) {
      GST_INFO_OBJECT (parent, "no tracefs mount; kernel fault bridge off");
      return NULL;
    }
    created = gst_mpp_fault_create_instance (root, parent);
    g_free (root);
    if (created == NULL)
      return NULL;
    instance = g_strdup (created);
  }

  /* No island driver, no events. This is the ordinary outcome on a host build
   * and on any non-RK3588 target. */
  events = g_build_filename (instance, MPP_EVENT_DIR, NULL);
  if (!g_file_test (events, G_FILE_TEST_IS_DIR)) {
    GST_INFO_OBJECT (parent, "no rockchip_mpp trace events at %s", events);
    g_free (events);
    if (created != NULL) {
      g_rmdir (created);
      g_free (created);
    }
    g_free (instance);
    return NULL;
  }
  g_free (events);

  /* The three events the join needs. mpp_task_done is hygiene only -- it
   * retires completed entries so the bounded map stays useful -- so its
   * absence is tolerated. */
  if (!gst_mpp_fault_enable_event (instance, "mpp_task_queued") ||
      !gst_mpp_fault_enable_event (instance, "mpp_core_selected") ||
      !gst_mpp_fault_enable_event (instance, "mpp_task_error")) {
    GST_WARNING_OBJECT (parent,
        "cannot enable rockchip_mpp trace events; fault bridge off");
    if (created != NULL) {
      g_rmdir (created);
      g_free (created);
    }
    g_free (instance);
    return NULL;
  }
  gst_mpp_fault_enable_event (instance, "mpp_task_done");

  buffer_path = g_build_filename (instance, "buffer_size_kb", NULL);
  gst_mpp_fault_write_file (buffer_path, TRACE_BUFFER_SIZE_KB);
  g_free (buffer_path);

  pipe_path = g_build_filename (instance, "trace_pipe", NULL);
  fd = open (pipe_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    GST_WARNING_OBJECT (parent, "cannot open %s: %s", pipe_path,
        g_strerror (errno));
    g_free (pipe_path);
    if (created != NULL) {
      g_rmdir (created);
      g_free (created);
    }
    g_free (instance);
    return NULL;
  }
  g_free (pipe_path);

  sessions = g_getenv ("GST_MPP_FAULT_SESSIONS");

  bridge = g_new0 (GstMppFaultBridge, 1);
  gst_mpp_fault_correlator_init (&bridge->corr);
  bridge->created_instance = created;
  bridge->instance_dir = instance;
  bridge->sessions_path = g_strdup ((sessions != NULL && sessions[0] != '\0')
      ? sessions : SESSIONS_SUMMARY);
  bridge->pipe_fd = fd;
  bridge->partial = g_string_new (NULL);
  bridge->threshold = gst_mpp_fault_threshold ();

  GST_INFO_OBJECT (parent, "kernel fault bridge armed on %s (threshold %u)",
      bridge->instance_dir, bridge->threshold);
  return bridge;
}

void
gst_mpp_fault_bridge_free (GstMppFaultBridge * bridge)
{
  if (bridge == NULL)
    return;

  if (bridge->pipe_fd >= 0)
    close (bridge->pipe_fd);
  if (bridge->created_instance != NULL) {
    g_rmdir (bridge->created_instance);
    g_free (bridge->created_instance);
  }
  g_free (bridge->instance_dir);
  g_free (bridge->sessions_path);
  if (bridge->partial != NULL)
    g_string_free (bridge->partial, TRUE);
  g_free (bridge);
}

guint
gst_mpp_fault_bridge_resolve_sessions (GstMppFaultBridge * bridge,
    GstObject * parent)
{
  GHashTable *tids;
  gchar *summary = NULL;
  GError *error = NULL;
  guint added;

  if (bridge == NULL)
    return 0;

  tids = gst_mpp_fault_read_own_tids ();
  if (tids == NULL) {
    GST_WARNING_OBJECT (parent, "cannot read /proc/self/task");
    return 0;
  }

  if (!g_file_get_contents (bridge->sessions_path, &summary, NULL, &error)) {
    GST_INFO_OBJECT (parent, "cannot read %s: %s", bridge->sessions_path,
        error != NULL ? error->message : "unknown error");
    g_clear_error (&error);
    g_hash_table_unref (tids);
    return 0;
  }

  added = gst_mpp_fault_sessions_from_summary (summary, tids, &bridge->corr);
  g_free (summary);
  g_hash_table_unref (tids);

  /* Resolved even when nothing was added: a successful read that found no
   * session of ours is a real answer, and it keeps every fault FOREIGN rather
   * than UNATTRIBUTED. */
  gst_mpp_fault_correlator_mark_resolved (&bridge->corr);

  GST_INFO_OBJECT (parent, "resolved %u new owned rkvenc session(s), %u total",
      added, bridge->corr.n_owned);
  return added;
}

static gboolean
gst_mpp_fault_account (GstMppFaultBridge * bridge, gint64 now)
{
  if (bridge->window_started == 0 ||
      now - bridge->window_started >= GST_MPP_FAULT_WINDOW_US) {
    bridge->window_started = now;
    bridge->window_faults = 0;
  }

  bridge->window_faults++;
  if (bridge->window_faults < bridge->threshold)
    return FALSE;

  /* Reset so a wedged encoder producing a fault per frame asks for at most one
   * restart per window; the restart budget itself is the outer bound. */
  bridge->window_started = 0;
  bridge->window_faults = 0;
  return TRUE;
}

gboolean
gst_mpp_fault_bridge_poll (GstMppFaultBridge * bridge, GstObject * parent)
{
  gchar buffer[TRACE_READ_CHUNK];
  gboolean crossed = FALSE;
  gint64 now;
  guint reads;

  if (bridge == NULL || bridge->pipe_fd < 0)
    return FALSE;

  now = g_get_monotonic_time ();
  if (bridge->last_poll != 0 && now - bridge->last_poll < POLL_INTERVAL_US)
    return FALSE;
  bridge->last_poll = now;

  for (reads = 0; reads < TRACE_READS_PER_POLL; reads++) {
    gssize got = read (bridge->pipe_fd, buffer, sizeof (buffer));
    gchar *start;
    gchar *newline;

    if (got <= 0) {
      if (got < 0 && errno == EINTR)
        continue;
      break;
    }

    g_string_append_len (bridge->partial, buffer, got);

    start = bridge->partial->str;
    while ((newline = strchr (start, '\n')) != NULL) {
      GstMppFaultLineVerdict verdict;

      *newline = '\0';
      verdict = gst_mpp_fault_correlator_feed_line (&bridge->corr, start);
      start = newline + 1;

      switch (verdict) {
        case GST_MPP_FAULT_LINE_FAULT_OWNED:
          GST_WARNING_OBJECT (parent,
              "kernel reported an RKVENC task error on one of our sessions");
          if (gst_mpp_fault_account (bridge, now))
            crossed = TRUE;
          break;
        case GST_MPP_FAULT_LINE_FAULT_FOREIGN:
          GST_DEBUG_OBJECT (parent,
              "ignoring RKVENC task error owned by another session");
          break;
        case GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED:
          GST_DEBUG_OBJECT (parent,
              "ignoring RKVENC task error that could not be attributed");
          break;
        case GST_MPP_FAULT_LINE_LOST_EVENTS:
          GST_WARNING_OBJECT (parent,
              "trace buffer overran; a fault may go unattributed");
          break;
        default:
          break;
      }
    }

    g_string_erase (bridge->partial, 0,
        (gssize) (start - bridge->partial->str));

    /* A record longer than any plausible trace line means we are accumulating
     * something that is not line oriented. Drop it rather than grow forever. */
    if (bridge->partial->len > 64 * 1024)
      g_string_truncate (bridge->partial, 0);
  }

  return crossed;
}

guint64
gst_mpp_fault_bridge_owned_faults (const GstMppFaultBridge * bridge)
{
  return bridge != NULL ? bridge->corr.owned_faults : 0;
}
