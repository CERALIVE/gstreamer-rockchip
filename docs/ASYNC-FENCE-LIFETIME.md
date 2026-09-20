# Async fence lifetime

`async-depth` remains default-off. The original standalone throughput evidence
does not qualify a plugin's buffer lifetime: its first implementation closed
fences on timeout and ignored the wait result. New pipe-fence regressions were
RED with both `timeout must not close an outstanding hardware fence` and
`an unsignalled output must never reach downstream`.

## Contract

- Parent `submit_input_buffer` still owns negotiation and QoS drops. For async
  conversion, the queued input is consumed once and the parent output-allocation
  path preserves metadata. Pending and ready records hold both buffers.
- The single state mutex owns pending, ready, quarantine and flushing. Pollers
  retain a record reference while waiting outside that mutex, so concurrent
  flush/reaping cannot recycle the fd underneath a wait.
- A 100 ms timeout is a mode-switch threshold, not permission to release DMA
  memory. The frame is logically dropped, counted once, and quarantined. Future
  submissions use sync for the rest of that session. Terminal fence errors
  also drop output instead of returning potentially corrupted pixels.
- FLUSH_START marks flushing and moves both states to quarantine, then chains
  immediately to the parent. FLUSH_STOP polls quarantine nonblocking and lets
  the parent reset segment/QoS. Every other serialized event drains older
  output first, including TAG, custom serialized events and CAPS. Every event
  is chained exactly once; no direct event push replaces parent handling.
- Stop quarantines the remaining states and waits for terminal fences and
  outstanding submit/poll references. At two seconds it posts one error and
  keeps waiting. Closing a sync-file does not cancel the job, so neither a
  timeout nor teardown can return a hardware-owned buffer to its pool.
- An async-success result without a fence is a protocol failure, not synchronous
  completion. It posts an error and retains both buffers indefinitely because
  no completion can be proven. A fence returned alongside a failure is likewise
  retained until terminal. The missing-receipt case has no safe in-process
  reclamation path; the library/driver contract must be repaired before reuse.
- Missing Opt rejects the property with a warning and keeps zero. CPU staging
  remains synchronous. Failed submissions are typed failures, never retries
  whose effects or counters would be hidden.

The converter uses `gst_mpp_rga_fence_status` as a non-owning poll. The legacy
wait helper closes only terminal descriptors; it leaves a timed-out descriptor
with its caller. A polling error or invalid fd is not proof of DMA completion.

## Executable coverage

The Meson rgaconvert suite covers pre-signalled normal completion, unsignalled
pipe timeout with output/refcount/fd assertions, FLUSH_START propagation with
input and output retained, stop past two seconds with one bus error, and a real
GstHarness chain through parent negotiation/allocation/generation, TAG/custom
events, changed caps and EOS. Host DMA-BUF memory is a test memfd, not hardware.

## Rock fault injection — 2026-09-20

`tests/board/c6b-fence-lifetime.c` is test-only and never installed. Build with:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -Wl,--export-dynamic \
  tests/board/c6b-fence-lifetime.c \
  $(pkg-config --cflags --libs gstreamer-check-1.0 gstreamer-video-1.0 \
    gstreamer-allocators-1.0) -ldl -o c6b-fence-lifetime
```

Run only under the bench's exclusive ownership mechanism, with
`CERALIVE_BOARD_TEST=1`, a private `GST_PLUGIN_PATH`/`GST_REGISTRY`, and the exact
candidate plugin. It opens no capture device and touches no product service.
The test's real dma-heap allocator supplies generated NV16 frames. Its poll
interposer delays observation of one **real RGA sync-file**, never the driver
ioctl, never hardware execution. Explicit disarm exposes actual completion.
Cleanup disarms first and joins an active stop thread before tearing down.

Rock kernel `7.2.0-ceralive-rk3588`, driver 1.3.11, process-local repaired R1
provider (librga PR25): normal, timeout, flush and stop arms PASS. Exact flat
NV12 pixel bytes and PTS match; the timed-out frame is never output; input
references stay retained through the forced wait and return to baseline after
terminal observation. Stop remains blocked beyond two seconds, posts exactly
one error, and finishes after disarm. This simulates missing completion
notification, **not a hung RGA job or kernel reset**. Host tests independently
assert output retention and close-once behavior.

The same standalone d6 benchmark measured 4K 207.1→240.8 fps (+16.3%) and
1080p 758.0→953.5 fps (+25.8%) for FD-sync versus async-depth1. It is not an
in-element throughput measurement or end-to-end capture latency claim.

Installed provider bytes remained unchanged, engine/UI stayed active, session
scratch was removed, and captured kernel logs had no `RGA_BLIT fail`, KASAN or
`BUG:` matches. OPi was off-limits and not contacted. No release, image pin,
cache adoption, PR readiness or merge is implied.
