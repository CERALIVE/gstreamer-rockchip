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
| `mpph264enc` / `mpph265enc` / `mppjpegenc` input conversion | legacy `c_RkRgaBlit` | `encode-convert` |
| `mppvideodec` output conversion | legacy `c_RkRgaBlit` | `decode-convert` |
| `mppjpegdec` output conversion | legacy `c_RkRgaBlit` | `jpeg-convert` |
| `rgaconvert` | im2d `improcess` | `rgaconvert` |
| `rgacompositor` primary copy/scale | im2d `improcess` | `rgacompositor-copy` |
| `rgacompositor` secondary blend | geometry-aware im2d `improcess` composite | `rgacompositor` |

`gstmpprgabackend.c` owns both. `gst_mpp_rga_backend_blit()` and
`gst_mpp_rga_backend_process()` / `gst_mpp_rga_backend_composite()` differ only
in which op they invoke; they enter through the same
`gst_mpp_rga_backend_begin()` and leave through the same
`gst_mpp_rga_backend_finish()`, so availability, tuple health, and `ENODEV`
handling behave identically whichever API ran. The composite helper uses
`improcess` because librga 2.2.0's C `imcomposite` macro has no rectangle
arguments; `improcess` is the geometry-bearing primitive behind the same blend
mode and keeps scale, placement, and alpha in one hardware pass. For NV12
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

## The trial-verified initialization sequence

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
| `rgacompositor` | NV12 + BGRA DMA-BUF | NV12 DMA-BUF | im2d | one primary copy/scale + one three-channel composite pass | *not yet measured* |
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
