# Encoder runtime contract

## EOS drain admission [EXISTS — host regression]

`gst_mpp_enc_reset()` publishes its existing always-drain policy while holding
the video encoder stream lock, **before** publishing `flushing` and releasing
the stream lock to acquire the encoder mutex. The early flushing notification
still wakes a capacity-blocked input producer; moving that notification after
the mutex acquisition would risk deadlock.

Previously the output task could acquire the released stream lock while
`flushing == TRUE` and `draining == FALSE`. A valid pending packet then took
the drop path, leaving EOS successful but no encoded output. This surfaced in
PR #47's Bookworm CI run `35488380577` at the existing `enc-dmabuf` output
assertion. It is a pre-existing encoder scheduling race, not a GStreamer 1.22
API difference or a converter-reaper interaction: the affected encoder code
is unchanged from that PR's base, the failing test creates no `rgaconvert`,
and Meson runs its converter suite in a separate process.

The deterministic H.264/H.265 cases in `tests/check/enc-dmabuf.c` hold the real
encoder mutex during EOS, release a pending mock packet after observing
flushing, and wait for the output critical section before unblocking reset.
The existing non-mappable DMA-BUF, exact FD-import, negotiated caps and zero
copy/drop/rejection assertions remain enforced. Restoring the old assignment
order fails both cases on arm64 Bookworm/GStreamer 1.22.0 **and**
Trixie/GStreamer 1.26.2. No sleep is used as evidence of packet completion.

This changes neither the bounded drain budget nor MPP calls, buffers, package
pins or DMA handling. The converter's separate five-second stop bound,
two-second diagnostic and terminal-fence-owned reaper are untouched. Host mock
coverage is not hardware or release qualification.

## Late input during teardown [EXISTS — host regression; board rerun outstanding]

All four encoder subclasses (H.264, H.265, VP8 and JPEG) check the atomic
flushing flag **before** applying per-frame properties. A rejected frame goes
straight to the common handler, which retains ownership of frame disposal and
returns `GST_FLOW_FLUSHING`. Its mutex, task-start guard, capacity-wait wakeup
and second flushing check are unchanged. A genuine property/configuration
failure while running still returns `GST_FLOW_NOT_NEGOTIATED`.

Final `PAUSED→READY` reset stops the output task and marks properties dirty.
An upstream queue can still deliver before the parent state change deactivates
the sink. Previously H.264/H.265 reapplied those properties and renegotiated
output caps before reaching the common flushing guard. Against already-cleared
src caps this returned `NOT_NEGOTIATED` and raised upstream stream errors.
VP8/JPEG did not renegotiate there, but unnecessarily configured MPP in the same
window. The subclass check runs under the video encoder stream lock, which also
serializes final reset; the common handler still rechecks after releasing that
lock to acquire its mutex and after waiting for capacity.

`tests/check/enc-teardown.c` uses a real `videotestsrc ! queue ! encoder !
fakesink` pipeline with mock MPP. A buffer probe holds the second queued input;
a test-only parent-class rendezvous releases it after the **real** final reset
and src-pad deactivation. No reset/dirty/admission implementation is copied and
no `FLUSH_START` workaround is sent. A synchronous bus handler counts errors
even during shutdown. All waits have deadlines and class callbacks are restored.

Before the fix, both H.26x cases returned `-4` and posted two bus errors each;
VP8/JPEG returned `-2` but each applied configuration once. After the fix, all
four return `-2`, with zero property applications and zero bus errors. The full
Trixie/GStreamer 1.26.2 host suite passes. This is deterministic software-path
evidence, not twenty instrumented Rock cycles or a release/install receipt.

## Composition primary color selection [EXISTS]

The 2026-09-16 OPi trace follows BT.709 from HDMI NV16 through `rgaconvert`,
rate normalization and the capture queue, then observes BT.601 at compositor
src and encoder input. Independent ffprobe output reports SMPTE170M primaries,
transfer and matrix. The encoder maps the caps it receives correctly; the loss
is compositor format selection, not the already-fixed converter fixation.

`GstVideoAggregator`'s default selector constructs plain-memory possible caps
from each input's video info. These cannot intersect the compositor's DMA-BUF
downstream caps, so the parent can fall back to a default color tuple. The
compositor's subsequent geometry override never repaired that tuple.

The compositor now supplies `find_best_format`, selecting the primary NV12
accumulator's video info. The parent still constructs preferred caps and retains
downstream alternatives; no encoder VUI mapping, CSC coefficient, allocation or
layout changes. `test_primary_color_survives_unconstrained_output` negotiates
actual compositor output without constraining downstream colorimetry, covering
BT.709, BT.601 and full-range BT.601. It failed on BT.709 before the fix and
passes afterwards; a forced-BT.601 mutation fails the same assertion. Existing
explicit-color and im2d-descriptor tests remain in the suite.

Before the primary has negotiated, the parent initializes the callback's video
info to UNKNOWN and parses its existing fixed-caps fallback if it stays UNKNOWN.
`test_unconfigured_primary_uses_parent_caps_fallback` exercises that actual
`update_caps` chain with a requested but unnegotiated primary on both supported
GStreamer versions; no uninitialized video info is consumed.

Host tests establish negotiated metadata and backend dispatch, not pixel
accuracy or a new hardware qualification. A full acceptance rerun remains a
separate task; no release or installation is implied by this change.

The focused OPi slot-B candidate check subsequently encoded six independent
1080p30 HEVC sessions, one per preset. `ffprobe -show_entries stream=...`
reported BT.709 primaries, transfer and matrix in all six bitstreams. The caps
trace agreed at capture, compositor output and encoder input. All 1,465 observed
program frames retained compositor-to-encoder identity and matched the MPP
import inode, with zero recorded CPU-copy calls. These are bounded regression
checks, not the separate full acceptance rerun or a pixel-accuracy benchmark.
The candidate used the already-merged pattern-geometry fix and the companion
librga NV12-pattern candidate. Loader logs confirmed the per-process plugin and
library paths; installed hashes and saved geometry were restored afterwards.

## Composition DMA-BUF boundary [EXISTS]

The H.264/H.265 sink templates retain their plain raw caps and add **linear
NV12** with `memory:DMABuf`. No DMA_DRM/modifier, deep-colour, new factory or
property contract is added. No allocation, alignment, conversion or pixel code
changes: accepted buffers still use `gst_mpp_enc_convert()`'s existing
`gst_mpp_allocator_import_gst_memory()` path. Incompatible memory/layouts retain
the existing fail-closed production behavior; advertising a feature does not
make every layout directly importable.

### Root cause and single-source contrast

The installed item-30 control refused `rgacompositor ! queue ! tee ! mpph265enc`
before opening a capture source. The owner is **element pad templates**, not
engine graph construction: the pre-fix `gstmpph265enc.c:176–182` and
`gstmpph264enc.c:117–123` advertised plain `video/x-raw` only, whereas
`gst/rockchiprga/gstrgacompositor.c:22–34` advertises DMA-BUF-only NV12 output.
Queue and tee propagate that incompatibility; a capsfilter cannot manufacture
an intersection between those features.

The successful ordinary HDMI control is not contradictory. In cerastream
`100e84e`, `crates/cerastream/src/engine/encode.rs` constructs plain raw
`leg_output_caps` for switching. Composition uses explicit DMA-BUF input caps
and the compositor's DMA-BUF-only src template. **Plain caps do not prove a CPU
copy:** the actual `GstMemory` can still be DMA-BUF and take the encoder's import
path. Conversely, explicit DMA-BUF caps alone are not an FD-identity proof.

`tests/check/enc-dmabuf.c` attempts actual `gst_pad_link()` calls through the
real compositor, queue, tee and encoder elements for all six presets and both
codecs. Before the template change, the H.265 row reports
`preview_tee -> mpph265enc refused: no common format`; an explicit-DMA-BUF frame
push returns `GST_FLOW_NOT_NEGOTIATED`. The plain-caps import control passes.
After the change, all links succeed with DMA-BUF-only queried caps.

Separate GstHarness cases push aligned NV12 at 1920×1080p30 (1088-row storage)
and 3840×2160p60000/1001 under both explicit and plain caps. They require mock
encoded output, the original FD at the real plugin's MPP import call, retained
negotiated features and zero `conversion-fallback-frames`,
`conversion-dropped-frames`, and `layout-rejections`, with CPU fallback and RGA
conversion disabled. The host memfd is marked non-mappable. Mock MPP records
the import argument but allocates its own stand-in storage: these checks prove
plugin dispatch, **not hardware FD identity or decoded video**.

Full arm64 builds and all **12/12 Meson suites** pass under host QEMU in
Bookworm/GStreamer 1.22.0 and Trixie/GStreamer 1.26.2, with no skipped suites.
Both MPP ABI-closure checks and GLIBC-gate self-tests pass; the declared
Bookworm target passes its GLIBC floor. The static package contract passes.

### Separate finding: ordinary 1080p30 PLAYING failure [PARTIAL]

Retained `RUN-30-OPI-20260915` evidence at root ledger commit `847318bd`
(`docs/media-island/ledger/phase7.md`, `job-005-observe.sh.log` and
`job-008-caps-controls.sh.log`) shows the non-composited 1080p30 request passed
linking but returned `pipeline did not reach PLAYING: Element failed to change
its state` at monotonic 41345.672456372. The later native 4K59.94 control decoded
1,156 frames over 19.285933 seconds with BT.709 primaries/transfer/matrix and
limited range. That is historical control evidence, not this patch's board QA.

The retained logs do not name the failing element or contain its GStreamer
ERROR/debug details. Lower output geometry/rate requires additional upstream
normalization relative to the native mode, but scaling, rate negotiation,
allocation, capture state and encoder activation are not distinguished by
these receipts. This is a separate **failure stage**, not a proven independent
root cause and not a failure this caps-only patch claims to fix. A future
authorized diagnostic needs caps/state tracing armed before the ordinary
1080p30 Start, the per-element bus error and conversion counters, followed by
the unchanged native control. No such board run was authorized for this fix.

Item 30 remains open until the real-source >=600-second composition, per-frame
FD trace, zero counters, VUI and IDR rows pass. Ten-minute UVC power endurance
was never exercised and remains **untested**, not power-failed. No board access,
installation, camera change, merge, tag or release is part of this software fix.

## Latency, recovery and colorimetry

The MPP H.264 and H.265 encoders report one frame of codec work plus the
currently tracked pending depth as fixed latency. The value is recalculated
after each successful `MPP_ENC_SET_CFG` application.

Runtime failures from frame submission or packet retrieval trigger a complete
MPP context restart unless MPP reports its non-blocking timeout/no-capacity
result. Recovery drains and destroys the old context, creates and initializes a
new one, and reapplies the complete retained configuration. Three restarts are
allowed in any ten-second window. A fourth failure posts
`GST_STREAM_ERROR_ENCODE` with `encoder restart budget exhausted`. The
read-only `encoder-restarts` property reports successful restarts over the
element lifetime.

### Hardware-error propagation gap [PARTIAL]

**The bounded restart exists, but real hardware task failures cannot trigger it
through the pinned library's configured async APIs.** This is a production
recovery/observability gap, not just a test-harness limitation. No propagation
fix or libmpp-pin change is implemented by the test-only interposer.

Verified against installed `.4` source
`f8960191ea14360347ceb9b96b6dce72f26b743d` and main
`f84526ae1264bf1d4975d0eb4f4e2f9002f87ba5`: both have encoder blob
`2d18d7ebfbb93f1359dd67577f00343e87230bd1`. The plugin sets nonblocking input
and a 1ms output timeout (`gstmppenc.c:1204–1214`), rejects NOK/TIMEOUT as restart
stimuli (`1315–1326`) and increments only after successful recreation (`1299`).
The public call sites are `2194–2198` and `2229–2232`.

Pinned MPP source `194af181db3a02a095c01db84e176d972e19b216` supplies the missing
cross-layer link:

- [`mpp/mpp.cpp:820–891`](https://github.com/tsukumijima/mpp-rockchip/blob/194af181db3a02a095c01db84e176d972e19b216/mpp/mpp.cpp#L820-L891):
  async put returns only OK/NOK; async get only OK/NOK/TIMEOUT. None triggers
  the plugin restart on this valid initialized path.
- [`mpp/codec/mpp_enc_impl.cpp:3258–3314`](https://github.com/tsukumijima/mpp-rockchip/blob/194af181db3a02a095c01db84e176d972e19b216/mpp/codec/mpp_enc_impl.cpp#L3258-L3314):
  a worker failure forces a future IDR and queues a zero-length packet. Get
  returns OK for that packet; plugin `2260–2269,2324–2329` drops it as though it
  were a rate-control drop. An empty packet does not uniquely identify a fault.
- [`hal_h265e_vepu580.c:3306–3319`](https://github.com/tsukumijima/mpp-rockchip/blob/194af181db3a02a095c01db84e176d972e19b216/mpp/hal/rkenc/h265e/hal_h265e_vepu580.c#L3306-L3319):
  H.265's non-split path overwrites the poll return with hardware-status checking,
  potentially swallowing the error even before worker completion.

The [standalone MPI test](../tests/mpi-interposer/README.md) proves a qualifying
public error causes the unchanged plugin to restart and continue synthetic host
output. **It does not satisfy item 30's island-knob requirement.** A separately
scoped cross-layer bridge or approved propagation change must preserve task-error
provenance through HAL, worker and public MPI, including H.265's earlier overwrite,
without confusing legitimate rate-control drops or breaking frame ownership.
Another kernel errno knob cannot extend the public async return set. MNH-27's
libmpp freeze remains intact; no hardware result follows from these host tests.

### Cross-layer kernel fault bridge [PARTIAL — mechanism only, opt-in, no board result]

This is the separately scoped bridge the gap above names. It supplies the
missing signal **around** libmpp rather than through it: the fault travels
kernel → tracefs/procfs → plugin, so the pinned `librockchip-mpp1` is neither
rebuilt nor re-pinned and MNH-27 is untouched.

**Why a side channel at all.** The island already records a complete
session→task→fault correlation, and it records it only in its own ftrace
events. `mpp_task_queued` carries `session->index` — the same index
`/proc/mpp_service/sessions-summary` prints — and `mpp_task_error` carries the
core and task that failed. Both are plain integers in their `TP_printk`, with
no pointer hashing. Nothing in that chain is new work: the events, the procfs
summary, `CONFIG_DEBUG_FS`/`FTRACE`/`TRACEPOINTS` and both `sys-kernel-debug`
and `sys-kernel-tracing` mounts are already in the shipped image.

**Opt-in, and deliberately off by default.** `GST_MPP_FAULT_BRIDGE=1` arms it.
Unset, the element behaves exactly as it did before this existed: no tracefs
instance is created, no event is enabled, `kernel-faults` stays 0, and no
restart can originate here. It is off because the correlation rules are proven
by host tests while the *stimulus* is not — see the boundary at the end of this
section.

| Variable | Meaning |
|---|---|
| `GST_MPP_FAULT_BRIDGE` | `1` arms the bridge. Anything else leaves it off. |
| `GST_MPP_FAULT_BRIDGE_THRESHOLD` | Owned faults required inside 2 s before a restart is requested. Default 3. |
| `GST_MPP_FAULT_TRACEFS` | Tracefs root. Defaults to `/sys/kernel/tracing`, then `/sys/kernel/debug/tracing`. |
| `GST_MPP_FAULT_INSTANCE` | Use this trace instance directly instead of creating one. Test seam. |
| `GST_MPP_FAULT_SESSIONS` | Session summary path. Defaults to `/proc/mpp_service/sessions-summary`. |

**The three-way join, and why two hops are not enough.** `task_id` is
`atomic_fetch_inc()` per **taskqueue**, not global, so two queues both start at
zero and their ids collide. `mpp_task_error` carries `core_id` but not the
queue, so the join key is `(task_id, core_id)` — and `mpp_task_queued` does not
carry a core. `mpp_core_selected` is the third hop that supplies it:

```
mpp_task_queued   session=11 task=41 client=16     -> (task 41) belongs to session 11
mpp_core_selected task=41 core=0 idle=0x3          -> (task 41, core 0)
mpp_task_error    core=0 task=41 irq_status=0x100  -> session 11
```

`mpp_task_done` is consumed too, purely to retire completed entries so the
bounded map stays useful. `mpp_task_started` and `mpp_reset` are ignored:
`mpp_reset` carries no session at all and is attributable only by adjacency, so
a bare reset is never charged to a session.

**Ownership, and the TID trap.** The `pid` field in `sessions-summary` is the
**creating thread's TID**, not the process pid, and that thread can die while
the session lives. Ownership is therefore resolved once at `start()` — on the
thread that just called `mpp_init()` — by intersecting `/proc/self/task/*` with
the summary's rkvenc-core sessions, and the resulting index set is cached.
A successful context restart re-resolves, because destroying and recreating the
context closes that kernel session and opens a new one with a new index; the
result is **unioned** into the cached set rather than replacing it, so a later
read that no longer sees an already-known session cannot un-own it.

**Every ambiguity resolves away from acting.** A fault is acted on only when it
joins to a session this element owns. All four of these are refused instead:

- a fault whose `mpp_task_queued` was never observed (tracing started late, or
  the ring overran);
- a fault arriving before ownership has been resolved even once;
- a fault on a session belonging to another process — the shape of cerastream's
  capture-probe child, whose encoder runs in a separate process entirely;
- a `mpp_core_selected` for a `task_id` held by two taskqueues at once. Binding
  either candidate is a coin flip whose wrong side restarts a healthy encoder
  on somebody else's fault, so both candidates are abandoned. Losing detection
  is the safe failure; a false restart is not.

**Fail-open everywhere.** No tracefs mount, no `events/rockchip_mpp` directory,
an unwritable `enable`, an unopenable `trace_pipe`, an unreadable summary or an
unparseable line each disable the bridge with one log line and leave encoding
untouched. An instance the bridge created is removed again on any of those
paths and on element stop. The bridge is an observer and is never load-bearing.

**What it drives.** Nothing in `gst_mpp_enc_restart_context()` changed. On
crossing the threshold the bridge calls the unchanged
`gst_mpp_enc_handle_runtime_error()` with `MPP_ERR_VPUHW` — libmpp's own "VPU
hardware error", which is what `mpp_task_error` reports and is neither of the
two values that handler filters out. So the restart is the same bounded
three-per-ten-seconds recovery, and `encoder-restarts` counts it the same way.
Polling happens on the output task thread under the stream lock, the same
context as the two pre-existing call sites, so no new concurrency is
introduced. It is rate limited to 100 Hz and bounded to 64 KiB per poll.

`kernel-faults` is a new read-only `guint64` reporting RKVENC task errors the
kernel attributed to this element's own sessions. It is additive, like the
three conversion counters and `encoder-restarts`, and exists so a board
follow-up can tell "detected but below threshold" from "not detected at all".

**IOMMU faults are detected only INDIRECTLY, and must not be described
otherwise.** The MPP IOMMU handlers record nothing and emit no tracepoint. The
faulting task then fails to complete and the ~500 ms timeout worker produces
the `mpp_task_error`. The signal arrives, late, as a timeout — good enough for
recovery, wrong to call IOMMU fault detection.

**Proof boundary.** `tests/check/fault-bridge.c` (21 cases) drives the
correlator and the real reader with synthetic trace lines in the exact
`TP_printk` shapes and with a real fd on a real file;
`tests/check/enc-fault-bridge.c` (4 cases) drives the shipped `mpph264enc`
through a synthetic tracefs and asserts `encoder-restarts` moves for an owned
fault and does **not** move for a foreign or unjoinable one, with a positive
control in the same element and run. Both suites are mutation-verified.

None of that is a hardware result. **No board has produced a real RKVENC fault
through this path.** Confirming that a deliberately injected hardware fault
reaches the restart end to end needs an `edge-test` image carrying
`CONFIG_ROCKCHIP_MPP_CERALIVE_TEST`, which is in the production kernel's
`forbidden-symbols.list` and cannot ship. That validation, and any decision to
arm the bridge by default, are a separate follow-up.

Negotiated BT.601, BT.709, and BT.2020 colorimetry is written to MPP's
`prep:colorspace`, `prep:colorprim`, `prep:colortrc`, and `prep:range` keys.
Full range maps to MPP's JPEG range and limited range maps to MPEG range. Caps
without an explicit `colorimetry` field write no color keys; renegotiation from
specified to unspecified starts from a fresh MPP config so old values cannot
leak into the new stream.

sRGB transfer is also supported: GStreamer `GST_VIDEO_TRANSFER_SRGB` (enum 7)
maps to `MPP_FRAME_TRC_IEC61966_2_1` (H.26x transfer code 13). The discriminating
`2:4:7:1` tuple writes `prep:colorspace=6`, `prep:colorprim=1`,
`prep:colortrc=13`, and `prep:range=MPP_FRAME_RANGE_MPEG`. Its expected SPS is
`video_full_range_flag=0`, `matrix_coefficients=6`,
`transfer_characteristics=13`, `colour_primaries=1`, with video-signal and
colour-description presence flags set. The range enum itself is not the SPS
full-range flag. This is metadata transport, not a pixel-conversion claim.

### Unmapped values (unchanged)

Each unsupported axis warns and returns before **any** of the four color-key
writes. An unsupported explicit tuple on an already configured encoder can
therefore leave prior color keys in place; "omitting VUI" describes skipped
configuration, not a guarantee of absent bits. This separate behavior is not
changed by adding sRGB.

- Matrix: UNKNOWN, RGB/identity, FCC, SMPTE240M.
- Primaries: UNKNOWN, BT470M, SMPTE240M, FILM, ADOBERGB, SMPTEST428,
  SMPTERP431, SMPTEEG432, EBU3213.
- Transfer: UNKNOWN, GAMMA10/18/20/22/28, SMPTE240M, LOG100/316, ADOBERGB,
  SMPTE2084/PQ, ARIB_STD_B67/HLG.
- Range: UNKNOWN; both defined full and limited ranges already map.

All-UNKNOWN colorimetry is an intentional no-op. The named `sRGB` preset uses
an RGB/identity matrix and is not interchangeable with YUV `2:4:7:1`; adding
the transfer case does not add an RGB-matrix mapping or broaden negotiation.

`tests/check/enc-colorimetry.c` checks the exact tuple from a cold start and
BT.709→sRGB→BT.709 color-only caps transitions on both encoder factories.
Before the fix, its two added cases failed with missing cold-start keys and
stale BT.709 keys respectively. These are MPP configuration assertions, not
hardware-generated bitstream evidence.

### 2026-09-14 — sRGB SPS serialization evidence [PARTIAL]

The plugin change is three added lines; inherited CRLF, factories, properties,
defaults, caps negotiation and package pins are unchanged. Full arm64 builds
and all **11/11 Meson suites** pass in bookworm/GStreamer 1.22.0 and
trixie/GStreamer 1.26.2 under host QEMU user emulation. No suites skipped.
Both MPP ABI-closure gates and GLIBC gate self-tests pass; the trixie plugin
passes the repository's declared GLIBC floor. Both changed C files pass clangd
with the bookworm target headers configured locally.

For a stronger check than captured config alone, a separate process called the
**real pinned library's software header writers**, not the mock's packet output:

1. The actual plugin negotiated `bt709`→`2:4:7:1`→`bt709` at unchanged geometry
   through GstHarness, once for each codec. The existing mock captured its four
   `prep:*` values after each transition.
2. An isolated serializer process, with **no `LD_PRELOAD`**, applied those
   captured integers through real `mpp_enc_cfg_set_s32`. It used
   `h264e_sps_update`/`h264e_sps_to_packet` for H.264 and
   `h265e_set_extra_info`/`h265e_get_extra_info` for H.265. No `mpp_create`,
   `mpp_init`, pixel buffers, frame submission or encoder device was used.
3. A native host executable independently parsed the written Annex-B bytes
   with GStreamer's H.264/H.265 codec parsers (1.28.7), asserting all presence
   flags plus each leg's expected color values. It imports no MPP headers or
   serializer code. Both pre-fix B files fail that oracle (exit 9); all six
   post-fix files pass.

The library is `librockchip-mpp1_1.5.0-1_arm64.deb`, package SHA-256
`fe839d41010def25b2c096581815fd26214680bf9720fc47ff2c7afe501f6bcd`.
Private struct declarations came from its release source,
[`194af181db3a02a095c01db84e176d972e19b216`](https://github.com/tsukumijima/mpp-rockchip/tree/194af181db3a02a095c01db84e176d972e19b216).
H.265's SoC query used the no-device-tree fallback, not an emulated RK3588.

Every parsed file has one SPS, with `vui_parameters_present_flag=1`,
`video_signal_type_present_flag=1`, `colour_description_present_flag=1`,
`video_full_range_flag=0`, and `colour_primaries=1`:

| Header input | H.264 transfer / matrix | H.265 transfer / matrix |
|---|---|---|
| Pre-fix B after A | **1 / 1 (wrong: stale BT.709)** | **1 / 1 (wrong: stale BT.709)** |
| Fixed A | 1 / 1 | 1 / 1 |
| Fixed B (`2:4:7:1`) | **13 / 6** | **13 / 6** |
| Fixed return to A | 1 / 1 | 1 / 1 |

Retained B artifacts:

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| H.264 SPS | 31 | `26851d20bd3e65cf03d52de6ee91e1a940bb3fd70958606f8b48aa353bd97075` |
| H.265 VPS/SPS/PPS | 85 | `10d82223f16769f211a3e9d1182a1b4a9af5c8ac4887349936ce7d6c614c1f2c` |

Local raw evidence, probe sources and logs are retained in the ignored
`test-results/u6-srgb/` directory. The initial environment setup needed a
root-capable isolated rootfs and correct temporary-directory permissions; an
ABI-gate attempt also failed DNS until the resolver was mounted. These setup
failures are not RED regression evidence. The actual RED cases are the two
config regressions and the independently parsed stale-B headers above.

**Proof boundary:** these are real MPP-generated parameter-set bytes, but no
encoded picture or muxed stream was produced. This removes the demonstrated
sRGB mapping obstruction and verifies SPS serialization, **not** hardware
encoding, header delivery across a live switch, receiver continuity, pixel
accuracy, or U6's ten cycles. A candidate package must still be deployed by the
authorized hardware lane and its received muxed output parsed per active leg.
U6 remains uncredited; no board, PR, tag or release operation was performed.

Both upstream and downstream `GstForceKeyUnit` events request
`MPP_ENC_SET_IDR_FRAME` for the next submitted frame. With
`header-mode=each-idr`, that access unit also carries `GST_BUFFER_FLAG_HEADER`.
The software test backend verifies the control call and parses an H.264 type-5
NAL from a `videotestsrc` pipeline. Real H.264/H.265 bitstream and SPS VUI
verification with `ffprobe -show_streams` remains the board-gated todo 36; this
contract does not claim that hardware result.

The encoders are **PTS-only (no B-frames); DTS = PTS**. Every output frame sets
DTS explicitly rather than relying on an unset or inherited timestamp.
