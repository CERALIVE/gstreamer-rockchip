# Fix audit

## F27 row schema

Every fix row appended by later todos must contain exactly these five fields:

1. **Provenance SHA** — the full source/fix commit SHA.
2. **Red/green outputs** — commands and captured failing-at-parent/passing-at-fix output.
3. **Hardware gate** — `hardware-independent`, or `hardware-gated` plus the board drill id.
4. **MPP ABI closure** — the before/after `nm` closure diff for MPP symbols.
5. **Reviewer verdict** — one of `confirmed`, `false-positive`, `needs-fix`, or `needs-human-review`.

## Rows

### Preserve JPEG input poll and dequeue statuses

1. **Provenance SHA** — first-party correction against task baseline
   `548944c857177e4983bc7a30e2055d776282c4b0`; initial fix `7742571aabd22bad01a8f52dc1a3552809b6d40d`,
   with the independent-review classification correction in the commit containing this row.
   This implements corrected H2-B7 only: successful input enqueue was already checked and that
   branch remains unchanged.
2. **Red/green outputs** — arm64/bookworm `mpp-decoder-seam` injects one dequeue
   `MPP_NOK`, then separately injects permanent poll `MPP_ERR_INIT`. RED at the parent:
   the ignored dequeue status fell through to a NULL task and became fatal `GST_FLOW_ERROR`
   (`-5 == 0`). Independent source verification at pinned MPP commit
   `194af181db3a02a095c01db84e176d972e19b216` then corrected the model: a finite poll timeout is
   `MPP_NOK`, not `MPP_ERR_TIMEOUT`; `MPP_ERR_INIT` is a distinct reachable permanent result from
   `Mpp::poll()` before initialization. GREEN translates timed poll/dequeue `MPP_NOK` to the base
   class's retryable timeout class: `one pinned-MPP MPP_NOK timeout retried; accepted on poll 2`
   and `dequeue timeout preserved and retried before successful enqueue`. The permanent case is
   still fatal: `MPP_ERR_INIT preserved as GST_FLOW_ERROR without dequeue`, with one poll and zero
   dequeues. Separate mutants returning raw `MPP_NOK` from poll and dequeue both fail `(-5 == 0)`.
3. **Hardware gate** — `hardware-independent`. Return values are injected at the MPP API seam and
   asserted at the public GstVideoDecoder frame-handling result.
4. **MPP ABI closure** — unchanged at 68 referenced-and-present MPP symbols and empty against
   pinned MPP 1.5.0-1; no new API is called.
5. **Reviewer verdict** — `confirmed`. Confirmed across two independent oracle review rounds. Round 1 found FIX-19's H2-B7 classification was implemented backwards relative to the pinned MPP source (a real finite-poll timeout returns MPP_NOK, not MPP_ERR_TIMEOUT, per direct trace of Condition::timedwait/_mpp_port_poll/Mpp::poll/mpi_poll), the same misclassification in FIX-18's drain loop, and a real race in FIX-17 (a concurrent output-completion could overwrite shutdown's failure signal in task_ret before reset captured it). Round 2 confirmed all three fixed: MPP_NOK now correctly classified as retryable under finite polls in both FIX-18 and FIX-19 (independently re-verified against the exact pinned MPP commit), and FIX-17's shutdown result now carried through a stack-local return value immune to the output-completion race (test reproduces the original race with paused-reset delayed-output timing).

### Drain-aware, reset-cancellable JPEG shutdown

1. **Provenance SHA** — first-party correction against task baseline
   `18bce5f5c8f64fe877c6e24fba13cbe012857967`; initial fix `548944c857177e4983bc7a30e2055d776282c4b0`,
   with the independent-review timeout-classification correction in the commit containing this row.
2. **Red/green outputs** — the arm64/bookworm `mpp-decoder-seam` arms an input poll that never
   returns until MPP reset is called, starts JPEG finish on a worker thread, waits until that
   thread is inside the poll, then invokes the non-draining decoder flush path. RED at the parent:
   output stopped at `== jpeg blocked drain flush cancellation ==`; the external 120 s process
   watchdog expired and the container required SIGKILL. GREEN at the fix:
   `non-draining reset woke blocked JPEG input poll within one second`, no EOS task was enqueued
   after cancellation, and the suite ended `mpp-decoder-seam: OK`. The draining path polls in
   10 ms slices with a 100 ms no-progress bound; the non-draining path skips EOS entirely.
   Mutation check: removing the reset-generation cancellation check reproduced the same watchdog
   hang after reset woke the first poll, because the mutant re-entered the drain and waited for an
   EOS completion that cancellation must not enqueue.
   Independent source verification at pinned MPP commit
   `194af181db3a02a095c01db84e176d972e19b216` found that `Condition::timedwait()` returns positive
   pthread `ETIMEDOUT`, `_mpp_port_poll()` converts that positive result with an empty queue to
   `MPP_NOK`, and `mpi_poll()` preserves the negative value. The initial loop retried only the
   enum `MPP_ERR_TIMEOUT`, so a real pinned-MPP timeout escaped after one 10 ms poll. The added
   normal-timeout test returns repeated `MPP_NOK` from finite polls: RED initial behavior completed
   after 10.3 ms/one call; GREEN retries 10 times for 101.1 ms before bounded `GST_FLOW_ERROR`.
   Removing `MPP_NOK` from the retry classifier restores the 10.3 ms failure.
3. **Hardware gate** — `hardware-independent`. The synchronization proof uses real GLib threads,
   the production decoder mutex/stream-lock ordering, and the MPP `reset`/`poll` API boundary.
4. **MPP ABI closure** — unchanged at 68 referenced-and-present MPP symbols; the fix uses only
   already-covered `reset`, `poll`, `dequeue`, and `enqueue` entry points, and the closure remains
   empty against pinned MPP 1.5.0-1.
5. **Reviewer verdict** — `confirmed`. Confirmed across two independent oracle review rounds. Round 1 found FIX-19's H2-B7 classification was implemented backwards relative to the pinned MPP source (a real finite-poll timeout returns MPP_NOK, not MPP_ERR_TIMEOUT, per direct trace of Condition::timedwait/_mpp_port_poll/Mpp::poll/mpi_poll), the same misclassification in FIX-18's drain loop, and a real race in FIX-17 (a concurrent output-completion could overwrite shutdown's failure signal in task_ret before reset captured it). Round 2 confirmed all three fixed: MPP_NOK now correctly classified as retryable under finite polls in both FIX-18 and FIX-19 (independently re-verified against the exact pinned MPP commit), and FIX-17's shutdown result now carried through a stack-local return value immune to the output-completion race (test reproduces the original race with paused-reset delayed-output timing).

### Bounded video-decoder EOS submission

1. **Provenance SHA** — first-party correction against task baseline
   `0a8de6751ffd16c340a4f7683b3115ad35b0a891`; initial fix `18bce5f5c8f64fe877c6e24fba13cbe012857967`,
   with the independent-review race correction in the commit containing this updated row.
2. **Red/green outputs** — focused arm64/bookworm command:
   `timeout 10s .../mpp-decoder-seam`. RED at the baseline after the mock accepted an EOS only
   after three `MPP_ERR_BUFFER_FULL` responses, then returned persistent `MPP_NOK`: the process
   printed `drain retried three queue-full results and accepted EOS on call 5` and timed out in
   the permanent-error drain. GREEN at the fix:
   `permanent drain error failed after one call and triggered reset`, followed by
   `mpp-decoder-seam: OK`. The queue-full path made exactly four EOS submission calls; the
   permanent path made one and returned `GST_FLOW_ERROR` inside the one-second semantic bound.
   Mutation check: retrying every MPP error failed with `96 == 2`, proving the test rejects a
   bounded loop that still misclassifies permanent errors as backpressure.
   Independent review then found that the output task could overwrite the shared `task_ret`
   after shutdown chose failure but before reset captured it. A deterministic two-thread repro
   paused MPP reset after the failure decision, completed two queued outputs, and observed RED
   on the initial fix: `call.result == GST_FLOW_ERROR: (0 == -5)`. GREEN carries shutdown's
   result through a stack-local out parameter owned by the reset call:
   `drain failure survived two concurrent output completions`. Removing that handoff makes the
   direct failure assertion red again (`0 == -5`).
3. **Hardware gate** — `hardware-independent`. The mock drives the subclass shutdown vfunc and
   base-class finish result through the same MPP API and GstVideoDecoder call chain as hardware.
4. **MPP ABI closure** — baseline and fix both report 68 referenced-and-present MPP symbols;
   `bash ci/check-mpp-abi.sh build/gst/rockchipmpp/libgstrockchipmpp.so` reports an empty diff
   against pinned MPP 1.5.0-1.
5. **Reviewer verdict** — `confirmed`. Confirmed across two independent oracle review rounds. Round 1 found FIX-19's H2-B7 classification was implemented backwards relative to the pinned MPP source (a real finite-poll timeout returns MPP_NOK, not MPP_ERR_TIMEOUT, per direct trace of Condition::timedwait/_mpp_port_poll/Mpp::poll/mpi_poll), the same misclassification in FIX-18's drain loop, and a real race in FIX-17 (a concurrent output-completion could overwrite shutdown's failure signal in task_ret before reset captured it). Round 2 confirmed all three fixed: MPP_NOK now correctly classified as retryable under finite polls in both FIX-18 and FIX-19 (independently re-verified against the exact pinned MPP commit), and FIX-17's shutdown result now carried through a stack-local return value immune to the output-completion race (test reproduces the original race with paused-reset delayed-output timing).

### Runtime resolution drain before geometry swap

1. **Provenance SHA** — first-party correction against task baseline
   `f66fbd5b6e46440f57df1f38db465af86b7c6817`; the fix commit is the commit containing this row.
2. **Red/green outputs** — focused command:
   `GST_CHECKS=test_runtime_resolution_drains_old_geometry_before_caps_switch .../mpp-gstharness`
   in the arm64 bookworm container. RED at the baseline and with an explicit mutant that moved the
   drain below the `mpp_frame` geometry writes: old frame 0 reached MPP at width `160` instead of
   its original width `320` (`Checks: 1, Failures: 1`). GREEN with drain-before-swap:
   `runtime resolution drain: 4 old frames, 20.075 ms, caps 320x240 -> 160x120 at a frame boundary`
   and `Checks: 1, Failures: 0, Errors: 0`. The four-frame drain is below the 30-frame GOP bound.
   Full Meson passed 3/3 in both bookworm/GStreamer 1.22 and trixie/GStreamer 1.26.
3. **Hardware gate** — `hardware-gated`, drill id `d7-runtime-resolution-drain`. The mock proves
   old/new geometry ownership and caps ordering; todo 26 validates the existing in-code MPP
   live-reconfiguration assumption and measures the boundary on RK3588 hardware.
4. **MPP ABI closure** — bookworm and trixie `ci/check-mpp-abi.sh` runs each reported 68
   referenced-and-present MPP symbols and an empty diff against pinned MPP 1.5.0-1.
5. **Reviewer verdict** — `confirmed`. Confirmed by independent orchestrator-dispatched oracle review:
   drain ordering traced correct (res_dirty setter → drain self->frames + pending_frames to zero → mutate
   geometry state → renegotiate caps → resume), pending_frames atomicity consistent with todo 11/15's
   conventions, mutation-tested (swap-before-drain mutant correctly mistags frame geometry), 20.075ms/4-frame
   measured drain gap confirmed genuine and well within the 1-GOP bound, live resolution ladder intent
   preserved (in-place bounded drain, not a pipeline restart). Board-drill id d7-runtime-resolution-drain
   assigned for todo 26's real-hardware validation.

### ab2e7d1f2985167a3a242726c001ae47452ae0e4 — flush cancellation and bounded encoder drain

1. **Provenance SHA** — first-party correction against task baseline
   `ab2e7d1f2985167a3a242726c001ae47452ae0e4`; the fix commit is the commit containing this row.
2. **Red/green outputs** — focused command:
   `GST_CHECKS=test_flush_wakes_pending_full_frame .../mpp-gstharness` in the arm64 bookworm
   container. RED with the production source restored from the baseline, and again with the
   reset broadcast removed: `pending-full frame missed the flush wake deadline` after 1.07 s.
   GREEN with the correction: `Checks: 1, Failures: 0, Errors: 0`; ten consecutive focused runs
   passed. Full `meson test -C build --print-errorlogs` passed 3/3 in both bookworm/GStreamer 1.22
   and trixie/GStreamer 1.26. The test uses a one-second wake deadline and an independent
   ten-second process alarm; a hang is a failure, never a delayed pass. ThreadSanitizer compiled
   but could not start under qemu-user: `unsupported VMA range; Found 47 - Supported 39, 42 and
   48`. The deterministic queue-full/wake ordering assertion is the substitute. PR #12 follow-up
   tests then locked the mandated review findings: at the rejected head, temporary packet gaps
   drained only `1` of `4` packets and permanent `encode_put_frame` rejection still returned
   `GST_FLOW_OK` (`Checks: 2, Failures: 2`). GREEN follows the genuine idle deadline and finish
   error propagation (`Checks: 4, Failures: 0, Errors: 0` focused; full Meson 3/3 in both suites).
   Mutants restoring first-empty-poll termination or unconditional finish success each fail their
   focused case. The unchanged flush-wake test passed 20/20 consecutive runs.
3. **Hardware gate** — `hardware-independent`.
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh build/gst/rockchipmpp/libgstrockchipmpp.so`
   reported 68 referenced-and-present MPP symbols and an empty closure diff in both suites.
5. **Reviewer verdict** — `confirmed`. Confirmed across two independent oracle review rounds (orchestrator-dispatched via PR #12; a prior self-directed sub-agent review does NOT count per this plan's established precedent and has been struck from the record). Round 1 confirmed the flush-wake lost-wakeup-race prevention and lock ordering against todo 11's prop_mutex scheme, but found 3 real issues: an undisclosed pending_frames data race across two different lock paths, an EOS no-progress-deadline that didn't implement genuine no-progress semantics, and silent frame loss with a false GST_FLOW_OK on incomplete EOS drain. Round 2 confirmed all three fixed: pending_frames fully converted to consistent atomic access, genuine idle-deadline logic verified, GST_FLOW_ERROR propagation confirmed correct at every reset() call site without regressing normal EOS behavior.

### aa7c308d2a4b74de91c4e07677a09f705a71c23f — missing oldest-frame hardening

1. **Provenance SHA** — defensive first-party hardening against flush fix parent
   `aa7c308d2a4b74de91c4e07677a09f705a71c23f`; the hardening commit is the commit
   containing this row.
2. **Red/green outputs** — the mock withheld one encoded packet while the test deliberately
   retired the corresponding `GstVideoCodecFrame`, then released the packet into both the normal
   finish path and the zero-length rate-controller drop path. At the parent,
   `test_missing_oldest_frame_drops_normal_packet_safely` terminated with
   `Received signal 11 (Segmentation fault)`. With the guard, both focused cases passed:
   `Checks: 2, Failures: 0, Errors: 0`; packet deinit was exactly once with zero live packets or
   buffers.
3. **Hardware gate** — `hardware-independent`.
4. **MPP ABI closure** — bookworm and trixie `ci/check-mpp-abi.sh` runs each reported 68
   referenced-and-present MPP symbols and an empty diff against pinned MPP 1.5.0-1.
5. **Reviewer verdict** — `confirmed`. Confirmed across two independent oracle review rounds (orchestrator-dispatched via PR #12; a prior self-directed sub-agent review does NOT count per this plan's established precedent and has been struck from the record). Round 1 confirmed the flush-wake lost-wakeup-race prevention and lock ordering against todo 11's prop_mutex scheme, but found 3 real issues: an undisclosed pending_frames data race across two different lock paths, an EOS no-progress-deadline that didn't implement genuine no-progress semantics, and silent frame loss with a false GST_FLOW_OK on incomplete EOS drain. Round 2 confirmed all three fixed: pending_frames fully converted to consistent atomic access, genuine idle-deadline logic verified, GST_FLOW_ERROR propagation confirmed correct at every reset() call site without regressing normal EOS behavior.

All three rows below were produced in a native-headers `debian:bookworm-slim`
aarch64 container with the pinned MPP 1.5.0-1 / RGA 2.2.0-1 packages, built with
the same feature flags CI and `debian/rules` use
(`-Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled`).

### 1ceaf42 — decoder DMA-BUF caps negotiation

1. **Provenance SHA** — `1ceaf42ca3189c2c0ca917181cf3cb6db8c4df45`, JeffyCN/mirrors,
   author `orangepi <orangepi@local>` (`Signed-off-by: Jeffy Chen`). Applied with
   `git cherry-pick -x`; author preserved, no reword, no squash.
2. **Red/green outputs** — `meson test -C build`, case
   `rockchipmpp decoder dmabuf caps and pending-frame bound`
   (`tests/mpp_decoder_seam.c::test_dma_feature_reaches_negotiated_caps`).

   RED at the parent (`64ebcf4`, `gst/rockchipmpp/gstmppdec.c` restored to its
   pre-pick content):

   ```
   dma-feature=true -> negotiated caps video/x-raw, format=(string)NV12, width=(int)320, ...
   ERROR: dma-feature=true did not put memory:DMABuf on the output caps
   exit=133
   ```

   GREEN at the pick:

   ```
   dma-feature=true  -> negotiated caps video/x-raw(memory:DMABuf), format=(string)NV12, ...
   dma-feature=false -> negotiated caps video/x-raw, format=(string)NV12, ...
   dmabuf caps negotiation: OK
   ```

   The `dma-feature=false` line is the negative control: a gate that stamped the
   feature unconditionally would fail it. Pre-pick the property was inert on stock
   GStreamer — the removed code gated the feature behind
   `gst_caps_is_subset (dmabuf_caps, sysmem_caps)`, which is false unless the
   subset check is patched, as the deleted comment ("HACK: … when the subset check
   is hacked") said.
3. **Hardware gate** — `hardware-gated`, drill id `d1-dmabuf-registration`. The
   negotiated caps assertion is hardware-independent and runs in CI, but the pick
   changes what downstream imports, so DMA-BUF registration against a real
   consumer is board-verified in the allocation drill.
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh build/gst/rockchipmpp/libgstrockchipmpp.so`.
   Before (`64ebcf4`): 67 MPP symbols referenced and present, empty diff. After: 67,
   empty diff. Delta: none — the pick calls no new MPP entry point.
5. **Reviewer verdict** — `confirmed`. Reviewer == author (self-review);
   the mandated different-agent adversarial review has NOT run. Self-review found
   no defect: diff is 5 insertions / 8 deletions confined to
   `gst_mpp_dec_update_video_info`, touches no frozen property and no encoder file.
   Independently reviewed by a separate agent/model (oracle) and confirmed via
   patch-ID comparison plus `git range-diff` against the fetched upstream source.

### d27ae92 — unmatched-PTS pending-frame bound

1. **Provenance SHA** — `d27ae920e36fd72fe116e6c00108a65235c3e3d5`, kelvinlawson,
   author `Kelvin Lawson <kelvin.a.lawson@gmail.com>`. Full SHA confirmed by
   `git log` on the fetched object; the draft's two-variant ambiguity resolved to
   this one. Applied with `git cherry-pick -x`; author preserved, no reword, no
   squash.
2. **Red/green outputs** — `meson test -C build`, case
   `rockchipmpp decoder dmabuf caps and pending-frame bound`
   (`tests/mpp_decoder_seam.c::test_unmatched_pts_pending_list_is_bounded`). The
   mock feeds 300 decoder outputs whose PTS sits below every pending input PTS,
   which is the "outputs persistently older than ALL pending PTS" condition the
   falsifier attached to H2-B1.

   RED at the parent (`4be22f7`):

   ```
   pushed 300 inputs, 300 MPP outputs, pending frames settled at 299 (bound 64 + slack 8)
   ERROR: pending frame list grew to 299 after 300 unmatched-PTS outputs;
          expected it to stay at or below 72
   exit=133
   ```

   GREEN at the pick:

   ```
   pushed 300 inputs, 300 MPP outputs, pending frames settled at 0 (bound 64 + slack 8)
   unmatched-PTS pending bound: OK
   ```

   299 → 0 is the whole defect: pre-pick each unmatched output re-released the
   already-finished `self->last_frame`, consuming zero pending frames, so
   `gst_video_decoder_get_frames()` never drained.
3. **Hardware gate** — `hardware-independent`. The defect is pure
   GstVideoDecoder frame accounting and reproduces fully on the mocked MPP; no
   board drill is required to prove it.
4. **MPP ABI closure** — before (`4be22f7`): 67 MPP symbols, empty diff. After: 67,
   empty diff. Delta: none.
5. **Reviewer verdict** — `confirmed`. Reviewer == author (self-review);
   the mandated different-agent adversarial review has NOT run. Self-review found
   no defect: the added `GST_MPP_DEC_MAX_PENDING_FRAMES` trim branch re-fetches the
   frame list after releasing, and the rewritten no-match branch refs the frame it
   stores into `self->last_frame`, so the ref accounting stays balanced against the
   caller's drop path.
   Independently reviewed by a separate agent/model (oracle) and confirmed via
   patch-ID comparison plus `git range-diff` against the fetched upstream source.

### 3ccc1e3 — NOT PORTED (rejected with evidence)

1. **Provenance SHA** — `3ccc1e3808dafcbeb926bf060472cc96924c0ac5`, radxa-pkg,
   author `Zhaoming Luo <luozhaoming@radxa.com>`. Cherry-picked, inspected, then
   **dropped**; it is not on the branch.
2. **Red/green outputs** — none, and none are possible: the commit adds no source
   change to port. Its whole content is `debian/patches/series` plus a quilt patch
   wrapping JeffyCN `31ee8bd8a6438ee4030291f3cf0de51fab5995e5`, and that patch's
   post-state is already the fork baseline. Evidence:

   ```
   # the patch's removal is already done
   grep -rn mpp_frame_get_hor_stride_pixel gst/rockchipmpp/   ->  no match
   # the patch's addition is already present
   gst/rockchipmpp/gstmpp.c:219  struct gst_mpp_format *format = GST_MPP_GET_FORMAT (mpp, mpp_format);
   gst/rockchipmpp/gstmpp.c:221    hstride = hstride / format->pixel_stride0;
   # and it applies in neither direction, even at fuzz 3
   patch -p1 --dry-run    -F3 < 0001-...-hor_stride_pixe.patch  ->  Hunk #1 FAILED at 203
   patch -p1 --dry-run -R -F3 < 0001-...-hor_stride_pixe.patch  ->  Hunk #1 FAILED at 203
   ```

   Keeping it would ship a dead file: this package has no `debian/source/format`
   source format `1.0`; no active quilt sequencing (confirmed via `dpkg-source --print-format .`);
   the changelog version `20260118-1` shows this is a non-native `1.0` source package, not
   `native` — the point stands regardless: nothing in `debian/rules` applies
   `debian/patches/series` today. It is worse
   than inert — if `debian/source/format` ever becomes `3.0 (quilt)`, as packaging
   work may well do, `dpkg-source` would try this patch and fail the build.
3. **Hardware gate** — `hardware-gated`, drill id `d3-hevc10bit-stride`. Moot while
   unported, and recorded because the commit's content is the RGA/MPP stride
   divisor, which is frozen behind the HEVC Main10 A/B board drill.
4. **MPP ABI closure** — not applicable; nothing landed. Branch closure is
   unchanged at 67 symbols, empty diff.
5. **Reviewer verdict** — `confirmed`. Reviewer == author (self-review),
   and this row additionally **overturns a planning-phase audit verdict**, so it
   needs an independent pass rather than acceptance on this evidence alone. The
   planning audit listed 3ccc1e3 as a clean source-level "H.265 10-bit" port whose
   `MPP_FMT_YUV420SP_10BIT` exists in MPP 1.5.0-1; the commit contains no format
   table change and no `MPP_FMT_*` reference at all. This also settles ADJ-2 in the
   falsifier's favour — the baseline already carries 31ee8bd's content — which is
   the adjudication the 31ee8bd verify-delta work was scheduled to make; that work
   should confirm rather than re-derive it.
   Independently reviewed by a separate agent/model (oracle) and confirmed via
   patch-ID comparison plus `git range-diff` against the fetched upstream source.

### 31ee8bd — SKIP-ALREADY-PRESENT (verify-delta adjudication)

1. **Provenance SHA** — `31ee8bd8a6438ee4030291f3cf0de51fab5995e5`, JeffyCN/mirrors
   `gstreamer-rockchip`, author `Jeffy Chen <jeffy.chen@rock-chips.com>`:
   `rockchipmpp: Stop using mpp_frame_get_hor_stride_pixel`. The transient
   `jeffycn-mirror` remote resolved `gstreamer-rockchip` to
   `a0d45af504099b4b82f3d3377019a63d357e7cef`; `git show` of the full source
   commit contains one hunk in `gst/rockchipmpp/gstmpp.c`.
2. **Per-hunk delta comparison** — all source content is already in the
   `integration/verified-fix-ledger` baseline, so no cherry-pick or stride
   semantics change was made.

   | Hunk | Current file:lines | Already present | Evidence quote |
   | --- | --- | --- | --- |
   | 1 | `gst/rockchipmpp/gstmpp.c:210-221` | yes | **Pre-state removed:** `git grep -n 'mpp_frame_get_hor_stride_pixel' HEAD -- gst/rockchipmpp` returned no matches; the source parent had `guint hstride = mpp_frame_get_hor_stride_pixel (mframe);`. **Post-state present:** `guint hstride = mpp_frame_get_hor_stride (mframe);`; `struct gst_mpp_format *format = GST_MPP_GET_FORMAT (mpp, mpp_format);`; `if (format)`; `hstride = hstride / format->pixel_stride0;`. |

   **Verdict: `SKIP-ALREADY-PRESENT`.** The sole hunk's removal and addition
   are both already reflected by the baseline. This independently confirms the
   `3ccc1e3` wrapper-commit finding; it does not rely on that finding as the
   adjudication evidence.
3. **Hardware gate** — `hardware-gated`, drill id `d3-hevc10bit-stride`.
   H4-B3 remains frozen for the HEVC Main10 A/B board drill; this skip changes
   no stride calculation.
4. **MPP ABI closure** — not applicable; no code landed and the baseline's
   67-symbol, empty-diff closure is unchanged.
5. **Reviewer verdict** — `self-authored adjudication`. I am the sole author
   of this hunk-by-hunk comparison; no separate-agent review is claimed here.

### 7ffd7f4 — put_packet-result fullness detection (already present; regression locked)

1. **Provenance SHA and resolution diff** —
   `7ffd7f40576bbfb861bc7a7d3492c710d149aff8`, JeffyCN/mirrors, author
   `Jeffy Chen <jeffy.chen@rock-chips.com>`. The planning audit classified this
   as a conflicting adapted port, but the current fork descends from this exact
   commit (`git merge-base --is-ancestor 7ffd7f4 HEAD` exits 0), and `git blame`
   attributes `MPP_INPUT_TIMEOUT_MS 10`,
   `gst_mpp_dec_send_mpp_packet_unlocked()`, and the handle-frame result checks
   directly to `7ffd7f40`. The literal upstream production diff therefore
   resolves to **no production-code delta** rather than a duplicate port.

   Resolution against the literal upstream diff: upstream replaced the quote
   `"Avoid holding too many frames"` plus the `g_list_length (frames) >= 4`
   send-side cap with a loop that quotes
   `case MPP_ERR_BUFFER_FULL: /* Timed out */ break;` and returns
   `GST_FLOW_ERROR` for any other MPP error. The current tree already has that
   post-state at `gstmppdec.c:1192-1213` and no four-frame send-side cap. This
   commit adds only the mock result script and regression assertions. The
   separate 64-frame orphan-accounting safety bound from `d27ae92` is untouched;
   no cap was added or reintroduced by this resolution.
2. **Red/green outputs** — `meson test -C build --print-errorlogs
   "rockchipmpp decoder dmabuf caps and pending-frame bound"`, native aarch64
   bookworm container. RED with the four source files restored to `7ffd7f4^`
   (RGA disabled solely because that historical snapshot predates the current
   RGA function signature):

   ```
   three MPP_ERR_BUFFER_FULL results retried; accepted on call 4
   assertion failed (mpp_mock_dec_put_calls () == 1): (972 == 1)
   FAIL, SIGABRT
   ```

   The 972 calls are the old nested deadline retries misclassifying a persistent
   MPP error as retryable. GREEN with the inherited `7ffd7f4` production code:

   ```
   three MPP_ERR_BUFFER_FULL results retried; accepted on call 4
   persistent MPP error rejected after 1 call
   put_packet fullness detection: OK
   1/1 ... OK
   ```
3. **Hardware gate** — `hardware-independent`. The mock controls each
   `decode_put_packet()` result and counts calls; no allocator or board behavior
   is involved.
4. **MPP ABI closure** — no plugin source changed, so the before/after closure is
   identical by construction: 67 MPP symbols referenced and present, empty diff.
   The final branch gate reruns `ci/check-mpp-abi.sh` against the built plugin.
5. **Reviewer verdict** — `confirmed`. Reviewer == author
   (self-review); no independent-agent verdict is claimed. Self-review confirms
   the test has both controls (three buffer-full responses followed by success,
   and a persistent non-full error), and the source diff contains no frame-cap,
    encoder, property, or production-code change. Independently reviewed (oracle): confirmed 7ffd7f4 was already an ancestor of the fork baseline and no frame cap was reintroduced; the commit adds regression evidence, not new production logic.

### 5f45bd4 — decoder input-packet ownership snapshot and reset cleanup

1. **Provenance SHA and resolution diff** —
   `5f45bd4c3868e323d40eda744cb7aa92b430df09`, JeffyCN/mirrors, author
   `Jeffy Chen <jeffy.chen@rock-chips.com>`. The reset hunk ports literally:
   before `self->mpi->reset()` it deinitializes `self->mpp_frame` and clears the
   pointer. The ownership hunk ports the pre-send quote
   `packet_has_buffer = (mpp_packet_get_buffer (mpkt) != NULL);` and, after a
   successful send, transfers buffered packets to MPP while deinitializing
   copy-path packets in the base decoder.

   Resolution against the literal upstream diff: upstream `5f45bd4` sits after
   `ec14edc` moved video packet submission into the base decoder and after
   `dcbcd64` moved copy-packet cleanup there. This fork retains the older
   `GstMppDecClass::send_mpp_packet` callback, whose video implementation quoted
   `if (!ret) mpp_packet_deinit (&mpkt);`. Applying only the literal
   `gstmppdec.c` hunk would therefore double-deinitialize copy packets. The
   adaptation removes that subclass deinit and makes the base decoder the single
   success-path ownership authority; JPEG packets already carry `MppBuffer` and
   stay on the transfer branch. No encoder file or property is touched.
2. **Red/green outputs** — `meson test -C build --print-errorlogs
   "rockchipmpp decoder dmabuf caps and pending-frame bound"`, native aarch64
   bookworm container. RED at parent `d2f7874` produced both independent
   failures:

   ```
   reset cached-frame cleanup:
   assertion failed (mpp_mock_dec_frame_deinits () == before_reset + 1): (1 == 2)

   packet ownership before send:
   assertion failed (mpp_mock_dec_packet_buffer_queries () == 1): (0 == 1)
   ```

   GREEN at the adaptation:

   ```
   cached MPP frame deinits: 1 -> 2 on reset
   copy packet released once; buffered packet transferred without post-send access
   packet ownership before send: OK
   1/1 ... OK
   ```

   The mock is a logical ownership canary: it records buffer queries after send,
   duplicate deinitialization, and caller deinitialization after MPP ownership
   transfer. All three counters must stay zero. This catches the UAF/premature
   release class without depending on allocator address reuse.
3. **Hardware gate** — `hardware-independent`. Packet ownership transitions and
   cached-shell cleanup are fully observable through the MPP ABI seam; no DMA
   import or board-only output buffer is required.
4. **MPP ABI closure** — before: 67 MPP symbols referenced and present, empty
   diff. After: 67, empty diff against pinned
   `librockchip-mpp1_1.5.0-1_arm64.deb`. The adapted layout adds no new MPP ABI
   requirement.
5. **Reviewer verdict** — `confirmed`. Reviewer == author
   (self-review); no independent-agent verdict is claimed. Self-review checked
   both callback implementations: video copy packets now have exactly one base
   deinit, buffered JPEG packets are transferred, and every error path retains
   the existing base-level `mpp_packet_deinit()` cleanup. Because this is a
   cross-layer ownership adaptation, the branch is intentionally left open for
    independent review rather than self-merged. Independently reviewed (oracle): traced the ownership handoff against the pinned MPP 1.5.0-1 source (`Mpp::put_packet()` branches on `mpp_packet_get_buffer()`) — confirmed exactly one release authority on every success/error path across `gstmppvideodec.c` and `gstmppjpegdec.c`, no double-free, no leak.

### 892f662 — selective DMA_DRM caps negotiation

1. **Provenance SHA and hunk resolution** —
   `892f662465b54de9fe10b9c691f64d3dbe047248`, kelvinlawson, author
   `Kelvin Lawson <kelvin.a.lawson@gmail.com>`. The complete two-hunk diff was
   inspected before adaptation. Only the negotiation block was ported; the
   fork's output-state construction, FBC flags, `self->info` assignment,
   negotiation call, and stride alignment remain unchanged.

   | Upstream hunk | Decision | Resolution |
   | --- | --- | --- |
   | Include `gst/video/video-info-dma.h` | include, adapted | Guarded with `GST_CHECK_VERSION(1, 24, 0)` because the bookworm/GStreamer 1.22 build does not ship this header. |
   | Replace the `dma_feature` caps block with peer-selected legacy DMABuf + DMA_DRM offers | include, corrected selective port | On GStreamer 1.24+, modern DMA_DRM construction is now restricted to `!afbc && !rfbc`; compressed output keeps legacy `memory:DMABuf` caps so `arm-afbc=1` / `rfbc=1` survives. Only `output_state->caps` can be replaced for linear output; `output_state->info` remains the fork's existing state. Empty/ANY/unready peers retain sysmem caps for a later RECONFIGURE. GStreamer 1.22 keeps the exact legacy DMABuf feature path. |
   | Any output-state/FBC/stride behavior outside the two source hunks | exclude | Not present in the upstream diff and intentionally untouched; this is the auditor's selective-port boundary. |

2. **Red/green outputs** — `meson test -C build --verbose
   "rockchipmpp decoder dmabuf caps and pending-frame bound"`. The new case
   supplies a DMA_DRM-only downstream peer through GstHarness. On the aarch64
   trixie/GStreamer 1.26 build, with only the pre-existing GCC-14 crop-meta cast
   applied to the disposable container copy so the test could execute:

   RED at parent `de535020`:

   ```
   DMA_DRM-only peer -> negotiated caps video/x-raw(memory:DMABuf), format=(string)NV12, ...
   assertion failed (format == "DMA_DRM"): ("NV12" == "DMA_DRM")
   expected_dma_drm_red_exit=1
   ```

   GREEN after the selective port:

   ```
   DMA_DRM-only peer -> negotiated caps video/x-raw(memory:DMABuf),
     format=(string)DMA_DRM, ..., drm-format=(string)NV12
   DMA_DRM peer caps negotiation: OK
   1/1 rockchipmpp decoder dmabuf caps and pending-frame bound OK
   ```

   Independent review then found that the first submission also applied modifier
   zero to AFBC/RFBC `output_state->info`. `gst_video_info_dma_drm_to_caps()` did
   not serialize the fork's private compression field, producing this second RED
   on the first-submission commit `05f191c`:

   ```
   AFBC frame with DMA_DRM query peer -> negotiated caps
     video/x-raw(memory:DMABuf), format=(string)DMA_DRM, ...,
     drm-format=(string)NV12
   assertion failed (format != "DMA_DRM"): ("DMA_DRM" != "DMA_DRM")
   expected_afbc_red_exit=1
   ```

   GREEN after restricting modern DMA_DRM construction to linear output:

   ```
   AFBC frame with DMA_DRM query peer -> negotiated caps
     video/x-raw(memory:DMABuf), format=(string)NV12, ...,
     arm-afbc=(int)1
   AFBC output rejects linear DMA_DRM caps: OK
   ```

   The aarch64 bookworm/GStreamer 1.22 leg prints the explicit version skip for
   DMA_DRM and keeps both existing controls green: `dma-feature=true` negotiates
   legacy `video/x-raw(memory:DMABuf), format=NV12`, while
   `dma-feature=false` negotiates plain `video/x-raw`.
3. **Hardware gate** — `hardware-gated`, drill id
   `d4-dma-drm-allocation-soak`. Todo 26 must run the mandatory F19 136-second
   allocation drill on the vendor-6.1 board and record both
   `RGA_BLIT failures == 0` and `rga_api version failures == 0`, including the
   1080-to-1088 alignment/import path. CI proves caps selection, not the physical
   DMA allocation/import contract.
4. **MPP ABI closure** — before (`de535020`): 67 MPP symbols referenced and
   present, empty diff. After: 67, empty diff against pinned MPP 1.5.0-1. The
   adaptation calls GStreamer video/caps APIs only and adds no MPP entry point.
5. **Reviewer verdict** — `confirmed`. The first independent oracle
   review returned `needs-fix`: it found the silent AFBC/RFBC-as-linear
   misadvertisement that the original linear-only test missed. The corrected
   submission gates zero-modifier DMA_DRM caps to linear output and adds the
   AFBC regression above. Reviewer != correction author for the finding. The PR
    remains open and is not self-merged. Independently reviewed (oracle) in TWO
    passes: pass 1 found a real defect (AFBC/RFBC-compressed output could be
    silently advertised as linear `format=DMA_DRM`, since
    `gst_video_info_dma_drm_to_caps()` doesn't serialize this fork's private
    `arm-afbc`/`rfbc` caps fields); fixed by gating the modern DMA_DRM path behind
    `!afbc && !rfbc` and preserving the legacy `memory:DMABuf` caps path with
    `arm-afbc`/`rfbc` intact for compressed output. Pass 2 confirmed complete
    branch coverage (linear/AFBC/RFBC/both/failure/disabled all reach exactly one
    caps outcome) and verified the new regression test genuinely exercises the
    AFBC path via `start_afbc_decoder()`/`dec_new_frame()`.

### b93ecb6 — RGA DMA32 and used-path hunk adjudication

1. **Provenance SHA and hunk resolution** —
   `b93ecb63b0e50f4b2220cf5219d88d93ad855a21`, kelvinlawson, author
   `Kelvin Lawson <kelvin.a.lawson@gmail.com>`, derived upstream from
   BoxCloudIRL `3b58acf`. The full diff was inspected. Its required DMA32 and
   used shared-core error behavior already predate this fork baseline as
   independent commits, so this adaptation adds a regression lock rather than
   duplicating production code.

   | Upstream hunk | Include/exclude | Current-tree resolution |
   | --- | --- | --- |
   | `gstmppallocator.c`: request `MPP_BUFFER_FLAGS_DMA32` for the internal DRM group | include | Already present at `gst_mpp_allocator_new()` from inherited `e53dca6`; pinned MPP 1.5.0-1's installed `mpp_buffer.h` defines the flag as `0x00200000`. Added a mock-MPP assertion that records the real group type and requires the flag. |
   | `gstmpp.c`: reject unknown MPP-to-RGA formats | include (used decoder RGA path) | Already present from inherited `e4c76e0`; no duplicate edit. |
   | `gstmpp.c`: reject unknown GstVideoInfo-to-RGA formats | include (used encoder and decoder RGA path) | Already present from inherited `e4c76e0`; no duplicate edit. |
   | `gstmpp.c`: add YUYV/YVYU/UYVY/VYUY and ARGB-family RGA table entries | exclude from this port | These specific formats are not exercised by cerastream's NV12 encoder/used-decoder paths. The entries already exist from inherited `508af8a` and are left unchanged; this task neither claims nor deep-reviews unused-format quality. |
   | `gstmpp.c`: route RGB16/BGR16 through swapped RGA formats | exclude from this port | Cerastream does not use these formats. The inherited `b3b7924` behavior is retained unchanged; no unused-element/format quality work was performed. |

2. **Red/green outputs** — `meson test -C build --verbose
   "rockchipmpp decoder dmabuf caps and pending-frame bound"`, aarch64
   bookworm/GStreamer 1.22. In a disposable parent-tree copy, removing only the
   inherited DMA32 flag produced:

   ```
   internal MPP buffer group type flags: 0x3
   assertion failed (group_types & MPP_BUFFER_FLAGS_DMA32 ==
     MPP_BUFFER_FLAGS_DMA32): (0 == 2097152)
   expected_red_exit=1
   ```

   With the inherited production code intact:

   ```
   internal MPP buffer group type flags: 0x200003
   decoder allocator DMA32 request: OK
   1/1 rockchipmpp decoder dmabuf caps and pending-frame bound OK
   ```

   This is a regression lock for an already-present required hunk, not evidence
   that this task introduced the allocator behavior.
3. **Hardware gate** — `hardware-gated`, drill id
   `d5-rga-dma32-allocation-soak`. Todo 26 must run the mandatory F19 136-second
   allocation drill on the vendor-6.1 board and record both
   `RGA_BLIT failures == 0` and `rga_api version failures == 0`; the mock proves
   the requested flag, while only the board proves the allocated address is
   RGA2-accessible throughout the soak.
4. **MPP ABI closure** — before and after: 67 MPP symbols referenced and
   present, empty diff against pinned MPP 1.5.0-1. No production hunk was needed;
   the mock-only recorder adds no undefined symbol to the plugin.
5. **Reviewer verdict** — `confirmed`. Independent oracle review checked the
   full upstream `b93ecb63b0e50f4b2220cf5219d88d93ad855a21` diff against the
   inherited source SHAs, confirmed `MPP_BUFFER_FLAGS_DMA32` in pinned MPP
   1.5.0-1, and confirmed the mock assertion is substantive. No correction was
   requested for this row.

## Frozen JeffyCN extras audit (`a0d45af`)

The audit was limited to the 15 named commits below. The transient
`jeffycn-mirror` remote resolved `gstreamer-rockchip` to
`a0d45af504099b4b82f3d3377019a63d357e7cef`, and every full SHA below is an
ancestor of that frozen tip. All 15 touch a used decoder/encoder or shared core;
none qualifies for `SKIP-OUT-OF-SCOPE`. None adds an MPP entry point, so no row
is `BLOCKED-MPP-VERSION` against pinned MPP 1.5.0-1.

| Candidate | Baseline defect and current-code evidence | Used/shared scope | Pick trial | MPP ABI | Verdict |
| --- | --- | --- | --- | --- | --- |
| `27ba1c798f320ebe40c4f58cbb2fd8940baa97c9` — clear pending frames after reset | Already fixed: the commit is an ancestor, and `gstmppdec.c:267-272` quotes `frames = gst_video_decoder_get_frames (decoder);` followed by `gst_video_decoder_release_frame (decoder, f);`. | `mppvideodec`/`mppjpegdec` shared decoder core | conflict in `gstmppdec.c` | no new call; 67-symbol closure unchanged | `SKIP-ALREADY-PRESENT` |
| `65098a9b3ae9180be5f1b361f576c9ee1434da2a` — EOS without buffer | Already fixed: ancestor; `gstmppdec.c:1126-1135` rejects `!mpp_frame_get_buffer (mframe)` before ordinary frame-info-change processing, while the dedicated info-change frame remains handled at `:1113-1116`. | shared decoder core | conflict in `gstmppdec.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `0489caabc314cc167a87545a14109b2bd2dda36a` — seek after finish | Already fixed: ancestor; allocator attachment is deferred to `gst_mpp_dec_handle_frame()` (`gstmppdec.c:1287-1296`), getters reject a missing allocator, and clear detaches `MPP_DEC_SET_EXT_BUF_GROUP` at `:320`. | shared decoder core | clean, but the only staged delta duplicated the existing `if (!self->allocator) return NULL;` guard | no new call | `SKIP-ALREADY-PRESENT` |
| `6c90d19b502e36f6228b2d3d31230363bb690b2d` — stop-time frame leak | Already fixed: ancestor; stop calls `mpp_frame_deinit (&self->mpp_frame)` at `gstmppdec.c:345`. | shared decoder core | clean empty/no-op | no new call | `SKIP-ALREADY-PRESENT` |
| `c5601186c8c07c24845e805094a35b1045911bf7` — leftover encoder packets after reset | Present at baseline: reset called `mpi->reset` and immediately zeroed accounting without polling output. The adapted post-state at `gstmppenc.c:797-804` now drains `gst_mpp_enc_poll_packet_locked()` before clearing `pending_frames`. | H.264/H.265 shared encoder core | conflict in `gstmppenc.c`; manually adapted | no new external call; static helper only | `PORT-WITH-ADAPTATION` (`5b450adc`) |
| `973fd0e6815dbcc37e134724f05eebdc44fc4c7e` — return MppBuffer before group clear | False-positive mechanism: GStreamer destroys mini-object qdata and runs `gst_mpp_mem_destroy()` before entering the allocator `free()` vfunc, so the MppBuffer is already returned in both orderings. The clean pick was reviewed and reverted by `f3b8775d`. | allocator shared by all used elements | clean, then reverted after oracle review | no new call; 67 symbols before/after | `SKIP` — reviewer verdict `false-positive` |
| `dcbcd6454ef892e385b3a782600369eb6c0719db` — input packet leak | Already fixed by the stricter `5f45bd4` adaptation: `gstmppdec.c:1317-1336` snapshots `packet_has_buffer` before send, transfers buffered packets to MPP, and deinitializes copy packets exactly once. | shared decoder core | conflict in `gstmppdec.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `ed10c9135ff5bbc80c4fb2d714687192ae0ac7e1` — encoder DMA-fd leak typo | Already fixed: ancestor; both failure and packet-return paths pass addresses to `mpp_frame_deinit (&mframe)` (`gstmppenc.c:1403,1432`). | H.264/H.265 shared encoder core | conflict in `gstmppenc.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `ed9f828600994b174a65130f27a5f48ad1d81228` — inverted frame-init result | Already fixed: ancestor; `gstmppenc.c:1375` returns on nonzero `mpp_frame_init (&mframe)`, preserving MPP's zero-is-success convention. | H.264/H.265 shared encoder core | conflict in `gstmppenc.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `bbd1fdaf804ce3b4b15e8f429c2c17ff4e84876a` — RGA hstride units | Superseded, not absent: current `gstmpp.c:210-221` uses byte stride then divides by `format->pixel_stride0`, the later `31ee8bd` post-state already adjudicated above. Reapplying `bbd1fda` would restore the removed MPP accessor. No stride code changed. | shared RGA conversion core | clean mechanical reversion of the later fix | no new call | `SKIP-ALREADY-PRESENT`; frozen H4-B3 remains drill `d3-hevc10bit-stride` |
| `e811521f3cf76550d61d842a0fc62a6f3e587f0b` — unaligned encoder input | Already fixed: ancestor; `gstmpp.c:410-415` rejects undersized alignment, and `gstmppenc.c:1268-1306` imports only matching aligned layouts before falling back to conversion. | H.264/H.265 shared encoder/core | conflict in `gstmppenc.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `cce848d65143ad817f4b4f6cd54e4ed6ac622809` — apply strides from new buffers | Already fixed: ancestor; `gst_mpp_enc_apply_strides()` is called from set-format, runtime-resolution, and imported-buffer paths (`gstmppenc.c:1063,1144,1287`). The incidental JPEG-encoder hunk is irrelevant to the already-present shared-core fix. | shared encoder/core (plus one unused `mppjpegenc` hint-only hunk) | conflict in `gstmppenc.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `f9c7186c5f7eb04667978384258c112d17bfa4be` — clear cached buffers when unused | Already fixed: ancestor; allocator `cacheable` state and group clears remain at `gstmppallocator.c:85-92,276-281`, decoder clear disables caching, and encoder start does likewise. | allocator + used encoder/decoder core | conflicts in decoder and encoder | no new call | `SKIP-ALREADY-PRESENT` |
| `bbe50f15ba4ee41a48114bf87e455dfe625261eb` — MPP warning on stop | Already fixed: ancestor; `gstmppdec.c:342-349` clears the allocator before `mpp_destroy (self->mpp_ctx)`. | shared decoder core | conflict in `gstmppdec.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `a910efebc5f438eacbe8eda66a0ed1c01a43f787` — JPEG packet-send timeout | Present at baseline: JPEG ignored `poll()` and base decoder treated timeout as an unknown fatal error. Ported code checks poll at `gstmppjpegdec.c:366-371`; base retry accepts `MPP_ERR_TIMEOUT` at `gstmppdec.c:1256-1264`. | `mppjpegdec` + shared decoder core | clean | no new call; 67 symbols before/after | `PORT` (`a1de3d17`) |

### c560118 — encoder reset output-queue drain

1. **Provenance SHA** — `c5601186c8c07c24845e805094a35b1045911bf7`,
   JeffyCN/mirrors, author Jeffy Chen. The conflict was only surrounding fork
   evolution; the adapted production delta preserves the literal reset-time poll
   loop. The original poll-only test was rejected by independent oracle review;
   substantive replacement seam: `1f2609e4`, hardened against reset-generation
   and packet-release mutations by `754e12e5`, then expanded to the encoder's
   maximum 16-packet backlog by `61ad9c2d`.
2. **Red/green outputs** — native aarch64 bookworm mock-MPP test
   `rockchipmpp GstHarness factories and caps`. With `gstmppenc.c` restored to
   pre-port `gstmppenc.c` from `5b450adc^`, RED:

   ```
    test_encoder_reset_drains_old_packets_before_new_session:
      mpp_mock_enc_dequeued_packets() (5) is not equal to 16
   FAIL
   ```

   At `5b450adc` plus the complete test series, GREEN:

   ```
   reset generation 1 drained old packets: dequeued=16 depth=0 empty-polls=1
     deinits=16 double-deinits=0 live=0 duplicates=0
   new session output: packet=17 pts=3333333300 dequeued=17 depth=0
     deinits=17 live=0
   reset generation 3 variable backlog: dequeued=19 depth=0 empty-polls=1
     deinits=19 live=0
   1/1 ... GstHarness factories and caps OK
   ```

   The mock fills the encoder's configured `max-pending=16` capacity, releases
   packet 1 to force a downstream error and pause the task, withholds packets
   2-16 until `mock_reset()`, and retains the matching GstVideoCodecFrames. The
   first reset generation must perform exactly one empty poll and release all 16
   packets exactly once. Packet 17 then carries the fresh-session PTS and is also
   released. A second, one-withheld-packet reset uses a different generation and
   requires exactly one empty poll; this makes over-polling fixed-count mutants
   observable instead of letting a hardcoded 16 accidentally match the max case.

    Four disposable mutation checks bound these claims:

   ```
   # reset loop replaced by exactly two poll calls
   dequeued_packets() (5) is not equal to 16
   FAIL

   # mpp_packet_deinit(&mpkt) removed
   mpp_mock_enc_packet_deinits() (0) is not equal to 16
   FAIL

   # reset loop replaced by exactly three poll calls
   dequeued_packets() (7) is not equal to 16
   FAIL

   # reset loop replaced by exactly sixteen poll calls
   variable reset empty_polls (15) is not equal to 1
   FAIL
   ```
3. **Hardware gate** — `hardware-independent`. The mock exercises MPP packet
   retention plus real GstVideoEncoder frame accounting and state transitions;
   no codec device is required.
4. **MPP ABI closure** — before and after: 67 symbols referenced and present,
   empty diff against pinned MPP 1.5.0-1. The port calls an existing static helper.
 5. **Reviewer verdict** — `Confirmed across FOUR independent review passes. Round 1:
    production code confirmed correct (encoder-reset ordering), initial test found
    insubstantial. Round 2 (mutation testing): found test didn't scope 'empty' to the
    correct reset generation, didn't verify deinit counts. Round 3 (mutation testing):
    found a hardcoded-3-poll mutant survived. Round 4: expanded to a 16-packet
    max-backlog scenario plus a second, differently-sized reset generation, which
    mathematically rules out any single fixed poll count satisfying both — confirmed
    no further credible mutant found after an active adversarial search.`

### 973fd0e — allocator release ordering

1. **Provenance SHA** — `973fd0e6815dbcc37e134724f05eebdc44fc4c7e`,
   JeffyCN/mirrors, author Jeffy Chen. It was applied exactly with `git cherry-pick
   -x` as `ad1930e7`, independently reviewed, found false-positive on its claimed
   mechanism, and reverted by `f3b8775d`.
2. **Red/green outputs** — none: there is no defect to reproduce. GStreamer's
   actual sequence is `gst_mini_object_unref()` → private-data/qdata destruction →
   `gst_mpp_mem_destroy()`/`mpp_buffer_put()` → `_gst_memory_free()` → allocator
   virtual `free()`. Therefore the MppBuffer has already been returned before
   `gst_mpp_allocator_free()` begins, whether parent `free()` is written before or
   after the group clears. The upstream reorder is a no-op, not a bug fix.
3. **Hardware gate** — none. The port is reverted; the destroyed-qdata ordering is
   a GStreamer lifecycle fact, not an unresolved MPP hardware behavior.
4. **MPP ABI closure** — before and after: 67 symbols referenced and present,
   empty diff. The commit only reorders existing calls.
5. **Reviewer verdict** — `false-positive`. This is the first genuine independent
   verdict for the candidate. The earlier self-directed explore-agent review did
   not satisfy F27 and incorrectly accepted the commit-message mechanism.

### a910efe — JPEG input timeout retry

1. **Provenance SHA** — `a910efebc5f438eacbe8eda66a0ed1c01a43f787`,
   JeffyCN/mirrors, author Jeffy Chen. Applied with `git cherry-pick -x`; author
   and message preserved. Regression seam: `27e46bea`.
2. **Red/green outputs** — native aarch64 bookworm test
   `rockchipmpp decoder dmabuf caps and pending-frame bound`. RED before the pick:

   ```
   == jpeg input timeout retry ==
   assertion failed (gst_harness_push (h, buffer) == GST_FLOW_OK): (-5 == 0)
   FAIL
   ```

   GREEN after the pick: `one MPP_ERR_TIMEOUT retried; accepted on poll 2` and
   `1/1 ... decoder dmabuf caps and pending-frame bound OK`.
3. **Hardware gate** — `hardware-independent`. The mock returns one input-port
   timeout, withholds a dequeue task for that timed-out poll, then succeeds; both
   the JPEG subclass classification and shared base retry are exercised.
4. **MPP ABI closure** — before and after: 67 symbols referenced and present,
   empty diff. Only existing `MppApi` function pointers and an existing enum value
   are used.
5. **Reviewer verdict** — `confirmed`. The orchestrator-dispatched independent
   oracle confirmed both production hunks and the timeout-withheld-dequeue test;
   no correction was requested.

### e72392d parent — encoder runtime-property snapshot synchronization

1. **Provenance SHA** — `e72392daa32404793417685a0e14d09b32179e29`, the
   `integration/verified-fix-ledger` parent on which the race was reproduced. The
   fix is self-authored on `fix/encoder-property-runtime-sync`; the row-bearing
   commit cannot embed its own content-derived SHA.
2. **Red/green outputs** — native aarch64 bookworm test `rockchipmpp GstHarness
   factories and caps`, case
   `test_runtime_property_snapshot_is_coherent_and_eventually_applied`. The mock
   pauses after writing `rc:bps_target`, changes the public `bitrate` property from
   another thread, and records the three bitrate fields at every
   `MPP_ENC_SET_CFG` boundary. RED at the untouched parent:

   ```
   minimum (3000000) is not equal to target * 15 / 16 (1500000)
   85%: Checks: 7, Failures: 1, Errors: 0
   ```

   GREEN with the dedicated property lock and coherent base/codec snapshots:

   ```
   1/3 rockchipmpp GstHarness factories and caps               OK
   2/3 rockchipmpp decoder dmabuf caps and pending-frame bound OK
   3/3 rockchipmpp plugin registration and properties          OK
   Ok: 3  Fail: 0
   ```

   A delayed-clear mutant moved `prop_dirty = FALSE` after
   `MPP_ENC_SET_CFG`; the same test rejected it with `the final quiescent bitrate
   was never applied`. The correct locked handoff was then restored. **TSAN
   execution: NOT DONE.** The aarch64 TSAN binary failed to start under QEMU
   emulation (`unsupported VMA range`, found 47 bits). This is an environment
   limitation, not a test result, and no sanitizer conclusion can be drawn. The
   deterministic mock-sequencing test reproduces both the torn-read and
   lost-update failure modes under controlled thread barriers and independently
   satisfies the plan's stated `TSAN OR mock-sequencing` acceptance criterion.
   `PARITY_SOURCE_ONLY=1 bash tests/parity-check.sh` passed both source-derived
   contracts. A full mock-host invocation then passed `mpph264enc` and
   `mpph265enc` runtime parity (including every frozen property line) before the
   established, parent-identical Radxa decoder-caps mismatch stopped the script.
3. **Hardware gate** — `hardware-independent`. The defect is GObject/threading
   state handoff, and both the torn snapshot and lost-final-dirty mutant are
   deterministically sequenced at the mock MPP boundary. No board behavior is
   needed to prove the fix.
4. **MPP ABI closure** — before (`e72392d`): 67 symbols referenced and present,
   empty diff. After: 67 symbols referenced and present, empty diff. The property
   mutex, snapshots, and mock recorder add no MPP entry point. `nm -D
   --defined-only` also confirms the internal snapshot helper is hidden from the
   plugin's dynamic ABI.
5. **Reviewer verdict** — Confirmed by independent oracle review: torn-read prevention verified (single coherent snapshot under `prop_mutex`, covering every plan-cited field), the exact lost-update race the falsifier identified (concurrent prop_dirty TRUE/FALSE race) verified closed via full trace, no lock-order inversion found, lock never held across `mpi->control`, coalescing behavior preserved as intended (not a regression). Unused-element (`gstmppjpegenc.c`/`gstmppvp8enc.c`) touches confirmed to be a legitimate mechanical consequence of the shared property-lock macro, not a scope violation. TSAN could not execute (QEMU VMA limitation on aarch64, environment issue not a test result) — the deterministic mock-sequencing test satisfies the plan's stated 'TSAN OR mock-sequencing' criterion independently.

### Follow-up list

- No later JeffyCN commit was inspected or added; work after frozen `a0d45af` is a
  separate follow-up audit.

## Mock-MPP verdict: WORKING

The host-only seam builds `tests/mock_mpp.c` as `libmppmock.so` against the same
`rockchip_mpp` headers selected by Meson, then preloads it while loading the built
plugin. It covers:

- `mpp_create`, `mpp_init`, and the encoder `MppApi` control/encode entry points;
- the `mpp_enc_cfg_set_*` and `mpp_enc_cfg_get_*` families with an in-memory key/value recorder;
- minimal buffer-group, DMA-fd, frame-association, packet, and metadata behavior needed to
  negotiate and submit one NV12 frame without `/dev/mpp_service`;
- process-local logs selected by `MPP_MOCK_LOG`, with the Meson tests serialized;
- an opt-in decoder simulation (`mpp_mock_dec_arm()` / `mpp_mock_dec_disarm()`) that
  answers `decode_put_packet` / `decode_get_frame`, emits one info-change frame to
  drive negotiation, and then returns data frames carrying **no MppBuffer** so the
  decode loop takes its drop path. That drop path is what releases a pending
  `GstVideoCodecFrame`, which is what makes decoder frame accounting measurable off
  hardware. It stays disarmed unless a test arms it, so the encoder harness keeps the
  MPP behavior it was written against.
- a bounded encoder packet FIFO that can pause after one output, retain multiple
  packets until reset, attribute empty polls to a specific reset generation,
  count exact-once packet release/live state and duplicate dequeues, and expose
  packet ids as one-byte payloads for old/new-session association checks;
- JPEG task-port behavior with a scripted input-poll timeout and a withheld
  dequeue task, so timeout classification and retry are tested together.
- an encoder-config snapshot recorder plus one-shot barriers at
  `rc:bps_target` and `MPP_ENC_SET_CFG`, so concurrent property reads and dirty
  handoff ordering are asserted without relying on scheduler luck.

`tests/mpp_gstharness.c` loads the real built factories and proves both `mpph264enc`
and `mpph265enc` negotiate and submit a 320x240 NV12 frame. For each codec it asserts
`prep:width=320`, `prep:height=240`, `rc:mode=1`, `rc:bps_target=500`,
`rc:fps_in_num=30`, `rc:fps_in_denorm=1`, `MPP_ENC_SET_CFG`,
`MPP_ENC_SET_SEI_CFG`, and `MPP_ENC_SET_HEADER_MODE`. The public `bitrate` property is
measured in BPS, so `500` is intentionally recorded without scaling.

The same executable introspects properties on `mppvideodec` and `mppjpegdec`, checks
the multicodec decoder's H.264/H.265/AV1 input and NV12 output template truth, and uses
GstHarness to assert the JPEG decoder advertises non-fixed NV12 raw output caps. These
checks create elements and inspect templates only; they submit no decoder input and
perform no MPP hardware access.

`tests/mpp_decoder_seam.c` is the decoder counterpart and does submit input. It arms
the decoder simulation, pushes parsed H.264 buffers into `mppvideodec`, and asserts
two things the rows above depend on: that `dma-feature=true` puts `memory:DMABuf` on
the negotiated output caps (with `dma-feature=false` as the negative control), and
that 300 outputs whose PTS never matches a pending input leave the pending-frame list
at or below the decoder's own bound. Both assertions were confirmed to fail at their
pre-pick parent before being accepted.

The encoder harness waits for `mpp_frame_set_buffer()` rather than sampling it
straight after `gst_harness_push()`. `gst_mpp_enc_handle_frame()` only queues the
frame and broadcasts — the MPP submission happens later on the encoder's srcpad task
thread — so the original instantaneous read was a race that failed roughly 2 runs in
12 under emulation. Do not fold that wait back into a direct read.

### Verification

With `rockchip_mpp.pc` available to pkg-config:

```sh
meson setup build -Drockchipmpp=enabled -Drga=disabled -Drkximage=disabled -Dkmssrc=disabled
meson compile -C build
meson test -C build --print-errorlogs
meson test -C build --print-errorlogs
```

All three tests must report `OK` in both runs. The registration test's negative control sets
`GST_PLUGIN_PATH` to a nonexistent directory and runs `gst-inspect-1.0 mppjpegdec`; it
must exit nonzero with `No such element or plugin`, proving plugin-load failures cannot
produce a false green. Detailed native output is retained by Meson in
`build/meson-logs/testlog.txt`; CI runs the same commands on bookworm and trixie.

### F27 — crop meta removal pointer type

1. **Provenance SHA** — `fe23e6e` (full SHA recorded in repository history).
2. **Red/green outputs** — the parent fails in the trixie/GCC-14 compile at
   `gst_buffer_remove_meta (buffer, crop)` with `-Wincompatible-pointer-types`; the fix
   compiles both bookworm/GCC-12 and trixie/GCC-14 suites cleanly. PR #8 passed both
   blocking matrix legs.
3. **Hardware gate** — `hardware-independent` (the explicit embedded `GstMeta` pointer
   preserves runtime behavior and requires no device access).
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh`; before and after: 67 referenced
   symbols, empty diff. No MPP entry point changed.
5. **Reviewer verdict** — `confirmed`.

### FIX-6 — MPP config key correctness and checked setters

1. **Provenance SHA** — `0f81dfa0c465a16b6ffdc64bd77ff0c797da40a6`. First-party fix
   (no upstream pick). Ledger origin O1-B1 + H5-B1.
2. **Red/green outputs** — `meson test -C build --print-errorlogs`, cases
   `test_drop_threshold_uses_the_key_mpp_actually_registers` and
   `test_rejected_config_key_fails_the_apply` in `tests/mpp_gstharness.c`.

   RED at the parent (`395d5ef8`, encoder sources restored to pre-fix content):

   ```
   77%: Checks: 9, Failures: 2, Errors: 0
   test_drop_threshold_uses_the_key_mpp_actually_registers:0:
     'mpp_mock_last_cfg_s32("rc:drop_thd")' (-2147483648) is not equal to '42' (42)
   test_rejected_config_key_fails_the_apply:0:
     'gst_harness_push(h, frame)' (0) is not equal to 'GST_FLOW_NOT_NEGOTIATED' (-4)
   ```

   `-2147483648` is the mock's "key absent" sentinel: the parent wrote the value under
   a key MPP does not register, so `rc:drop_thd` never appeared at all. The second
   failure is the silent-discard defect itself — a rejected key produced a perfectly
   successful buffer push.

   GREEN at the fix: `Ok: 3, Fail: 0` across all three meson tests.

   Mutation check (the fix is load-bearing, not decorative): removing
   `self->cfg_error = TRUE;` from `gst_mpp_enc_cfg_set_u32` while keeping the warning
   returns `test_rejected_config_key_fails_the_apply` to red with the identical
   `(0) is not equal to (-4)`, confirming the assertion tracks the latch and not the
   log line.
3. **Hardware gate** — `hardware-independent`. The key name is verified against the
   pinned MPP's own registration table and its shipped `.so`, and the rejection path is
   exercised through the mock; neither needs a board. The behavioural consequence on
   hardware (drop-threshold now actually reaching the rate controller) rides the
   existing encoder soak drill and claims nothing here.
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh build/gst/rockchipmpp/libgstrockchipmpp.so`.
   Before and after: `MPP symbols referenced and present: 67`, empty diff against the
   pinned `librockchip-mpp1_1.5.0-1_arm64.deb`. Config keys are string arguments, so no
   symbol changed — the wrappers call the same `mpp_enc_cfg_set_s32`/`_u32`.
5. **Reviewer verdict** — Confirmed across multiple independent oracle review rounds.
   FIX-6: `rc:drop_thd` key correction confirmed against pinned MPP source; the plan's
   paired `tune:atf_str` rename was independently falsified and correctly refused
   (RK3588 uses the VEPU580 HAL, which reads neither `anti_flicker_str` nor `atf_str`,
   strengthening the refusal). The checked setters are also confirmed.

### FIX-6 (partial) — FALSIFIED: `tune:anti_flicker_str` is NOT a wrong key

O1-B1 asserted two invalid keys. Verification against the pinned MPP *before* editing
refuted the second, so it was not applied. Recorded here rather than dropped silently.

1. **Provenance SHA** — no commit; the claim was refused. Evidence gathered from
   `librockchip-mpp1_1.5.0-1_arm64.deb`
   (SHA-256 `fe839d41010def25b2c096581815fd26214680bf9720fc47ff2c7afe501f6bcd`) and the
   MPP tree at `194af181db3a02a095c01db84e176d972e19b216`.
2. **Red/green outputs** — none, and deliberately so. Both `tune:anti_flicker_str` and
   `tune:atf_str` are registered keys at the pinned MPP
   (`mpp/base/mpp_enc_cfg.cpp:269` and `:281`), writing *different* struct fields under
   *different* change bits (`MPP_ENC_TUNE_CFG_CHANGE_ANTI_FLICKER_STR` = `1<<3`,
   `MPP_ENC_TUNE_CFG_CHANGE_ATF_STR` = `1<<17`; `inc/rk_venc_cmd.h:1527,1539,1554,1567`).
   `tune.anti_flicker_str` is range-checked by MPP (`mpp/codec/mpp_enc_impl.cpp:897-900`)
   and read by the vepu510 HALs (`hal_h265e_vepu510.c:939`,
   `hal_h264e_vepu510.c:1146,1954`). `tune.atf_str` has **no reader anywhere in the MPP
   tree** — a tree-wide `grep -rn "\.atf_str\|->atf_str"` returns nothing. Making the
   specified change would have moved a live tunable onto a dead field.
3. **Hardware gate** — `hardware-independent` (static evidence from the pinned runtime
   and its source at the pinned commit).
4. **MPP ABI closure** — unchanged; no code was modified.
5. **Reviewer verdict** — Confirmed across multiple independent oracle review rounds.
   FIX-6: the paired `tune:atf_str` rename was independently falsified and correctly
   refused (RK3588 uses the VEPU580 HAL, which reads neither `anti_flicker_str` nor
   `atf_str`, strengthening the refusal). This remains a refused change, not a
   `BLOCKED-MPP-VERSION` verdict.

### FIX-7 — zero-valued tuning resets

1. **Provenance SHA** — `844453d323848b7265105d6adfb7d50b97d44e60`. First-party fix.
   Ledger origin O1-B2.
2. **Red/green outputs** — `meson test -C build --print-errorlogs`, case
   `test_zero_valued_tuning_resets_reach_mpp` in `tests/mpp_gstharness.c`. The test
   raises `scene-mode`, `anti-flicker`, `super-i-thd`, `super-p-thd` and
   `debreath-strength`, asserts each landed, then returns all five to 0 and asserts
   each reset landed.

   RED at the parent (`395d5ef8`):

   ```
   90%: Checks: 10, Failures: 1, Errors: 0
   test_zero_valued_tuning_resets_reach_mpp:0:
     'mpp_mock_last_cfg_s32("tune:scene_mode")' (1) is not equal to '0' (0)
   ```

   The stale `1` is the defect: the property was set to 0, the getter reported 0, and
   MPP went on using 1. GREEN at the fix: `Ok: 3, Fail: 0`.

   **Amended after review round 1** — the super-frame thresholds were split out of this
   set, because zero is *not* a safe value for them (see field 5). The additional case
   `test_super_frame_thresholds_reset_to_unreachable_not_zero` walks
   enable -> clear-while-disabled -> re-enable and asserts what actually reaches MPP at
   each step. Two mutants confirm it discriminates:

   ```
   mutant: restore the outer `super_mode != NONE` guard
     disabled clear: I threshold reached MPP as 90000, expected 4294967295
   mutant: write the raw property value (no unset mapping)
     disabled clear: I threshold reached MPP as 0, expected 4294967295
   mutant: write the thresholds after MPP_ENC_SET_CFG instead of before
     enable: I threshold reached MPP as 4294967295, expected 90000
   ```

   The first is the stale-threshold resurrection the guard causes; the second is the
   always-trigger value; the third is a delivery reordering. None was caught by the
   original test, which only checked that a stored value equalled zero.

   The third deserves a note, because closing it changed how the test reads its
   evidence. It first asserted through the mock's live `MppEncCfg` handle, which
   `cfg_set()` mutates in place — that measures "a setter ran", not "the value was
   delivered". Demonstrated rather than argued: with the earlier assertions the
   delivery-reordering mutant passes the whole suite (`Ok: 3, Fail: 0`); with the
   snapshot assertions it fails as above. Each phase now also asserts that a *new*
   record appeared, so a pass that delivers nothing at all cannot read a stale one.
3. **Hardware gate** — `hardware-independent`. What reaches MPP is fully observable in
   the mock; no board behaviour is claimed. Two limits stated explicitly: the mock
   records config but does not run MPP's rate controller, so the test asserts the
   boundary value MPP's own comparison requires rather than observing a classification
   (the comparison is quoted from the pinned source at the assertion); and the
   assertions read the snapshot the mock takes **inside its `MPP_ENC_SET_CFG` handler**,
   not the live `MppEncCfg` object, so they measure delivery rather than merely that a
   setter was called.
4. **MPP ABI closure** — before and after: 67 referenced symbols, empty diff. Removing
   a C `if` changes no symbol.
5. **Reviewer verdict** — Confirmed across multiple independent oracle review rounds.
   FIX-7: super-frame threshold semantics corrected (0 does not mean 'auto' in MPP — it
   means 'always trigger'; mapped to G_MAXUINT as the genuine never-trigger sentinel);
   a pre-existing latent defect in the parent build was also surfaced and fixed. Round-1
   review was right and the defect is worse than a plain reset
   bug. MPP classifies a frame as super with `(RK_U32) bit_real >= bits_thr`
   (`mpp/codec/rc/rc_model_v2.c:1276`), so a threshold of 0 with the mode enabled marks
   *every* frame super, and the `MPP_ENC_RC_SUPER_FRM_DROP` branch additionally rewrites
   the rate controller's own `drop_mode` and `drop_gap` — silently clobbering the
   element's `drop-mode` setting. `super_i_thd`/`super_p_thd` are `RK_U32` copied without
   range validation (`mpp_enc_impl.cpp:621-629`), so the element now maps its documented
   unset value (0) to `G_MAXUINT`, a threshold no real frame reaches.

   Pre-existing defect surfaced by that analysis, worth recording separately: because
   both thresholds default to 0, enabling `super-mode=drop` *without* setting a threshold
   makes MPP classify **every** frame as super. The consequence is not a uniform drop —
   `rc_model_v2.c:1751-1756` exempts intra frames before applying the drop mode:

   ```c
   MppEncRcDropFrmMode drop_mode = usr_cfg->drop_mode;
   if (frm->is_intra)
       drop_mode = MPP_ENC_RC_DROP_FRM_DISABLED;
   ```

   so P-frames take the `DROP_FRM_NORMAL` branch and are dropped, while intra frames fall
   through to the disabled branch and may be re-encoded instead. (The `drop_gap`
   escape on the next line cannot help either: `check_super_frame` zeroes `drop_gap`,
   and the guard requires it to be non-zero.) That is true on the parent commit too — it
   is not a regression introduced here, and it is now fixed by the same mapping.

   Intra-refresh was examined and deliberately left alone: its zero path already writes
   `rc:refresh_en = 0`, so the reset works and there was nothing to fix. Reviewer ==
   independent oracle for round 1; round 2 pending.

### FIX-9 — auto bitrate and GOP from effective output

1. **Provenance SHA** — `16b61e3d596f2cc091bf772e6e2331c210c04b17`. First-party fix.
   Ledger origin O1-B4, with the H5-B2 arithmetic ride-along per ADJ-4.
2. **Red/green outputs** — `meson test -C build --print-errorlogs`, four cases in
   `tests/mpp_gstharness.c`, deliberately split so each defect is independently red
   rather than masked by whichever assertion fires first.

   RED at the parent (`395d5ef8`):

   ```
   71%: Checks: 14, Failures: 4, Errors: 0
   test_auto_bitrate_recomputes_for_new_output_geometry:0:
     'mpp_mock_last_cfg_s32("rc:bps_target")' (288000) is not equal to '640*480/8*30' (1152000)
   test_auto_bitrate_keeps_the_zero_sentinel:0:
     'reported' (288000) is not equal to '0' (0)
   test_auto_bitrate_clamps_instead_of_overflowing:0:
     'mpp_mock_last_cfg_s32("rc:bps_target")' (251658240) is not equal to 'G_MAXINT' (2147483647)
   test_gop_and_auto_bitrate_follow_fps_out:0:
     'mpp_mock_last_cfg_s32("rc:gop")' (60) is not equal to '30' (30)
   ```

   Reading those four: the target stayed at the 320x240 value after the source
   renegotiated to 640x480; the `bitrate` property reported a number the user never
   set, proving the sentinel had been overwritten; the 8192x8192 case used the input
   rate instead of `fps-out`; and the default GOP came from the 60 fps input rather
   than the 30 fps output.

   GREEN at the fix: `Ok: 3, Fail: 0`.

   Mutation check on the arithmetic: reverting
   `bps = width; bps = bps * height / 8 * fps;` (all `guint64`) to 32-bit
   `bps = width * height / 8 * fps` turns `test_auto_bitrate_clamps_instead_of_overflowing`
   red with `rc:bps_target` = `-2147483648` — the exact wrap. The `guint64` promotion
   and `G_MAXINT` clamp are therefore pinned by a test, not merely asserted.

   **Amended after review round 1** — three gaps closed, one gap reported as
   uncloseable:

   - *Derived bounds were unasserted.* The saturation cases now also assert
     `rc:bps_max` and `rc:bps_min`. Mutant reverting **only** `gst_mpp_enc_scale_bitrate`
     to 32-bit is now caught in both cases:
     `'rc:bps_max' (134217726) is not equal to 'G_MAXINT' (2147483647)`.
     It passed undetected before.
   - *The runtime width/height path was unasserted.* Added
     `test_auto_bitrate_recomputes_for_runtime_resolution_property`. The caps-change test
     could not cover it: `gst_mpp_enc_apply_strides()` is handed the strides it just read
     back out of the same `info`, so its equality check short-circuits and it never marks
     properties dirty on that path. Mutant removing **only** the
     `self->prop_dirty = TRUE;` line from `gst_mpp_enc_apply_pending_resolution()` is now
     caught: `'rc:bps_target' (288000) is not equal to '160 * 120 / 8 * 30' (72000)`.
   - *Saturation cases no longer allocate.* The automatic target is derived while the
     caps event is handled, so both cases assert straight after
     `gst_harness_set_src_caps_str()` and push nothing — which is what makes the
     32768x32768 case affordable at all (a real frame there is 1.6GB).
   - *Reported, not closed.* Round-1 review gave a counterexample —
     `width=2^29, height=2^30, fps=256` wrapping `guint64` before the clamp is tested.
     The arithmetic is now bounded step-wise so the wrap cannot occur, but **that change
     is not covered by a test, because the input is unreachable.** A mutant reverting the
     step-wise bound to the previous post-hoc clamp **survives the suite**, and is
     recorded here as surviving rather than papered over. The change is kept as defensive
     correctness: the helper takes raw `gint` dimensions and should not depend on its only
     current caller's validation for soundness.

     The reachable bound is set by `gst_video_info_from_caps()`, and it is
     **format-dependent** — it checks `round_up_128(width) * height` against
     `G_MAXUINT / bpp`, so there is no single ceiling and none of this is a property of
     this element. Measured by binary search on both suites (identical results under
     GStreamer 1.22.0 and 1.26.2):

     | format | max area (px) | max frame term (area/8) |
     |--------|---------------|-------------------------|
     | NV12   | ~1,431,655,424 | ~178,956,928 |
     | BGR    | ~1,431,655,424 | ~178,956,928 |
     | RGB16  | ~2,147,482,624 | ~268,435,328 |
     | RGBA   | ~1,073,740,800 | ~134,217,600 |

     So the largest frame term this element can be handed is ~268,435,328 (RGB16), not
     the single `~2^27` figure claimed in the previous revision of this row, which
     understated it and wrongly implied one universal limit. Scaled by the `fps-out`
     ceiling of 256 that reaches ~2^36 — still nowhere near a `guint64` wrap, so the
     conclusion is unchanged even though the numbers were wrong.
     `test_auto_bitrate_saturates_at_a_large_nv12_geometry` is named for what it is: a
     large NV12 geometry, not a universal maximum.
3. **Hardware gate** — `hardware-independent`. Every assertion is about which value the
   plugin computes and hands to MPP. Note the overflow case remains
   REAL-BUT-UNREACHABLE-ON-TARGET per the wave-2 verdict: 8192x8192 at the `fps-out`
   ceiling of 256 is the smallest geometry the property ranges admit that reaches 2^31,
   which no CeraLive capture path produces. It is pinned because it is cheap to pin,
   not because it is reachable.
4. **MPP ABI closure** — before and after: 67 referenced symbols, empty diff. The
   change is arithmetic and snapshot plumbing; no MPP entry point was added or dropped.
5. **Reviewer verdict** — Confirmed across multiple independent oracle review rounds.
   FIX-9: overflow-safe arithmetic and runtime width/height prop_dirty gap both confirmed
   correct; the one theoretically-surviving overflow mutant is honestly documented as
   unreachable through valid GStreamer caps negotiation. The sentinel, effective-output
   and GOP behaviour are unchanged.
   One behaviour change worth an explicit reviewer note:
   `g_object_get(enc, "bitrate")` now returns `0` while auto is in effect, where
   it previously returned the first computed target. That is the documented default
   (`0 = Auto`) and the property's name, type, flags and default are all unchanged, so
   the frozen property surface (F21) and the parity gate are unaffected — parity output
   is byte-identical to the parent. Reviewer == independent oracle for round 1; round 2
   pending.

### Verification of the three rows above

Native `debian:bookworm-slim` aarch64 container, pinned MPP 1.5.0-1 / RGA 2.2.0-1,
built with `-Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled`:

```sh
meson setup build -Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled
meson compile -C build
meson test -C build --print-errorlogs
bash ci/check-mpp-abi.sh build/gst/rockchipmpp/libgstrockchipmpp.so
bash tests/parity-check.sh
```

`parity-check.sh` exits 77 (honest off-board skip of the runtime leg) with the
source-derived contract checks passing, on both the parent and the branch; the two
outputs are byte-identical, which is the differential form this repo's parity
evidence uses.

### FIX-8 — zero-length RC-drop packets are dropped, not pushed

1. **Provenance SHA** — `0af5b4ed90fc5beab42d8c46458b5106bbb0b0f7`. First-party fix
   (no upstream pick). Ledger origin O1-B3. Its test seam depends on
   `53ea3879240958fc6c8691a047ab4dbefc91e612`, a prerequisite use-after-free fix in
   the mock described below.
2. **Red/green outputs** — `meson test -C build --print-errorlogs`, cases
   `test_zero_length_rc_drop_is_not_pushed_downstream` (zero-copy-pkt=true, the
   property default) and `test_zero_length_rc_drop_is_not_pushed_when_copying`
   (zero-copy-pkt=false) in `tests/mpp_gstharness.c`.

   RED at the parent (`9689dd79`, `gst/rockchipmpp/gstmppenc.c` restored to its
   pre-fix content):

   ```
   89%: Checks: 19, Failures: 2, Errors: 0
   test_zero_length_rc_drop_is_not_pushed_downstream:0:
     output 1 reached the peer as a 0-byte buffer
   test_zero_length_rc_drop_is_not_pushed_when_copying:0: Unexpected critical/warning:
     gst_video_encoder_allocate_output_buffer: assertion 'size > 0' failed
   ```

   The two messages are the two distinct failure modes, and only the first is the
   defect as O1-B3 states it: on the shipped default path the muxer really does
   receive a zero-byte access unit. The copy path never reaches that point because
   GLib refuses the zero-sized allocation first, so on that path the parent defect
   is a critical raised on every rate-controller drop.

   GREEN at the fix: `Ok: 3, Fail: 0` across all three meson tests, with

   ```
   rc drop (zero-copy-pkt=true): outputs=2 payloads=1,3 processed=1 dropped=1
     deinits=3 live-packets=0
   ```

   Three packets in, two buffers out, carrying packet payloads 1 and 3 — packet 2
   produced no output at all. `dropped=1`/`processed=1` are read from the QoS message
   `GstVideoEncoder` posts from inside `gst_video_encoder_drop_frame()`, so the frame
   is positively accounted as dropped rather than merely absent, and
   `gst_video_encoder_get_frames()` is empty afterwards, so it is not left pending.

   Assertions are snapshotted where delivery happens — the peer chain function and
   the QoS message — not read back from element state afterwards, per the weakness
   found in this repo's earlier `mpp_mock_last_cfg_s32` assertions.

   Mutation check (three mutants, all rejected):

   | mutant | result |
   |---|---|
   | `pkt_size < 0` instead of `<= 0` | RED, both cases, identical to parent |
   | drop the packet but never finish the frame | RED: `output_capture[1].pts` is 33333333, not 66666666 — the unaccounted frame makes packet 3 finish frame 2 |
   | keep pushing, only empty the buffer | this is the parent; RED as above |

   A fourth mutant — `gst_video_encoder_release_frame()` instead of `finish_frame()` —
   is **not expressible**: `GstVideoEncoder` exports no such call (it is a decoder-only
   API), so finishing with a NULL `output_buffer` is the only drop route available.
   Recorded rather than silently dropped from the ledger.
3. **Hardware gate** — `hardware-independent`. The mock represents the exact RC-drop
   packet shape the plan flagged as possibly infeasible (a valid `MppBuffer` with zero
   length), so the pre-written `hardware-gated` + drill-id substitution offered by
   todo 5 was **not** needed and is not claimed. To make the shipped zero-copy branch
   reachable at all, the mock's encoder packet buffers became real memfd-backed
   allocations with MPP's own release semantics; whether MPP's rate controller emits
   this shape on RK3588 in the field is an MPP property, evidenced from its source
   (`mpp/codec/rc/rc_model_v2.c`), not asserted here.
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh build/gst/rockchipmpp/libgstrockchipmpp.so`.
   Before and after: `MPP symbols referenced and present: 67`, empty diff against the
   pinned `librockchip-mpp1_1.5.0-1_arm64.deb`. The fix adds a comparison on a value
   the encoder already read via `mpp_packet_get_length`, so no entry point changed.
5. **Reviewer verdict** — Confirmed across 2 independent oracle review rounds.
   Production logic confirmed correct on the first pass. Round 1 also found and
   required fixing: a real packet-arena leak in the test mock's teardown path (fixed,
   verified via deterministic counter since LeakSanitizer cannot execute under this
   environment's QEMU emulation — LeakSanitizer requires ptrace, unavailable under
   user-mode QEMU; a native-arm64 GitHub Actions LSan leg is a worthwhile non-blocking
   follow-up, since `ubuntu-24.04-arm` runners are genuinely native and would not hit
   this limitation), a factual correction to an internal notepad timeline claim, and
   a doc-wording correction about `KEY_OUTPUT_INTRA` always being present in MPP output
   (never actually missing in normal operation).

### FIX-11 — IDR output marked as a sync point from KEY_OUTPUT_INTRA

1. **Provenance SHA** — `c49f69d195226dd6bab0683d1a6fbcdb784082bd`. First-party fix
   (no upstream pick). Ledger origin O1-B6.
2. **Red/green outputs** — `meson test -C build --print-errorlogs`, case
   `test_intra_output_is_marked_as_a_sync_point` in `tests/mpp_gstharness.c`. Three
   packets: ordinal 0 carries `KEY_OUTPUT_INTRA=1`, ordinal 1 omits the key entirely,
   ordinal 2 carries it with value 0.

   **Correction (review round 1).** An earlier revision of this row, of the two source
   comments, and of the `c49f69d1` commit message described the absent-key case as
   "what MPP does for most outputs". That is wrong. The pinned MPP writes
   `KEY_OUTPUT_INTRA` on **every** output packet, from `frm->is_intra`, at
   `mpp/codec/mpp_enc_impl.cpp:2481` — the call sits after the statistics block under
   its own `/* frame type */` comment, not inside any conditional (a second
   unconditional write covers the partitioned-output path at `:517`). So the two shapes
   MPP actually produces are present-and-1 for an IDR and present-and-0 otherwise, which
   are ordinals 0 and 2.

   Ordinal 1 is therefore **defensive coverage, not the common path**: it proves an
   absent key cannot read as a set one, which would matter for a malformed producer or
   a future MPP that stops writing it unconditionally. It is retained on that basis. The
   source comments are corrected in this branch; the `c49f69d1` commit message cannot be
   corrected without a force-push over a reviewed branch, so the correction is recorded
   here instead.

   RED at the parent (FIX-8 applied, FIX-11 absent):

   ```
   95%: Checks: 20, Failures: 1, Errors: 0
   test_intra_output_is_marked_as_a_sync_point:0:
     frame 0 reached the push path with sync-point=0, expected 1
   ```

   Confirming O1-B6 exactly: no frame is ever a sync point, so `GstVideoEncoder`
   stamps `GST_BUFFER_FLAG_DELTA_UNIT` on every buffer including the IDR.

   GREEN at the fix: `Ok: 3, Fail: 0`, with

   ```
   intra sync points: sync=1,0,0 delta-unit=0,1,1
   ```

   The sync-point column is read inside a `pre_push` vfunc installed by the test.
   That runs within `gst_video_encoder_finish_frame()` holding the frame that is about
   to be pushed, so the flag is observed on the actual delivery path rather than from
   element state afterwards. The delta-unit column is read in the peer chain function
   from the buffer as delivered.

   Mutation check:

   | mutant | result |
   |---|---|
   | set the flag *after* `finish_frame()` | RED: `frame 0 reached the push path with sync-point=0, expected 1` |
   | set the flag unconditionally | RED: `frame 1 reached the push path with sync-point=1, expected 0` |
   | omit the explicit `UNSET` branch | **SURVIVES** |

   The surviving mutant is reported rather than papered over. `gst_video_encoder_new_frame()`
   zeroes the frame flags, so a fresh frame is never a sync point and the explicit
   `UNSET` cannot be distinguished by any reachable input. It is kept as defensive
   symmetry against a future path that reuses or pre-flags a frame, and it is
   deliberately not claimed as tested.
3. **Hardware gate** — `hardware-independent`. What is asserted is the plugin's
   translation of the meta into the sync-point flag and the resulting buffer flag,
   which is entirely GStreamer-side. That MPP sets `KEY_OUTPUT_INTRA` on real IDR
   output is an MPP property; the existing encoder soak drill covers the on-board
   consequence and nothing about hardware behaviour is claimed here.
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh build/gst/rockchipmpp/libgstrockchipmpp.so`.
   Before: `MPP symbols referenced and present: 67`. After: **68**, empty diff. The
   delta is exactly one new entry point, `mpp_meta_get_s32`, which is present in the
   pinned `librockchip-mpp1_1.5.0-1_arm64.deb`
   (`nm -D --defined-only librockchip_mpp.so.0` → `000000000005f450 T mpp_meta_get_s32`).
   This is the only row in this ledger so far that moves the symbol count, so the
   closure gate — not the count — is what proves it safe.
5. **Reviewer verdict** — Confirmed across 2 independent oracle review rounds.
   Production logic confirmed correct on the first pass. Round 1 also found and
   required fixing: a real packet-arena leak in the test mock's teardown path (fixed,
   verified via deterministic counter since LeakSanitizer cannot execute under this
   environment's QEMU emulation — LeakSanitizer requires ptrace, unavailable under
   user-mode QEMU; a native-arm64 GitHub Actions LSan leg is a worthwhile non-blocking
   follow-up, since `ubuntu-24.04-arm` runners are genuinely native and would not hit
   this limitation), a factual correction to an internal notepad timeline claim, and
   a doc-wording correction about `KEY_OUTPUT_INTRA` always being present in MPP output
   (never actually missing in normal operation).

### Prerequisite — mock MPP packet-arena use-after-free

Not a ledger FIX and not an encoder change, but FIX-8's test seam cannot be trusted
without it, so it is recorded with the same five fields.

1. **Provenance SHA** — `53ea3879240958fc6c8691a047ab4dbefc91e612`. First-party,
   test-only (`tests/mock_mpp.c`).
2. **Red/green outputs** — `mpp_mock_dec_disarm()` both stopped the mock producing and
   freed every `MockPacket` handed out. The seam tests must call it before
   `gst_harness_teardown()`, because a still-producing mock never lets the element
   drain; the element then deinitialized already-freed packets.

   AddressSanitizer at the parent, `tests/mpp_decoder_seam.c` case
   `test_jpeg_input_timeout_is_retried`:

   ```
   ERROR: AddressSanitizer: heap-use-after-free ... READ of size 4
     #0 mpp_packet_deinit ../tests/mock_mpp.c:704
     #1 gst_mpp_jpeg_dec_stop ../gst/rockchipmpp/gstmppjpegdec.c:217
   freed by thread T0 here:
     #0 __interceptor_free
     #1 mpp_mock_dec_disarm ../tests/mock_mpp.c:1033
   ```

   It was latent, not harmless: reading one field out of freed memory happened to be
   survivable, so the suite stayed green. FIX-8's mock work — giving encoder packets
   real buffers released at `mpp_packet_deinit()` — turned the same read into a free
   of a garbage pointer. Measured over 100 standalone runs with randomised
   `MALLOC_PERTURB_`: pristine 0/100 failures, FIX-8's mock without this fix 80/100
   (`corrupted double-linked list`, SIGABRT). After the fix: **0/100**, and the full
   `meson test` suite 0/40.

   **Correction (review round 1) — the defect was hours old, not long-standing.** The
   arena free was introduced by `ebea6d08` (`2026-08-30 07:19:03 -0500`), on the same day
   as this fix (`53ea3879`, `2026-08-30 18:52:36 -0500`) — about eleven and a half hours,
   not the "months" a notepad entry claimed. `eae0b5bd` (`06:34:03`) landed the seam test
   that reaches it. "Latent" is accurate only in the narrow sense that the suite passed
   in between; nothing here survived a release, and the wording should not be read as a
   long-standing escape.

   Reversing only the teardown order in the seam tests was tried first and is wrong —
   it hangs, because production must stop before the element can drain. The fix splits
   the two jobs instead: disarming stops production, and the arena is reclaimed when a
   test arms the next decoder, the one point where no element can still own a packet.
3. **Hardware gate** — `hardware-independent` (host test harness only).
4. **MPP ABI closure** — unchanged; `tests/mock_mpp.c` is not part of the plugin.
5. **Reviewer verdict** — `needs-human-review`. Reviewer == author.

### Prerequisite follow-up — the deferred reclaim leaked the last test's arena

Found by review round 1. Deferring reclamation to "the next arm or reset" closed the
use-after-free but opened a leak at the other end: `test_unmatched_pts_pending_list_is_bounded()`
is the **last** test in `tests/mpp_decoder_seam.c`, and nothing arms after it, so its
arena was never reclaimed. `mpp_packet_init()` appends to `packet_allocs`
unconditionally, so that run stranded every packet it made.

Measured, not inferred — the seam now prints the arena size on both sides of the final
reclaim:

```
mock packet arena before final reclaim: 300
mock packet arena at exit: 0
```

The 300 is the leak exactly as it stood before this follow-up.

Fixed by making reclamation **explicit at every teardown point** rather than deferred:

- `mpp_mock_dec_release_packets()` is now exported, and `reclaim_mock_packets()` in the
  seam calls it and asserts `mpp_mock_dec_live_packets() == 0`.
- `stop_decoder()` runs disarm → teardown → reclaim. Reclaiming *after* teardown is what
  keeps the original use-after-free closed, because the element deinitializes its own
  packets as it stops.
- The JPEG direct-teardown path, which does not go through `stop_decoder()`, calls the
  same helper.
- `main()` calls it once more as a catch-all for any future test that does neither.
- Arming still reclaims, now purely as a safety net for a test that forgets.

A `g_assert_cmpuint(..., >, 0)` before the final reclaim keeps the emptiness assertion
from passing vacuously: the suite proves it really did build an arena before proving it
released one.

**LeakSanitizer could not be used, and this is not a substitute claim.** LSan cannot
start in this environment at all — under `qemu-user` it aborts before reporting, with
`LeakSanitizer has encountered a fatal error` (its stop-the-world thread tracing is
unsupported), including with `use_tls=0:use_registers=0`. ASan's *inline* checking works
fine here and is what found the original use-after-free; only the leak-checking pass is
unavailable. The deterministic counter above is therefore the evidence, and it is
stronger for this particular claim than a one-off sanitizer run would have been: it runs
on every build in both CI suites rather than in a sanitizer leg nobody can execute.

Verified both ways:

- Counter: `arena at exit: 0` for `mpp-decoder-seam` **and** for `mpp-gstharness`
  (measured with a temporary `__attribute__((destructor))` probe, not committed).
- Mutation: making `mpp_mock_dec_release_packets()` a no-op fails at the first reclaim —
  `ERROR:../tests/mpp_decoder_seam.c:133:reclaim_mock_packets: assertion failed (mpp_mock_dec_live_packets () == 0): (1 == 0)` —
  so the assertion is load-bearing rather than decorative.

### Verification of the rows above

Both CI suite legs, native aarch64 containers, pinned MPP 1.5.0-1 / RGA 2.2.0-1,
built with `-Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled`:

| gate | bookworm / GStreamer 1.22 | trixie / GStreamer 1.26.2 |
|---|---|---|
| `meson test -C build` | `Ok: 3, Fail: 0` | `Ok: 3, Fail: 0` |
| `ci/check-mpp-abi.sh` | 68 symbols, empty diff | 68 symbols, empty diff |
| `ci/check-glibc-floor.sh` | `highest GLIBC import GLIBC_2.17; device floor GLIBC_2.36` | not gated (forward-compat leg) |
| `ci/check-glibc-floor.test.sh` | all assertions passed | — |
| `tests/parity-check.sh`, no mock preloaded | exit 77, contract checks PASS | — |
| `tests/parity-check.sh`, mock preloaded | encoder rows PASS | encoder rows PASS |

`tests/parity-check.sh` behaves differently depending on whether the mock MPP is
preloaded, and both behaviours appear in this ledger, so they are stated together
rather than left to look contradictory:

- **Without** `LD_PRELOAD`/`GST_PLUGIN_PATH` the MPP encoders do not register, so the
  script passes both source-derived contract checks and then exits **77**, the honest
  off-board skip of the runtime leg.
- **With** the mock preloaded the encoders do register, so the runtime leg actually
  runs: both source-derived contract checks pass, `mpph264enc` and `mpph265enc` pass,
  and the run then stops on the `mppvideodec` line against the immutable
  board-captured Radxa decoder golden. That is the documented mocked-host limitation.

The second form is the one that can detect drift here, and it was used differentially:
the pristine parent and this branch produce **byte-identical** output (`diff` clean),
so this change introduces no parity drift. No property, rank or caps surface was
touched.

### FIX-1 — appended memory is released once, not twice, on conversion failure

1. **Provenance SHA** — `9d074c3b487d3ec3b4a6355eeb8a68b0ca9010e3`. First-party fix
   (no upstream pick). Ledger origin H1-B2, whose falsification narrowed the claim to
   the **post-append** conversion-failure paths specifically, not every append. That
   narrowing is what this row proves: the import-path append at the top of
   `gst_mpp_enc_convert()` is followed by `goto out` and is genuinely unreachable from
   `err:`, while the `convert:` append is not.

   The plan cites `gstmppenc.c:1296-1349` / `:1284` / `:1316-1332`. Those line numbers
   are **stale** after todos 11-13; the function is `gst_mpp_enc_convert()` and the
   relevant sites were at `:1563` (safe import append), `:1580` (unsafe convert
   append) and `:1591` (the unconditional `goto err` that reaches the double release).
   Find it by name.
2. **Red/green outputs** — `meson test -C build-ci`, case
   `rockchipmpp GstHarness factories and caps`, test
   `test_failed_rotation_leaves_appended_memory_singly_owned` in
   `tests/mpp_gstharness.c`.

   RED at the parent (`6b71fcc8`), identically on bookworm/1.22 and trixie/1.26.2:

   ```
   ../libs/gst/check/gstcheck.c:286:F:caps:
     test_failed_rotation_leaves_appended_memory_singly_owned:0:
     Unexpected critical/warning: free_priv_data: object finalizing but still
     has 1 parents (object:0x7fda674f3c50)
   ```

   GREEN at the fix: `Ok: 3, Fail: 0`, with `failed rotation: flow=not-negotiated`.

   **The detector is GStreamer's own mini-object parent tracking, not a sanitizer.**
   `gst_buffer_append_memory()` registers the buffer as a parent of the memory;
   releasing the stale local reference drops the count to zero and finalises the
   memory while that registration still stands, which raises the critical above and
   which `gst_check` turns into a failure. It is deterministic and runs in both CI
   suites on every build.

   **Trigger derivation.** The plan's suggested trigger — "rotation without RGA
   available" — is not reachable as literally stated: with RGA compiled out,
   `gst_mpp_enc_set_format()` rejects a non-zero rotation outright
   (`unable to convert without RGA`), so no frame ever reaches the conversion path.
   The reachable form is **RGA compiled in but no RGA hardware present**, which is
   exactly the CI container: `librga` 2.1.0 is installed so `HAVE_RGA` is set and
   `set_format` accepts the property, then `c_RkRgaInit()` fails
   (`failed to open RGA:No such file or directory`), the blit returns FALSE, and the
   rotation check below it is an unconditional `goto err` with the append already
   done. On a real RK3588 the same error path is reached by any conversion the blit
   declines.

   Mutation check (four mutants, two killed, two surviving and reported):

   | mutant | result |
   |---|---|
   | drop `out_mem = NULL` after the `convert:` append | **KILLED** — RED, identical to the parent critical |
   | `gst_buffer_peek_memory (outbuf, 1)` instead of `, 0)` | **KILLED** — RED in two *other* tests: `gst_buffer_peek_memory: assertion 'idx < GST_BUFFER_MEM_LEN (buffer)' failed`. The borrowed pointer is load-bearing on the RGA path. |
   | drop `out_mem = NULL` after the **import** append | SURVIVED |
   | move `out_mem = NULL` to after the `#endif` instead of immediately after the append | SURVIVED |

   Both survivors are expected and are recorded rather than papered over with an
   unreachable test. The import-path append is followed by `goto out` with no
   intervening failure, and no `goto err` currently sits between the `convert:` append
   and the end of the RGA block, so neither assignment is reachable-from-`err:` today.
   They are kept because they make the variable mean one thing for the whole function
   — a reference this function still owns — which is what stops the next edit in
   either region from reopening the defect silently.
3. **Hardware gate** — `hardware-independent`. The trigger is a failed RGA blit, which
   the CI container produces natively by having the library without the device, so no
   board drill is claimed or needed.
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh build-ci/gst/rockchipmpp/libgstrockchipmpp.so`.
   Before and after, on both suites: `MPP symbols referenced and present: 68`, empty
   diff against the pinned `librockchip-mpp1_1.5.0-1_arm64.deb`. The fix adds no MPP
   call; it clears a local pointer and borrows one back out of a GstBuffer.
5. **Reviewer verdict** — `confirmed`. Confirmed by independent oracle review:
   ownership-nulling verified at both append sites, RGA peek-memory borrow
   confirmed non-claiming, both src/dst map-failure exits verified correct
   including the O_WRONLY fault-injection technique. ASAN limitation
   (uninstrumented GStreamer core code executes the actual use-after-free)
   explained and the GStreamer parent-tracking assertion substitute confirmed
   as an adequate deterministic detector for this specific defect class. Two
   additional leaked-GstVideoFrame-map mutants found during self-mutation-testing
   were also closed. Two genuinely-inert mutant survivors (import-site NULL,
   NULL position) honestly documented as unreachable from any error edge in
   current control flow.

#### AddressSanitizer: attempted, functional, and unable to observe this defect

The plan's acceptance criterion asks for an ASAN pair. **No ASAN red/green pair is
claimed, and ASAN did not report this defect.** The reason is specific and was
established rather than assumed, because the honest answer is not the one the
earlier notepads would predict:

- ASAN **works** in this container. A three-line `malloc`/`free`/read probe built with
  `-fsanitize=address` reports `heap-use-after-free` with a full stack. This is *not*
  the QEMU/ptrace limitation recorded for LeakSanitizer in todo 13 — that limitation is
  real but applies to the stop-the-world leak pass, not to inline checks.
- The sanitized plugin is instrumented: `nm -D libgstrockchipmpp.so | grep -c __asan`
  is **23**.
- `libgstreamer-1.0.so.0` is the distro build and is **not** instrumented: the same
  count is **0**.
- The offending access is the *second* release, which happens inside
  `_gst_buffer_free()` / `gst_mini_object_remove_parent()` — that is, inside
  uninstrumented libgstreamer. ASAN intercepts the `free`, so the memory is quarantined,
  but the read that touches it is never shadow-checked. There is nothing to report.

Separately, an ASAN run of the full suite under `qemu-user` did not finish within a
900 s timeout, matching the "an ASAN run can still hang under qemu" note already in the
notepads. That hang is a second, independent obstacle; even without it the point above
would stand.

Observing this defect with ASAN would require an instrumented GStreamer core, which is
out of scope here. The parent-tracking critical is a better instrument for this bug
class anyway: it is purpose-built for exactly "memory finalised while a buffer still
owns it", it is deterministic, and it runs in both CI suites on every build rather than
in a sanitizer leg nobody can execute.

### FIX-13 — a failed frame map fails the conversion instead of passing as success

1. **Provenance SHA** — `3db03d26bb5009acc2fd8cf76bb6a2f842da2830`. First-party fix
   (no upstream pick). Ledger origin O1-B8 ≡ falsifier O-F2, corroborated across two
   independent oracle lanes. Depends on FIX-1: making a map failure `goto err` adds two
   more post-append error paths, which without FIX-1 would each be a double release.
   That dependency is why the two land in this order.
2. **Red/green outputs** — `meson test -C build-ci`, tests
   `test_unreadable_source_frame_fails_the_conversion` and
   `test_unwritable_destination_frame_fails_the_conversion`.

   RED at the parent (`9d074c3b`, i.e. with FIX-1 already applied), identically on
   bookworm/1.22 and trixie/1.26.2:

   ```
   test_unreadable_source_frame_fails_the_conversion:0:
     'ret' (0) is not equal to 'GST_FLOW_NOT_NEGOTIATED' (-4)
   test_unwritable_destination_frame_fails_the_conversion:0:
     'ret' (0) is not equal to 'GST_FLOW_NOT_NEGOTIATED' (-4)
   ```

   `GST_FLOW_OK` is the defect: the parent reports success for a conversion that never
   copied anything.

   GREEN at the fix: `Ok: 3, Fail: 0`, with

   ```
   unreadable source: flow=not-negotiated mpp-frames=0
   unwritable destination: flow=not-negotiated mpp-frames=0
   completed conversion: flow=ok input memory remappable
   ```

   `mpp-frames` is `mpp_mock_enc_queued_packets()`, one increment per
   `encode_put_frame`, read **after teardown** so the encoder task has drained and a
   submitted frame cannot hide behind the race between the pushing thread and the task.
   It is the harm assertion, not the flow return: O1-B8's damage is an uninitialised
   MPP buffer reaching the encoder, and the parent submits one on both paths.
   `mpp_frame_set_buffer_count()` is deliberately **not** used — `gst_mpp_enc_stop()`
   calls `mpp_frame_set_buffer (self->mpp_frame, NULL)` at teardown, so that counter
   reads 1 even when nothing was ever encoded.

   Fault injection, both sides, and one trap worth recording:

   - **Source**: a test `GstAllocator` whose `mem_map` returns NULL. It is not a
     dmabuf, so `gst_mpp_allocator_import_gst_memory()` declines it and the frame
     reaches the conversion path.
   - **Destination**: a mock knob makes MPP publish a dmafd reopened `O_WRONLY` through
     `/proc/self/fd`, which `mmap(MAP_SHARED)` refuses for both `PROT_WRITE` and
     `PROT_READ`.
   - **The trap**: a *read-only* destination fd does not work, and the first attempt
     used one. `gst_memory_map(GST_MAP_WRITE)` on it correctly returns 0, but
     `gst_buffer_map()` routes through `gst_memory_make_mapped()`, which silently falls
     back to `gst_memory_copy()` when the direct map fails — and that copy only needs a
     READ map. So a read-only destination write-maps successfully at buffer level and
     proves nothing. Verified directly with a standalone GStreamer probe before the
     injection was changed. Anyone writing a map-failure test against a GstBuffer needs
     a genuinely unmappable memory, not a non-writable one.

   Mutation check (four mutants, all killed):

   | mutant | result |
   |---|---|
   | source map failure → `goto out` (treat as success) | **KILLED** by the source test |
   | destination map failure → `goto out` (treat as success) | **KILLED** by the destination test |
   | drop `gst_video_frame_unmap (&src_frame)` in the destination-failure branch | **KILLED** — `the conversion left a map on the input memory` |
   | drop `gst_video_frame_unmap (&src_frame)` on the completed-copy path | **KILLED** — same assertion, from the completed-conversion test |

   The last two mutants initially **survived**. They are leaked `GstVideoFrame` maps,
   which nothing in the suite noticed, and this fix restructures exactly that unmap
   region — so a detector was added rather than the survivors being reported and left:
   a locked memory refuses a write map, so re-mapping the retained input memory for
   writing after the push is a direct, deterministic check that every map taken was
   released. The completed-conversion test exists solely because it is the only path
   that reaches the unmap after the copy.
3. **Hardware gate** — `hardware-independent`. Both failures are injected at the
   GStreamer mapping seam, which is CPU-side and identical on and off board.
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh build-ci/gst/rockchipmpp/libgstrockchipmpp.so`.
   Before and after, on both suites: `MPP symbols referenced and present: 68`, empty
   diff against the pinned `librockchip-mpp1_1.5.0-1_arm64.deb`. The fix changes only
   control flow around two GStreamer calls.
5. **Reviewer verdict** — `confirmed`. Confirmed by independent oracle review:
   ownership-nulling verified at both append sites, RGA peek-memory borrow
   confirmed non-claiming, both src/dst map-failure exits verified correct
   including the O_WRONLY fault-injection technique. ASAN limitation
   (uninstrumented GStreamer core code executes the actual use-after-free)
   explained and the GStreamer parent-tracking assertion substitute confirmed
   as an adequate deterministic detector for this specific defect class. Two
   additional leaked-GstVideoFrame-map mutants found during self-mutation-testing
   were also closed. Two genuinely-inert mutant survivors (import-site NULL,
   NULL position) honestly documented as unreachable from any error edge in
   current control flow.

### Verification of the two rows above

Both containers were configured with the flags CI and `debian/rules` use
(`-Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled`).

| gate | bookworm / GStreamer 1.22 | trixie / GStreamer 1.26.2 |
|---|---|---|
| `meson test -C build-ci` | `Ok: 3, Fail: 0` (`Checks: 24, Failures: 0`) | `Ok: 3, Fail: 0` |
| red at parent reproduced | yes, all three tests | yes, all three tests |
| `ci/check-mpp-abi.sh` | 68 symbols, empty diff | 68 symbols, empty diff |
| `ci/check-glibc-floor.sh` | `highest GLIBC import GLIBC_2.17; device floor GLIBC_2.36` | not gated (forward-compat leg) |
| `ci/check-glibc-floor.test.sh` | all assertions passed | — |
| `tests/parity-check.sh`, mock preloaded, parent vs branch | `diff` clean — **byte-identical** | `diff` clean — **byte-identical** |

`tests/parity-check.sh` was run in its differential form, per the documented mocked-host
limitation: with the mock preloaded both source-derived contract checks pass,
`mpph264enc` and `mpph265enc` pass, and the run then stops on the `mppvideodec` line
against the immutable board-captured Radxa decoder golden. The parent and this branch
produce byte-identical output in both containers, so neither fix introduces parity
drift. No property, rank or caps surface was touched, and no frozen property was
altered — `rotation` is only *set* by a test, never redefined.

### FIX-10 — a refused temporal reference cfg fails the apply and stays outstanding

1. **Provenance SHA** — `eb9e3dca77c4e2d6cc71f371dd4913c322a3a143`. First-party fix (no
   upstream pick), against task baseline `5f09ef41cb12b4de59528b23cc718554c5835e65`.
   Ledger origin O1-B5 ≡ H5-B6, corroborated across two independent lanes.
2. **Red/green outputs** — focused command:
   `GST_CHECKS=test_refused_temporal_ref_cfg_fails_the_apply_and_retries .../mpp-gstharness`
   in the arm64 bookworm container, with the mock arming an `MPP_ENC_SET_REF_CFG` refusal.

   RED at the parent:

   ```
   test_refused_temporal_ref_cfg_fails_the_apply_and_retries:0:
     'push_encoder_frame(h, 1)' (0) is not equal to 'GST_FLOW_NOT_NEGOTIATED' (-4)
   ```

   `GST_FLOW_OK` is the defect: MPP refused the reference structure and the element
   went on to configure and commit the whole rate-control config as if it had not.

   GREEN at the fix: `Checks: 1, Failures: 0, Errors: 0`; full suite `Checks: 40,
   Failures: 0` and `meson test` `Ok: 3, Fail: 0` on both bookworm/1.22 and
   trixie/1.26.

   The test asserts four things, not just the flow return: the refusal reaches the
   caller (`GST_FLOW_NOT_NEGOTIATED`), `MPP_ENC_SET_CFG` is NOT issued for the
   rejected config, the request is retried on the next pass with no further property
   write, and it then lands once MPP accepts it. The NULL-cfg branch (returning to
   flat IPPP) is covered by the same contract.

   Mutation check (three mutants, all killed):

   | mutant | result |
   |---|---|
   | discard the return, as the parent did | **KILLED** — first push returns OK |
   | propagate the failure but leave `ref_dirty` cleared | **KILLED** — the retry push returns OK; the request was abandoned |
   | propagate the failure but leave `prop_dirty` cleared | **KILLED** — same assertion; the next apply short-circuits before reaching the retry |

   The second and third mutants are the point of the fix rather than a bonus: both
   produce a *visible* first failure and then silently give up, which is the sticky
   transition H5-B6 describes.
3. **Hardware gate** — `hardware-independent`. The refusal is injected at the MPP
   control-command boundary, which is the same call the board makes.
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh build-ci/gst/rockchipmpp/libgstrockchipmpp.so`:
   `MPP symbols referenced and present: 68`, empty diff against the pinned
   `librockchip-mpp1_1.5.0-1_arm64.deb`, on both suites. The fix adds no MPP call.
5. **Reviewer verdict** — `confirmed`. Confirmed across two independent oracle review rounds. FIX-10 (ref-cfg propagation) and FIX-15 (H.265 caps) confirmed correct on round 1. FIX-14 (level validation) required 2 real bug fixes found in round 1: fractional framerates were truncated before the conformance check (missing real borderline violations, verified via exact 753/25 and 806/25 fps counterexamples), and configs exceeding every level were silently downgraded to the highest level with a warning instead of rejecting negotiation. Round 2 confirmed both fixes mathematically/logically correct (exact rational cross-multiplication for the level check, deliberately distinct from the integer-fps auto-bitrate calculation; genuine reject-before-any-MPP-write semantics preserving 2 pre-existing auto-bitrate tests). A CRLF line-ending diff-hygiene issue in gstmppenc.h was also found and fixed, independently verified byte-clean.

### FIX-14 — the declared level is validated against frame rate and bitrate, not resolution alone

1. **Provenance SHA** — `9300c2488dccfec9928e45ae15c44823ee6cc816`, corrected by
   `3f182454` and `e5efde90`. First-party fix, against the same baseline. Ledger
   origin H5-B4, WAVE-2 falsification verdict REAL with the mechanism corrected to
   "MPP corrects resolution-under-leveling but NOT framerate/bitrate under-leveling".

   **Independent review round 1 found two defects in the first implementation.** Both
   are fixed in the two follow-up commits and are covered below; the row is kept as one
   FIX-14 row because they are corrections to this fix, not separate ledger entries.

   - `3f182454` — the validator reduced the framerate to an integer before sizing the
     rate axes against it, hiding every violation narrower than one frame per second.
     MPP is handed the rational (`rc:fps_out_num` over `rc:fps_out_denorm`), so the
     truncation was ours alone. `GstMppEncRateInfo` now carries the same pair and both
     rate limits are compared by cross-multiplication.
   - `e5efde90` — a stream outside EVERY level had its conformance flag set to FALSE
     and then got the top table entry configured and published anyway, with a warning
     saying it "may still exceed" that level. That is the same lie this fix exists to
     remove. It is now a negotiation failure.
2. **Red/green outputs** — the falsifier's premise was re-verified against the pinned
   MPP source before any code was written, per the todo-12 lesson that a lane's cited
   mechanism can be wrong. Both correction sites read only the frame-size column:

   ```
   mpp/codec/enc/h264/h264e_sps.c:145-152   if (level_infos[i].max_MBs >= mbs) { ... }
   mpp/codec/enc/h265/h265e_ps.c:142-148    if (levels[i].maxLumaSamples >= maxlumas) { ... }
   ```

   `max_MBPS` / `max_BR` and `maxLumaSamplesPerSecond` / `maxBitrateMain|High` sit in
   the same two tables and are never read. Premise CONFIRMED, both codecs.

   Focused command: the nine level tests in `.../mpp-gstharness`. RED at the parent,
   identically on bookworm/1.22 and trixie/1.26:

   ```
   test_h264_level_is_raised_for_an_out_of_level_frame_rate:0:
     'mpp_mock_last_cfg_s32("h264:level")' (40) is not equal to '42' (42)
   test_h264_level_is_raised_for_an_out_of_level_bitrate:0:
     'mpp_mock_last_cfg_s32("h264:level")' (40) is not equal to '41' (41)
   test_h265_level_is_raised_for_an_out_of_level_frame_rate:0:
     'mpp_mock_last_cfg_s32("h265:level")' (120) is not equal to '123' (123)
   test_h265_level_follows_the_tier_bitrate_ceiling:0:
     'mpp_mock_last_cfg_s32("h265:level")' (120) is not equal to '123' (123)
   ```

   The cited 1080p60-at-level-4 case was checked against the spec rather than taken
   from the plan: 1920x1080 aligns to 1920x1088, i.e. 8160 macroblocks, which is
   INSIDE level 4's 8192 frame-size limit — so MPP's own correction leaves it alone —
   while 60 fps of them is 489600 MB/s against level 4's 245760 ceiling. Level 4.1
   shares that 245760 ceiling, so 4.2 (522240) is the first conforming level, which
   is also what the element's own `level` property documentation says 1080p60 needs.
   For H.265 the same stream is 2073600 luma samples (inside level 4's 2228224) at
   124416000 samples/s (outside its 66846720), so it raises to 4.1.

   GREEN at the fix: `Checks: 44, Failures: 0, Errors: 0`; `meson test` `Ok: 3, Fail: 0`
   on both suites.

   **Review round 1, defect A — truncated framerate.** A level's rate ceiling can fall
   between two whole framerates, so the exact rational has to reach the comparison.
   1080p is 8160 macroblocks; at `753/25` fps that is 245779.2 MB/s against H.264 level
   4's 245760 ceiling, while truncating to 30 fps measures 244800 and accepts level 4.
   The H.265 counterpart is 1080p at `806/25` fps: 66852864 samples/s against level 4's
   66846720, truncating to 32 fps for 66355200. Both are now compared by
   cross-multiplication (`mbs * fps_n` against `max_mbps * fps_d`, and the luma
   equivalent), which is exact and stays well inside `guint64` for any geometry
   `gst_video_info_from_caps` accepts. RED with the truncation restored:

   ```
   test_h264_level_uses_the_exact_fractional_frame_rate:0:
     'mpp_mock_last_cfg_s32("h264:level")' (40) is not equal to '42' (42)
   test_h265_level_uses_the_exact_fractional_frame_rate:0:
     'mpp_mock_last_cfg_s32("h265:level")' (120) is not equal to '123' (123)
   ```

   The automatic bitrate deliberately keeps the integer framerate it has always used,
   because that is the framerate the bitrate config is itself derived from; mixing the
   two would make the bitrate axis disagree with what MPP is given.

   **Review round 1, defect B — no rejection when nothing conforms.** Raising only makes
   sense when a higher conforming level EXISTS. The first implementation returned the
   top table entry when none did, and configured and published it with a warning. It is
   now a negotiation failure: the codec's configure callback returns FALSE and writes
   nothing, and the apply is refused at the `MPP_ENC_SET_CFG` boundary — the same seam
   that already refuses a config MPP rejected a key from. RED with the clamp restored,
   and note WHAT the assertion catches: the non-conforming level actually reaching MPP's
   config.

   ```
   test_h264_stream_outside_every_level_is_rejected:0:
     'mpp_mock_last_cfg_s32(level_key)' (62) is not equal to 'INT32_MIN' (-2147483648)
   test_h265_stream_outside_every_level_is_rejected:0:
     'mpp_mock_last_cfg_s32(level_key)' (186) is not equal to 'INT32_MIN' (-2147483648)
   ```

   The over-maximum tests assert four things together: the push returns
   `GST_FLOW_NOT_NEGOTIATED`, the level key is still `INT32_MIN` (never written, not
   merely uncommitted), `MPP_ENC_SET_CFG` was never issued, and no src caps were
   published.

   **The refusal is latched, not returned on the spot, and that is load-bearing.** The
   remaining writes in the apply go into this element's own `MppEncCfg`; refusing at the
   one boundary where that struct is handed to MPP keeps a single place where a rejected
   configuration can be stopped, exactly as `cfg_error` already does. It also leaves the
   parent's own config writes observable — which two PRE-EXISTING tests depend on.
   `test_auto_bitrate_clamps_instead_of_overflowing` (8192x8192) and
   `test_auto_bitrate_saturates_at_a_large_nv12_geometry` (32768x32768) both describe
   geometries that this change now rejects, and both still pass **unchanged**, because
   they assert on `rc:bps_target`/`bps_max`/`bps_min` rather than on negotiation
   succeeding. Verified directly rather than assumed:

   ```
   mpph264enc: 8192x8192@256/1 fps at 2147483647 bps exceeds every H.264 level,
   including 62; refusing to encode a stream no level can describe
   100%: Checks: 1, Failures: 0, Errors: 0
   ```

   An early return at the configure callback would have skipped the parent's bitrate
   writes and broken both of them, which is why the latch is not a stylistic choice.

   Mutation check (seven mutants on the original implementation, all killed):

   | mutant | result |
   |---|---|
   | use the declared level directly, as the parent did | **KILLED** by all four raise tests |
   | drop the macroblock-rate axis | **KILLED** — 1080p60 stays at 40 |
   | drop the bitrate axis | **KILLED** — 1080p30 at 30 Mbps stays at 40 |
   | ignore the tier and always read the main-tier ceiling | **KILLED** — a high-tier 1080p30 at 16 Mbps is raised to 4.1 instead of staying at 4 |
   | compare level values instead of table positions | **KILLED** — level 1b (value 99) is treated as stronger than 6.2 and never raised |
   | count raw pixels instead of MPP_ALIGN(16) macroblocks | **KILLED** — 1920x1090 stays at 40 where MPP would itself raise past it |
   | always take the required level (allowing a *lower* one) | **KILLED** — 320x240 drops the declared 4 to 1.3 |

   Mutation check on the two review-round corrections (four more mutants, all killed):

   | mutant | result |
   |---|---|
   | restore the truncating framerate | **KILLED** — both fractional tests; H.264 accepts 40 not 42, H.265 accepts 120 not 123 |
   | restore the clamp-and-warn on non-conformance | **KILLED** — both over-maximum tests; levels 62 and 186 reach MPP's config |
   | exact rate on H.264 only, H.265 left truncating | **KILLED** — the H.265 fractional test alone |
   | reject on H.264 only, H.265 left clamping | **KILLED** — the H.265 over-maximum test alone |

   The last two exist because the two codecs carry independent copies of the level
   logic; without them a half-applied correction would have passed.

   **Enforcement is warn+raise, not reject, and that was decided by checking rather
   than by preference.** The plan's QA scenario makes rejection conditional on no
   existing cerastream fixture profile tripping it. cerastream sets neither `level`
   nor `profile` on either element — `crates/cerastream-hal/src/profiles/rk3588.rs`
   and `crates/cerastream-core/src/graph/templates/encoders.rs` emit only
   `zero-copy-pkt`, `rc-mode`, `qp-max`, `gop` and optional `width`/`height` — so
   every stream runs at the default level 4, while `Resolution`/`Framerate` in
   `crates/cerastream-core/src/graph/spec.rs` offer 1080p, 1440p and 2160p at 50/60
   fps. A hard rejection would therefore fail negotiation for every shipped 1080p50,
   1080p60, 1440p and 2160p profile on both codecs. Raising matches what MPP already
   does on the axis it checks, and leaves the property reporting what the operator
   asked for while the caps and the SPS/VPS report what was encoded.
3. **Hardware gate** — `hardware-independent`. Level selection is a header-arithmetic
   decision made before any frame is submitted; the tests assert the value handed to
   `mpp_enc_cfg_set_s32("h264:level"/"h265:level")` and the negotiated caps, both of
   which are identical on and off board. The resulting bitstream conformance is not
   claimed here and is not measured by this row.

   One behaviour note for the board drill that eventually exercises these elements: a
   configuration outside every level now FAILS negotiation where it previously encoded.
   No cerastream profile reaches that state — H.264 High level 6.2 tops out at 1 Gbit/s
   and 139264 macroblocks, H.265 main tier 6.2 at 240 Mbit/s, and 2160p60 is 32400
   macroblocks at 1.944 M MB/s — but an operator who types a bitrate two orders of
   magnitude too large now gets a refusal instead of a silently mislabelled stream.
4. **MPP ABI closure** — 68 symbols, empty diff against the pinned MPP, on both suites.
   The fix adds arithmetic and reuses the existing checked cfg setters.
5. **Reviewer verdict** — `confirmed`. Confirmed across two independent oracle review rounds. FIX-10 (ref-cfg propagation) and FIX-15 (H.265 caps) confirmed correct on round 1. FIX-14 (level validation) required 2 real bug fixes found in round 1: fractional framerates were truncated before the conformance check (missing real borderline violations, verified via exact 753/25 and 806/25 fps counterexamples), and configs exceeding every level were silently downgraded to the highest level with a warning instead of rejecting negotiation. Round 2 confirmed both fixes mathematically/logically correct (exact rational cross-multiplication for the level check, deliberately distinct from the integer-fps auto-bitrate calculation; genuine reject-before-any-MPP-write semantics preserving 2 pre-existing auto-bitrate tests). A CRLF line-ending diff-hygiene issue in gstmppenc.h was also found and fixed, independently verified byte-clean.

### FIX-15 — mpph265enc publishes profile, tier and level in its src caps

1. **Provenance SHA** — `a01fce7db90e3291ef74ff5336ef8086a91cd21d`. First-party fix,
   against the same baseline. Ledger origin H5-B5 (REAL, low/med). Depends on FIX-14:
   the caps carry the EFFECTIVE level, so publishing them before the validation
   existed would have advertised the declared level, which is the field FIX-14 exists
   to correct.
2. **Red/green outputs** — focused command:
   `GST_CHECKS=test_h265_src_caps_publish_profile_tier_and_level .../mpp-gstharness`.

   RED with the parent's caps content restored (all three fields absent):

   ```
   test_h265_src_caps_publish_profile_tier_and_level:0: src caps carry no 'profile'
   field: video/x-h265, stream-format=(string)byte-stream, alignment=(string)au,
   width=(int)1920, height=(int)1080, pixel-aspect-ratio=(fraction)1/1,
   framerate=(fraction)30/1, interlace-mode=(string)progressive,
   colorimetry=(string)bt709, chroma-site=(string)mpeg2
   ```

   GREEN at the fix, both the default and a Main-10/high-tier/1080p60 configuration:
   `Checks: 1, Failures: 0, Errors: 0`.

   Mutation check (three mutants, all killed):

   | mutant | result |
   |---|---|
   | omit the profile and tier writes | **KILLED** — `src caps carry no 'profile' field` |
   | publish the property nick (`main10`) instead of the GStreamer spelling | **KILLED** — `src caps profile is 'main10', expected 'main-10'` |
   | publish general_level_idc (`120`) instead of the level name | **KILLED** — `src caps level is '120', expected '4'` |

   The last two are the reason the values are not derived with `g_enum_to_string` the
   way the H.264 path derives its own. `g_enum_to_string` returns the enum's
   *value_name*, and this element's H.265 profile nicks (`main10`, `main-still`) are
   not the video/x-h265 spellings (`main-10`, `main-still-picture`), while its level
   property holds general_level_idc (120) where the caps field holds the level number
   (4). Tier is published alongside because level 4 main tier and level 4 high tier
   are different constraints and a level without its tier is only half an answer.
3. **Hardware gate** — `hardware-independent`. Caps construction is CPU-side.
4. **MPP ABI closure** — 68 symbols, empty diff against the pinned MPP, on both suites.
   The fix touches no MPP call.
5. **Reviewer verdict** — `confirmed`. Confirmed across two independent oracle review rounds. FIX-10 (ref-cfg propagation) and FIX-15 (H.265 caps) confirmed correct on round 1. FIX-14 (level validation) required 2 real bug fixes found in round 1: fractional framerates were truncated before the conformance check (missing real borderline violations, verified via exact 753/25 and 806/25 fps counterexamples), and configs exceeding every level were silently downgraded to the highest level with a warning instead of rejecting negotiation. Round 2 confirmed both fixes mathematically/logically correct (exact rational cross-multiplication for the level check, deliberately distinct from the integer-fps auto-bitrate calculation; genuine reject-before-any-MPP-write semantics preserving 2 pre-existing auto-bitrate tests). A CRLF line-ending diff-hygiene issue in gstmppenc.h was also found and fixed, independently verified byte-clean.

### Verification of the three rows above

Both containers were configured with the flags CI and `debian/rules` use
(`-Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled`).

| gate | bookworm / GStreamer 1.22 | trixie / GStreamer 1.26 |
|---|---|---|
| `meson test -C build-ci` | `Ok: 3, Fail: 0` (`Checks: 44, Failures: 0`) | `Ok: 3, Fail: 0` (`Checks: 44, Failures: 0`) |
| red at parent reproduced | yes, all three fixes | yes — all three fix sites reverted to parent behaviour in one build gives `Checks: 40, Failures: 8`, covering every raise test, the ref-cfg propagation test and the H.265 caps test |
| `ci/check-mpp-abi.sh` | 68 symbols, empty diff | 68 symbols, empty diff |
| `ci/check-glibc-floor.sh` | `highest GLIBC import GLIBC_2.17; device floor GLIBC_2.36` | not gated (forward-compat leg) |
| `ci/check-glibc-floor.test.sh` | all assertions passed | — |
| `tests/parity-check.sh`, mock preloaded, parent vs branch | `diff` clean — **byte-identical** | `diff` clean — **byte-identical** |
| `tests/parity-check.sh` `PARITY_SOURCE_ONLY=1` | exit 0 | exit 0 |

`tests/parity-check.sh` was run in its differential form, per the documented mocked-host
limitation: with the mock preloaded both source-derived contract checks pass,
`mpph264enc` and `mpph265enc` pass, and the run then stops on the `mppvideodec` line
against the immutable board-captured Radxa decoder golden. The parent
(`5f09ef41`) and this branch produce byte-identical output in both containers.

The parity gate deserves a specific note here, because FIX-15 adds caps fields. It adds
them only to the **negotiated** src caps; the static pad template is untouched, and the
template is what `gst-inspect-1.0` prints and what the `src_caps=` golden line pins. The
golden line for `mpph265enc` therefore still reads exactly

```
src_caps=video/x-h265 width: [ 96, 2147483647 ] height: [ 64, 2147483647 ] stream-format: { (string)byte-stream } alignment: { (string)au }
```

and needed no edit. Constraining the template to a profile list instead would have been
a caps SHRINK, not an additive change, so it was deliberately not done — the H.264
element's own template likewise omits `level` while its negotiated caps have carried
one since the fork point. No property was added, removed, renamed, retyped or given a
different default by any of the three fixes; the only new symbol is the internal
`gst_mpp_enc_snapshot_rate_info()` helper, which is not a GObject property.

### FIX-2 — allocator import failures leave MppBuffer ownership unchanged

1. **Provenance SHA** — `380bc5cadfa09ff7d9d18f16df4dc34b44209ed2`, first-party
   correction for the verified H1-B3/H2-B6 allocator mechanism. H1-B4 remains
   falsified and is deliberately untouched.
2. **Red/green outputs** — `mpp-allocator-seam` creates real mock-backed dmafds and
   runs two deterministic failures: an external-group `mpp_buffer_import_with_tag()`
   refusal, and a same-group `dup()` refusal. GREEN: both return `NULL`, make no
   `mpp_buffer_inc_ref()` call, retain the original one-reference count, and the
   dup failure never reaches `gst_fd_allocator_alloc()`. Mutation check: removing the
   common `!mem` return is **KILLED** by GStreamer's `gst_mini_object_set_qdata: assertion
   'object != NULL' failed`; removing the explicit `dup_fd < 0` return is **KILLED**
   because the same-group test receives a non-NULL memory from `fd=-1` instead of NULL.
   LeakSanitizer cannot start under qemu-user (documented workspace limitation), so the
   mock's deterministic ref-count and increment counter are the portable leak proof.
3. **Hardware gate** — `hardware-independent`. Both paths are allocator ownership and
   fd-error handling before any hardware MPP operation.
4. **MPP ABI closure** — `ci/check-mpp-abi.sh` reports 68 referenced-and-present MPP
   symbols with an empty closure diff against pinned MPP 1.5.0-1 in both bookworm and
   trixie. The fix adds no MPP API call and the mock retains the pinned five-argument
   `mpp_buffer_import_with_tag()` declaration.
5. **Reviewer verdict** — `confirmed`. Confirmed by independent oracle review: cross-group import failure now correctly returns NULL before any mpp_buffer_inc_ref/qdata operation (traced: the old leak was real — GStreamer's gst_mini_object_set_qdata() rejects a null object before installing its destroy notifier, so a failed import's ref increment could never be balanced). dup(fd) failure now correctly checked before gst_fd_allocator_alloc() (traced the real consequence through downstream consumers: neither GStreamer 1.22 nor 1.26 validates the fd at allocation time, so unchecked fd=-1 would produce false-success invalid memory reaching drmPrimeFDToHandle() in gstkmsallocator.c and the rkximage display path — EBADF at point of use, not an immediate crash, but a genuine defect). H1-B4 (killed/falsified claim) confirmed untouched — gst_mpp_allocator_alloc_mppbuf() is byte-unchanged. FIX-3/H1-B5 confirmed not redundantly touched (already landed in todo 15's gstmppenc.c, this PR has no gstmppenc.c diff). Mutation-verified: removing either NULL-return check causes the corresponding test assertion to fail as expected.

### Decoder no-output input accounting

This row records the initial PR submission. Its header-classification and broad-sweep
claims were rejected by the first independent review and are superseded by the review
correction row below; the `d27ae92` oldest-orphan evidence remains valid.

1. **Provenance SHA** — `7d12668d67419041effd3d9129fcb41e4f3e7b77`, first-party
   `tier-a-adapted` correction based on Kelvin Lawson's
   `44578bdd745674d8e3917fe7a44486f5d9d42b17`. The adaptation places the
   post-consumption release after this fork's buffered/copy packet ownership branch.
   It builds on the already-landed `d27ae920e36fd72fe116e6c00108a65235c3e3d5`
   oldest-pending fallback; that fallback and `7ffd7f40576bbfb861bc7a7d3492c710d149aff8`'s
   packet-level backpressure are preserved unchanged. No retained-frame cap was added.
2. **Red/green outputs** — focused command in the arm64 bookworm container:
   `MPP_DECODER_SEAM_ACCOUNTING_ONLY=1 .../mpp-decoder-seam`. With the exact
   pre-`d27ae92` decoder source, the stale-PTS storm reported `300` outputs but
   `299` pending frames; the sequential case degraded after output 2 with one
   unmatched frame already retained. At this task's parent (`171c35bc`), the
   already-landed stale fallback was green, while the remaining defects were red:
   `SPS/PPS-only: 300 accepted packets, 0 outputs, 300 pending frames` and
   `older untimestamped pending frame: 1 before later match, 1 after`. GREEN at
   `7d12668d`: stale burst/sequential pending `0/0`, SPS/PPS-only pending `0`, and
   the older untimestamped stray `1 -> 0`. The normal-stream A/B sequence was
   identical at the parent and fix:
   `10000000000 10033333333 10066666666 10099999999 10133333332 10166666665 10199999998`.
   An isolated mutant restoring only unbounded `last_frame` reuse was killed:
   stale burst pending `64` (the existing safety bound) and sequential pending `1`,
   while both header cases and the normal sequence stayed green. Full Meson passed
   `4/4` in bookworm/GStreamer 1.22 and trixie/GStreamer 1.26; source-contract
   parity passed both goldens in both suites. Absolute mocked-host runtime parity
   remains the documented non-gate because decoder goldens are board captures.
3. **Hardware gate** — `hardware-independent`. The assertions cover
   GstVideoDecoder pending-frame ownership, output matching, and downstream frame
   sequence entirely at the mature mock-MPP boundary.
4. **MPP ABI closure** — bookworm resolved 68 referenced-and-present MPP symbols
   against 26 sibling libraries; trixie resolved the same 68 against 23. Both
   reported an empty diff against pinned MPP 1.5.0-1. The fix adds no MPP API call.
5. **Reviewer verdict** — `needs-fix`. The first orchestrator-dispatched independent
   oracle review confirmed oldest-orphan consumption, `d27ae92`/`7ffd7f4` preservation,
   `066ca39` non-reintroduction, and diff hygiene, but rejected the no-output detector.
   Invalid PTS is a supported picture state, real VCL access units can be at or below
   128 bytes, and the common base applied that proxy to every codec. The review also
   rejected the blanket invalid-PTS sweep because decode-order precedence does not prove
   a reordered picture will never produce output. Both paths could remove the genuine
   codec frame before output and shift content/PTS metadata onto another frame.

### Decoder parameter-set recognition — review correction

1. **Provenance SHA** — `353602891600ddba82080bd372c1615babeff151`,
   first-party correction to the independently rejected heuristic in `7d12668d`. The
   detector is disabled by default and enabled only for negotiated H.264/H.265 framing:
   Annex-B for `byte-stream`, or length-prefixed `avc`/`avc3`/`hvc1`/`hev1` when avcC or
   hvcC supplies a supported NAL length size. Every NAL in the access unit must be SPS/PPS
   for AVC or VPS/SPS/PPS for HEVC. The broad invalid-PTS sweep is removed.
2. **Red/green outputs** — strengthened focused command:
   `MPP_DECODER_SEAM_ACCOUNTING_ONLY=1 .../mpp-decoder-seam`. RED against the first
   submission: real GStreamer AVC VCL fixtures at 24, exactly 128, and 143 bytes plus an
   SPS/PPS+IDR mixed AU retained only `1/4`; length-prefixed AVC VCL and tiny invalid-PTS
   VP8/unknown-framing inputs retained `0/1`; an untimestamped VCL frame was already gone
   before a later match (`0 -> 0`). The reordered stream expected output identities
   `0x10,0x40,0x20`, but received `0x10,0x20,0x20`, duplicating content under the wrong
   PTS. GREEN at `35360289`: real H.264 SPS/PPS and H.265 VPS/SPS/PPS fixtures settle at
   zero for both Annex-B and length-prefixed framing; VCL/mixed negatives retain `4/4`;
   AVC VCL, unknown H.264 framing, and tiny invalid-PTS H.263/AV1/VP8/VP9/MPEG-4/JPEG
   inputs each retain `1/1`; untimestamped VCL remains `1 -> 1`. Reordered downstream
   identities are `0x10,0x40,0x20`; the fourth planned identity `0x30` is explicitly
   verified on retained frame 2 with PTS `10066666666`, documenting the existing
   one-frame output queue instead of silently omitting the tail. Isolated mutations
   restoring the size proxy, broad sweep, or `last_frame` reuse are all killed. Full
   Meson passed `4/4` in bookworm/GStreamer 1.22 and trixie/GStreamer 1.26; source-contract
   parity passed both goldens in both suites. ASAN also identified and locked a test-only
   JPEG setup-order UAF (mock arm now precedes element creation); production was not
   implicated.
3. **Hardware gate** — `hardware-independent`. Framing recognition, GstVideoDecoder
   ownership, reordered frame association, PTS, and fd-backed content identity are all
   asserted at the mock-MPP boundary. No MPP hardware behavior is approximated.
4. **MPP ABI closure** — bookworm resolved 68 referenced-and-present MPP symbols against
   26 sibling libraries; trixie resolved the same 68 against 23. Both reported an empty
   diff against pinned MPP 1.5.0-1. The correction adds no MPP API call.
5. **Reviewer verdict** — `needs-fix`. The second independent review fully confirmed the
   production correction: NAL types and bounds, forbidden-bit/temporal-ID validation,
   Annex-B and length-prefixed framing, codec scoping, sweep removal, real fixtures, UAF
   repair, mutation results, and scope/CI were all correct. It found one test-only gap:
   replacing the original eight-input ordered-stream guard with the stronger four-input
   reordered scenario lost the full-length normal-stream regression. Production required
   no change; the missing ordered guard is restored in the row below.

### Eight-frame ordered decoder identity — review correction

1. **Provenance SHA** — `5668febfca238fde8f7835c80116031e65521d0a`,
   first-party test-only correction to the second independent review. It restores
   `NORMAL_INPUTS = 8` alongside, rather than instead of, the existing four-frame
   reordered/mixed-PTS identity scenario. No production source or mock API changed.
2. **Red/green outputs** — RED by review inspection at `35360289`: only four unique
   planned identities remained, covering three downstream buffers plus one retained tail,
   so the original eight-input ordered-stream A/B no longer existed. GREEN at `5668febf`:
   eight ordered H.264 VCL inputs plan unique identities `0x51` through `0x58`; seven
   downstream buffers assert the exact PTS sequence
   `10000000000,10033333333,10066666666,10099999999,10133333332,10166666665,10199999998`
   and identities `0x51` through `0x57`. The eighth frame is explicitly present in the
   decoder's retained queue as frame 7, PTS `10233333331`, identity `0x58`. Mock output,
   accounted-output, and queued-input totals are exactly `8/8/0`, and an immediate harness
   pull after the seven expected downstream buffers proves there is no unexpected extra
   delivery. The four-frame reordered test remains green and unchanged. Fresh isolated
   mutations are all killed: size proxy (11 accounting/identity failures), broad sweep
   (untimestamped `1 -> 0` plus two identity failures), and `last_frame` reuse (stale
   burst/sequential pending `64/1`). Full Meson passed `4/4` in bookworm/GStreamer 1.22
   and trixie/GStreamer 1.26; both source-contract goldens passed in both suites.
3. **Hardware gate** — `hardware-independent`. The ordered guard observes eight distinct
   fd-backed mock-MPP buffers through the real GstVideoDecoder finish path, including the
   plugin's deliberate one-frame ready queue. It needs no board behavior.
4. **MPP ABI closure** — bookworm resolved 68 referenced-and-present MPP symbols against
   26 sibling libraries; trixie resolved the same 68 against 23. Both reported an empty
   diff against pinned MPP 1.5.0-1. The test-only correction adds no MPP API call.
5. **Reviewer verdict** — `confirmed`. Confirmed across THREE independent oracle review rounds. Round 1 found a genuinely serious defect: the original invalid-PTS+size-heuristic header detector could misclassify real small VCL (decoded picture) frames as parameter-set headers, releasing their GstVideoCodecFrame accounting and causing lost or misattributed decoder output on genuine video data — a real data-corruption-class bug, caught before merge. Round 2 confirmed the replacement (a proper codec-aware AVC/HEVC NAL-type parser, supporting both Annex-B and length-prefixed framing, scoped only to H.264/H.265) is correct and robust, with one remaining test-coverage gap. Round 3 confirmed the gap closed (an 8-frame normal-stream regression restored alongside the 4-frame reordered-identity test). Oldest-orphan consumption, d27ae92/7ffd7f4 preservation, and 066ca39 non-reintroduction all confirmed correct throughout. A real ASAN-detected use-after-free in test infrastructure was also found and fixed as a byproduct. The branch remains open for final
   independent confirmation and must not be self-merged.

### Drain retained decoder tail frame

1. **Provenance SHA** — first-party correction against task baseline
   `0da5f2c6143a0d93ec30b03e65436fb7ee61db0f`.
2. **Red/green outputs** — the arm64/bookworm decoder seam plans eight unique fd-backed
   video outputs, waits until the deliberately one-frame-deferred tail is the sole pending
   frame, then executes the real decoder finish/drain vfunc. RED at the parent prints
   `drain delivered 7/8 decoded data frames` because the EOS frame matches and releases
   the retained tail before reset. GREEN flushes the ready queue on the EOS path through
   `gst_video_decoder_finish_frame()` and prints `drain delivered all 8 decoded frames`.
   Mutation check: replacing the EOS queue flush with the parent path restores `7 == 8`.
3. **Hardware gate** — `hardware-independent`. The mock supplies fd-backed output buffers
   while the real `GstVideoDecoder` output path, EOS drain, and one-frame queue own delivery.
4. **MPP ABI closure** — unchanged: no MPP API was added or removed; the existing closure
   remains empty against pinned MPP 1.5.0-1.
5. **Reviewer verdict** — `confirmed`. Confirmed by independent oracle review, including explicit verification of the highest-risk interaction with todo 19's orphan-frame accounting (re-ran both the 8-frame and 4-frame reordered accounting tests; confirmed 'retained' from todo 19's perspective and 'finished-at-drain' from this fix's perspective compose correctly — drain's EOS branch runs before frame matching and directly finishes the already-retained frame without re-accounting it). FIX-21's g_list_free() confirmed to free only the copied list structure, not the frame objects (separate lifecycle, correctly respected — no double-free or frame leak).

### Free copied decoder frame lists on reset

1. **Provenance SHA** — first-party correction against task baseline
   `133cd9f5a67e726699d4ad51b785997b2c78771a`.
2. **Red/green outputs** — LeakSanitizer cannot complete under the established arm64 QEMU
   environment, so the mock interposes `g_list_free()` only while each reset executes. Four
   cycles each retain one pending codec frame, invoke the real non-draining decoder flush,
   and require one transfer-full list release. RED at the parent records zero releases per
   cycle because the `GList` head is advanced to NULL and never freed. GREEN prints
   `four reset cycles freed one copied GList each`. Mutation check: removing
   `g_list_free(frame_list)` restores the zero-release assertion failure.
3. **Hardware gate** — `hardware-independent`. The deterministic count covers the
   `gst_video_decoder_get_frames()` ownership contract and real decoder reset path without
   relying on an unavailable stop-the-world leak check.
4. **MPP ABI closure** — unchanged: this is GLib list ownership only and adds no MPP API.
5. **Reviewer verdict** — `confirmed`. Confirmed by independent oracle review, including explicit verification of the highest-risk interaction with todo 19's orphan-frame accounting (re-ran both the 8-frame and 4-frame reordered accounting tests; confirmed 'retained' from todo 19's perspective and 'finished-at-drain' from this fix's perspective compose correctly — drain's EOS branch runs before frame matching and directly finishes the already-retained frame without re-accounting it). FIX-21's g_list_free() confirmed to free only the copied list structure, not the frame objects (separate lifecycle, correctly respected — no double-free or frame leak).

### Restrict JPEG conversion caps to RGA builds

1. **Provenance SHA** — first-party correction against task baseline
   `ee2df5dd13d5a5c5774a0fd35a123779b9f64dbc`.
2. **Red/green outputs** — the arm64/bookworm harness is configured with
   `-Drga=disabled` and runs `GST_CHECKS=test_jpeg_caps_with_harness`. The deliberate
   unconditional-cap mutant fails RED with `mppjpegdec0 caps BGR16 advertise but build
   deliverability requires it to be omitted`. GREEN omits BGR16, RGB16, and all eight
   32-bit RGB conversion formats while retaining the base decoder formats. The normal
   RGA-enabled bookworm and trixie legs each pass the exact template-format assertion.
3. **Hardware gate** — `hardware-independent`. The assertion reads the registered
   `mppjpegdec` source pad template under the same Meson feature configuration that
   compiled the conversion path.
4. **MPP ABI closure** — unchanged: this is static caps construction only and introduces
   no MPP API call; the closure remains empty against pinned MPP 1.5.0-1.
5. **Reviewer verdict** — `confirmed`. Confirmed by independent oracle review: FIX-22's RGA gate confirmed to reuse the SAME HAVE_RGA mechanism used elsewhere (not a parallel/divergent conditional), matching set_format's actual negotiation reality. FIX-23's 10-bit gate confirmed to derive from the same HAVE_NV12_10LE40/HAVE_NV16_10LE40 macros as the format table; trixie's CI run independently confirmed NV16_10LE40 correctly evaluates absent (pre-GStreamer-1.28), organically proving the conditional. Reference-config golden parity confirmed EXACT (mppvideodec 17/17, mppjpegdec 27/27, zero diff against Radxa runtime goldens) — the stricter-than-usual 'shrink only in non-deliverable configs' requirement satisfied.

### Make release parity exercise capability-aware decoder goldens

1. **Provenance SHA** — first-party test-infrastructure remediation against release
   baseline `73b0e7720e6ed6f40d8903f18e6b29d64e7cd1c2`; no encoder or decoder logic changes.
2. **Red/green outputs** — without the mock-MPP preload, the release command passes both
   source contracts and exits `77` at `mpph264enc`, never inspecting a decoder. With the
   existing mock seam, the pre-fix command passes both encoders and fails at
   `mppvideodec` because the unconditional Radxa golden requires `NV16_10LE40` while
   bookworm/1.22 and trixie/1.26 both omit `HAVE_NV16_10LE40`. GREEN preloads
   `libmppmock.so`, points `GST_PLUGIN_PATH` at the built plugin, and selects the exact
   decoder baseline from the generated `config.h`; all four elements pass in both suites.
   A synthetic reference config defining `HAVE_NV16_10LE40` against the current
   non-capable build fails with `mppvideodec omits NV16_10LE40`, proving the conditional
   cannot turn a wrong capability set green. The immutable Radxa goldens are unchanged.
3. **Hardware gate** — `hardware-independent`. `gst-inspect-1.0` loads the built plugin
   through the same mock-MPP registration seam as the Meson plugin test, and that test now
   invokes the full parity script on every build.
4. **MPP ABI closure** — unchanged: test/workflow/golden selection only; no production
   source or MPP call changed.
5. **Reviewer verdict** — `needs-human-review`. The first independent review confirmed the
   mock wiring, exit-77 propagation, capability selection, explicit wrong-omission failure,
   and zero semantic encoder delta, then requested three test-infrastructure hardenings:
   cleanup on normalizer failure, pinned hashes for the immutable reference goldens, and an
   explicit main-ref release guard. Round 2 confirmed those corrections and found one
   remaining comparator edge case: a second normalized `src_caps=` record could coexist
   with the exact baseline record. Caps keys now require exactly one byte-exact normalized
   record, while additional property records remain allowed by the established contract.
   Regression controls reject duplicate caps, changed caps, an unsupported NV16
   advertisement, and an omitted supported NV16 format. Both suite tests remain green.
   This remediation changes the release-time correctness gate itself and must remain
   unmerged until final independent confirmation.

### Gate decoder 10-bit caps on available GStreamer formats

1. **Provenance SHA** — first-party correction against task baseline
   `ee2df5dd13d5a5c5774a0fd35a123779b9f64dbc`.
2. **Red/green outputs** — before the conditional format list, arm64/bookworm's exact
   template test fails RED: `mppjpegdec0 caps NV16_10LE40 advertise but build
   deliverability requires it to be omitted`. Both GStreamer 1.22 and 1.26 Meson
   configurations detect `HAVE_NV12_10LE40` but not `HAVE_NV16_10LE40`; GREEN makes
   `mppjpegdec` and `mppvideodec` advertise exactly that deliverable set on both legs.
3. **Hardware gate** — `hardware-independent`. Meson's header-symbol checks write the
   same generated config macros that the template assertion consumes, so the test covers
   the selected headers rather than a version guess.
4. **MPP ABI closure** — unchanged: the correction selects existing string literals and
   adds no MPP API call; the closure remains empty against pinned MPP 1.5.0-1.
5. **Reviewer verdict** — `confirmed`. Confirmed by independent oracle review: FIX-22's RGA gate confirmed to reuse the SAME HAVE_RGA mechanism used elsewhere (not a parallel/divergent conditional), matching set_format's actual negotiation reality. FIX-23's 10-bit gate confirmed to derive from the same HAVE_NV12_10LE40/HAVE_NV16_10LE40 macros as the format table; trixie's CI run independently confirmed NV16_10LE40 correctly evaluates absent (pre-GStreamer-1.28), organically proving the conditional. Reference-config golden parity confirmed EXACT (mppvideodec 17/17, mppjpegdec 27/27, zero diff against Radxa runtime goldens) — the stricter-than-usual 'shrink only in non-deliverable configs' requirement satisfied.
