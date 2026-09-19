/*
 * C6b measurement harness — gstreamer-rockchip island/perf.
 *
 * Answers exactly two go/no-go questions, on real hardware, with no GStreamer,
 * no capture device and no engine involvement:
 *
 *   C6b-perf  : what does the per-frame DMA-BUF fd path cost relative to a
 *               pre-imported handle? (the cache's whole saving)
 *   C6b-async : does one-frame IM_ASYNC pipelining raise sustained throughput?
 *
 * Every mode runs the SAME conversion on the SAME two DMA-BUFs so the only
 * variable is how the buffers are described to librga and whether the submit
 * blocks. Sequential single-threaded submissions only: no fault injection, no
 * process death mid-submit, so the known rga_job_commit UAF path is not
 * exercised.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <rga/RgaApi.h>
#include <rga/im2d.h>

#define MAX_ITERS 20000

static int
alloc_dmabuf (size_t size, const char **heap_name)
{
  static const char *const heaps[] = {
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/system",
  };
  struct dma_heap_allocation_data allocation;

  for (size_t i = 0; i < sizeof (heaps) / sizeof (heaps[0]); i++) {
    int heap_fd = open (heaps[i], O_RDWR | O_CLOEXEC);

    if (heap_fd < 0)
      continue;
    memset (&allocation, 0, sizeof (allocation));
    allocation.len = size;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl (heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation) == 0) {
      close (heap_fd);
      *heap_name = heaps[i];
      return (int) allocation.fd;
    }
    close (heap_fd);
  }
  return -1;
}

static double
now_us (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec * 1e6 + (double) ts.tv_nsec / 1e3;
}

static int
cmp_double (const void *a, const void *b)
{
  double x = *(const double *) a;
  double y = *(const double *) b;

  return (x > y) - (x < y);
}

typedef struct
{
  double mean;
  double p50;
  double p95;
  double min;
  double max;
  unsigned count;
} Stats;

static Stats
summarise (double *samples, unsigned n)
{
  Stats s = { 0, 0, 0, 0, 0, 0 };
  double total = 0;

  if (n == 0)
    return s;
  qsort (samples, n, sizeof (double), cmp_double);
  for (unsigned i = 0; i < n; i++)
    total += samples[i];
  s.count = n;
  s.mean = total / n;
  s.p50 = samples[n / 2];
  s.p95 = samples[(unsigned) ((double) n * 0.95)];
  s.min = samples[0];
  s.max = samples[n - 1];
  return s;
}

static void
report (const char *mode, Stats s)
{
  printf ("MODE=%s n=%u mean_us=%.2f p50_us=%.2f p95_us=%.2f "
      "min_us=%.2f max_us=%.2f fps=%.1f\n",
      mode, s.count, s.mean, s.p50, s.p95, s.min, s.max,
      s.mean > 0 ? 1e6 / s.mean : 0.0);
}

int
main (int argc, char **argv)
{
  int width = 3840;
  int height = 2160;
  unsigned iters = 600;
  unsigned warmup = 60;
  const char *src_heap = NULL;
  const char *dst_heap = NULL;
  int src_fd;
  int dst_fd;
  size_t src_size;
  size_t dst_size;
  rga_buffer_handle_t src_handle = 0;
  rga_buffer_handle_t dst_handle = 0;
  double *samples;
  im_rect zero = { 0, 0, 0, 0 };
  int src_format = RK_FORMAT_YCbCr_422_SP;    /* NV16 */
  int dst_format = RK_FORMAT_YCbCr_420_SP;    /* NV12 */
  const char *version;

  for (int i = 1; i < argc; i++) {
    if (!strcmp (argv[i], "--width") && i + 1 < argc)
      width = atoi (argv[++i]);
    else if (!strcmp (argv[i], "--height") && i + 1 < argc)
      height = atoi (argv[++i]);
    else if (!strcmp (argv[i], "--iters") && i + 1 < argc)
      iters = (unsigned) atoi (argv[++i]);
    else if (!strcmp (argv[i], "--warmup") && i + 1 < argc)
      warmup = (unsigned) atoi (argv[++i]);
    else {
      fprintf (stderr, "usage: %s [--width N] [--height N] [--iters N]"
          " [--warmup N]\n", argv[0]);
      return 2;
    }
  }
  if (iters == 0 || iters > MAX_ITERS) {
    fprintf (stderr, "iters out of range\n");
    return 2;
  }

  version = querystring (RGA_VERSION);
  printf ("RGA_VERSION_BEGIN\n%s\nRGA_VERSION_END\n", version ? version : "?");

  src_size = (size_t) width * height * 2;       /* NV16 */
  dst_size = (size_t) width * height * 3 / 2;   /* NV12 */

  src_fd = alloc_dmabuf (src_size, &src_heap);
  dst_fd = alloc_dmabuf (dst_size, &dst_heap);
  if (src_fd < 0 || dst_fd < 0) {
    fprintf (stderr, "dma-heap allocation failed: %s\n", strerror (errno));
    return 1;
  }
  printf ("GEOMETRY=%dx%d src_heap=%s dst_heap=%s src_bytes=%zu dst_bytes=%zu\n",
      width, height, src_heap, dst_heap, src_size, dst_size);

  samples = calloc (iters, sizeof (double));
  if (!samples)
    return 1;

  /* ---- Mode fd: today's path. wrapbuffer_fd + synchronous improcess. ---- */
  {
    unsigned taken = 0;
    unsigned failures = 0;

    for (unsigned i = 0; i < warmup + iters; i++) {
      rga_buffer_t src = wrapbuffer_fd_t (src_fd, width, height, width,
          height, src_format);
      rga_buffer_t dst = wrapbuffer_fd_t (dst_fd, width, height, width,
          height, dst_format);
      rga_buffer_t pat;
      double t0;
      IM_STATUS status;

      memset (&pat, 0, sizeof (pat));
      t0 = now_us ();
      status = improcess (src, dst, pat, zero, zero, zero, IM_SYNC);
      if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        if (failures++ == 0)
          fprintf (stderr, "fd-mode improcess status=%d (%s) errno=%d\n",
              status, imStrError (status), errno);
        continue;
      }
      if (i >= warmup)
        samples[taken++] = now_us () - t0;
    }
    printf ("FAILURES_fd=%u\n", failures);
    report ("fd_sync", summarise (samples, taken));
  }

  /* ---- Mode import: the pure per-buffer import/release ioctl pair. ----
   * This is the userspace half of what the cache removes; the kernel-side
   * mapping cost shows up as the fd_sync vs handle_sync delta above/below. */
  {
    unsigned taken = 0;
    unsigned failures = 0;
    im_handle_param_t param;

    memset (&param, 0, sizeof (param));
    param.width = (uint32_t) width;
    param.height = (uint32_t) height;
    param.format = (uint32_t) src_format;

    for (unsigned i = 0; i < warmup + iters; i++) {
      double t0 = now_us ();
      rga_buffer_handle_t h = importbuffer_fd (src_fd, &param);

      if (h == 0) {
        failures++;
        continue;
      }
      releasebuffer_handle (h);
      if (i >= warmup)
        samples[taken++] = now_us () - t0;
    }
    printf ("FAILURES_import=%u\n", failures);
    report ("import_release_pair", summarise (samples, taken));
  }

  /* ---- Handle probe: is the handle path usable AT ALL on this stack?
   * Single-plane RGBA is the control: if it fails too, the refusal cannot be
   * a chroma-plane descriptor problem. ---- */
  {
    im_handle_param_t p;
    rga_buffer_handle_t a;
    rga_buffer_handle_t b;
    int probe_w = 256;
    int probe_h = 256;
    IM_STATUS status;

    memset (&p, 0, sizeof (p));
    p.width = (uint32_t) probe_w;
    p.height = (uint32_t) probe_h;
    p.format = (uint32_t) RK_FORMAT_RGBA_8888;
    a = importbuffer_fd (src_fd, &p);
    b = importbuffer_fd (dst_fd, &p);
    printf ("PROBE_rgba_handles src=%d dst=%d\n", a, b);
    if (a > 0 && b > 0) {
      rga_buffer_t s = wrapbuffer_handle_t (a, probe_w, probe_h, probe_w,
          probe_h, RK_FORMAT_RGBA_8888);
      rga_buffer_t d = wrapbuffer_handle_t (b, probe_w, probe_h, probe_w,
          probe_h, RK_FORMAT_RGBA_8888);
      rga_buffer_t pt;

      memset (&pt, 0, sizeof (pt));
      errno = 0;
      status = improcess (s, d, pt, zero, zero, zero, IM_SYNC);
      printf ("PROBE_rgba_handle_copy status=%d errno=%d (%s)\n",
          status, errno, imStrError (status));
    }
    /* Same geometry through fd, as the positive control. */
    {
      rga_buffer_t s = wrapbuffer_fd_t (src_fd, probe_w, probe_h, probe_w,
          probe_h, RK_FORMAT_RGBA_8888);
      rga_buffer_t d = wrapbuffer_fd_t (dst_fd, probe_w, probe_h, probe_w,
          probe_h, RK_FORMAT_RGBA_8888);
      rga_buffer_t pt;

      memset (&pt, 0, sizeof (pt));
      errno = 0;
      status = improcess (s, d, pt, zero, zero, zero, IM_SYNC);
      printf ("PROBE_rgba_fd_copy status=%d errno=%d (%s)\n",
          status, errno, imStrError (status));
    }
    if (a > 0)
      releasebuffer_handle (a);
    if (b > 0)
      releasebuffer_handle (b);
  }

  /* ---- Mode handle: the cached path. Import ONCE, wrapbuffer_handle per
   * frame, synchronous improcess. ---- */
  {
    unsigned taken = 0;
    unsigned failures = 0;
    im_handle_param_t sp;
    im_handle_param_t dp;

    memset (&sp, 0, sizeof (sp));
    sp.width = (uint32_t) width;
    sp.height = (uint32_t) height;
    sp.format = (uint32_t) src_format;
    memset (&dp, 0, sizeof (dp));
    dp.width = (uint32_t) width;
    dp.height = (uint32_t) height;
    dp.format = (uint32_t) dst_format;

    src_handle = importbuffer_fd (src_fd, &sp);
    dst_handle = importbuffer_fd (dst_fd, &dp);
    if (src_handle == 0 || dst_handle == 0) {
      fprintf (stderr, "importbuffer_fd failed src=%d dst=%d errno=%d\n",
          src_handle, dst_handle, errno);
    } else {
      for (unsigned i = 0; i < warmup + iters; i++) {
        rga_buffer_t src = wrapbuffer_handle_t (src_handle, width, height,
            width, height, src_format);
        rga_buffer_t dst = wrapbuffer_handle_t (dst_handle, width, height,
            width, height, dst_format);
        rga_buffer_t pat;
        double t0;
        IM_STATUS status;

        memset (&pat, 0, sizeof (pat));
        t0 = now_us ();
        status = improcess (src, dst, pat, zero, zero, zero, IM_SYNC);
        if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
          if (failures++ == 0)
            fprintf (stderr, "handle-mode improcess status=%d (%s) errno=%d\n",
                status, imStrError (status), errno);
          continue;
        }
        if (i >= warmup)
          samples[taken++] = now_us () - t0;
      }
      printf ("FAILURES_handle=%u\n", failures);
      report ("handle_sync", summarise (samples, taken));
    }
  }

  /* ---- Mode async: one-frame pipelining. Submit N with IM_ASYNC, then wait
   * on N-1's release fence — exactly the depth-1 contract C6b-async would
   * implement inside rgaconvert. Measures END-TO-END wall time for the whole
   * run so the reported per-frame figure is sustained THROUGHPUT, which is
   * what the gate is about, not per-call latency.
   *
   * Frames ALTERNATE between two destination buffers. With depth 1 two jobs
   * are in flight at once, and a real rgaconvert would draw each output from a
   * pool, so writing both to one buffer would measure a shape the element
   * cannot produce — and would let the hardware overlap two writes to the same
   * memory, which is a data race, not a throughput result. ---- */
  {
    unsigned taken = 0;
    unsigned failures = 0;
    int pending_fence = -1;
    double run_start;
    double run_end;
    unsigned submitted = 0;
    const char *dst2_heap = NULL;
    int dst_fds[2];

    dst_fds[0] = dst_fd;
    dst_fds[1] = alloc_dmabuf (dst_size, &dst2_heap);
    if (dst_fds[1] < 0) {
      fprintf (stderr, "second destination allocation failed: %s\n",
          strerror (errno));
      dst_fds[1] = dst_fd;
    }
    printf ("ASYNC_DST_BUFFERS=%d\n", dst_fds[1] == dst_fd ? 1 : 2);

    /* Warm up, synchronously, so the comparison starts from the same state. */
    for (unsigned i = 0; i < warmup; i++) {
      rga_buffer_t src = wrapbuffer_fd_t (src_fd, width, height, width,
          height, src_format);
      rga_buffer_t dst = wrapbuffer_fd_t (dst_fd, width, height, width,
          height, dst_format);
      rga_buffer_t pat;

      memset (&pat, 0, sizeof (pat));
      improcess (src, dst, pat, zero, zero, zero, IM_SYNC);
    }

    run_start = now_us ();
    for (unsigned i = 0; i < iters; i++) {
      rga_buffer_t src = wrapbuffer_fd_t (src_fd, width, height, width,
          height, src_format);
      rga_buffer_t dst = wrapbuffer_fd_t (dst_fds[i & 1], width, height, width,
          height, dst_format);
      rga_buffer_t pat;
      int fence = -1;
      IM_STATUS status;
      double t0 = now_us ();

      memset (&pat, 0, sizeof (pat));
      status = improcessOpt (src, dst, pat, zero, zero, zero, -1, &fence,
          NULL, IM_ASYNC);
      if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        if (failures++ == 0)
          fprintf (stderr, "async improcessOpt status=%d (%s) errno=%d\n",
              status, imStrError (status), errno);
        if (fence >= 0)
          close (fence);
        continue;
      }
      submitted++;

      /* Retire frame N-1 while N is in flight. */
      if (pending_fence >= 0) {
        struct pollfd pfd = { pending_fence, POLLIN, 0 };
        int rc = poll (&pfd, 1, 2000);

        if (rc <= 0 && failures++ == 0)
          fprintf (stderr, "async fence poll rc=%d errno=%d\n", rc, errno);
        close (pending_fence);
      }
      pending_fence = fence;
      if (i >= 1)
        samples[taken++] = now_us () - t0;
    }
    /* Drain: never leave a fence unretired, and never exit with hardware
     * still writing a buffer we are about to free. */
    if (pending_fence >= 0) {
      struct pollfd pfd = { pending_fence, POLLIN, 0 };

      poll (&pfd, 1, 2000);
      close (pending_fence);
      pending_fence = -1;
    }
    run_end = now_us ();
    if (dst_fds[1] != dst_fd)
      close (dst_fds[1]);

    printf ("FAILURES_async=%u SUBMITTED_async=%u\n", failures, submitted);
    report ("async_depth1_percall", summarise (samples, taken));
    if (submitted > 0)
      printf ("MODE=async_depth1_sustained n=%u mean_us=%.2f fps=%.1f\n",
          submitted, (run_end - run_start) / submitted,
          1e6 / ((run_end - run_start) / submitted));
  }

  /* ---- Mode fd_sync_sustained: the same end-to-end measure for the sync
   * path, so the async comparison is like-for-like. ---- */
  {
    double run_start = now_us ();
    unsigned ok = 0;

    for (unsigned i = 0; i < iters; i++) {
      rga_buffer_t src = wrapbuffer_fd_t (src_fd, width, height, width,
          height, src_format);
      rga_buffer_t dst = wrapbuffer_fd_t (dst_fd, width, height, width,
          height, dst_format);
      rga_buffer_t pat;
      IM_STATUS status;

      memset (&pat, 0, sizeof (pat));
      status = improcess (src, dst, pat, zero, zero, zero, IM_SYNC);
      if (status == IM_STATUS_SUCCESS || status == IM_STATUS_NOERROR)
        ok++;
    }
    if (ok > 0) {
      double elapsed = now_us () - run_start;

      printf ("MODE=fd_sync_sustained n=%u mean_us=%.2f fps=%.1f\n",
          ok, elapsed / ok, 1e6 / (elapsed / ok));
    }
  }

  if (src_handle)
    releasebuffer_handle (src_handle);
  if (dst_handle)
    releasebuffer_handle (dst_handle);
  free (samples);
  close (src_fd);
  close (dst_fd);
  printf ("DONE\n");
  return 0;
}
