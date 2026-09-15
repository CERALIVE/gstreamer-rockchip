/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fake-mpi.h"
#include "interposer.h"

typedef struct {
  MppCtx ctx;
  MppApi *api;
  FakeFrame frame;
  MPP_RET ret;
} Call;

static Call create(MppCtxType type) {
  Call call = {0};
  g_assert_cmpint(mpp_create(&call.ctx, &call.api), ==, MPP_OK);
  g_assert_cmpint(mpp_init(call.ctx, type, MPP_VIDEO_CodingAVC), ==, MPP_OK);
  call.frame.marker = 0x12345678;
  return call;
}

static void healthy(Call *call) {
  for (guint i = 0; i < 3; i++) {
    MppPacket packet = NULL;
    g_assert_cmpint(call->api->encode_put_frame(call->ctx, &call->frame), ==,
                    MPP_OK);
    g_assert_cmpint(call->api->encode_get_packet(call->ctx, &packet), ==, MPP_OK);
    g_assert_cmpuint(mpp_packet_get_length(packet), >, 0);
  }
}

static void test_passthrough(void) {
  MppApi original = *fake_mpi_api();
  Call a = create(MPP_CTX_ENC), b = create(MPP_CTX_ENC);
  g_assert_true(a.api != b.api && a.api != fake_mpi_api());
  g_assert_cmpmem(fake_mpi_api(), sizeof(original), &original, sizeof(original));
  MppApi copy = *a.api;
  copy.encode_put_frame = original.encode_put_frame;
  copy.encode_get_packet = original.encode_get_packet;
  copy.control = original.control;
  copy.reset = original.reset;
  g_assert_cmpmem(&copy, sizeof(copy), &original, sizeof(original));
  healthy(&a);
  fake_mpi_put_result(a.ctx, MPP_NOK);
  g_assert_cmpint(a.api->encode_put_frame(a.ctx, &a.frame), ==, MPP_NOK);
  fake_mpi_put_result(a.ctx, MPP_ERR_VPUHW);
  g_assert_cmpint(a.api->encode_put_frame(a.ctx, &a.frame), ==, MPP_ERR_VPUHW);
  MppPacket packet = NULL;
  fake_mpi_packet_result(a.ctx, MPP_ERR_TIMEOUT, 0);
  g_assert_cmpint(a.api->encode_get_packet(a.ctx, &packet), ==, MPP_ERR_TIMEOUT);
  g_assert_null(packet);
  g_assert_cmpint(b.api->poll(b.ctx, MPP_PORT_OUTPUT, MPP_POLL_NON_BLOCK), ==,
                  MPP_ERR_TIMEOUT);
  g_assert_cmpint(mpp_create(NULL, &a.api), ==, MPP_ERR_NULL_PTR);
  g_assert_cmpint(mpp_destroy(a.ctx), ==, MPP_OK);
  g_assert_cmpint(mpp_destroy(b.ctx), ==, MPP_OK);
}

static void test_pre_submit(void) {
  Call target = create(MPP_CTX_ENC), preview = create(MPP_CTX_ENC);
  healthy(&target);
  healthy(&preview);
  guint64 id = mpi_test_context_id(target.ctx);
  g_assert_cmpint(mpi_test_arm(id), ==, MPI_TEST_ARMED);
  g_assert_cmpint(mpi_test_arm(id), ==, MPI_TEST_ALREADY_ARMED);
  g_assert_cmpint(mpi_test_arm(mpi_test_context_id(preview.ctx)), ==,
                  MPI_TEST_ALREADY_ARMED);
  g_assert_cmpint(preview.api->encode_put_frame(preview.ctx, &preview.frame), ==,
                  MPP_OK);
  FakeFrame before = target.frame;
  g_assert_cmpint(target.api->encode_put_frame(target.ctx, &target.frame), ==,
                  MPP_ERR_STREAM);
  g_assert_cmpmem(&target.frame, sizeof(before), &before, sizeof(before));
  g_assert_cmpint(target.api->encode_put_frame(target.ctx, &target.frame), ==,
                  MPP_OK);
  MpiTestStats stats;
  g_assert_true(mpi_test_snapshot(id, &stats));
  g_assert_cmpuint(stats.puts, ==, 5);
  g_assert_cmpuint(stats.real_puts, ==, 4);
  g_assert_cmpuint(stats.injections, ==, 1);
  g_assert_cmpint(mpi_test_arm(id), ==, MPI_TEST_SPENT);
  g_assert_cmpint(mpp_destroy(target.ctx), ==, MPP_OK);
  Call replacement = create(MPP_CTX_ENC);
  g_assert_cmpuint(mpi_test_context_id(replacement.ctx), !=, id);
  g_assert_false(mpi_test_snapshot(id, &stats));
  g_assert_cmpint(mpi_test_arm(id), ==, MPI_TEST_NOT_FOUND);
  g_assert_cmpint(replacement.api->encode_put_frame(replacement.ctx,
                                                  &replacement.frame), ==,
                  MPP_OK);
  g_assert_cmpint(mpp_destroy(replacement.ctx), ==, MPP_OK);
  g_assert_cmpint(mpp_destroy(preview.ctx), ==, MPP_OK);
}

static void test_readiness(void) {
  Call target = create(MPP_CTX_ENC), decoder = create(MPP_CTX_DEC);
  guint64 id = mpi_test_context_id(target.ctx);
  g_assert_cmpint(mpi_test_arm(0), ==, MPI_TEST_NOT_FOUND);
  g_assert_cmpint(mpi_test_arm(id), ==, MPI_TEST_NOT_READY);
  fake_mpi_packet_result(target.ctx, MPP_OK, 0);
  for (guint i = 0; i < 3; i++) {
    MppPacket packet = NULL;
    g_assert_cmpint(target.api->encode_put_frame(target.ctx, &target.frame), ==,
                    MPP_OK);
    g_assert_cmpint(target.api->encode_get_packet(target.ctx, &packet), ==,
                    MPP_OK);
  }
  g_assert_cmpint(mpi_test_arm(id), ==, MPI_TEST_NOT_READY);
  healthy(&decoder);
  g_assert_cmpint(mpi_test_arm(mpi_test_context_id(decoder.ctx)), ==,
                  MPI_TEST_NOT_READY);
  fake_mpi_packet_result(target.ctx, MPP_OK, 1);
  healthy(&target);
  g_assert_cmpint(mpi_test_arm(id), ==, MPI_TEST_ARMED);
  target.frame.eos = TRUE;
  g_assert_cmpint(target.api->encode_put_frame(target.ctx, &target.frame), ==,
                  MPP_OK);
  g_assert_cmpint(target.api->encode_put_frame(target.ctx, NULL), ==, MPP_OK);
  target.frame.eos = FALSE;
  g_assert_cmpint(target.api->encode_put_frame(target.ctx, &target.frame), ==,
                  MPP_ERR_STREAM);
  g_assert_cmpint(mpp_destroy(target.ctx), ==, MPP_OK);
  g_assert_cmpint(mpp_destroy(decoder.ctx), ==, MPP_OK);
}

static gpointer submit(gpointer data) {
  Call *call = data;
  call->ret = call->api->encode_put_frame(call->ctx, &call->frame);
  return NULL;
}

static gpointer destroy(gpointer data) {
  g_assert_cmpint(mpp_destroy(data), ==, MPP_OK);
  return NULL;
}

static void test_concurrent_one_shot(void) {
  Call target = create(MPP_CTX_ENC);
  healthy(&target);
  g_assert_cmpint(mpi_test_arm(mpi_test_context_id(target.ctx)), ==, MPI_TEST_ARMED);
  Call calls[16];
  GThread *threads[16];
  for (guint i = 0; i < G_N_ELEMENTS(calls); i++) {
    calls[i] = target;
    calls[i].frame.submissions = 0;
    threads[i] = g_thread_new("put", submit, &calls[i]);
  }
  guint injected = 0;
  for (guint i = 0; i < G_N_ELEMENTS(calls); i++) {
    g_thread_join(threads[i]);
    if (calls[i].ret == MPP_ERR_STREAM) {
      injected++;
      g_assert_cmpuint(calls[i].frame.submissions, ==, 0);
    } else {
      g_assert_cmpint(calls[i].ret, ==, MPP_OK);
      g_assert_cmpuint(calls[i].frame.submissions, ==, 1);
    }
  }
  g_assert_cmpuint(injected, ==, 1);
  g_assert_cmpint(mpp_destroy(target.ctx), ==, MPP_OK);
}

static void test_destroy_inflight(void) {
  Call a = create(MPP_CTX_ENC), b = create(MPP_CTX_ENC);
  guint64 id = mpi_test_context_id(a.ctx);
  fake_mpi_block_put(a.ctx);
  GThread *put_thread = g_thread_new("blocked-put", submit, &a);
  fake_mpi_wait_put(a.ctx);
  GThread *destroy_thread = g_thread_new("destroy", destroy, a.ctx);
  MpiTestStats stats;
  gint64 deadline = g_get_monotonic_time() + G_USEC_PER_SEC * 5;
  do {
    g_assert_true(mpi_test_snapshot(id, &stats));
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    g_usleep(1000);
  } while (!stats.closing);
  g_assert_cmpuint(fake_mpi_destroys(), ==, 0);
  /* The blocked vendor call must not hold the registry lock. */
  healthy(&b);
  fake_mpi_release_put(a.ctx);
  g_thread_join(put_thread);
  g_thread_join(destroy_thread);
  g_assert_cmpint(a.ret, ==, MPP_OK);
  g_assert_cmpuint(fake_mpi_destroys(), ==, 1);
  g_assert_cmpint(mpi_test_arm(id), ==, MPI_TEST_NOT_FOUND);
  g_assert_cmpint(mpp_destroy(b.ctx), ==, MPP_OK);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/mpi/passthrough", test_passthrough);
  g_test_add_func("/mpi/pre-submit", test_pre_submit);
  g_test_add_func("/mpi/readiness", test_readiness);
  g_test_add_func("/mpi/concurrent-one-shot", test_concurrent_one_shot);
  g_test_add_func("/mpi/destroy-inflight", test_destroy_inflight);
  return g_test_run();
}
