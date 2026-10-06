<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## Test and board-drill contract

**Late teardown input:** every encoder subclass must check atomic `flushing`
before per-frame property application, delegating rejection to the unchanged
common handler. H.264/H.265 otherwise renegotiate after final reset dirties
properties and clears output caps; VP8/JPEG share the ordering defect without
the caps call. `tests/check/enc-teardown.c` pins the real concurrent queue/state
transition with no FLUSH_START workaround, zero property applies and zero bus
errors. See `docs/ENCODER-RUNTIME-CONTRACT.md`; hardware rerun is separate.

**Cross-layer kernel fault bridge:** `gstmppfaultbridge.{c,h}` reads the
island's own `rockchip_mpp` ftrace events out of a private tracefs instance and
joins them, through `(task_id, core_id)`, back to the rkvenc sessions this
element owns — resolved once at `start()` by intersecting `/proc/self/task/*`
with `/proc/mpp_service/sessions-summary`, whose `pid` field is a **TID**. On
crossing its threshold it calls the **unchanged**
`gst_mpp_enc_handle_runtime_error()`, so the existing bounded restart and
`encoder-restarts` are the recovery. It is shared by all four encoder
subclasses through `gstmppenc.c`, exactly as the teardown fix above is. It is
**opt-in** (`GST_MPP_FAULT_BRIDGE=1`), fail-open on every error path, and
refuses to act on any fault it cannot attribute — including a `task_id` two
taskqueues hold at once, where it abandons both candidates rather than guess.
`tests/check/fault-bridge.c` and `tests/check/enc-fault-bridge.c` are
mutation-verified host suites; **no board has produced a real fault through this
path**, and doing so needs an `edge-test` kernel carrying the forbidden
`CONFIG_ROCKCHIP_MPP_CERALIVE_TEST` seam. See
`docs/ENCODER-RUNTIME-CONTRACT.md`.

Hardware-independent gates run in both bookworm/GStreamer 1.22 and
trixie/GStreamer 1.26 environments. The mock-MPP suites prove software state,
ownership, caps construction, and MPP ABI closure; they do not emulate RK3588 DMA
addresses, RGA2, or the encoder firmware.

The registration/parity checker indexes literal golden lines in Bash rather than
launching a process per line; the latter exhausted its unchanged 30-second
deadline under QEMU. Caps multiplicity and every baseline comparison remain
enforced. `tests/parity-comparator.test.sh` runs inside the registration gate and
rejects per-line grep launches while covering literal matching and negative cases.

The standalone **test-only MPI interposer [EXISTS]** is in
[`tests/mpi-interposer/`](../../tests/mpi-interposer/README.md). Build Check runs its
separate debug-only gate after the existing tests; it is absent from the
production build graph and has no install targets. It injects one pre-submit
`MPP_ERR_STREAM` on an explicitly selected healthy context, never writes the
counter and never changes plugin recovery or libmpp. Its independently decoded
synthetic host-output test is **not** a live engine/session or hardware receipt,
and does **not** discharge item 30's literal island-knob row. The production
async-error propagation gap and H.265 poll-error overwrite are recorded in
[`docs/ENCODER-RUNTIME-CONTRACT.md`](../ENCODER-RUNTIME-CONTRACT.md#hardware-error-propagation-gap-partial).
Do not replace that finding with another kernel errno knob or ship this harness.

The board suite is deliberately outside Meson:

| Drill | Hardware claim |
|---|---|
| `d1-runtime-parity.sh` | Package installation, registration of every built factory the board's SoC can carry, four-element golden contract. `mppvp8enc` is scored EXPECTED-ABSENT on an `rk3588` board and PRESENT-REQUIRED elsewhere. |
| `d2-radxa-fork-ab.sh` | Radxa/fork encode A/B over **H.265 primary and H.264 secondary**: 300/300 AUs per codec, SPS geometry/profile/level, zero `RGA_BLIT fail`, and `conversion-fallback-frames = 0` on the fork variant. |
| `d3-main10-stride-ab.sh` | Report-only Main10 current-vs-`*8/pixel_stride0` frame-checksum experiment. |
| `d4-allocation-soak.sh` | 136 s DMA allocation soak with live bitrate, resolution, and temporal-SVC changes, run **on a trial-verified librga backend** and scored on the three conversion counters. |
| `d5-rgaconvert-matrix.sh` | `rgaconvert` conversion matrix — {CSC, scale, crop, rotate} × representative format pairs, each cell measured as PSNR against a software reference of the same operation. |
| `d6-c6b-measurement.sh` | The C6b-perf / C6b-async go/no-go gate. Standalone im2d harness against the board's own librga: no plugin installed, no element instantiated, no capture device opened. Scores fd-vs-handle buffer description (with a single-plane RGBA control that separates a handle-path refusal from a chroma-plane question) and depth-1 `IM_ASYNC` sustained throughput, bracketed by two independent synchronous runs so clock or thermal drift cannot be read as an async gain. |

The latest executed verdicts and their hardware scope are recorded in
[`tests/board/DRILL-RESULTS.md`](../../tests/board/DRILL-RESULTS.md). That tracked
summary preserves failed and inconclusive outcomes; it is not a substitute for
the retained raw transcripts. A criteria change does not carry an old verdict
forward: when a drill's acceptance criteria are reworked, its prior result
becomes history and the drill is **not yet run** under the new criteria until a
board actually executes it.

Every script requires `CERALIVE_BOARD_TEST=1` and otherwise exits 77. Board
identity is supplied only through `BOARD_IP`, `BOARD_SSH_USER`, and
`BOARD_SSH_PASS`; repository files never locate credentials or reference a
workspace parent. d1/d2/d4/d5 also take package paths through environment
variables.

### The suite proves

- The exact package and kernel named in each transcript loaded on the reachable
  board used for that run.
- Registration/property/caps/rank behavior and the finite runtime observations
  scored by each completed drill.
- For d2/d4, zero matching `RGA_BLIT fail` and `rga_api version` journal lines in
  the measured window, and `conversion-fallback-frames = 0` read from the
  element's own end-of-run counter summary — never inferred from silence.
- For d3, only the enum written by its frame-count/checksum/error oracle:
  `CURRENT_CORRECT`, `ALTERNATIVE_CORRECT`, or `INCONCLUSIVE`.
- For d4, that the backend under test is the trial-verified librga one: the
  `mpprgabackend` probe logged a driver version at or above the 1.2.4 floor in
  that run. A run whose log carries no such probe line FAILS; availability is
  never assumed from the absence of an error.
- For d5, only the measured PSNR of each executed cell against its software
  reference. An unexecuted cell is `NOT-RUN`; it is never scored from a
  neighbouring cell, from a previous drill, or from the element's own logs.

### The suite does NOT prove

- Hardware not named by the transcript, including the separate mainline/edge 7.2
  fleet when a drill runs on the vendor 6.1 bench board.
- Long-term thermal, suspend/resume, OTA, or every capture-device path.
- That an `INCONCLUSIVE` d3 result authorizes a stride change. d3 is report-only;
  no shipped stride edit follows without decisive evidence and separate review.
- The pre-existing 4K59.94 H.265 SIGSEGV. That fault is out of scope and must not
  be chased or reclassified by these drills.
- ThreadSanitizer or LeakSanitizer cleanliness. TSAN cannot start under the known
  qemu-user VMA layout and LSan cannot complete there; deterministic mock seams
  and counters substitute only for the specific properties they assert.
- A result from an unreachable board. Such a run is `SKIPPED-unreachable` with an
  attempt transcript, never PASS.
- That d5's PSNR threshold means bit-exactness. RGA is a fixed-function 2D
  engine and its chroma resampling does not match libgstvideo's; a passing cell
  says the hardware result is faithful to the reference at the recorded dB, not
  that the two are identical. A cell that RGA cannot perform at all fails
  negotiation instead, which is a distinct, separately recorded outcome.

