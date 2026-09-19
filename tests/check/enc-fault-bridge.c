/* The kernel fault bridge driving the encoder's own bounded restart path.
 *
 * This is the element-level half of the cross-layer error bridge: a synthetic
 * tracefs instance stands in for the island's ftrace output, a synthetic
 * sessions-summary stands in for /proc/mpp_service, and the real shipped
 * element decides what to do with them. The restart mechanism itself is
 * unchanged and is the one `encoder-restarts` already counts.
 *
 * What these cases do NOT prove: that a real RKVENC job timeout, IOMMU fault
 * or reset on real silicon produces the events fed here. That needs a board
 * running an edge-test kernel with the CeraLive MPP fault-injection seam, and
 * is a separate, explicitly scoped follow-up.
 */

#include "enc-test-common.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define CAPS "video/x-raw,format=NV12,width=320,height=240,framerate=30/1"

/* Exactly the TP_printk shapes in the island's mpp_trace.h. */
#define QUEUED(task, session)                                                  \
  "kworker/0:1-22 [000] .... 10.000001: mpp_task_queued: "                     \
  "session=" #session " task=" #task " client=16\n"
#define CORE_SEL(task, core)                                                   \
  "kworker/0:1-22 [000] .... 10.000002: mpp_core_selected: "                   \
  "task=" #task " core=" #core " idle=0x3\n"
#define FAULT(task, core)                                                      \
  "irq/78-rkvenc-99 [002] d.h1. 10.501003: mpp_task_error: "                   \
  "core=" #core " task=" #task " irq_status=0x100\n"

/* One complete join for a session we own, and one for a session we do not. */
#define OWNED_FAULT(task) QUEUED(task, 11) CORE_SEL(task, 0) FAULT(task, 0)
#define FOREIGN_FAULT(task) QUEUED(task, 15) CORE_SEL(task, 1) FAULT(task, 1)

static gchar *instance_dir;
static gchar *pipe_path;

static void write_file(const gchar *path, const gchar *text) {
  fail_unless(g_file_set_contents(path, text, -1, NULL));
}

/* The element holds an open fd on trace_pipe for its whole lifetime, so new
 * records are APPENDED. Replacing the file would leave the reader on the old
 * inode and the test would pass by observing nothing at all. */
static void append_trace(const gchar *text) {
  gint fd = open(pipe_path, O_WRONLY | O_APPEND);

  fail_unless(fd >= 0);
  fail_unless_equals_int(write(fd, text, strlen(text)), (gint)strlen(text));
  close(fd);
}

/* @foreign_only omits this process from the summary entirely, so no rkvenc
 * session is ours and every fault below must be rejected. */
static void setup_fault_env(const gchar *threshold) {
  const gchar *events[] = {"mpp_task_queued", "mpp_core_selected",
                           "mpp_task_error", "mpp_task_done"};
  gchar *sessions_path;
  gchar *summary;
  guint i;

  instance_dir = g_dir_make_tmp("ceralive-encfault-XXXXXX", NULL);
  fail_unless(instance_dir != NULL);

  for (i = 0; i < G_N_ELEMENTS(events); i++) {
    gchar *event_dir = g_build_filename(instance_dir, "events", "rockchip_mpp",
                                        events[i], NULL);
    gchar *enable = g_build_filename(event_dir, "enable", NULL);

    fail_unless_equals_int(g_mkdir_with_parents(event_dir, 0755), 0);
    write_file(enable, "0\n");
    g_free(enable);
    g_free(event_dir);
  }

  pipe_path = g_build_filename(instance_dir, "trace_pipe", NULL);
  fail_unless(g_file_set_contents(pipe_path, "", 0, NULL));

  /* The summary's pid field is the CREATING THREAD's TID. This process's main
   * thread has TID == pid, so index 11 is ours and index 15 belongs to a pid
   * that is not one of our threads -- the shape of cerastream's capture-probe
   * child process. */
  sessions_path = g_build_filename(instance_dir, "sessions-summary", NULL);
  summary = g_strdup_printf("session: pid=%d index=11\n"
                            " device: fdbd0000.rkvenc-core\n"
                            "session: pid=999999 index=15\n"
                            " device: fdbd0000.rkvenc-core\n",
                            (gint)getpid());
  write_file(sessions_path, summary);

  g_setenv("GST_MPP_FAULT_BRIDGE", "1", TRUE);
  g_setenv("GST_MPP_FAULT_INSTANCE", instance_dir, TRUE);
  g_setenv("GST_MPP_FAULT_SESSIONS", sessions_path, TRUE);
  g_setenv("GST_MPP_FAULT_BRIDGE_THRESHOLD", threshold, TRUE);

  g_free(summary);
  g_free(sessions_path);
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

static void teardown_fault_env(void) {
  g_unsetenv("GST_MPP_FAULT_BRIDGE");
  g_unsetenv("GST_MPP_FAULT_INSTANCE");
  g_unsetenv("GST_MPP_FAULT_SESSIONS");
  g_unsetenv("GST_MPP_FAULT_BRIDGE_THRESHOLD");

  remove_tree(instance_dir);
  g_clear_pointer(&pipe_path, g_free);
  g_clear_pointer(&instance_dir, g_free);
}

static guint64 read_uint64(GstHarness *h, const gchar *property) {
  guint64 value = 0;

  g_object_get(h->element, property, &value, NULL);
  return value;
}

/* The output task only runs while frames are outstanding, so the bridge is
 * only polled while the encoder has work. Keeping frames flowing is what gives
 * it the chance to observe the trace. */
static guint64 drive(GstHarness *h, guint *index, guint64 want_restarts,
                     guint budget_ms) {
  gint64 deadline = g_get_monotonic_time() + budget_ms * 1000;
  guint64 restarts;

  do {
    restarts = read_uint64(h, "encoder-restarts");
    if (want_restarts != 0 && restarts >= want_restarts)
      return restarts;
    fail_unless_equals_int(enc_test_push(h, (*index)++, 30), GST_FLOW_OK);
    g_usleep(2000);
  } while (g_get_monotonic_time() < deadline);

  return read_uint64(h, "encoder-restarts");
}

GST_START_TEST(test_owned_kernel_fault_drives_the_existing_restart_path) {
  GstHarness *h;
  guint index = 0;
  unsigned creates_before;
  unsigned destroys_before;

  mpp_mock_reset();
  setup_fault_env("3");

  h = enc_test_harness("mpph264enc", CAPS);
  /* Baselined AFTER the harness, because plugin registration probes each
   * encoder factory with its own create/destroy pair. */
  creates_before = mpp_mock_enc_create_calls();
  destroys_before = mpp_mock_enc_destroy_calls();

  /* Below the threshold: a single recovered task error is not a reason to tear
   * down a healthy stream. */
  append_trace(OWNED_FAULT(41) OWNED_FAULT(42));
  fail_unless_equals_uint64(drive(h, &index, 0, 300), 0);
  fail_unless_equals_uint64(read_uint64(h, "kernel-faults"), 2);

  /* The third crosses it and reaches gst_mpp_enc_restart_context(). */
  append_trace(OWNED_FAULT(43));
  fail_unless_equals_uint64(drive(h, &index, 1, 5000), 1);

  fail_unless_equals_uint64(read_uint64(h, "kernel-faults"), 3);
  fail_unless_equals_int(mpp_mock_enc_create_calls(), creates_before + 1);
  fail_unless_equals_int(mpp_mock_enc_destroy_calls(), destroys_before + 1);

  gst_harness_teardown(h);
  teardown_fault_env();
}

GST_END_TEST;

/* The safety-critical property. cerastream runs an encoder in a CHILD PROCESS
 * for its capture probe; a fault in that child's MPP session must never
 * restart the live program's encoder. */
GST_START_TEST(test_foreign_session_fault_never_restarts_this_encoder) {
  GstHarness *h;
  guint index = 0;

  mpp_mock_reset();
  setup_fault_env("3");

  h = enc_test_harness("mpph264enc", CAPS);

  /* Nine complete joins -- three times the threshold -- all belonging to a
   * session owned by another process. */
  append_trace(FOREIGN_FAULT(41) FOREIGN_FAULT(42) FOREIGN_FAULT(43)
                   FOREIGN_FAULT(44) FOREIGN_FAULT(45) FOREIGN_FAULT(46)
                       FOREIGN_FAULT(47) FOREIGN_FAULT(48) FOREIGN_FAULT(49));

  fail_unless_equals_uint64(drive(h, &index, 0, 600), 0);
  fail_unless_equals_uint64(read_uint64(h, "kernel-faults"), 0);

  /* Non-vacuity, in the same element and the same run: the owned path still
   * fires, so the rejection above is a decision rather than an inert bridge. */
  append_trace(OWNED_FAULT(51) OWNED_FAULT(52) OWNED_FAULT(53));
  fail_unless_equals_uint64(drive(h, &index, 1, 5000), 1);
  fail_unless_equals_uint64(read_uint64(h, "kernel-faults"), 3);

  gst_harness_teardown(h);
  teardown_fault_env();
}

GST_END_TEST;

/* A fault whose queue event was never observed cannot be attributed, and an
 * unattributable fault is indistinguishable from a foreign one. */
GST_START_TEST(test_unjoinable_fault_never_restarts_this_encoder) {
  GstHarness *h;
  guint index = 0;

  mpp_mock_reset();
  setup_fault_env("1");

  h = enc_test_harness("mpph264enc", CAPS);

  append_trace(FAULT(41, 0) FAULT(42, 0) FAULT(43, 0));
  fail_unless_equals_uint64(drive(h, &index, 0, 600), 0);
  fail_unless_equals_uint64(read_uint64(h, "kernel-faults"), 0);

  append_trace(OWNED_FAULT(44));
  fail_unless_equals_uint64(drive(h, &index, 1, 5000), 1);

  gst_harness_teardown(h);
  teardown_fault_env();
}

GST_END_TEST;

/* Absent the opt-in the element must behave exactly as it did before the
 * bridge existed: no instance touched, no counter, no restart. */
GST_START_TEST(test_bridge_is_inert_when_not_enabled) {
  GstHarness *h;
  guint index = 0;

  mpp_mock_reset();
  setup_fault_env("1");
  g_unsetenv("GST_MPP_FAULT_BRIDGE");

  h = enc_test_harness("mpph264enc", CAPS);

  append_trace(OWNED_FAULT(41) OWNED_FAULT(42) OWNED_FAULT(43));
  fail_unless_equals_uint64(drive(h, &index, 0, 600), 0);
  fail_unless_equals_uint64(read_uint64(h, "kernel-faults"), 0);

  /* The enable file is untouched, so nothing armed a trace instance. */
  {
    gchar *enable = g_build_filename(instance_dir, "events", "rockchip_mpp",
                                     "mpp_task_error", "enable", NULL);
    gchar *content = NULL;

    fail_unless(g_file_get_contents(enable, &content, NULL, NULL));
    fail_unless_equals_string(content, "0\n");
    g_free(content);
    g_free(enable);
  }

  gst_harness_teardown(h);
  teardown_fault_env();
}

GST_END_TEST;

static Suite *enc_fault_bridge_suite(void) {
  Suite *suite = suite_create("enc-fault-bridge");
  TCase *test_case = tcase_create("kernel-fault-restart");

  tcase_set_timeout(test_case, 60);
  tcase_add_test(test_case,
                 test_owned_kernel_fault_drives_the_existing_restart_path);
  tcase_add_test(test_case,
                 test_foreign_session_fault_never_restarts_this_encoder);
  tcase_add_test(test_case, test_unjoinable_fault_never_restarts_this_encoder);
  tcase_add_test(test_case, test_bridge_is_inert_when_not_enabled);
  suite_add_tcase(suite, test_case);
  return suite;
}

GST_CHECK_MAIN(enc_fault_bridge);
