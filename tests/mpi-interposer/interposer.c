/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#ifndef CERALIVE_MPI_TEST_ONLY
#error "MPI fault injection is available only in the standalone test build"
#endif
#ifdef NDEBUG
#error "MPI fault injection must not be compiled as a release build"
#endif

#include "interposer.h"
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

typedef struct {
  MppCtx ctx;
  MppApi original;
  MppApi wrapped;
  MpiTestStats stats;
  guint active;
  gboolean encoder;
} Context;

static GMutex mutex;
static GMutex lifecycle;
static GCond idle;
static GList *contexts;
static guint64 next_id, sequence, armed_id;
static gboolean spent;
static gsize resolved;
static MPP_RET (*real_create)(MppCtx *, MppApi **);
static MPP_RET (*real_init)(MppCtx, MppCtxType, MppCodingType);
static MPP_RET (*real_destroy)(MppCtx);

static void resolve(void) {
  if (g_once_init_enter(&resolved)) {
    real_create = (MPP_RET (*)(MppCtx *, MppApi **))dlsym(RTLD_NEXT, "mpp_create");
    real_init = (MPP_RET (*)(MppCtx, MppCtxType, MppCodingType))
        dlsym(RTLD_NEXT, "mpp_init");
    real_destroy = (MPP_RET (*)(MppCtx))dlsym(RTLD_NEXT, "mpp_destroy");
    if (!real_create || !real_init || !real_destroy)
      g_error("MPI test instrumentation: missing next-provider lifecycle symbol");
    g_once_init_leave(&resolved, 1);
  }
}

static Context *find_context(MppCtx ctx) {
  for (GList *it = contexts; it; it = it->next) {
    Context *entry = it->data;
    if (entry->ctx == ctx)
      return entry;
  }
  return NULL;
}

static Context *find_id(guint64 id) {
  for (GList *it = contexts; it; it = it->next) {
    Context *entry = it->data;
    if (entry->stats.id == id)
      return entry;
  }
  return NULL;
}

/* All records share the registry lock, so seq orders injection and recreation
 * even if the allocator reuses the same MppCtx address. */
static void record(Context *entry, const char *op, MPP_RET ret, gint64 value) {
  fprintf(stderr,
          "mpi-test seq=%" G_GUINT64_FORMAT " us=%" G_GINT64_FORMAT
          " pid=%ld ctx=%p id=%" G_GUINT64_FORMAT
          " op=%s ret=%d value=%" G_GINT64_FORMAT
          " puts=%" G_GUINT64_FORMAT " real_puts=%" G_GUINT64_FORMAT
          " healthy=%" G_GUINT64_FORMAT " injected=%" G_GUINT64_FORMAT "\n",
          ++sequence, g_get_monotonic_time(), (long)getpid(), entry->ctx,
          entry->stats.id, op, ret, value, entry->stats.puts,
          entry->stats.real_puts, entry->stats.healthy_packets,
          entry->stats.injections);
}

static Context *acquire(MppCtx ctx) {
  g_mutex_lock(&mutex);
  Context *entry = find_context(ctx);
  if (!entry || entry->stats.closing)
    g_error("MPI test instrumentation: callback on an untracked/closing context");
  entry->active++;
  g_mutex_unlock(&mutex);
  return entry;
}

static void release(Context *entry) {
  g_mutex_lock(&mutex);
  entry->active--;
  if (!entry->active)
    g_cond_broadcast(&idle);
  g_mutex_unlock(&mutex);
}

static MPP_RET put_frame(MppCtx ctx, MppFrame frame) {
  Context *entry = acquire(ctx);
  gboolean input = frame && !mpp_frame_get_eos(frame);
  g_mutex_lock(&mutex);
  entry->stats.puts++;
  if (input && armed_id == entry->stats.id) {
    armed_id = 0;
    spent = TRUE;
    entry->stats.injections++;
    record(entry, "inject-put-before-submit", MPP_ERR_STREAM, 0);
    g_mutex_unlock(&mutex);
    release(entry);
    /* Do not call MPI, deinit, or retain frame here. The plugin still owns it. */
    return MPP_ERR_STREAM;
  }
  entry->stats.real_puts++;
  g_mutex_unlock(&mutex);

  MPP_RET ret = entry->original.encode_put_frame(ctx, frame);
  g_mutex_lock(&mutex);
  if (ret == MPP_OK && input)
    entry->stats.successful_puts++;
  record(entry, "put", ret, 0);
  g_mutex_unlock(&mutex);
  release(entry);
  return ret;
}

static MPP_RET get_packet(MppCtx ctx, MppPacket *packet) {
  Context *entry = acquire(ctx);
  MPP_RET ret = entry->original.encode_get_packet(ctx, packet);
  gsize length = ret == MPP_OK && packet && *packet
                     ? mpp_packet_get_length(*packet)
                     : 0;
  g_mutex_lock(&mutex);
  if (length) {
    entry->stats.healthy_packets++;
    record(entry, "packet", ret, (gint64)length);
  }
  g_mutex_unlock(&mutex);
  release(entry);
  return ret;
}

static MPP_RET control(MppCtx ctx, MpiCmd cmd, MppParam param) {
  Context *entry = acquire(ctx);
  MPP_RET ret = entry->original.control(ctx, cmd, param);
  g_mutex_lock(&mutex);
  record(entry, "control", ret, cmd);
  g_mutex_unlock(&mutex);
  release(entry);
  return ret;
}

static MPP_RET reset(MppCtx ctx) {
  Context *entry = acquire(ctx);
  MPP_RET ret = entry->original.reset(ctx);
  g_mutex_lock(&mutex);
  record(entry, "reset", ret, 0);
  g_mutex_unlock(&mutex);
  release(entry);
  return ret;
}

MPP_RET mpp_create(MppCtx *ctx, MppApi **mpi) {
  resolve();
  g_mutex_lock(&lifecycle);
  MPP_RET ret = real_create(ctx, mpi);
  if (ret != MPP_OK) {
    g_mutex_unlock(&lifecycle);
    return ret;
  }
  if (!ctx || !*ctx || !mpi || !*mpi || (*mpi)->size != sizeof(MppApi))
    g_error("MPI test instrumentation: unsupported MppApi layout");

  Context *entry = g_new0(Context, 1);
  entry->ctx = *ctx;
  entry->original = **mpi;
  entry->wrapped = **mpi;
  if (entry->original.encode_put_frame)
    entry->wrapped.encode_put_frame = put_frame;
  if (entry->original.encode_get_packet)
    entry->wrapped.encode_get_packet = get_packet;
  if (entry->original.control)
    entry->wrapped.control = control;
  if (entry->original.reset)
    entry->wrapped.reset = reset;

  g_mutex_lock(&mutex);
  if (find_context(*ctx) || next_id == G_MAXUINT64)
    g_error("MPI test instrumentation: duplicate context or exhausted IDs");
  entry->stats.id = ++next_id;
  contexts = g_list_prepend(contexts, entry);
  *mpi = &entry->wrapped;
  record(entry, "create", ret, entry->original.version);
  g_mutex_unlock(&mutex);
  g_mutex_unlock(&lifecycle);
  return ret;
}

MPP_RET mpp_init(MppCtx ctx, MppCtxType type, MppCodingType coding) {
  resolve();
  g_mutex_lock(&mutex);
  gboolean tracked = find_context(ctx) != NULL;
  g_mutex_unlock(&mutex);
  if (!tracked)
    return real_init(ctx, type, coding);
  Context *entry = acquire(ctx);
  MPP_RET ret = real_init(ctx, type, coding);
  g_mutex_lock(&mutex);
  entry->encoder = ret == MPP_OK && type == MPP_CTX_ENC;
  record(entry, "init", ret, coding);
  g_mutex_unlock(&mutex);
  release(entry);
  return ret;
}

MPP_RET mpp_destroy(MppCtx ctx) {
  resolve();
  g_mutex_lock(&mutex);
  Context *entry = find_context(ctx);
  if (!entry) {
    g_mutex_unlock(&mutex);
    return real_destroy(ctx);
  }
  if (entry->stats.closing)
    g_error("MPI test instrumentation: concurrent destroy");
  entry->stats.closing = TRUE;
  if (armed_id == entry->stats.id)
    armed_id = 0;
  while (entry->active)
    g_cond_wait(&idle, &mutex);
  g_mutex_unlock(&mutex);

  g_mutex_lock(&lifecycle);
  MPP_RET ret = real_destroy(ctx);
  g_mutex_lock(&mutex);
  record(entry, "destroy", ret, 0);
  if (ret == MPP_OK) {
    contexts = g_list_remove(contexts, entry);
    g_free(entry);
  } else {
    entry->stats.closing = FALSE;
  }
  g_mutex_unlock(&mutex);
  g_mutex_unlock(&lifecycle);
  return ret;
}

guint64 mpi_test_context_id(MppCtx ctx) {
  g_mutex_lock(&mutex);
  Context *entry = find_context(ctx);
  guint64 id = entry ? entry->stats.id : 0;
  g_mutex_unlock(&mutex);
  return id;
}

gboolean mpi_test_snapshot(guint64 id, MpiTestStats *stats) {
  if (!stats)
    return FALSE;
  g_mutex_lock(&mutex);
  Context *entry = find_id(id);
  if (entry)
    *stats = entry->stats;
  g_mutex_unlock(&mutex);
  return entry != NULL;
}

MpiTestArmResult mpi_test_arm(guint64 id) {
  g_mutex_lock(&mutex);
  Context *entry = find_id(id);
  MpiTestArmResult result;
  if (!entry || entry->stats.closing)
    result = MPI_TEST_NOT_FOUND;
  else if (spent)
    result = MPI_TEST_SPENT;
  else if (armed_id)
    result = MPI_TEST_ALREADY_ARMED;
  else if (!entry->encoder || entry->stats.successful_puts < 3 ||
           entry->stats.healthy_packets < 3)
    result = MPI_TEST_NOT_READY;
  else {
    armed_id = id;
    record(entry, "arm", MPP_OK, 0);
    result = MPI_TEST_ARMED;
  }
  g_mutex_unlock(&mutex);
  return result;
}
