/* Correlation and ownership rules of the cross-layer kernel fault bridge.
 *
 * Everything here is driven with SYNTHETIC trace lines in the exact format the
 * island's mpp_trace.h TP_printk emits, and with synthetic procfs text. No
 * kernel, no board and no MPP context is involved, so what these cases prove is
 * the join and the isolation rule -- not that a real RKVENC fault reaches them.
 */

#include <gst/check/gstcheck.h>

#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "../../gst/rockchipmpp/gstmppfaultbridge.h"

/* Verbatim shapes from rk3588-media-island tests/fixtures/telemetry/, including
 * the "kworker/0:1-22" comm whose own colon and dash a naive strstr on the
 * event name would trip over. */
#define QUEUED(task, session)                                                  \
  "kworker/0:1-22 [000] .... 10.000001: mpp_task_queued: "                     \
  "session=" #session " task=" #task " client=16"
#define CORE_SEL(task, core)                                                   \
  "kworker/0:1-22 [000] .... 10.000002: mpp_core_selected: "                   \
  "task=" #task " core=" #core " idle=0x3"
#define DONE(task, core)                                                       \
  "kworker/0:1-22 [000] .... 10.001003: mpp_task_done: "                       \
  "core=" #core " task=" #task " ns=1000000"
#define ERROR(task, core)                                                      \
  "irq/78-rkvenc-99 [002] d.h1. 10.501003: mpp_task_error: "                   \
  "core=" #core " task=" #task " irq_status=0x100"

static GstMppFaultCorrelator corr;

static void setup_resolved_owner(guint32 session) {
  gst_mpp_fault_correlator_init(&corr);
  gst_mpp_fault_correlator_add_session(&corr, session);
  gst_mpp_fault_correlator_mark_resolved(&corr);
}

static GstMppFaultLineVerdict feed(const gchar *line) {
  return gst_mpp_fault_correlator_feed_line(&corr, line);
}

/* --------------------------------------------------------------- the join */

GST_START_TEST(test_queued_core_error_joins_to_owned_session) {
  setup_resolved_owner(11);

  fail_unless_equals_int(feed(QUEUED(41, 11)), GST_MPP_FAULT_LINE_QUEUED);
  fail_unless_equals_int(feed(CORE_SEL(41, 0)),
                         GST_MPP_FAULT_LINE_CORE_SELECTED);
  fail_unless_equals_int(feed(ERROR(41, 0)), GST_MPP_FAULT_LINE_FAULT_OWNED);

  fail_unless_equals_uint64(corr.owned_faults, 1);
  fail_unless_equals_uint64(corr.foreign_faults, 0);
  fail_unless_equals_uint64(corr.unattributed_faults, 0);
}

GST_END_TEST;

/* THE isolation guarantee. cerastream runs an encoder in a CHILD PROCESS for
 * its capture probe, and that child's MPP session is a different session
 * belonging to a different pid. A fault there must never restart the live
 * program's encoder. */
GST_START_TEST(test_foreign_session_fault_is_never_owned) {
  setup_resolved_owner(11);

  fail_unless_equals_int(feed(QUEUED(41, 15)), GST_MPP_FAULT_LINE_QUEUED);
  fail_unless_equals_int(feed(CORE_SEL(41, 0)),
                         GST_MPP_FAULT_LINE_CORE_SELECTED);
  fail_unless_equals_int(feed(ERROR(41, 0)), GST_MPP_FAULT_LINE_FAULT_FOREIGN);

  fail_unless_equals_uint64(corr.owned_faults, 0);
  fail_unless_equals_uint64(corr.foreign_faults, 1);

  /* Positive control in the same correlator state: the owned session still
   * resolves, so the rejection above is a decision and not an inert path. */
  fail_unless_equals_int(feed(QUEUED(42, 11)), GST_MPP_FAULT_LINE_QUEUED);
  fail_unless_equals_int(feed(CORE_SEL(42, 0)),
                         GST_MPP_FAULT_LINE_CORE_SELECTED);
  fail_unless_equals_int(feed(ERROR(42, 0)), GST_MPP_FAULT_LINE_FAULT_OWNED);
  fail_unless_equals_uint64(corr.owned_faults, 1);
}

GST_END_TEST;

/* task_id is atomic_fetch_inc() per TASKQUEUE, not global, so two queues both
 * start at 0 and their ids collide. mpp_task_error carries core_id but not the
 * queue, so (task_id, core_id) is what disambiguates -- which is why the join
 * needs mpp_core_selected as a third hop rather than task_id alone. */
GST_START_TEST(test_cross_taskqueue_task_id_collision_disambiguated_by_core) {
  setup_resolved_owner(11);

  /* Same task_id 41 on two taskqueues, each fully bound before the next is
   * queued. Ours lands on core 0, the foreign session's on core 1. */
  fail_unless_equals_int(feed(QUEUED(41, 11)), GST_MPP_FAULT_LINE_QUEUED);
  fail_unless_equals_int(feed(CORE_SEL(41, 0)),
                         GST_MPP_FAULT_LINE_CORE_SELECTED);
  fail_unless_equals_int(feed(QUEUED(41, 15)), GST_MPP_FAULT_LINE_QUEUED);
  fail_unless_equals_int(feed(CORE_SEL(41, 1)),
                         GST_MPP_FAULT_LINE_CORE_SELECTED);

  /* The foreign one faults. A task_id-only join would have matched ours. */
  fail_unless_equals_int(feed(ERROR(41, 1)), GST_MPP_FAULT_LINE_FAULT_FOREIGN);
  fail_unless_equals_uint64(corr.owned_faults, 0);

  /* Ours is still joinable afterwards, on its own core. */
  fail_unless_equals_int(feed(ERROR(41, 0)), GST_MPP_FAULT_LINE_FAULT_OWNED);
  fail_unless_equals_uint64(corr.owned_faults, 1);
}

GST_END_TEST;

/* When two taskqueues hold the same id at the SAME time, mpp_core_selected
 * cannot say which one the core belongs to. Binding either is a coin flip
 * whose wrong side restarts this encoder on another process's fault, so both
 * candidates are abandoned and the faults go unattributed. */
GST_START_TEST(test_simultaneous_task_id_collision_is_refused_not_guessed) {
  setup_resolved_owner(11);

  fail_unless_equals_int(feed(QUEUED(41, 11)), GST_MPP_FAULT_LINE_QUEUED);
  fail_unless_equals_int(feed(QUEUED(41, 15)), GST_MPP_FAULT_LINE_QUEUED);
  fail_unless_equals_int(feed(CORE_SEL(41, 0)), GST_MPP_FAULT_LINE_AMBIGUOUS);

  fail_unless_equals_int(feed(ERROR(41, 0)),
                         GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED);
  fail_unless_equals_int(feed(ERROR(41, 1)),
                         GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED);
  fail_unless_equals_uint64(corr.owned_faults, 0);
  fail_unless_equals_uint64(corr.foreign_faults, 0);

  /* The abandoned id leaves nothing stale behind: the next clean pair on the
   * same id resolves normally. */
  fail_unless_equals_int(feed(QUEUED(41, 11)), GST_MPP_FAULT_LINE_QUEUED);
  fail_unless_equals_int(feed(CORE_SEL(41, 0)),
                         GST_MPP_FAULT_LINE_CORE_SELECTED);
  fail_unless_equals_int(feed(ERROR(41, 0)), GST_MPP_FAULT_LINE_FAULT_OWNED);
  fail_unless_equals_uint64(corr.owned_faults, 1);
}

GST_END_TEST;

/* mpp_core_selected binds an entry that is still unbound, so a second core
 * announcement with no new queue event has nothing to bind. */
GST_START_TEST(test_core_selected_binds_only_an_unassigned_entry) {
  setup_resolved_owner(11);

  feed(QUEUED(41, 11));
  feed(CORE_SEL(41, 0));
  fail_unless_equals_int(feed(CORE_SEL(41, 1)), GST_MPP_FAULT_LINE_IGNORED);

  /* So core 1 has no entry and cannot be attributed to us. */
  fail_unless_equals_int(feed(ERROR(41, 1)),
                         GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED);
  fail_unless_equals_uint64(corr.owned_faults, 0);
}

GST_END_TEST;

/* A fault whose queue event was never seen -- tracing started late, or the ring
 * overran -- is unattributable. Guessing "ours" here is exactly the regression
 * that would restart a healthy stream. */
GST_START_TEST(test_fault_without_a_queued_event_is_unattributed) {
  setup_resolved_owner(11);

  fail_unless_equals_int(feed(ERROR(41, 0)),
                         GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED);
  fail_unless_equals_uint64(corr.owned_faults, 0);
  fail_unless_equals_uint64(corr.unattributed_faults, 1);
}

GST_END_TEST;

/* Until ownership has been resolved once, "we do not know whose this is" must
 * not read as "ours". */
GST_START_TEST(test_fault_before_ownership_resolution_is_unattributed) {
  gst_mpp_fault_correlator_init(&corr);
  gst_mpp_fault_correlator_add_session(&corr, 11);
  /* deliberately not marked resolved */

  feed(QUEUED(41, 11));
  feed(CORE_SEL(41, 0));
  fail_unless_equals_int(feed(ERROR(41, 0)),
                         GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED);
  fail_unless_equals_uint64(corr.owned_faults, 0);
}

GST_END_TEST;

/* A completed task is retired, so its (task_id, core_id) cannot shadow a later
 * reuse of the same pair by another session. */
GST_START_TEST(test_done_retires_the_entry) {
  setup_resolved_owner(11);

  feed(QUEUED(41, 11));
  feed(CORE_SEL(41, 0));
  fail_unless_equals_int(feed(DONE(41, 0)), GST_MPP_FAULT_LINE_RETIRED);

  fail_unless_equals_int(feed(ERROR(41, 0)),
                         GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED);
  fail_unless_equals_uint64(corr.owned_faults, 0);

  /* The reused pair now belongs to somebody else, and resolves that way. */
  feed(QUEUED(41, 15));
  feed(CORE_SEL(41, 0));
  fail_unless_equals_int(feed(ERROR(41, 0)), GST_MPP_FAULT_LINE_FAULT_FOREIGN);
}

GST_END_TEST;

/* An error consumes its entry too, so one queued task cannot be charged twice.
 */
GST_START_TEST(test_error_consumes_its_entry) {
  setup_resolved_owner(11);

  feed(QUEUED(41, 11));
  feed(CORE_SEL(41, 0));
  fail_unless_equals_int(feed(ERROR(41, 0)), GST_MPP_FAULT_LINE_FAULT_OWNED);
  fail_unless_equals_int(feed(ERROR(41, 0)),
                         GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED);
  fail_unless_equals_uint64(corr.owned_faults, 1);
}

GST_END_TEST;

/* The map is bounded, so a long-running stream cannot grow it without limit.
 * Eviction degrades a fault to UNATTRIBUTED, never to a wrong owner. */
GST_START_TEST(test_task_map_is_bounded) {
  guint i;
  gchar *line;

  setup_resolved_owner(11);

  line = g_strdup_printf("kworker/0:1-22 [000] .... 10.0: "
                         "mpp_task_queued: session=11 task=%u client=16",
                         1u);
  feed(line);
  g_free(line);
  feed("kworker/0:1-22 [000] .... 10.0: mpp_core_selected: "
       "task=1 core=0 idle=0x3");

  /* Push the first entry out with a full map's worth of newer ones. */
  for (i = 0; i < GST_MPP_FAULT_TASK_MAP_SIZE; i++) {
    line = g_strdup_printf("kworker/0:1-22 [000] .... 10.0: "
                           "mpp_task_queued: session=11 task=%u client=16",
                           i + 1000);
    feed(line);
    g_free(line);
  }

  fail_unless_equals_int(feed("irq/78-rkvenc-99 [002] d.h1. 11.0: "
                              "mpp_task_error: core=0 task=1 irq_status=0x100"),
                         GST_MPP_FAULT_LINE_FAULT_UNATTRIBUTED);
}

GST_END_TEST;

/* ------------------------------------------------------------ line parsing */

/* The event name is matched at a token boundary, because a trace record starts
 * with the task comm and a comm legitimately contains both ':' and '-'. */
GST_START_TEST(test_comm_containing_colon_and_dash_parses) {
  setup_resolved_owner(11);

  fail_unless_equals_int(feed("kworker/0:1-22 [000] .... 10.000001: "
                              "mpp_task_queued: session=11 task=41 client=16"),
                         GST_MPP_FAULT_LINE_QUEUED);
  fail_unless_equals_int(feed("mpp_task_error-9 [001] .... 10.1: "
                              "mpp_core_selected: task=41 core=0 idle=0x3"),
                         GST_MPP_FAULT_LINE_CORE_SELECTED);
  fail_unless_equals_int(
      feed("cerastream:enc-3964 [002] d.h1. 10.5: "
           "mpp_task_error: core=0 task=41 irq_status=0x100"),
      GST_MPP_FAULT_LINE_FAULT_OWNED);
}

GST_END_TEST;

GST_START_TEST(test_noise_and_headers_are_ignored) {
  setup_resolved_owner(11);

  fail_unless_equals_int(feed(""), GST_MPP_FAULT_LINE_IGNORED);
  fail_unless_equals_int(feed("# tracer: nop"), GST_MPP_FAULT_LINE_IGNORED);
  fail_unless_equals_int(feed("sh-1 [000] .... 1.0: mpp_task_started: "
                              "core=0 task=41"),
                         GST_MPP_FAULT_LINE_IGNORED);
  fail_unless_equals_int(feed("sh-1 [000] .... 1.0: mpp_reset: "
                              "core=0 reason=2"),
                         GST_MPP_FAULT_LINE_IGNORED);
  /* Truncated payloads must not be guessed at. */
  fail_unless_equals_int(feed("sh-1 [000] .... 1.0: mpp_task_queued: "
                              "session=11"),
                         GST_MPP_FAULT_LINE_IGNORED);
}

GST_END_TEST;

/* mpp_task_queued is per-frame, so a small ring can drop join keys. That is a
 * health signal, not a correctness problem: a lost key downgrades a later
 * fault to UNATTRIBUTED. */
GST_START_TEST(test_lost_events_marker_is_counted) {
  setup_resolved_owner(11);

  fail_unless_equals_int(feed("CPU:1 [LOST 128 EVENTS]"),
                         GST_MPP_FAULT_LINE_LOST_EVENTS);
  fail_unless_equals_uint64(corr.lost_events, 1);
}

GST_END_TEST;

/* -------------------------------------------------------- owned session set */

/* Verbatim shape of a real Orange Pi 5+ capture, IOVA dump and encoder table
 * included -- the abbreviated form in the island's own docs is not what a board
 * prints, so the parser scans for the two lines it needs. */
#define REAL_SUMMARY_HEAD                                                      \
  "session iova range dump:\n"                                                 \
  "   0: 0x00000000fe000000..0x00000000febddfff (     12152 KiB)\n"            \
  "   1: 0x00000000fb000000..0x00000000fbbddfff (     12152 KiB)\n"
#define REAL_SUMMARY_TABLE                                                     \
  "------------------------------------------------------------\n"             \
  "| session|  device|   width|  height|  format|\n"                           \
  "|      11|  RKVENC|    3840|    2160|    h265|\n"

GST_START_TEST(test_summary_selects_only_our_own_rkvenc_sessions) {
  GHashTable *tids = g_hash_table_new(g_direct_hash, g_direct_equal);
  gchar *summary;
  guint added;

  g_hash_table_add(tids, GUINT_TO_POINTER(3964u));

  summary =
      g_strconcat(REAL_SUMMARY_HEAD,
                  "session: pid=3964 index=11\n"
                  " device: fdbd0000.rkvenc-core\n"
                  " memory: 50 MiB\n",
                  REAL_SUMMARY_TABLE, REAL_SUMMARY_HEAD,
                  /* another process entirely -- the capture-probe child */
                  "session: pid=4026 index=15\n"
                  " device: fdbd0000.rkvenc-core\n"
                  " memory: 50 MiB\n",
                  REAL_SUMMARY_TABLE, NULL);

  gst_mpp_fault_correlator_init(&corr);
  added = gst_mpp_fault_sessions_from_summary(summary, tids, &corr);

  fail_unless_equals_int(added, 1);
  fail_unless(gst_mpp_fault_correlator_owns_session(&corr, 11));
  fail_if(gst_mpp_fault_correlator_owns_session(&corr, 15));

  g_free(summary);
  g_hash_table_unref(tids);
}

GST_END_TEST;

/* A decode or RGA session of ours is not an encode session, and must not let an
 * unrelated fault reach the encoder. */
GST_START_TEST(test_summary_ignores_non_rkvenc_devices) {
  GHashTable *tids = g_hash_table_new(g_direct_hash, g_direct_equal);
  const gchar *summary = "session: pid=3964 index=7\n"
                         " device: fdc38000.rkvdec-core\n"
                         "session: pid=3964 index=8\n"
                         " device: fdb60000.rga\n";

  g_hash_table_add(tids, GUINT_TO_POINTER(3964u));
  gst_mpp_fault_correlator_init(&corr);

  fail_unless_equals_int(
      gst_mpp_fault_sessions_from_summary(summary, tids, &corr), 0);
  fail_unless_equals_int(corr.n_owned, 0);

  g_hash_table_unref(tids);
}

GST_END_TEST;

/* A session record with no device line must not inherit the next record's
 * device, and a malformed record must not abort the scan. */
GST_START_TEST(test_summary_parses_defensively) {
  GHashTable *tids = g_hash_table_new(g_direct_hash, g_direct_equal);
  const gchar *summary = "session: pid=3964\n"
                         "session: index=9\n"
                         "session: pid=3964 index=12\n"
                         "session: pid=3964 index=13\n"
                         " device: fdbd0000.rkvenc-core\n";

  g_hash_table_add(tids, GUINT_TO_POINTER(3964u));
  gst_mpp_fault_correlator_init(&corr);

  fail_unless_equals_int(
      gst_mpp_fault_sessions_from_summary(summary, tids, &corr), 1);
  fail_unless(gst_mpp_fault_correlator_owns_session(&corr, 13));
  fail_if(gst_mpp_fault_correlator_owns_session(&corr, 12));

  g_hash_table_unref(tids);
}

GST_END_TEST;

GST_START_TEST(test_owned_session_set_unions_and_is_bounded) {
  guint i;

  gst_mpp_fault_correlator_init(&corr);

  fail_unless(gst_mpp_fault_correlator_add_session(&corr, 11));
  /* A repeated resolve must not double-count; it is a union, not an append. */
  fail_if(gst_mpp_fault_correlator_add_session(&corr, 11));
  fail_unless_equals_int(corr.n_owned, 1);

  for (i = 0; i < GST_MPP_FAULT_MAX_OWNED_SESSIONS + 8; i++)
    gst_mpp_fault_correlator_add_session(&corr, 100 + i);

  fail_unless_equals_int(corr.n_owned, GST_MPP_FAULT_MAX_OWNED_SESSIONS);
  /* Newest survives, oldest is dropped. */
  fail_unless(gst_mpp_fault_correlator_owns_session(
      &corr, 100 + GST_MPP_FAULT_MAX_OWNED_SESSIONS + 7));
  fail_if(gst_mpp_fault_correlator_owns_session(&corr, 11));
}

GST_END_TEST;

/* ------------------------------------------------------------ bridge arming */

static gchar *make_instance(gboolean with_events) {
  gchar *dir = g_dir_make_tmp("ceralive-faultbridge-XXXXXX", NULL);
  gchar *pipe_path;

  fail_unless(dir != NULL);

  if (with_events) {
    const gchar *events[] = {"mpp_task_queued", "mpp_core_selected",
                             "mpp_task_error", "mpp_task_done"};
    guint i;

    for (i = 0; i < G_N_ELEMENTS(events); i++) {
      gchar *event_dir =
          g_build_filename(dir, "events", "rockchip_mpp", events[i], NULL);
      gchar *enable = g_build_filename(event_dir, "enable", NULL);

      fail_unless_equals_int(g_mkdir_with_parents(event_dir, 0755), 0);
      fail_unless(g_file_set_contents(enable, "0\n", -1, NULL));
      g_free(enable);
      g_free(event_dir);
    }
  }

  pipe_path = g_build_filename(dir, "trace_pipe", NULL);
  fail_unless(g_file_set_contents(pipe_path, "", 0, NULL));
  g_free(pipe_path);
  return dir;
}

/* The bridge holds an open fd on trace_pipe, so new records must be APPENDED.
 * g_file_set_contents() would rename a replacement into place and leave the
 * reader on the old inode, which is exactly the shape of a test that passes by
 * observing nothing. */
static void append_trace(const gchar *path, const gchar *text) {
  gint fd = open(path, O_WRONLY | O_APPEND);

  fail_unless(fd >= 0);
  fail_unless_equals_int(write(fd, text, strlen(text)), (gint)strlen(text));
  close(fd);
}

static void remove_tree(const gchar *path) {
  GDir *dir = g_dir_open(path, 0, NULL);
  const gchar *name;

  if (dir != NULL) {
    while ((name = g_dir_read_name(dir)) != NULL) {
      gchar *child = g_build_filename(path, name, NULL);

      if (g_file_test(child, G_FILE_TEST_IS_DIR))
        remove_tree(child);
      else
        g_unlink(child);
      g_free(child);
    }
    g_dir_close(dir);
  }
  g_rmdir(path);
}

GST_START_TEST(test_bridge_is_off_unless_explicitly_enabled) {
  gchar *dir = make_instance(TRUE);

  g_unsetenv("GST_MPP_FAULT_BRIDGE");
  g_setenv("GST_MPP_FAULT_INSTANCE", dir, TRUE);

  fail_unless(gst_mpp_fault_bridge_new(NULL) == NULL);

  g_unsetenv("GST_MPP_FAULT_INSTANCE");
  remove_tree(dir);
  g_free(dir);
}

GST_END_TEST;

/* No island driver means no rockchip_mpp event directory, which is the
 * ordinary outcome on a host build and on any non-RK3588 target. It must
 * disable the bridge, not fail the element. */
GST_START_TEST(test_bridge_fails_open_without_rockchip_events) {
  gchar *dir = make_instance(FALSE);

  g_setenv("GST_MPP_FAULT_BRIDGE", "1", TRUE);
  g_setenv("GST_MPP_FAULT_INSTANCE", dir, TRUE);

  fail_unless(gst_mpp_fault_bridge_new(NULL) == NULL);

  g_unsetenv("GST_MPP_FAULT_BRIDGE");
  g_unsetenv("GST_MPP_FAULT_INSTANCE");
  remove_tree(dir);
  g_free(dir);
}

GST_END_TEST;

/* A tracefs root whose instance directory can be created but which carries no
 * rockchip_mpp events must leave nothing behind. */
GST_START_TEST(test_bridge_removes_the_instance_it_created) {
  gchar *root = g_dir_make_tmp("ceralive-tracefs-XXXXXX", NULL);
  gchar *instances = g_build_filename(root, "instances", NULL);

  fail_unless(root != NULL);
  fail_unless_equals_int(g_mkdir_with_parents(instances, 0755), 0);

  g_setenv("GST_MPP_FAULT_BRIDGE", "1", TRUE);
  g_unsetenv("GST_MPP_FAULT_INSTANCE");
  g_setenv("GST_MPP_FAULT_TRACEFS", root, TRUE);

  fail_unless(gst_mpp_fault_bridge_new(NULL) == NULL);

  {
    GDir *dir = g_dir_open(instances, 0, NULL);

    fail_unless(dir != NULL);
    fail_unless(g_dir_read_name(dir) == NULL);
    g_dir_close(dir);
  }

  g_unsetenv("GST_MPP_FAULT_BRIDGE");
  g_unsetenv("GST_MPP_FAULT_TRACEFS");
  g_free(instances);
  remove_tree(root);
  g_free(root);
}

GST_END_TEST;

/* End to end through the real reader: enable writes, a real fd on trace_pipe,
 * partial-line reassembly, ownership from a real procfs read of this process's
 * own thread ids, and the threshold. */
GST_START_TEST(test_bridge_reads_pipe_and_reports_threshold) {
  gchar *dir = make_instance(TRUE);
  gchar *pipe_path = g_build_filename(dir, "trace_pipe", NULL);
  gchar *sessions_path = g_build_filename(dir, "sessions-summary", NULL);
  gchar *summary;
  gchar *enabled = NULL;
  gchar *enable_path;
  GstMppFaultBridge *bridge;
  GString *trace;

  /* The summary's pid field is a TID. This process's main thread has TID ==
   * pid, so naming it makes exactly one session ours. */
  summary = g_strdup_printf("session: pid=%d index=11\n"
                            " device: fdbd0000.rkvenc-core\n"
                            "session: pid=999999 index=15\n"
                            " device: fdbd0000.rkvenc-core\n",
                            (gint)getpid());
  fail_unless(g_file_set_contents(sessions_path, summary, -1, NULL));

  g_setenv("GST_MPP_FAULT_BRIDGE", "1", TRUE);
  g_setenv("GST_MPP_FAULT_INSTANCE", dir, TRUE);
  g_setenv("GST_MPP_FAULT_SESSIONS", sessions_path, TRUE);
  g_setenv("GST_MPP_FAULT_BRIDGE_THRESHOLD", "3", TRUE);

  bridge = gst_mpp_fault_bridge_new(NULL);
  fail_unless(bridge != NULL);

  /* The reader really wrote the event enables. */
  enable_path = g_build_filename(dir, "events", "rockchip_mpp",
                                 "mpp_task_error", "enable", NULL);
  fail_unless(g_file_get_contents(enable_path, &enabled, NULL, NULL));
  fail_unless_equals_string(enabled, "1");
  g_free(enabled);
  g_free(enable_path);

  fail_unless_equals_int(gst_mpp_fault_bridge_resolve_sessions(bridge, NULL),
                         1);

  /* Two owned faults plus a foreign one: below the threshold of three owned. */
  trace = g_string_new(NULL);
  g_string_append(trace,
                  QUEUED(41, 11) "\n" CORE_SEL(41, 0) "\n" ERROR(41, 0) "\n");
  g_string_append(trace,
                  QUEUED(42, 11) "\n" CORE_SEL(42, 0) "\n" ERROR(42, 0) "\n");
  g_string_append(trace,
                  QUEUED(43, 15) "\n" CORE_SEL(43, 1) "\n" ERROR(43, 1) "\n");
  append_trace(pipe_path, trace->str);

  fail_if(gst_mpp_fault_bridge_poll(bridge, NULL));
  fail_unless_equals_uint64(gst_mpp_fault_bridge_owned_faults(bridge), 2);

  /* The third owned fault crosses it. The poll interval is rate limited, so
   * wait past it rather than racing the clock. */
  append_trace(pipe_path,
               QUEUED(44, 11) "\n" CORE_SEL(44, 0) "\n" ERROR(44, 0) "\n");
  g_usleep(20000);

  fail_unless(gst_mpp_fault_bridge_poll(bridge, NULL));
  fail_unless_equals_uint64(gst_mpp_fault_bridge_owned_faults(bridge), 3);

  g_string_free(trace, TRUE);
  gst_mpp_fault_bridge_free(bridge);

  g_unsetenv("GST_MPP_FAULT_BRIDGE");
  g_unsetenv("GST_MPP_FAULT_INSTANCE");
  g_unsetenv("GST_MPP_FAULT_SESSIONS");
  g_unsetenv("GST_MPP_FAULT_BRIDGE_THRESHOLD");
  g_free(summary);
  g_free(sessions_path);
  g_free(pipe_path);
  remove_tree(dir);
  g_free(dir);
}

GST_END_TEST;

/* A trace record can straddle a read boundary; a half-line must be carried,
 * not dropped and not parsed. */
GST_START_TEST(test_bridge_reassembles_a_split_line) {
  gchar *dir = make_instance(TRUE);
  gchar *pipe_path = g_build_filename(dir, "trace_pipe", NULL);
  gchar *sessions_path = g_build_filename(dir, "sessions-summary", NULL);
  gchar *summary;
  GstMppFaultBridge *bridge;
  const gchar *whole = QUEUED(41, 11) "\n" CORE_SEL(41, 0) "\n" ERROR(41, 0);
  gchar *head;

  summary = g_strdup_printf("session: pid=%d index=11\n"
                            " device: fdbd0000.rkvenc-core\n",
                            (gint)getpid());
  fail_unless(g_file_set_contents(sessions_path, summary, -1, NULL));

  g_setenv("GST_MPP_FAULT_BRIDGE", "1", TRUE);
  g_setenv("GST_MPP_FAULT_INSTANCE", dir, TRUE);
  g_setenv("GST_MPP_FAULT_SESSIONS", sessions_path, TRUE);
  g_setenv("GST_MPP_FAULT_BRIDGE_THRESHOLD", "1", TRUE);

  bridge = gst_mpp_fault_bridge_new(NULL);
  fail_unless(bridge != NULL);
  fail_unless_equals_int(gst_mpp_fault_bridge_resolve_sessions(bridge, NULL),
                         1);

  /* Stop mid-way through the error record. */
  head = g_strndup(whole, strlen(whole) - 12);
  append_trace(pipe_path, head);
  fail_if(gst_mpp_fault_bridge_poll(bridge, NULL));
  fail_unless_equals_uint64(gst_mpp_fault_bridge_owned_faults(bridge), 0);

  /* Only the remainder, so the record is only complete once the two halves are
   * joined by the reader. */
  append_trace(pipe_path, whole + strlen(whole) - 12);
  append_trace(pipe_path, "\n");
  g_usleep(20000);
  fail_unless(gst_mpp_fault_bridge_poll(bridge, NULL));
  fail_unless_equals_uint64(gst_mpp_fault_bridge_owned_faults(bridge), 1);

  g_free(head);
  gst_mpp_fault_bridge_free(bridge);
  g_unsetenv("GST_MPP_FAULT_BRIDGE");
  g_unsetenv("GST_MPP_FAULT_INSTANCE");
  g_unsetenv("GST_MPP_FAULT_SESSIONS");
  g_unsetenv("GST_MPP_FAULT_BRIDGE_THRESHOLD");
  g_free(summary);
  g_free(sessions_path);
  g_free(pipe_path);
  remove_tree(dir);
  g_free(dir);
}

GST_END_TEST;

static Suite *fault_bridge_suite(void) {
  Suite *suite = suite_create("fault-bridge");
  TCase *join = tcase_create("join");
  TCase *parse = tcase_create("parse");
  TCase *sessions = tcase_create("sessions");
  TCase *bridge = tcase_create("bridge");

  tcase_add_test(join, test_queued_core_error_joins_to_owned_session);
  tcase_add_test(join, test_foreign_session_fault_is_never_owned);
  tcase_add_test(join,
                 test_cross_taskqueue_task_id_collision_disambiguated_by_core);
  tcase_add_test(join,
                 test_simultaneous_task_id_collision_is_refused_not_guessed);
  tcase_add_test(join, test_core_selected_binds_only_an_unassigned_entry);
  tcase_add_test(join, test_fault_without_a_queued_event_is_unattributed);
  tcase_add_test(join, test_fault_before_ownership_resolution_is_unattributed);
  tcase_add_test(join, test_done_retires_the_entry);
  tcase_add_test(join, test_error_consumes_its_entry);
  tcase_add_test(join, test_task_map_is_bounded);
  suite_add_tcase(suite, join);

  tcase_add_test(parse, test_comm_containing_colon_and_dash_parses);
  tcase_add_test(parse, test_noise_and_headers_are_ignored);
  tcase_add_test(parse, test_lost_events_marker_is_counted);
  suite_add_tcase(suite, parse);

  tcase_add_test(sessions, test_summary_selects_only_our_own_rkvenc_sessions);
  tcase_add_test(sessions, test_summary_ignores_non_rkvenc_devices);
  tcase_add_test(sessions, test_summary_parses_defensively);
  tcase_add_test(sessions, test_owned_session_set_unions_and_is_bounded);
  suite_add_tcase(suite, sessions);

  tcase_add_test(bridge, test_bridge_is_off_unless_explicitly_enabled);
  tcase_add_test(bridge, test_bridge_fails_open_without_rockchip_events);
  tcase_add_test(bridge, test_bridge_removes_the_instance_it_created);
  tcase_add_test(bridge, test_bridge_reads_pipe_and_reports_threshold);
  tcase_add_test(bridge, test_bridge_reassembles_a_split_line);
  suite_add_tcase(suite, bridge);

  return suite;
}

GST_CHECK_MAIN(fault_bridge);
