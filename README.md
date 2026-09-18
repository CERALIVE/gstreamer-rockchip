# CeraLive gstreamer-rockchip

GStreamer plugins for Rockchip MPP hardware encode/decode on RK3588 devices.
This public CeraLive fork maintains and validates the H.264/H.265 encoder and
decoder paths used by the CeraLive streaming stack, and extends the upstream
nine-factory plugin set to eleven with two first-party librga elements.

## Maintainer notice

This fork is maintained by **CeraLive** at
<https://github.com/CERALIVE/gstreamer-rockchip>. Issues and pull requests for the
CeraLive package belong there, not on an upstream project. The Debian package is
`gstreamer1.0-rockchip-ceralive`; its upstream-style release series begins at
`1.14.4+ceralive.1`.

The fork keeps the plugin filename `libgstrockchipmpp.so` and replaces the
historical `gstreamer1.0-rockchip1` and `belabox-gstreamer1.0-rockchip` packages.
Its four engine-critical elements are `mpph264enc`, `mpph265enc`,
`mppvideodec`, and `mppjpegdec`. Five additional upstream factories remain part
of the package and registration contract; `rgaconvert` and `rgacompositor` add
the two first-party librga factories.

**The package version matters:** `1.14.4+ceralive.1` carries **nine** factories,
with neither `rgaconvert` nor `rgacompositor`. This tree adds both elements and
encoder hygiene for `1.14.4+ceralive.2`; check the
[release assets](https://github.com/CERALIVE/gstreamer-rockchip/releases) for
package availability. The release is being pulled forward to unblock board
validation, not to claim that a device image has passed it. The earlier d5
matrix recorded six failing quality cells, and the negotiated-colorimetry fix
still needs its hardware rerun. See
[`tests/board/DRILL-RESULTS.md`](tests/board/DRILL-RESULTS.md) for the actual
hardware evidence and remaining limits.

## Build

The RK3588 build requires GStreamer development headers, Rockchip MPP, librga,
libdrm, and X11 development files. The repository's CI scripts install the pinned
MPP/RGA development packages used by the device contract. RGA uses the paired
`librga-ceralive-dev` and `librga2-ceralive` packages from the CeraLive librga
R1 release `1.10.5+ceralive.1`, with URLs and SHA-256 pins in `ci/mpp-pin.env`.
The runtime retains `librga.so.2` and provides `librga2 (= 2.2.0)`. The plugin
package therefore keeps its `librga2` virtual dependency, also allowing the
legacy Radxa runtime for rollback; it does not depend on the provider's new name.

The two required Build Check lanes deliberately use different RGA inputs:

| Build environment | RGA runtime / headers | What a green result proves |
|---|---|---|
| Bookworm / GStreamer 1.22 / arm64 | R1 header-only overlay; Radxa `librga2` / dev link `2.2.0-1` | GStreamer 1.22 portability and older-runtime fallback; **not R1 runtime support on Bookworm**. |
| Trixie / GStreamer 1.26 / arm64 | CeraLive R1 `1.10.5+ceralive.1` | Plugin build and tests with the pinned RGA configuration; not board qualification. |

Only Bookworm's dependency-install step sets `RGA_COMPAT_SUITE=bookworm`.
The installer verifies the actual distro before using the compatibility pair;
unknown selectors fail. With no selector, `ci/mpp-pin.env` selects R1. Neither
lane is optional, and their failures still fail `Build Check summary`. The
installer extracts only R1 headers on Bookworm, never installing R1 packages.

The published **librga R0/R1 dependency** (`librga2-ceralive`, not this plugin
package) requires `libc6 (>= 2.38)`. Bookworm has
glibc 2.36, so lowering `Depends` or forcing installation is not a solution.
This is distinct from the plugin's Trixie release contract, which declares
`libc6 (>= 2.41)`. Retargeting the plugin does not rebuild or alter librga's bytes.
No Bookworm librga build is planned. CeraLive's APT publishes Trixie variants
only, so the Bookworm lane's legacy Radxa pair is a permanent input rather than
a stopgap: it keeps the GStreamer 1.22 build and test coverage alive without
claiming R0/R1-on-Bookworm support. librga's own Bookworm CI jobs compile and test
the source there for the same portability reason and publish nothing.
See [`AGENTS.md`](AGENTS.md#release-publishing-policy).

```bash
bash ci/install-build-deps.sh
meson setup build --prefix=/usr \
  -Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled
meson compile -C build
meson test -C build --print-errorlogs
```

For the Bookworm portability build, export `TARGET_SUITE=bookworm` and prefix
the dependency-install command with `RGA_COMPAT_SUITE=bookworm`. Subsequent
build and test commands are unchanged. Packaging refuses a container whose
actual suite differs from the selected target.

**Production matches Debian 13 Trixie.** `ci/target-suite.env` defaults to
Trixie / glibc 2.41 / GStreamer 1.26. Publish Release gates both suite builds,
then installs each package in a fresh matching container with its selected RGA
runtime. The smoke verifies build-suite metadata, runtime checksums, dependency
closure and plugin registration. Only the Trixie `.deb` and checksum reach the
GitHub release and APT dispatch. The Bookworm package has a `~bookworm` suffix
and stays in an internal `portability-bookworm` artifact; it is not published.
Both install smokes must pass. This preserves source/package portability,
not R0/R1 binary compatibility on Bookworm or hardware qualification.

Build the arm64 Debian package with:

```bash
bash packaging/build-deb.sh
bash packaging/package-contract.sh
```

Build Check also runs `packaging/rga-provider-contract.test.sh` and the staged
contract (`bash packaging/package-contract.sh stage-deb-arm64 dist`) on both
Debian suites. The latter checks the installed SONAME owner and its virtual
dependency relationship, not just the package's declared dependency text.

Hardware-independent tests use the mock MPP seam. RK3588-only acceptance is in
`tests/board/`; those scripts are explicitly gated and record their own verdicts.
See [`AGENTS.md`](AGENTS.md) for the exact proof boundary, frozen contracts, and
contribution rules.

Encoder latency, bounded context recovery, colorimetry/VUI configuration,
forced-IDR handling, and the PTS/DTS contract are documented in
[`docs/ENCODER-RUNTIME-CONTRACT.md`](docs/ENCODER-RUNTIME-CONTRACT.md).

Use the [standalone MPI interposer tests](tests/mpi-interposer/README.md) for
host-only recovery checks. They prove plugin recovery from an injected public
MPI error, not hardware error propagation or item 30's island-knob requirement.
The interposer is excluded from production builds and is never installed.

The encoder also maps sRGB transfer for explicitly tagged YUV input, including
`2:4:7:1` (limited range, BT.601 matrix, sRGB transfer, BT.709 primaries).
This writes IEC 61966-2-1 transfer code 13 to MPP rather than omitting all VUI
color keys. Cold-start and color-only renegotiation regressions run without a
board; they do not qualify hardware output or live-switch continuity.

## RGA conversion safety

**C6b-colour (host implementation; hardware qualification pending):** the shared
backend requires im2d ≥1.10.5 **headers**, resolves `improcessOpt` dynamically,
and loads with an older runtime through seven-argument `improcess` (one warning;
interpolation/async disabled). Encoder/decoder blits use im2d;
`GST_MPP_RGA_LEGACY_BLIT=1` retains their one-release rollback. `rgaconvert`
adds `interpolation=default|linear|cubic`, default `default`. Unsupported CSC
combinations retain D29's default-matrix path, warn once per element and count
`csc-fallback-frames`, separately from CPU copies. The
[C6b contract](docs/RGA-MPP-INTERACTION.md#c6b-colour-host-implementation-board-qualification-pending)
supersedes historical full-CSC/refusal wording below. Both board matrices remain
NOT-RUN. C6b-perf and C6b-async are NOT-STARTED, measurement-gated.

The MPP encoder and decoders treat librga as available only after `/dev/rga`
answers `RGA_IOC_GET_DRVIER_VERSION` with driver version 1.2.4 or newer.
`c_RkRgaInit()` is retained for compatibility but is not an availability test.
Eight consecutive failures demote only the operation and input/output format
tuple that failed; other tuples continue using RGA, and a later trial success
restores the demoted tuple. `ENODEV` is the only blit failure that disables the
backend process-wide.

`conversion-fallback-frames`, `conversion-dropped-frames`, and
`layout-rejections` are read-only counters on the MPP encoder and decoder
elements. Their final values are also emitted at `GST_DEBUG` level when an
element returns to NULL. CPU frame copying is disabled in production; setting
`GST_MPP_ALLOW_CPU_COPY=1` enables that debug-only fallback. `GST_MPP_NO_RGA=1`
continues to force conversion refusal. Without an available 2D path, conversion
fails with `GST_FLOW_NOT_NEGOTIATED` rather than silently copying on the CPU.

The separate `rockchiprga` plugin registers `rgaconvert` and `rgacompositor` at
rank `NONE` for explicit engine selection. `rgaconvert` performs scale, crop,
color conversion, rotation, and flip as one librga `improcess` operation over
DMA-BUF input and output. Negotiated src caps select output geometry, including
the natural width/height swap for 90° and 270° rotation. System-memory staging
remains debug-only behind the same `GST_MPP_ALLOW_CPU_COPY=1` switch.

Color conversion uses the negotiated YUV matrix and range, including GStreamer's
resolution-based defaults when caps omit colorimetry. BT.601 and BT.709 are
explicitly passed to librga; same-color-space format/stride changes do not request
CSC. Unsupported modes fail rather than silently selecting another matrix. See
[`RGA ↔ MPP interaction`](docs/RGA-MPP-INTERACTION.md#im2d-colorimetry) for the
mapping and hardware limitations.

`rgacompositor` accepts at most two progressive DMA-BUF request pads: an NV12
primary and a BGRA overlay, producing NV12. The RGB overlay is required by
librga's NV12-output three-channel blend; `rgaconvert` can normalize a YUV
secondary to BGRA upstream. The compositor offers four corner-PiP presets, two
side-by-side PbP presets, and raw custom pad rectangles. Unconstrained output
colorimetry follows the primary NV12 input, not the generic aggregator's
plain-memory format-selection fallback. Explicit downstream constraints still
participate in negotiation. The [runtime contract](docs/ENCODER-RUNTIME-CONTRACT.md)
records the regression and its bitstream proof boundary. The presets retain
per-pad alpha and z-order. A two-input frame uses one primary copy/scale, a separate hardware BGRA
scale when the secondary is not already target-sized, and one geometry-aware
librga composite pass. The intermediate DMA-BUF pool is reused until its target
dimensions change and released when streaming stops. A lone primary is passed
through without an RGA or CPU pixel operation. Both factories remain discoverable
without hardware, but
NULL→READY fails with a typed error when `/dev/rga` does not pass the shared
driver-version probe. This probe does not execute a trial composite or check pixels.

**PiP is still blocked on librga R0 `1.10.1+ceralive.1`.** The OPi-B repair run
reached the corrected blend request, but R0 returned `IM_STATUS_NOT_SUPPORTED`
with `errno=0` because its validator rejects an NV12 destination even with an RGB
pattern. No decoded inset or composition pass is claimed. The
[pattern contract and board finding](docs/RGA-MPP-INTERACTION.md#compositor-pattern-contract)
separate that library blocker from the repaired plugin geometry. Set
`GST_DEBUG=mpprgabackend:2` to capture raw imconfig/improcess refusals rather than
diagnosing the compositor's generic negotiation message.

The H.264/H.265 encoder sinks accept linear NV12 `video/x-raw(memory:DMABuf)`
alongside their existing plain raw caps. This lets the compositor negotiate
through a queue/tee into the encoder's existing FD-import path without a CPU
converter or feature-stripping adapter. Host regressions cover all six presets
and both codecs; real-source composition endurance and per-frame FD identity
still require board qualification. The separate ordinary 1080p30 PLAYING failure
remains unresolved in the [runtime contract](docs/ENCODER-RUNTIME-CONTRACT.md).

`rgaconvert` releases the owned pool references it reads from allocation queries,
including rejected proposals and reordered pools. This fixes retained host-side
pool metadata across start/stop even when DMA-BUF occupancy returns to baseline.
The [pool lifetime note](docs/notes/allocation-pool-lifetime.md) distinguishes
the proven reference leak from allocator RSS warmup; no heap-trimming workaround
or full acceptance pass is implied.

The compositor also completes allocation negotiation with its own aligned
DMA-BUF pool, without re-entering the generic video-aggregator allocation path.
That parent path retains an extra allocator reference on the tested GStreamer
versions. A composed-buffer lifecycle regression covers allocator destruction
independently of element destruction; the same lifetime note records the
call-site profile, positive controls and the separate RSS acceptance limit.

## Upstream lineage and credits

This repository descends from the Rockchip plugin code through the JeffyCN,
BELABOX, and irlserver trees. CeraLive thanks:

- **Rockchip** and its contributors for the original MPP GStreamer plugins;
- **Jeffy Chen / JeffyCN** for the maintained `gstreamer-rockchip` line and fixes;
- **BELABOX** for carrying and rebasing the downstream plugin tree; and
- **Thomas “datagutt” Lekanger / irlserver** for the streaming-control additions
  inherited at the CeraLive fork point.

The shipped source also contains copyrighted work by **Rockchip Electronics Co.,
Ltd.**, **Collabora Ltd.**, **Igalia**, and **Julien Moutte**. Igalia and Julien
Moutte's notices apply to the `gst/rkximage/` subtree. The machine-readable,
file-scoped attribution is [`packaging/copyright`](packaging/copyright); source
headers remain authoritative.

## Colorimetry fixation and known limitations

The U1 fix preserves omitted same-family colorimetry during raw-format caps
fixation without overriding explicit color-conversion requests. On Orange Pi
5+, the previously failing omitted-colorimetry HDMI case now reaches EOS, but
explicit BT.709 output still fails with librga's `Not support full csc mode
[300]`. This is a genuine CSC capability gap, not a regression; its fix is
deferred to convergence todo 49 after librga R1 `1.10.5+ceralive.1`.

The corrected D24/d5 harness measures all 12 cells PASS on Orange Pi 5+, each
at or above the unchanged 30 dB PSNR threshold. BGR software references now set
`colorimetry=sRGB` on `rawvideoparse` itself, and DMA-BUF evidence markers are
matched even when interleaved with `GST_DEBUG` text. The five stale expected
chroma failures are removed; any below-threshold cell still fails the gate.
Rock's D24 half has **not been run**; the OPi measurements do not qualify Rock.
The earlier four-PASS/five-chroma-failure/three-rotation-failure result and the
owner's `1.14.4+ceralive.3` disposition remain historical evidence. See the
[board results](tests/board/DRILL-RESULTS.md#2026-09-08--u1-fixation-candidate-orange-pi-5-partial)
for the tested packages, PSNR cells, and proof limits.

## License

This project is free software under the **GNU Lesser General Public License,
version 2.1**. See [`COPYING`](COPYING). CeraLive modifications remain under the
same license; upstream copyright and license notices are preserved.
