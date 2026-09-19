# RGA ↔ MPP interaction

How this plugin set's librga calls sit alongside Rockchip MPP's own DMA-BUF and
allocation contract, and what each element does when the 2D path is unavailable.

This is a description of the code in this tree, not a specification of RGA. Every
claim below names the function that implements it, so a reader can check the
claim rather than trust it. Where a property has not been measured on hardware,
it is marked as unmeasured instead of asserted.

## Scope

| Covered | Not covered |
|---|---|
| The userspace half: what the plugin asks librga and MPP to do | The kernel driver. `multi_rga` ownership, IOMMU domains and per-plane validation live in the island repository |
| Availability, demotion, counters, and refusal semantics | Throughput, latency and thermal behaviour — unmeasured here |
| The 8-bit raster formats the elements actually name | 10-bit paths, which are out of RGA scope in this fork |

## Two call paths, one backend

The tree issues 2D work through two different librga APIs, and they deliberately
share a single availability decision and a single health table:

| Caller | librga entry point | Operation tag |
|---|---|---|
| `mpph264enc` / `mpph265enc` / `mppjpegenc` input conversion | im2d `improcess`; legacy `c_RkRgaBlit` only under `GST_MPP_RGA_LEGACY_BLIT=1` | `encode-convert` |
| `mppvideodec` output conversion | im2d `improcess`; legacy `c_RkRgaBlit` only under `GST_MPP_RGA_LEGACY_BLIT=1` | `decode-convert` |
| `mppjpegdec` output conversion | im2d `improcess`; legacy `c_RkRgaBlit` only under `GST_MPP_RGA_LEGACY_BLIT=1` | `jpeg-convert` |
| `rgaconvert` | im2d `improcess` | `rgaconvert` |
| `rgacompositor` primary copy/scale | im2d `improcess` | `rgacompositor-copy` |
| `rgacompositor` secondary BGRA pre-scale, when needed | im2d `improcess` | `rgacompositor-copy` (separate BGRA/BGRA health tuple) |
| `rgacompositor` secondary blend | geometry-aware im2d `improcess` composite | `rgacompositor` |

Both DSOs carry the im2d path: `GST_MPP_RGA_ENABLE_IM2D` is set in `config.h`
([`meson.build:93`](../meson.build)), not only in `rockchiprga`'s own `c_args`,
so `libgstrockchipmpp.so` reaches `improcess` too. That is checkable on a
shipped artifact rather than inferred from the build files —
`nm -D --undefined-only libgstrockchipmpp.so` lists `improcess` and
`imsetColorSpace` alongside the retained `c_RkRgaBlit` rollback, and does NOT
list `improcessOpt`, which is resolved through the explicit module handle.

`gstmpprgabackend.c` owns both. `gst_mpp_rga_backend_blit()` and
`gst_mpp_rga_backend_process()` / `gst_mpp_rga_backend_composite()` differ only
in which op they invoke; they enter through the same
`gst_mpp_rga_backend_begin()` and leave through the same
`gst_mpp_rga_backend_finish()`, so availability, tuple health, and `ENODEV`
handling behave identically whichever API ran. The composite helper uses
`improcess` because librga 2.2.0's C `imcomposite` macro has no rectangle
arguments; `improcess` is the geometry-bearing primitive behind the same blend
mode. Its primary source can scale, but its pattern input cannot. For NV12
output, librga requires the NV12 accumulator on the source/dst channels and the
BGRA overlay on the `pat` channel.

The two APIs differ in one respect that matters to a caller: the legacy blit
reports success as `ret >= 0`, while `improcess` returns an `IM_STATUS` whose
success value is strictly positive. `gst_mpp_rga_backend_process()` therefore
passes `ret > 0` where `gst_mpp_rga_backend_blit()` passes `ret >= 0`. Treating
im2d's zero as success would score `IM_STATUS_NOERROR`'s neighbours as clean
conversions.

The MPP plugin compiles without the im2d operation; `rockchiprga` enables it in
its own DSO via `-DGST_MPP_RGA_ENABLE_IM2D`. Both link the same sources.

## Compositor pattern contract

[EXISTS] `pat` is the second input image (hardware `src1`), and `prect` selects
its source crop; it is not a destination canvas or an instruction to scale that
image into `drect`. Larger backing storage is legal, but its **active crop** must
have exactly the destination rectangle's width and height. Merely shrinking
`prect` crops the secondary instead of shrinking the whole picture.

Primary sources:

- [R0 `rga_check_blend`, lines 1097–1128](https://github.com/CERALIVE/librga/blob/f4c3ee62ab354c2cbe22718f543fc0ba6e58365c/im2d_api/src/im2d_impl.cpp#L1097-L1128):
  explicit `src1 don't support scale` check, comparing both active axes and
  returning `IM_STATUS_NOT_SUPPORTED` on mismatch.
- [R1 `rga_check_blend`, lines 1157–1193](https://github.com/CERALIVE/librga/blob/57a1067a246c71fa6c9a355d1668884fda155dd5/im2d_api/src/im2d_impl.cpp#L1157-L1193):
  the same pattern-size restriction. `rga_task_submit` applies src/dst/pat
  rectangles before calling `rga_check`, before blit submission.
- [Rockchip FAQ A2.12](https://github.com/CERALIVE/librga/blob/57a1067a246c71fa6c9a355d1668884fda155dd5/docs/Rockchip_FAQ_RGA_EN.md#L1138-L1154):
  YUV output uses YUV background on `src`, RGB overlay on `src1/pat`, and
  `DST_OVER` to put the overlay above the background; blending happens in RGB.

`gst_rga_compositor_aggregate_frames()` therefore pre-scales the **entire** BGRA
secondary through the ordinary two-buffer im2d path when its dimensions differ
from its target. It then blends the target-sized pattern with equal-sized active
src/dst rectangles. The intermediate pool uses the existing 16-aligned DMA-heap
allocator and video metadata, is reused between frames, recreated on target-size
changes, and deactivated on stop. `IM_SYNC` keeps reuse safe after the blend.
An already-sized overlay bypasses allocation and scaling. No CPU pixels are
mapped or copied, and either allocation or scale failure drops the frame before
blending instead of producing primary-only output for that failed frame.

### OPi B, 2026-09-16: geometry corrected, composition still blocked

Bounded real-source reproduction: Orange Pi 5 Plus, slot B booted/good, A
inactive/good, kernel `7.2.0-ceralive-rk3588 #ceralive1 @1789515465`, engine
`2026.9.3`, installed plugin `.5`, librga R0 `1.10.1+ceralive.1`. HDMI was
rediscovered as `/dev/video0`, native NV16 3840×2160 at 59.94; BRIO colour as
`/dev/video3`, Device Caps `0x04200001`, YUYV 1920×1080 at 30 fps. A bounded
direct GStreamer graph converted these to NV12/BGRA, composed at 1080p30 and
encoded HEVC. This is not an engine start-lifecycle or endurance receipt.

The instrument-only plugin returned **`improcess=-1`, `errno=0`**, with active
src/dst `864,54,960,540` and active pat `0,0,1920,1080`. `imStrError` preserved:

```text
Unsupported function: Blend mode background layer unsupport non-RGB format,
dst format = 0xa00(nv12)
```

R0's `else if (!dst_isRGB)` at the first source citation rejects an NV12
destination **even with valid RGB pat**, before reaching the pat-size check.
R1 nests that guard under the no-pattern case. Thus the pattern geometry was
independently wrong, but it was not the first validator refusing this board run.
This is a userspace `IM_STATUS`, not a captured ioctl errno or a kernel failure.

The corrected plugin completed its added scale and reached the blend with:

```text
src/dst: size=1920x1080 stride=1920x1088 rect=864,54,960,540
pat:     size=960x540   stride=960x544  rect=0,0,960,540
status=-1 errno=0, same NV12-destination refusal
```

**Verdict: BLOCKED, not composed.** FFprobe decoded 30 HEVC frames emitted before
the blend failure; the inspected final frame was dark with no visible inset.
No 600-second soak or 20 teardown cycles ran. No plan acceptance is discharged.
The plugin was staged under `/tmp` only; the engine service was stopped to release
capture and restored after each run. Installed plugin/library hashes were
unchanged, staging removed, and both slots stayed good. No package, pin, driver,
UART, image or Rock change was made.

### Diagnostic and host-test boundaries

`GST_DEBUG=mpprgabackend:2` records failing composite status, immediately captured
errno, librga error text, surface/stride/crop descriptors, blend flags, alpha and
CSC. It distinguishes imconfig failures, where improcess was never called.
Logging restores errno before the existing device-loss/tuple accounting.

`tests/check/rgacompositor.c` runs geometry cases through the real backend's
wrapping and a **modeled, hardware-independent** improcess contract that checks
rectangle bounds, live FDs, and equal active pat/dst dimensions. It also asserts
whole-source scaling, both single-axis PbP mismatches, all PiP corners/custom,
alpha/zorder, equal-size bypass, aligned allocation, reuse, resize, stop and
failure cleanup. This is not real-library acceptance or pixel emulation.

Failing first: `pat cannot scale: active pat 1920x1080 differs from dst 960x540`.
With the fix, the compositor suite passes. Mutating only production
`pat_rect_width` to `pat_frame.width - 2` makes the same test fail with active
pat `958x540` versus dst `960x540`; the restored code passes. The fake-success
geometry oracle no longer accepts the defect. A separate log-capture test
deliberately clobbers errno during logging and verifies the original status,
errno and failure stage survive.

## im2d colorimetry

### C6b-colour (host implementation; board qualification pending)

This contract supersedes the historical full-CSC/refusal policy below. Build
against **R1 headers, im2d API ≥1.10.5**; the shared backend asserts the version.
That is not a minimum runtime version. It retains an explicit module from
`g_module_open("librga.so.2", G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL)` for the
plugin lifetime and resolves `improcessOpt` with `g_module_symbol`, never as an
undefined ELF dependency. Older runtimes use seven-argument `improcess`, with
interpolation/async disabled and one warning per backend initialization.
`GST_DEBUG=mpprgabackend:4` reports `improcessOpt resolved`; level 6 reports
actual synchronous Opt submissions. MPP encoder/decoder blits use this same
dispatcher, preserving pixel strides, crops, rotations and virtual-address input.
`GST_MPP_RGA_LEGACY_BLIT=1` selects the original blit rollback for one release,
with a warning. Neither route enables CPU copying.

The pure mapper retains complete input/output `GstVideoColorimetry` (range,
matrix, transfer and primaries). `imsetColorSpace` applies the directional enums
on the descriptors, **not** the transform usage bitfield: the namespaces overlap.

| Request | Selection |
|---|---|
| YUV→RGB, source 601 limited/full | `IM_YUV_TO_RGB_BT601_LIMIT/FULL` |
| YUV→RGB, source 709 limited | `IM_YUV_TO_RGB_BT709_LIMIT` |
| RGB→YUV, destination 601 limited/full | `IM_RGB_TO_YUV_BT601_LIMIT/FULL` |
| RGB→YUV, destination 709 limited | `IM_RGB_TO_YUV_BT709_LIMIT` |
| Equal complete YUV colorimetry; RGB→RGB | No CSC |
| 709 full, other/unknown matrix or range, limited RGB, differing YUV colorimetry (including primaries/transfer) | D29 default-matrix fallback; no full-CSC endpoint request |

Unsupported rows remain negotiable. They warn once per element, naming both
colorimetries, and increment read-only `csc-fallback-frames` after successful RGA
conversion (at most once per compositor frame). This does not claim requested
primaries/transfer were converted: RGA is not a gamut/transfer converter.
`conversion-fallback-frames` still counts CPU copies only. Failed/unavailable
submissions do not increment the CSC counter. Encoder input uses negotiated
video info; decoder input uses the MPP frame's ISO colour metadata, retaining
missing metadata as unknown instead of guessing.

`rgaconvert.interpolation` is additive: enum `default`, `linear`, `cubic`,
default `default`. The zeroed `im_opt_t` carries
`version=RGA_CURRENT_API_HEADER_VERSION`; Opt gets `-1`, `NULL`, `&opt` and
synchronous usage. No fences, cache or batching are added. Only
`IM_STATUS_SUCCESS` and `IM_STATUS_NOERROR` succeed. Unsupported/invalid requests
map to `GST_FLOW_NOT_NEGOTIATED`; execution/resource/unknown statuses map to
`GST_FLOW_ERROR`, retaining dropped-frame accounting.

Both suite builds use R1 headers. Bookworm keeps the Radxa runtime and dev link,
extracting **only headers** from the checksum-verified R1 archive into
`/usr/local/include/rga`. No R1 package is installed there and no glibc floor is
changed. Only Trixie is publishable. Build Check retains `plugin-<suite>`
candidate archives for exact-binary board testing.

d5 now uses an extracted candidate, isolated plugin path and fresh registry,
explicit BT.709 YUV caps/reference for every geometry, and the five D24 quality
limits in `tests/board/d5-quality-contract.sh`. Every cell executes. Unexpected
PASS, new FAIL and submission errors fail the drill; rotations are not waived.
The host test proves that removing a known-limit row fails.

**BOARD ROWS: the d5 matrix is now GREEN on both boards; the rest are NOT-RUN.**
All twelve d5 cells PASS against the BT.709 reference with an empty
expected-FAIL list, on Orange Pi 5+ and — as of 2026-09-19, against the same CI
candidate `.deb` (`bd6f0cbe8080400216f870ec355f0d5f622aff486d95e5f168fd3ba3dbd2304d`,
plugin resolved out of `/tmp/.../plugin/libgstrockchiprga.so`, never the
installed package) — on Rock 5B+ (`7.2.0-ceralive-rk3588`), worst chroma
33.72 dB against the unchanged 30 dB floor, `fallback=0 dropped=0
layout_rejections=0` on every cell.

**The R1 lookup and the Opt entry point are now PROVEN IN-ELEMENT on Rock**, out
of that same d5 run rather than from a separate drill: d5 already runs every
cell under `GST_DEBUG=rgaconvert:5,mpprgabackend:6`, so its retained per-cell
logs carry `gst_mpp_rga_resolve_api: improcessOpt resolved` in all 12 of 12
cells and `submitting synchronous improcessOpt` once per cell — one submission
per pushed buffer, 12 of 12 — with zero `improcess` failures. `gst-inspect-1.0
rgaconvert` on the same staged candidate reports `interpolation … Enum
"GstRgaInterpolation" Default: 0, "default"`. Separately, the D6 harness below
drives `improcessOpt` with real release fences at 3840×2160 against the same
runtime, so the Opt path is exercised at 4K as well, though librga-direct rather
than through the element.

**The older-runtime fallback is PROVEN on Rock, as a paired A/B against the R1
leg.** The pinned Radxa R0 `librga.so.2.1.0`
(`0b455344259c37fec821955e2de85bb5f76a34e69682217b514c407d8a35c6c3`, the
`librga2_2.2.0-1_arm64.deb` already byte-pinned in `ci/mpp-pin.env`) is selected
**for one process only** through `LD_LIBRARY_PATH` — `LD_DEBUG=libs` confirms the
loader took that copy — so nothing is installed and the sysext-backed `/usr`
keeps its R1 package, which is the isolated-evidence form this document's own
rule allows. `gst-inspect-1.0 rgaconvert` exits 0 and emits `improcessOpt
unavailable; using seven-argument improcess; interpolation and async disabled`
**exactly once** (count 1, with `improcessOpt resolved` count 0). Running the
same `tests/board/dmabuf-rgaconvert` binary on the same NV16 1280×720 bt709
source through both runtimes:

| Runtime | exit | output bytes | Opt submissions | fallback warning | counters |
|---|---:|---:|---:|---:|---|
| R1 `1.10.5+ceralive.1` (installed) | 0 | 1 382 400 | 1 | 0 | `0/0/0` |
| Radxa R0 `2.2.0-1` (process-local) | 0 | 1 382 400 | 0 | 1 | `0/0/0` |

Both outputs are **byte-identical**
(`a18fbf014d512678aaf38ff2b720463a0a3669c05b382cd78dc0bc10764a498e`), so the
seven-argument fallback is not merely loadable — it converts, and it converts to
the same pixels.

Still outstanding on both boards: BT.709-versus-601-reference PSNR deltas, and
d2 300/300 H.265 and H.264 with zero `RGA_BLIT fail`.

`GST_MPP_RGA_LEGACY_BLIT=1` is a **different switch** from the runtime fallback
above — it selects `c_RkRgaBlit` inside `gst_mpp_rga_real_blit()`, the encoder
and decoder seam, which `rgaconvert` never enters — and it remains unexercised
on hardware. An attempt on Rock did not reach it and is recorded here so the
next attempt does not repeat it: a
`videotestsrc ! video/x-raw,format={I420,NV16},1280x720 ! mpph265enc` pipeline
encodes 60/60 frames in both switch positions, byte-identical output, while the
`mpprgabackend` debug category emits **zero** lines in either leg — so the
encoder served those caps without entering the RGA seam at all, and the switch
had nothing to select. Reaching that seam needs an input the encoder genuinely
must blit; a run whose only evidence is "both legs produced frames" proves
nothing about the rollback. A
`videotestsrc`-fed pipeline is NOT a valid instrument for any of these —
GStreamer 1.22 `videotestsrc` does not honour a downstream DMA-BUF allocation
proposal, so such a pipeline fails to preroll and emits zero submissions, which
reads identically to a broken code path. Use `tests/board/dmabuf-rgaconvert.c`,
as d5 does. Stubs prove no
pixels or silicon. Do not APT-swap libraries on sysext-backed `/usr`: use the
separately approved restoration procedure, or label process-local selection as
isolated evidence, not an installed-runtime restoration.

### C6b-perf: NOT-ADOPTABLE at this pin — the handle path is refused by the driver

**Verdict: BLOCKED. No DMA-BUF handle import cache is implemented, and none can
be while librga R1 and the current island driver disagree about `v_addr`.**

The proposal was to `importbuffer_fd()` once per pool buffer and describe each
frame with `wrapbuffer_handle()`, so the per-job DMA-BUF attach/map is paid once
instead of per frame. Measured on a Rock 5B+ (kernel `7.2.0-ceralive-rk3588`,
`librga2-ceralive 1.10.5+ceralive.1`, RGA api `v1.10.5_[11]`) with
`tests/board/d6-c6b-measurement.sh`, **every handle-described submission failed**
— 440 of 440 at 4K NV16→NV12 and 440 of 440 at 1080p, on three independent runs:

```text
PROBE_rgba_handles src=1325 dst=1326          # import succeeds
PROBE_rgba_handle_copy status=0 errno=22 …    # the SAME buffers, by handle: EINVAL
PROBE_rgba_fd_copy    status=1 errno=0        # the SAME buffers, by fd: success
```

The RGBA 256×256 single-plane control is what makes the cause unambiguous: the
same two DMA-BUFs, the same geometry and the same `improcess()` call succeed when
described by fd and fail when described by handle, so the refusal is the handle
mechanism itself and not a chroma-plane, stride or format question.

Root cause, in two places that are each individually defensible:

- librga R1 `generate_blit_req()`
  ([`im2d_api/src/im2d_impl.cpp:3498-3502` at `1.10.5+ceralive.1`](https://github.com/CERALIVE/librga/blob/d57bc86e65b331948953442449618cadd3b0c7bc/im2d_api/src/im2d_impl.cpp#L3498-L3502))
  passes the handle as `yrgb_addr` and then derives the other two plane
  addresses from the *virtual* base pointer, which on the handle path is `NULL`:
  `uv_addr = (uintptr_t) srcBuf` is `0`, but
  `v_addr = (uintptr_t) srcBuf + srcVirW * srcVirH` is a non-zero integer that is
  not a handle. For 1920×1080 it is exactly `2073600`, which is what the driver
  then reports.
- The island's `rga_job_judgment_support_core()` validates the RGA2 capability
  of **all nine** `{src,dst,pat}.{yrgb,uv,v}_addr` fields whenever
  `handle_flag & 1`, skipping only fields that are zero. The stray `v_addr`
  therefore reaches `rga_mm_lookup_rga2_support()`, misses the handle table and
  returns `-EINVAL` before the job is ever scheduled:

```text
rga: This handle[2073600] is illegal.
rga: ID[2594]: task[0] job_commit failed.
rga: ID[2594]: request commit failed!
rga: ID[2594]: submit failed!
```

Neither side is obviously the defect: leaving a garbage `v_addr` in a
handle-mode request is a librga bug, and validating every non-zero address field
is the island being strict about a buffer it is about to program into hardware.
Fixing it is a change to one of those two repositories, not to this plugin, so it
is recorded here and not worked around. **Do not "fix" this in the plugin by
zeroing plane addresses behind librga's back** — the plugin does not own the
`rga_req` that `generate_blit_req()` builds.

What the cache would have been worth, if the path worked, is not left unstated.
The explicit import/release ioctl pair — the same dma_buf attach + map + page
array construction the fd path performs inside every job — costs **833 µs per
buffer at 4K** and **226 µs at 1080p**, against a whole-frame `fd_sync` time of
4859 µs and 1321 µs. Two buffers per frame therefore plausibly account for a
third of 4K frame time. That is an inference from an adjacent measurement, not a
measurement of the cache, and it is exactly why this stays BLOCKED rather than
NO-MEASURABLE-GAIN: the proposal is not refuted, it is unrunnable.

### C6b-async: measured on one board, gate met there, second board queued

**Verdict: ADOPT-RECOMMENDED on Rock 5B+; Orange Pi 5 Plus leg NOT RUN.** The
default stays synchronous regardless, so nothing in this claim changes shipped
behaviour.

Depth-1 pipelining — submit frame N with `IM_ASYNC`, retire frame N-1's release
fence while N is in flight — measured against the synchronous path on the same
buffers, in the same process, in one run:

| Geometry | sync fps | async depth-1 fps | gain | gate |
|---|---:|---:|---:|---|
| 3840×2160 NV16→NV12 | 207.0 | 243.8 | **+17.8 %** | ≥ 5 % |
| 1920×1080 NV16→NV12 | 758.7 | 971.8 | **+28.1 %** | ≥ 5 % |

Three independent runs of the same harness on the same board agree closely —
4K `+17.5 / +17.1 / +17.8 %`, 1080p `+26.0 / +28.7 / +28.1 %` — so the figure is
not a single lucky sample.

Two things about the method are load-bearing rather than incidental. The async
frames **alternate between two destination buffers** (`ASYNC_DST_BUFFERS=2` in
the transcript): at depth 1 two jobs are in flight at once and a real
`rgaconvert` would draw each output from a pool, so a single shared destination
would both model a shape the element cannot produce and let the hardware overlap
two writes to one allocation. The first two runs above used a single destination
and the third used two; the gain is unchanged, so it is not an artefact of that
overlap. And each run is bracketed by two synchronous measurements, one before
every other mode and one after every other mode — at 4K they read 4843 µs and
4831 µs per frame — so no clock or thermal drift large enough to explain the
async gain occurred across the run.

Latency does not regress by more than the contract allows: per-call p95 rises
from 4943 µs to 5331 µs at 4K, `+388 µs`, which is well inside one 60 fps frame
period (16 667 µs) — the frame is retired one submission later by construction,
not held longer.

The Orange Pi 5 Plus leg is **not run**: the board was held by another session
for the whole of this work. A one-board result does not satisfy the both-board
adoption rule, so this is recorded as a measurement, not as an adoption, and the
element ships no async property until the second board agrees.

### Historical pre-C6b implementation and evidence

[EXISTS] Both `improcess()` submission paths set `rga_buffer_t.color_space_mode`
after `wrapbuffer_fd()`. The API was checked against the SHA-pinned
`librga-dev_2.2.0-1_arm64.deb`, retained as the Bookworm compatibility pair
in `ci/mpp-pin.env`: its `rga/im2d_version.h`
identifies **1.10.1_[4]**, not API 2.2.0. `rga/im2d_type.h` defines the buffer
field and `IM_COLOR_SPACE_MODE`; `rga/im2d_single.h` declares the seven-argument
C `improcess()`. CSC is a buffer attribute, **not a usage flag**.
Trixie now builds against the paired CeraLive R1 `1.10.5+ceralive.1` assets.
The [build matrix boundary](../README.md#build) distinguishes legacy-source
portability from R0/R1 binary compatibility; neither lane proves hardware CSC.

`rgaconvert` reuses the input/output `GstVideoInfo` snapshots parsed in
`gst_rga_convert_set_caps()`, including the existing DMA_DRM caps path.
`rgacompositor` reuses the aggregator's output info and pad-info snapshots.
No extra caps query is performed. Matrix and range come from the **YUV source**
for YUV→RGB and the **YUV destination** for RGB→YUV; RGB's matrix is not a
choice between BT.601 and BT.709.

For conversion to/from full-range RGB, the ordinary directional modes are set
on the **destination buffer**, including YUV→RGB:

| YUV `GstVideoColorimetry.matrix` | `range` | YUV→RGB destination mode | RGB→YUV destination mode |
|---|---|---|---|
| `GST_VIDEO_COLOR_MATRIX_BT601` | `GST_VIDEO_COLOR_RANGE_16_235` | `IM_YUV_TO_RGB_BT601_LIMIT` | `IM_RGB_TO_YUV_BT601_LIMIT` |
| `GST_VIDEO_COLOR_MATRIX_BT601` | `GST_VIDEO_COLOR_RANGE_0_255` | `IM_YUV_TO_RGB_BT601_FULL` | `IM_RGB_TO_YUV_BT601_FULL` |
| `GST_VIDEO_COLOR_MATRIX_BT709` | `GST_VIDEO_COLOR_RANGE_16_235` | `IM_YUV_TO_RGB_BT709_LIMIT` | `IM_RGB_TO_YUV_BT709_LIMIT` |
| `GST_VIDEO_COLOR_MATRIX_BT709` | `GST_VIDEO_COLOR_RANGE_0_255` | Full-CSC endpoint pair, below | Full-CSC endpoint pair, below |

There is **no** `IM_YUV_TO_RGB_BT709_FULL` or `IM_RGB_TO_YUV_BT709_FULL`
directional enum in this header. BT.709 full range instead sets **both** buffer
endpoints: `IM_YUV_BT709_FULL_RANGE` on the YUV buffer and `IM_RGB_FULL` on RGB.
A genuine YUV matrix/range change similarly uses the corresponding endpoint
modes: `IM_YUV_BT601_LIMIT_RANGE`, `IM_YUV_BT601_FULL_RANGE`,
`IM_YUV_BT709_LIMIT_RANGE`, or `IM_YUV_BT709_FULL_RANGE`. These operations
require librga/hardware full-CSC support; a hardware refusal remains a typed
negotiation failure, never permission to fall back to a different matrix.
Ordinary modes are preferred where possible to avoid imposing that capability
requirement on basic BT.601/BT.709-limited conversions.

When both endpoints have the same color family, matrix and range, both modes
remain `IM_COLOR_SPACE_DEFAULT` (zero). NV12 stride changes, NV16→NV12 chroma
reshuffles, and RGB channel-order changes therefore do not request CSC. A
negotiated change of YUV matrix/range is a real color conversion even if the
pixel-format name remains NV12.

The compositor's NV12 + BGRA → NV12 blend is different from an NV12 copy:
librga blends in RGB internally. Its output buffer carries
`IM_YUV_TO_RGB_* | IM_RGB_TO_YUV_*`, using the accumulator/output's colorimetry
for both directions. The preceding primary copy/scale is configured separately;
when its colorimetry is unchanged it requests no CSC. A lone-primary passthrough
still performs no hardware operation. BT.709-full blending is refused: the
pinned API has no directional pair for it, and the full-CSC endpoint pair does
not encode the two-direction blend. Limited-range RGB and unsupported YUV
matrices (for example BT.2020) are also refused when CSC is required.

### Unspecified caps are already resolved by GStreamer

The U1 fixation change preserves an explicit input colorimetry when
the selected raw output format remains in the same YUV/RGB family and the
output has no colorimetry field. Identity alternatives retain it in both pad
directions. A YUV matrix is not copied into RGB caps, and explicit output
colorimetry is never overwritten. This avoids an accidental matrix change
during YUV subsampling or resizing; it does not implement a requested CSC.
Explicit BT.709 output from the OPi's `2:4:7:1` input still fails with librga's
`Not support full csc mode [300]`: a genuine CSC capability limitation, not
metadata loss or a regression. It is deferred to convergence todo 49, gated
on librga R1 `1.10.5+ceralive.1`, and is owner-approved as a documented
limitation for `.3`. The OPi proof, separate pre-existing rotation failures,
and release disposition are recorded in the 2026-09-08 section of
`tests/board/DRILL-RESULTS.md`.

`gst_video_info_from_caps()` and `gst_video_info_set_format()` populate the
defaults through GStreamer's private `set_default_colorimetry()` helper:
YUV height **≤576** selects BT.601 limited; height **>576** selects BT.709
limited. RGB defaults to full-range RGB/sRGB at every size. The backend consumes
those populated values rather than implementing its own threshold or assuming
librga's default. For scaling across the SD/HD boundary with both sides untagged,
the independently negotiated YUV endpoints can therefore require a matrix change.
Explicit caps override these defaults.

Source: GStreamer [1.22 video-info.c](https://github.com/GStreamer/gstreamer/blob/1.22.0/subprojects/gst-plugins-base/gst-libs/gst/video/video-info.c)
and [1.26 video-info.c](https://github.com/GStreamer/gstreamer/blob/1.26.0/subprojects/gst-plugins-base/gst-libs/gst/video/video-info.c).
This is a matrix/range correction only: RGA does not implement transfer-function
conversion, gamut/primaries conversion, or HDR tone mapping here. The legacy MPP
`c_RkRgaBlit` path is unchanged by this im2d fix.

### Proof boundary

`tests/check/rgaconvert.c` exercises negotiated caps through the real backend
wrapping into an intercepted `improcess()`, asserting BT.601/BT.709 limited/full
in both directions, SD/HD default selection, same-color-space no-CSC, genuine
YUV matrix/range changes, and unsupported-mode refusal. The compositor tests
assert the copy and blend descriptors separately and retain the existing
zero-operation passthrough test. These are hardware-independent call-contract
tests, not pixel-quality measurements.

The d5 software oracle now uses each untagged YUV endpoint's resolution-based
default, matching the DUT rather than the pre-fix implicit librga BT.601 mode.
Its 40 dB threshold is unchanged. **The updated d5 has not run on a board**;
the historical six failing cells in `tests/board/DRILL-RESULTS.md` remain
historical evidence, not a claimed pass for this fix.

## The driver-version-probed initialization sequence

`gst_mpp_rga_backend_init()` runs once per process, behind a `GOnce` in
`gst_mpp_rga_backend_get_default()`. In order:

1. **Open** `/dev/rga` with `O_RDWR | O_CLOEXEC`. A failure here records `errno`
   and leaves the backend unavailable.
2. **Ask the driver its version** with `RGA_IOC_GET_DRVIER_VERSION`. The
   misspelling is the kernel's and is preserved deliberately — it is the ABI
   name, not a typo to fix.
3. **Close the fd** and classify. The probe succeeds on a **non-negative** ioctl
   return, not on zero: the island's handler returns a positive `true` on a
   successful copy, so a `ret == 0` test would reject the very driver this fork
   targets while its version payload sat correctly filled in.
4. **Enforce the floor.** `gst_mpp_rga_version_supported()` requires ≥ 1.2.4.
   Below that, the backend stays unavailable and logs the version it saw.
5. **Only then** set `available`, and only then call `c_RkRgaInit()`. Its result
   is logged and otherwise ignored — it is a compatibility call, never an
    availability test.

The ordering is the whole point. `c_RkRgaInit()` can succeed against a device
this plugin cannot drive, so treating it as the availability oracle is what
produced the historical "RGA is fine" reports on boards where every blit failed.
This sequence submits no trial composite and checks no pixels. A successful
driver probe is not evidence that a later format/geometry tuple will compose.

Two escape hatches sit outside this sequence. `GST_MPP_NO_RGA=1` refuses RGA
before the backend is consulted at all (`gst_mpp_use_rga()`), and
`GST_MPP_ALLOW_CPU_COPY=1` re-enables the debug copy described below. Neither is
a production setting.

librga's own public headers do not export the multi-RGA version ioctl ABI, so
`gstmpprgabackend.h` carries the 28-byte `rga_version_t`-compatible layout and
the `_IOR('r', 1, …)` request locally. That local definition is an ABI mirror: it
must track the kernel, not diverge from it.

## Where librga meets MPP's allocation contract

RGA never allocates the frames it operates on. Every conversion in this tree is
"MPP (or a dma-heap pool) owns the buffer, RGA borrows its file descriptor for
the duration of one job". Four consequences follow, and each is enforced rather
than assumed.

### The fd is borrowed, and only a single-memory, zero-offset buffer qualifies

`gst_mpp_rga_convert()` takes the input fd only when the buffer holds exactly
one memory, that memory is DMA-BUF, and its offset is zero. Anything else falls
back to mapping the buffer and handing RGA a virtual address — and on the
encoder path that is already the degraded case.

`rgaconvert` and `rgacompositor` are stricter, because `wrapbuffer_fd()` has
**no plane-offset argument**. A direct import is honest only for a single
zero-offset DMA-BUF whose `GstVideoMeta` describes the standard linear layout:
two related planes for NV12 or one packed plane for BGRA. Multi-fd and offset
layouts that cannot be represented are counted as `layout-rejections` and
refused, never silently flattened into a wrong-looking frame.

### Strides cross the boundary in different units

MPP reports `hor_stride` in **bytes**; RGA's rect wants it in **pixels**. The
translation is one division by the format's `pixel_stride0`, in
`gst_mpp_rga_info_from_mpp_frame()`. That single expression is the entire subject
of the unresolved Main10 stride question that `d3-main10-stride-ab.sh`
investigates; it is report-only and no stride edit follows from static
confidence. Do not "simplify" it.

### RGA's own geometry rules are checked before submission

`gst_mpp_set_rga_info()` refuses rather than rounds: YUV rects are truncated to
even width and height, an **odd vertical stride is rejected outright**, and a
descriptor carrying neither an fd nor a virtual address is rejected. A rotation
that is not 0/90/180/270 is a layout rejection, not a silent no-rotation.

### Compressed and offset MPP output never reaches RGA

The decoder refuses to hand RGA an AFBC or RFBC frame, or one carrying a non-zero
MPP `offset_x`/`offset_y` (`gst_mpp_dec_get_gst_buffer()`). Those are recorded as
`layout-rejections`. Cropping is different: a `GstVideoCropMeta` is pushed *into*
the RGA source rect and the meta is removed once the blit succeeds, so the crop
is performed by the hardware rather than carried downstream.

### Pool ownership: `rgaconvert` prefers not to own the output

`gst_rga_convert_decide_allocation()` scans the downstream pools and, if any is
DMA-BUF backed, moves it into the selected slot — so `rgaconvert ! mpph265enc`
writes straight into the encoder's own MPP pool with no intermediate. Only when
no downstream DMA-BUF pool exists does the element create its own dma-heap pool,
`system-uncached` first and `system` as fallback.

Its pools round width and height up to 16 (`GST_RGA_DMA_HEAP_ALIGNMENT`), and
`propose_allocation` publishes that padding as `GstVideoAlignment` so upstream
allocates a buffer RGA can address. This is distinct from, and must not be
confused with, the MPP encoder's own 1080→1088 alignment, which remains its own
runtime contract.

`rgacompositor` always selects that same shared dma-heap allocator for its
output pool. It cannot write through a system-memory allocation and has no
debug staging path. The one-input case is different: when only `sink_0` has a
frame and its NV12 caps match the output, `create_output_buffer` returns a
reference to that input buffer, so neither a pool allocation nor an RGA pass is
needed for that frame.

## Demotion semantics

Health is tracked per **(operation, input format, output format)** tuple, in
`gstmpprgatuple.c`:

- Eight consecutive failures on a tuple demote **that tuple only**
  (`GST_MPP_RGA_DEMOTION_THRESHOLD`). Every other tuple keeps using RGA.
- Any success clears that tuple's consecutive-failure count.
- A demoted tuple skips eight requests and then receives exactly one recovery
  trial (`GST_MPP_RGA_RETRY_INTERVAL`). A trial success restores it. This is what
  prevents per-frame hammering of a path that is failing while still allowing
  recovery without restarting the process.
- **No ordinary blit failure changes process-wide availability.** Only two things
  do: a failed probe at init, and `ENODEV` observed on a job — the device
  disappearing underneath a running pipeline.

The deliberate consequence: a board on which NV24 conversion fails still encodes
NV12 at full speed, because the failing tuple is quarantined rather than the
device.

## What each element does when RGA cannot serve

This is the part that differs per element, and the differences are intentional.

### `mpph26xenc` / `mppjpegenc` — the only CPU-copy site in the tree

`gst_mpp_enc_convert()` attempts the RGA blit, and on failure evaluates whether
a CPU copy could even be correct. It cannot be when a rotation was requested or
when the input and output formats differ — a `gst_video_frame_copy()` performs
neither — so those cases go straight to a typed
`GST_ELEMENT_ERROR (CORE, NEGOTIATION)` reading `no 2D converter available for
encode-convert`, and the encoder returns `GST_FLOW_NOT_NEGOTIATED`.

When a copy *would* be correct, it still only runs if `GST_MPP_ALLOW_CPU_COPY=1`
is set exactly. Otherwise the frame is dropped with the same typed error. A
successful copy increments `conversion-fallback-frames`.

This is the **single** `gst_video_frame_copy()` call in the tree, and it is
guarded. `grep -n gst_video_frame_copy gst/rockchipmpp/*.c gst/rockchiprga/*.c`
returning exactly one hit is a standing invariant.

### `mppvideodec` / `mppjpegdec` — the decoder never retries on the CPU

The decoder has **no CPU-copy path at all**, and `GST_MPP_ALLOW_CPU_COPY` does
not reach it. In `gst_mpp_dec_get_gst_buffer()`, if conversion is required and
the RGA attempt does not return `GST_MPP_RGA_SUCCESS`, the decoder:

1. increments `layout-rejections` when the cause was a rejected layout, an
   AFBC/RFBC frame, or a non-zero MPP offset;
2. increments `conversion-dropped-frames` unconditionally;
3. posts `GST_ELEMENT_ERROR (CORE, NEGOTIATION)` naming the operation;
4. sets `task_ret = GST_FLOW_NOT_NEGOTIATED`;
5. unrefs the buffer and returns `NULL` — the frame is dropped.

So on the decode side, `conversion-fallback-frames` is structurally always zero
and `conversion-dropped-frames` is the whole story. A decoder that cannot convert
fails loudly and immediately; it does not degrade into a software converter that
would miss frame deadlines silently.

### `rgaconvert` — refusal at three stages

- **NULL→READY** runs the shared driver-version trial and posts
  `GST_ELEMENT_ERROR (RESOURCE, NOT_FOUND)` if it fails. The factory still
  registers: an absent `/dev/rga` disables activation, never discovery, so a
  graph built on a host without the device fails with a readable reason instead
  of "no such element".
- **Caps** exclude 10-bit formats entirely.
- **Per buffer**, a system-memory input or output is refused with
  `GST_FLOW_NOT_NEGOTIATED` unless `GST_MPP_ALLOW_CPU_COPY=1` permits DMA-BUF
  staging; a staged frame increments `conversion-fallback-frames` once.

### `rgacompositor` — two inputs, no CPU substitute

- **NULL→READY** performs the same trial as `rgaconvert`; failure is
  `GST_ELEMENT_ERROR (RESOURCE, NOT_FOUND)` while the factory remains registered.
- **Caps** admit a progressive NV12 DMA-BUF primary, progressive BGRA DMA-BUF
  overlay, and progressive NV12 DMA-BUF output. A pad or output buffer that
  does not describe one zero-offset linear allocation with the format's
  expected plane count is a layout rejection.
- **Pad count** is capped at two (`sink_0`, `sink_1`). A third request is refused.
- **Per frame**, two inputs issue exactly one background `improcess` and one
  composite operation. A rejected rectangle or backend failure returns
  `GST_FLOW_NOT_NEGOTIATED`; no environment variable enables a CPU substitute.
- **Primary only** returns the original `sink_0` buffer by reference when caps
  match, with zero conversion or composite calls.

## `mppjpegdec` tries MPP's own post-processor first

MJPEG decode does **not** reach for RGA first. `gst_mpp_jpeg_dec_set_format()`
prefers MPP's internal post-processor, and only what the PP cannot do falls
through to RGA. The exact order, as implemented:

1. If the source and destination formats differ, try the PP via
   `gst_mpp_jpeg_dec_try_pp_convert()`, which issues
   `MPP_DEC_SET_OUTPUT_FORMAT` and returns the format MPP accepted.
2. The PP is attempted in two situations only: when the source format is
   `UNKNOWN`, where PP conversion is **required** and a refusal is a hard
   `unsupported video format` error; and when the destination geometry equals the
   source geometry — that is, **no scaling is requested**.
3. If the PP accepted a format, that becomes the decoder's source format, and
   the residual difference RGA must close shrinks or vanishes.
4. Only if a difference in format *or* geometry survives is `convert` set, and at
   that point an unavailable RGA is a typed
   `no 2D converter available for jpeg-convert` negotiation error.

The PP's own format list is fixed in `gst_mpp_jpeg_dec_pp_formats[]`: NV12,
RGB16, BGR16, and the eight 8888 orderings. A scaling request always leaves the
PP path, because step 2 gates on equal geometry — which is why a scaling MJPEG
graph is an RGA consumer and a same-size format change usually is not.

## Format matrix

Rows are `(element, in-format, out-format, backend)`. **Every verdict cell is a
placeholder.** These are filled by the hardware-measurement todo, from
`d5-rgaconvert-matrix.sh` output and board transcripts — not from reading this
tree, and not from RGA documentation.

The Expectation column *is* derived from this tree, and says only what the code
does: `mapped` means the format resolves to a real `RgaSURF_FORMAT` and a
conversion will be attempted; `refused` means the code declines before any job is
submitted, and names why.

| Element | In | Out | Backend | Expectation from the code | Measured verdict |
|---|---|---|---|---|---|
| `rgaconvert` | NV12 | NV16 | im2d | mapped | *not yet measured* |
| `rgaconvert` | NV16 | NV12 | im2d | mapped | *not yet measured* |
| `rgaconvert` | NV12 | NV12 | im2d | mapped (scale/crop/rotate only) | *not yet measured* |
| `rgaconvert` | BGR | NV12 | im2d | mapped | *not yet measured* |
| `rgaconvert` | RGB16 | NV12 | im2d | mapped, with the 565 byte-order swap below | *not yet measured* |
| `rgaconvert` | BGRA | NV12 | im2d | mapped | *not yet measured* |
| `rgaconvert` | YUY2 | NV12 | im2d | mapped | *not yet measured* |
| `rgaconvert` | NV24 | any | im2d | **refused** — not in the element's caps | n/a, by construction |
| `rgaconvert` | NV12_10LE40 / P010 | any | im2d | **refused** at caps — 10-bit is out of scope | n/a, by construction |
| `rgacompositor` | NV12 + BGRA DMA-BUF | NV12 DMA-BUF | im2d | primary copy/scale + optional BGRA pre-scale + three-channel blend | *R0 blend blocked; see board finding above* |
| `rgacompositor` | NV12 DMA-BUF × 1 | NV12 DMA-BUF | passthrough | matching `sink_0` buffer forwarded by reference | *not yet measured* |
| `rgacompositor` | system memory / wrong pad-role format | NV12 DMA-BUF | none | **refused** at caps; no CPU path | n/a, by construction |
| `mpph26xenc` | NV16 | NV12 | legacy blit | mapped | *not yet measured* |
| `mpph26xenc` | BGR | NV12 | legacy blit | mapped | *not yet measured* |
| `mpph26xenc` | RGB16 | NV12 | legacy blit | mapped; MPP's own RGB16 is not trusted, see below | *not yet measured* |
| `mpph26xenc` | NV24 | NV12 | legacy blit | **refused** — `NV24` has no RGA format, so the descriptor is a layout rejection | n/a, by construction |
| `mppvideodec` | NV12 | NV12 | legacy blit | mapped (scale/crop/rotate only) | *not yet measured* |
| `mppvideodec` | NV12 (AFBC/RFBC) | any | legacy blit | **refused** — compressed frames never reach RGA | n/a, by construction |
| `mppvideodec` | any, non-zero MPP offset | any | legacy blit | **refused** — layout rejection | n/a, by construction |
| `mppjpegdec` | MJPEG → NV12, same size | NV12 | MPP PP | PP first; RGA not reached | *not yet measured* |
| `mppjpegdec` | MJPEG → NV12, scaled | NV12 | legacy blit | PP declines on geometry, RGA closes the difference | *not yet measured* |

Two entries in the table need their reason stated once rather than rediscovered:

- **`NV24` and `Y444` are mapped to MPP formats but to no RGA format.** The
  format table in `gstmpp.c` records `UNKNOWN` for their RGA column, so any
  conversion naming them is rejected at descriptor construction. This is a
  property of the format table, not a runtime failure, and it will not demote a
  tuple because no job is ever submitted.
- **`RGB16`/`BGR16` are swapped on purpose.** The table maps GStreamer `RGB16`
  to `BGR_565` and `BGR16` to `RGB_565`, because the RGA3 kernel driver swaps
  them; the comment in `gstmpp.c` records that MPP's own 565 handling ignores
  `MPP_FRAME_FMT_LE_MASK`, which is why the encoder routes 565 through RGA at
  all. Reverse either half of this and the colours invert.

## Counters

Three read-only `guint64` properties on `mpph264enc`, `mpph265enc`,
`mppjpegenc`, `mppvideodec`, `mppjpegdec`, `rgaconvert`, and `rgacompositor`, all
implemented once in `gstmppconversionstats.c`:

| Property | Increments when |
|---|---|
| `conversion-fallback-frames` | A frame took a CPU-copy or DMA-BUF-staging path. **Zero in production**, because both paths require `GST_MPP_ALLOW_CPU_COPY=1`. |
| `conversion-dropped-frames` | A frame was dropped because no 2D converter could serve it. |
| `layout-rejections` | A descriptor could not be expressed to RGA — unrepresentable planes, compressed frames, MPP offsets, illegal rotation. |

Each element also emits a `conversion summary: fallback=… dropped=…
layout-rejections=…` line at `GST_DEBUG` when it returns to NULL. The board
drills read that line rather than the property, because a `gst-launch` graph has
no handle on the element after it stops — and they treat a **missing** summary
line as a failure, since reading zero out of an absent line would score a build
with no counters as a clean run.

## Diagnosing on a board

```sh
# Did the backend actually come up, and against which driver version?
GST_DEBUG=mpprgabackend:4 gst-inspect-1.0 rgaconvert >/dev/null
GST_DEBUG=mpprgabackend:4 gst-inspect-1.0 rgacompositor >/dev/null

# Per-run conversion accounting for an encode graph.
GST_DEBUG=mppenc:5 gst-launch-1.0 … 2>&1 | grep 'conversion summary'

# Prove a refusal is the typed one and not a crash.
GST_MPP_NO_RGA=1 gst-launch-1.0 … 2>&1 | grep 'no 2D converter available'
```

A `conversion-dropped-frames` that climbs while `layout-rejections` stays flat
points at blit failures and tuple demotion; the reverse points at descriptors the
plugin refused to build. `RGA tuple demoted after 8 consecutive failures` names
the exact operation and format pair.

## Related

- [`AGENTS.md`](../AGENTS.md) — frozen contracts, the factory table, and the
  board-drill contract.
- [`tests/board/DRILL-RESULTS.md`](../tests/board/DRILL-RESULTS.md) — executed
  verdicts, and the `NOT-RUN` rows this document's matrix defers to.
- [`docs/fix-audit.md`](fix-audit.md) — per-fix evidence ledger.
