/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright 2026 CERALIVE. Test-only fence-observation delay, never installed. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gst/check/gstharness.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/gstvideometa.h>

typedef struct { GstDmaBufAllocator parent; } BoardDmaAllocator;
typedef struct { GstDmaBufAllocatorClass parent; } BoardDmaAllocatorClass;
G_DEFINE_TYPE(BoardDmaAllocator, board_dma_allocator, GST_TYPE_DMABUF_ALLOCATOR);

static GstMemory *board_alloc(GstAllocator *allocator, gsize size,
                             GstAllocationParams *params)
{
    (void)params;
    int heap = open("/dev/dma_heap/system-uncached", O_RDWR | O_CLOEXEC);
    if (heap < 0) return NULL;
    struct dma_heap_allocation_data request = {
        .len = (size + 4095) & ~(gsize)4095, .fd_flags = O_RDWR | O_CLOEXEC
    };
    int ret = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &request);
    close(heap);
    if (ret < 0) return NULL;
    return gst_dmabuf_allocator_alloc(allocator, request.fd, size);
}

static void board_dma_allocator_class_init(BoardDmaAllocatorClass *klass)
{
    GST_ALLOCATOR_CLASS(klass)->alloc = board_alloc;
}

static void board_dma_allocator_init(BoardDmaAllocator *allocator)
{
    (void)allocator;
}

static gint gate_enabled;
static gint gate_fd = -1;
static gint intercepted;
static GstHarness *active_harness;
static GThread *active_thread;

static void cleanup(void)
{
    g_atomic_int_set(&gate_enabled, 0);
    if (active_thread) {
        g_thread_join(active_thread);
        active_thread = NULL;
    }
    if (active_harness) {
        gst_harness_teardown(active_harness);
        active_harness = NULL;
    }
}

/* Delay observation, never the real RGA ioctl or its completion. The fd stays
 * open and the real sync_file becomes observable when the controller disarms. */
int poll(struct pollfd *fds, nfds_t count, int timeout)
{
    int (*real_poll)(struct pollfd *, nfds_t, int);
    *(void **)(&real_poll) = dlsym(RTLD_NEXT, "poll");
    if (!real_poll) _exit(125);
    if (g_atomic_int_get(&gate_enabled) && count == 1 && fds[0].fd >= 0) {
        char path[64], target[128];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fds[0].fd);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = 0;
            if (strstr(target, "sync_file")) {
                g_atomic_int_compare_and_exchange(&gate_fd, -1, fds[0].fd);
                if (g_atomic_int_get(&gate_fd) == fds[0].fd) {
                    g_atomic_int_inc(&intercepted);
                    fds[0].revents = 0;
                    if (timeout > 0) g_usleep((gulong)timeout * 1000);
                    return 0;
                }
            }
        }
    }
    return real_poll(fds, count, timeout);
}

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

static GstHarness *new_harness(void)
{
    GstHarness *h = gst_harness_new("rgaconvert");
    active_harness = h;
    GstAllocator *allocator = g_object_new(board_dma_allocator_get_type(), NULL);
    if (!h || !allocator) return NULL;
    gst_object_ref_sink(allocator);
    gst_harness_set_propose_allocator(h, allocator, NULL);
    g_object_set(h->element, "async-depth", 1, NULL);
    gst_harness_set_caps_str(h,
        "video/x-raw(memory:DMABuf),format=NV16,width=320,height=240,framerate=30/1,colorimetry=bt709",
        "video/x-raw(memory:DMABuf),format=NV12,width=320,height=240,framerate=30/1,colorimetry=bt709");
    return h;
}

static GstBuffer *input_frame(guint index)
{
    GstAllocator *allocator = g_object_new(board_dma_allocator_get_type(), NULL);
    GstBuffer *buffer = gst_buffer_new_allocate(allocator, 320 * 240 * 2, NULL);
    gst_object_unref(allocator);
    if (!buffer) return NULL;
    gst_buffer_memset(buffer, 0, 32 + index * 16, 320 * 240);
    gst_buffer_memset(buffer, 320 * 240, 128, 320 * 240);
    GST_BUFFER_PTS(buffer) = index * GST_SECOND;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    return buffer;
}

static gboolean pixels_match(GstBuffer *buffer, guint index)
{
    GstMapInfo map;
    gboolean ok = GST_BUFFER_PTS(buffer) == index * GST_SECOND &&
        gst_buffer_map(buffer, &map, GST_MAP_READ);
    if (!ok) return FALSE;
    ok = map.size == 320 * 240 * 3 / 2;
    for (gsize i = 0; ok && i < map.size; i++)
        ok = map.data[i] == (i < 320 * 240 ? 32 + index * 16 : 128);
    gst_buffer_unmap(buffer, &map);
    return ok;
}

static int pull_frame(GstHarness *h, guint index)
{
    GstBuffer *buffer = gst_harness_try_pull(h);
    REQUIRE(buffer != NULL);
    gboolean ok = pixels_match(buffer, index);
    gst_buffer_unref(buffer);
    REQUIRE(ok);
    return 0;
}

static void arm_gate(void)
{
    g_atomic_int_set(&gate_fd, -1);
    g_atomic_int_set(&intercepted, 0);
    g_atomic_int_set(&gate_enabled, 1);
}

typedef struct { GstElement *element; gint done; } Stop;
static gpointer stop_element(gpointer data)
{
    Stop *stop = data;
    gst_element_set_state(stop->element, GST_STATE_NULL);
    g_atomic_int_set(&stop->done, 1);
    return NULL;
}

int main(int argc, char **argv)
{
    if (g_strcmp0(g_getenv("CERALIVE_BOARD_TEST"), "1")) return 77;
    atexit(cleanup);
    gst_init(&argc, &argv);
    GstHarness *h = new_harness();
    REQUIRE(h);
    REQUIRE(gst_harness_push(h, input_frame(1)) == GST_FLOW_OK);
    REQUIRE(gst_harness_buffers_in_queue(h) == 0);
    REQUIRE(gst_harness_push(h, input_frame(2)) == GST_FLOW_OK);
    REQUIRE(!pull_frame(h, 1));
    REQUIRE(gst_harness_push_event(h, gst_event_new_eos()));
    REQUIRE(!pull_frame(h, 2));
    cleanup();
    puts("PASS normal: exact pixels and timestamps, EOS drains");

    h = new_harness();
    REQUIRE(h);
    GstBuffer *held = input_frame(1);
    REQUIRE(held);
    arm_gate();
    REQUIRE(gst_harness_push(h, gst_buffer_ref(held)) == GST_FLOW_OK);
    REQUIRE(gst_harness_push(h, input_frame(2)) == GST_FLOW_OK);
    REQUIRE(g_atomic_int_get(&intercepted) > 0);
    REQUIRE(gst_harness_buffers_in_queue(h) == 0);
    REQUIRE(GST_MINI_OBJECT_REFCOUNT_VALUE(held) == 2);
    REQUIRE(fcntl(g_atomic_int_get(&gate_fd), F_GETFD) >= 0);
    guint64 dropped = 0;
    g_object_get(h->element, "conversion-dropped-frames", &dropped, NULL);
    REQUIRE(dropped == 1);
    g_atomic_int_set(&gate_enabled, 0);
    REQUIRE(gst_harness_push(h, input_frame(3)) == GST_FLOW_OK);
    REQUIRE(!pull_frame(h, 2));
    REQUIRE(!pull_frame(h, 3));
    REQUIRE(gst_harness_buffers_in_queue(h) == 0);
    REQUIRE(GST_MINI_OBJECT_REFCOUNT_VALUE(held) == 1);
    gst_buffer_unref(held);
    cleanup();
    puts("PASS timeout: no unfinished output, input retained, later frames synchronous and exact");

    h = new_harness();
    REQUIRE(h);
    held = input_frame(1);
    arm_gate();
    REQUIRE(gst_harness_push(h, gst_buffer_ref(held)) == GST_FLOW_OK);
    gint64 begin = g_get_monotonic_time();
    REQUIRE(gst_harness_push_event(h, gst_event_new_flush_start()));
    REQUIRE(g_get_monotonic_time() - begin < 100000);
    REQUIRE(gst_harness_push_event(h, gst_event_new_flush_stop(TRUE)));
    REQUIRE(g_atomic_int_get(&intercepted) > 0);
    REQUIRE(GST_MINI_OBJECT_REFCOUNT_VALUE(held) == 2);
    REQUIRE(gst_harness_buffers_in_queue(h) == 0);
    g_atomic_int_set(&gate_enabled, 0);
    for (guint i = 0; i < 1000 && GST_MINI_OBJECT_REFCOUNT_VALUE(held) > 1; i++) {
        REQUIRE(gst_harness_push_event(h, gst_event_new_flush_stop(TRUE)));
        g_usleep(1000);
    }
    REQUIRE(GST_MINI_OBJECT_REFCOUNT_VALUE(held) == 1);
    gst_buffer_unref(held);
    cleanup();
    puts("PASS flush: prompt event, quarantine retains input until terminal, no output");

    h = new_harness();
    REQUIRE(h);
    held = input_frame(1);
    GstBus *bus = gst_bus_new();
    gst_element_set_bus(h->element, bus);
    arm_gate();
    REQUIRE(gst_harness_push(h, gst_buffer_ref(held)) == GST_FLOW_OK);
    Stop stop = { .element = h->element };
    GThread *thread = g_thread_new("delayed-stop", stop_element, &stop);
    active_thread = thread;
    g_usleep(2300000);
    REQUIRE(g_atomic_int_get(&intercepted) > 0);
    REQUIRE(!g_atomic_int_get(&stop.done));
    REQUIRE(GST_MINI_OBJECT_REFCOUNT_VALUE(held) == 2);
    GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    REQUIRE(message);
    gst_message_unref(message);
    REQUIRE(!gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR));
    g_atomic_int_set(&gate_enabled, 0);
    g_thread_join(thread);
    active_thread = NULL;
    REQUIRE(g_atomic_int_get(&stop.done));
    REQUIRE(GST_MINI_OBJECT_REFCOUNT_VALUE(held) == 1);
    gst_buffer_unref(held);
    gst_object_unref(bus);
    cleanup();
    puts("PASS stop: unsignalled past 2s retains ownership, one error, explicit disarm releases");
    return 0;
}
