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
  keeps waiting. At a separate five-second monotonic stop deadline it transfers
  unresolved records to the process-lifetime reaper, posts a second error, and
  returns. Closing a sync-file does not cancel the job: neither threshold returns
  a hardware-owned buffer to its pool.
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

### Signalled does not mean successful

Linux `sync_file` reports `POLLIN` for both successful and error-signalled
fences. After readiness, the backend reads `SYNC_IOC_FILE_INFO` with a zeroed
`sync_file_info` (`num_fences=0`, aggregate status only): positive status is
COMPLETE, negative status is ERROR, and zero stays PENDING with a warning.
Interrupted queries retry; a failed query warns and stays PENDING. `POLLERR`,
`POLLHUP` and `POLLNVAL` likewise retain ownership rather than masquerading as
a terminal fence error. The synchronous `-1` sentinel and missing-fence sentinel
keep their existing meanings.

The converter already handles the enum correctly: `take_ready` returns output
only on COMPLETE; ERROR increments drops, disables async and returns
`GST_FLOW_ERROR` without delivering the pixels. Both local quarantine reaping
and the independent worker release a terminal ERROR just as they release
COMPLETE, after outstanding host references permit destruction. Neither path
uses raw poll flags, and no caller or stop deadline changes are required.

**Driver contract limit:** this relies on a terminal sync-file signal following
hardware retirement. The separate kernel-source investigation found that
explicit request cancellation can error-signal even when reset failure leaves
the job DMA-owned. This status-classification fix neither invokes cancellation
nor repairs that driver defect, and is not an unconditional quiescence proof.
It preserves the existing terminal-release policy; unverified/pending work is
never force-freed. Kernel cancellation/failed-reset safety remains separate.

## Bounded stop and escaped ownership

The five-second ceiling bounds the **wait inside the converter stop callback**,
not arbitrary native calls, GStreamer state-lock acquisition, scheduling delays,
or the entire application's teardown. It gives recovery three more seconds
after the existing two-second diagnostic without making a lost fence hold stop
forever. This is an exceptional fault-path budget, not a promise to fit within
cerastream's whole-teardown five seconds: its EOS/ring/send drains and egress
join already consume part of that separate budget. Normal completion returns
immediately and retains the previous ownership behavior.

The precedent is cerastream's `cerastream-transport/src/rist.rs` and
`srt_transport.rs`: remove the owning reference under a lock and hand it to a
dedicated reaper, which destroys it only once other users have released their
references. RGA additionally requires a terminal completion fence. The converter
uses explicit atomic frame reference counts for this sole-owner check; the last
submitter's release publishes the returned fence before the reaper inspects it.
The queue contains frame records only, **no owning element or backend pointer**.

One lazy `rga-quarantine` thread polls every retained record nonblocking at
100 ms intervals. A permanently pending record never blocks another record's
reclamation. Buffer destruction runs outside the queue mutex, so a pool callback
cannot deadlock a concurrent handoff. There is no stop/finalize join. GStreamer
loads plugins resident in both supported versions, keeping the worker's code
mapped. As with any outstanding GStreamer objects, callers must not invoke
`gst_deinit()` while escaped buffers remain: this is not a drain/cancellation
API. If worker creation fails, the queue still retains ownership, emits an
explicit error, and retries worker creation on the next escape.

Streaming callbacks retain their own element reference while running. A native
submit that itself outlives stop therefore keeps the element alive until its
callback returns; the reaper cannot release its frame while that callback holds
a frame reference. Restart is refused while old submit/wait activity remains.
A frame awaiting only hardware completion does **not** prevent element
finalization. Repeated stop/finalize does not reacquire escaped frames.

### Operator signal and residual risk

Each deadline escape increments a mutex-protected process-wide 64-bit
`quarantine-escapes` counter **once per stop escalation**, not once per frame.
The new `GST_ELEMENT_ERROR` detail includes that exact cumulative count,
`escaped-frames`, `outstanding-frames`, `active-waiters` and `active-submits`.
With `GST_DEBUG=rgaconvert:2`, the reaper also logs reclamation counts and a
reminder every 30 seconds while unresolved frames remain. Counts persist across
element destruction and stream sessions, resetting only with the process.
Growing escape/outstanding counts are a driver/hardware investigation signal,
not an ordinary successful-stop statistic.

**A fence that never becomes terminal retains its frame forever.** This is the
accepted alternative to silently recycling DMA-active storage, not a repair for
a wedged RGA device. Only the stop wait and reaper thread count are bounded;
aggregate retained buffers are NOT bounded across repeated faulting sessions.
Missing-fence submissions also stay retained. No fence is synthesized, no device
reset is attempted, and no driver/library cancellation guarantee is assumed.
This change does not attribute or fix the separate per-cycle RSS residual.

## Executable coverage

The Meson rgaconvert suite covers pre-signalled normal completion, unsignalled
pipe timeout with output/refcount/fd assertions, FLUSH_START propagation with
input and output retained, stop past two seconds with one bus error, and a real
GstHarness chain through parent negotiation/allocation/generation, TAG/custom
events, changed caps and EOS. Host DMA-BUF memory is a test memfd, not hardware.

The stop-bound regression uses two independently owned converters and unsignalled
eventfds. Each stop must take at least five seconds but less than 6.5 seconds
(test scheduling allowance), preserve both buffers/fd, and post the unchanged
two-second error plus the counted five-second escalation. Both elements and test
backends are destroyed before either fence is signalled. The second frame must
be genuinely destroyed while the first remains pending, then the first must be
destroyed on its own signal. Repeated stop/finalize must not wait again. A second
regression blocks native submission across the deadline and proves host lifetime,
restart refusal, late fence publication, flushing return and eventual release.

Failing-first: unchanged `455ee6f8` times out after 15 seconds in stop. Disabling
only reaper reclamation makes the new test fail with zero destroyed buffers
instead of two. Both are distinct negative controls: a bounded return alone
must not disguise a permanently detached leak. These tests are host/container
ownership evidence only; no new hardware run accompanies this fix.

The error-status regression extends the same eventfd/pipe fixture with a
test-only ioctl seam; real `poll()` reports exactly `POLLIN` while the query
returns `-EIO`. On unchanged PR #47 head `c8962c2a`, the assertion returns
COMPLETE (1) instead of ERROR (2). With the query it passes. Additional cases
cover active status, query failure, EINTR retry, hung-up/invalid descriptors,
failed-output rejection with drop accounting, and terminal-error reclamation
through both local quarantine and the independent worker after element/backend
destruction. Existing successful-completion assertions are unchanged. These
are mocked sync-file status semantics, not a real kernel fence or hardware
reset qualification.

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
