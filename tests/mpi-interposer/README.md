# Test MPI-error recovery without changing the plugin [EXISTS]

**This proves MPI-error → plugin restart → synthetic host-output continuation.
It does NOT discharge item 30's literal “inject an MPP error via the island test
knob” requirement. No board or live cerastream session is tested here.** The
[production propagation gap](../../docs/ENCODER-RUNTIME-CONTRACT.md#hardware-error-propagation-gap-partial)
needs a cross-layer error bridge or a separately approved libmpp change;
another kernel errno knob is insufficient.

## Run the host gate

Use the normal pinned MPP/RGA development environment, plus FFmpeg with libx264
and libx265, Meson, GCC and ShellCheck. No RK3588 devices are required. From
this repository root:

1. Build and run the existing controls first:

   ```sh
   meson setup build --prefix=/usr -Drkximage=enabled -Drockchipmpp=enabled \
     -Dkmssrc=enabled -Drga=enabled
   meson compile -C build
   meson test -C build --print-errorlogs
   ```

2. Run the separate interposer gate:

   ```sh
   shellcheck tests/mpi-interposer/check.sh
   bash tests/mpi-interposer/check.sh build
   ```

The script prints its unique `test-results/mpi-interposer.*` evidence directory.
Keep its logs, decoder output, elementary streams and build metadata. Build Check
runs it in both Bookworm and Trixie. Missing instrumentation, decoder, output,
restart log or expected counter delta is a failure, never a skip.

The standalone project requires **all** of `--buildtype=debug`,
`-Dtest-only=true`, and assertions enabled. Default, debugoptimized, release,
minsize and NDEBUG builds fail configuration. Source compilation without the
test macro also fails. Nothing is installed, and the production Meson graph,
plugin and packaging scripts never reference this project. The gate checks the
empty install manifest and absence of test targets/symbols/dependencies in the
normal build. There is no production environment-variable activation switch.

## What is wrapped

`libmpi-test-interposer.so` resolves the next `mpp_create`, `mpp_init` and
`mpp_destroy` with `RTLD_NEXT`. Every successful create receives a private copy
of the returned `MppApi`; the vendor table is never modified. Unknown API sizes
and broken instrumentation abort with a harness error rather than returning a
synthetic MPP error that could falsely count as a successful test injection.

Put, get, reset and control callbacks keep their original context and arguments.
Only an explicitly armed, non-null, non-EOS put returns `MPP_ERR_STREAM`, once
per process, **before** the real put is called. That branch neither retains nor
deinitializes the frame. All other calls retain their actual results and output
pointers. The plugin, not the interposer, owns rejection cleanup and the counter.

Arming requires an initialized encoder with at least three successful input
submissions and three nonempty output packets. This is an instrumentation health
floor, **not a decoder or receiver-health assertion**. The integration test also
independently decodes output before arming.

IDs are assigned per context lifetime. Destruction cancels an unconsumed arm;
recreated contexts never inherit it. A consumed arm cannot be rearmed anywhere
in that process. In-flight wrapped callbacks keep their table alive until they
return. Real calls run outside the registry lock; only create/destroy lifecycle
operations are serialized to avoid pointer-reuse races. This does not legalize
using a table after destruction, unloading an active interposer, or destroying
a context concurrently with an unwrapped vendor operation. Follow MPP's caller
lifetime contract. Do not fork a running multithreaded encoder with this loaded.

`mpi-test` stderr records carry sequence, monotonic time, PID, context address,
lifetime ID, operation, return and input/output counts. A reused address is not
a reused lifetime ID. Controls log their command IDs without interpreting or
changing parameter data. Logging can perturb timing; this is not a performance
or vendor-call wall-clock-bound benchmark.

## Select the program encoder, not a preview or preflight

For a future separately authorized **in-process test observer**, retain a strong
reference to the actual engine pipeline's `venc_bps` element and use
`program-target.h`:

1. Resolve that element from the running program pipeline, not from a creation
   ordinal, guessed MPP address, codec, bitrate or matching geometry.
2. Independently verify advancing decoded receiver output and record engine
   PID/start time, service restart count, actual session ID and element identity.
3. Call `mpi_test_program_id(program)`. Require a nonzero lifetime ID.
4. Call `mpi_test_arm_program(program, expected_id)` exactly once and require
   `MPI_TEST_ARMED`. The helper checks name, runtime encoder type and instance
   size, and holds the encoder stream lock across context lookup and arming.
   A stale ID, wrong type/name, insufficient output or spent arm is rejected.
5. Correlate one `inject-put-before-submit ret=-1004`, successful old destroy /
   new create / timeout controls / init / SET_CFG, the same element's **read-only
   property delta +1**, unchanged engine/session identity, and independently
   decoded postfault output. Any missing term fails the end-to-end result.

No observer is wired into cerastream in this change. The helper uses this
repository's private `GstMppEnc` header: identify the actually loaded plugin and
libmpp bytes before any future use; instance size alone is not an ABI identity
proof. Source `.4` (`f8960191`) and the tested main (`f84526ae`) share encoder
implementation blob `2d18d7ebfbb93f1359dd67577f00343e87230bd1` and header blob
`9d1bda6d41f89db702b88259ff2b48bd69e83fcb`. Historical test A used plugin `.2`;
production B's `.4` identity must not be assigned to A.

On a future hardware lane, preload **only** the interposer before the real
pinned MPP provider, never `libfake-mpi.so` or `libfixture-mpp.so`. Launch-time
preload is required; loading it after contexts exist cannot wrap their tables.
Remove all test instrumentation after the authorized lane. This document grants
no board lease, deployment or release permission.

## Evidence boundaries

- `interposer-test.c` uses a read-only shared fake API table with separate
  contexts. It checks passthrough/error results, exact pre-submit frame ownership,
  non-target isolation, stale IDs, readiness, EOS, 16-way one-shot contention,
  and destruction while a vendor callback is in flight.
- `fixture-mpp.c` builds a separate variant of the existing mock, without editing
  it. It replaces only its synthetic packet payload with FFmpeg-generated black
  and white IDR access units. It does **not** encode the supplied raw input.
- `plugin-test.c` loads the real unmodified H.264/H.265 plugin, runs disarmed and
  armed cases with packet copying and zero-copy, and reads the actual GObject
  property. A separate FFmpeg process has `LD_PRELOAD` removed and decodes the
  pre-arm three pictures, complete eight pictures and postfault five pictures
  afresh. Every Y/U/V sample must match the expected alternating colour. Missing,
  duplicated or stalled output cannot pass by repeating one static picture.
- The PID, element pointer, PLAYING state, PTS and GStreamer stream ID are real
  **host harness** identities. They are not an engine PID/session proof.
- Existing `tests/check/enc-restart.c` remains untouched: four put-STREAM errors
  give exactly three recreate/configure cycles then budget exhaustion;
  get-STREAM increments once and get-TIMEOUT never increments.

Hardware encoding, real cerastream/SRT session continuation, and propagation
from an island knob remain **NOT RUN / NOT DISCHARGED**. Even a future successful
hardware MPI-interposer run would not satisfy the literal kernel-stimulus row.

## 2026-09-15 host verification [PARTIAL]

The standalone gate passed all five unit cases and eight codec/memory/mode
integration cases in both Bookworm/GStreamer1.22.0 and Trixie/GStreamer1.26.2,
under host arm64 QEMU. Default/release build exclusion and the empty install /
production-linkage checks passed in both. The Bookworm complete plugin gate
passed12/12 plus its GLIBC-floor/self-tests, pinned MPP ABI closure and static
package contract.

**Earlier blocked run:** Trixie's final unchanged full run was
11/12: the existing registration suite hit its30s limit under QEMU. An earlier
run also failed a DMA-BUF output assertion; that case passed on the final run.
Both passed unchanged in isolation, but no cause was established and no failure
was waived. No timeout, assertion or existing test was changed. Retained logs:
`final-gate-{bookworm,trixie}.log`, `legacy-isolated-trixie.log` and
`standalone-gate-trixie.log` under `test-results/`. This is not unconditional PR
readiness, hardware qualification, or permission to close item30.

### Registration deadline diagnosis and repair

The existing checker repeatedly spawned emulated `grep` processes for individual
golden lines. This was test-orchestration cost, not an interposer interaction:
the old suite never loads the interposer, all tests are serialized, and the
registration/parity subprocesses use unique registries. A fresh baseline full
run completed registration in 14.66 s; its immediate isolated run timed out at
30.02 s. Isolation therefore does not clear the failure.

Timestamped Bash traces place the slowdown in ordinary exact-line checks: the
same first `grep` took 21.625 ms versus 104.198 ms. Source-contract checking took
2.13 s versus 8.40 s. A separate monitored run consumed 19.24 CPU-seconds over
19.05 wall-seconds, with zero major page faults or swaps. This establishes
CPU-bound emulated checker work; it does not prove global CPU saturation or
identify the historical host scheduling/frequency event.

The checker now indexes those same literal lines once in Bash. In paired full
runs pinned to CPU 27 with one CPU-only helper, registration fell from 22.52 s
to 11.43 s (18.57 s margin), including eleven new comparator regressions; the
GstHarness control stayed at 14.97/15.00 s. Exact-line `grep` launches in the
parity trace fell from 406 to 2. The new comparator's no-subprocess tripwire
fails against the old implementation; all eleven semantic outcomes also match
the old comparator with real grep. Missing/changed/duplicate caps and missing or
changed properties still fail; additive properties and literal metacharacters
retain their prior meaning. No timeout, golden, or assertion was relaxed.

Evidence: `test-results/registration-{full,isolated,loaded-baseline,loaded-indexed}.*`
and `registration-comparator-{red,green,baseline-equivalence}.log`. These timings
are host diagnostics, not hardware performance. The earlier DMA-BUF assertion
has not been assigned this cause; its failed receipt remains part of the record.

### Final closure gate

Both complete suites now pass without affinity, load helpers, tracing, retries,
or timeout overrides. The final gate uses normal Meson defaults.

| Gate | Bookworm / GStreamer 1.22.0 | Trixie / GStreamer 1.26.2 |
|---|---|---|
| Complete plugin build and Meson suite | PASS, 12/12, no skips | PASS, 12/12, no skips |
| Registration, including eleven new regressions | 9.80 s; 20.20 s margin | 6.78 s; 23.22 s margin |
| Standalone interposer | 5/5 unit, 8/8 integration | 5/5 unit, 8/8 integration |
| Release/configuration exclusions and empty install manifest | PASS | PASS |
| No test symbols, targets or dependency in production plugin | PASS | PASS |
| GLIBC gate self-tests | PASS | PASS |
| Declared target GLIBC floor | PASS, 2.17 ≤ 2.36 | Not applicable: forward-compatibility suite |
| Pinned MPP ABI closure and static package contract | PASS | PASS |

Receipts: `test-results/closure-{bookworm,trixie}.log`; interposer artifacts in
`mpi-interposer.huXHV3` (Bookworm) and `mpi-interposer.4csk2G` (Trixie).
The production encoder blob remains `2d18d7ebfbb93f1359dd67577f00343e87230bd1`.
These green host gates do not discharge item 30 or explain the historical DMA-BUF
failure; no board, live-engine, or hardware-media acceptance was run.
