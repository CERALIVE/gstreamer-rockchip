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
R0 release `1.10.1+ceralive.1`, with URLs and SHA-256 pins in `ci/mpp-pin.env`.
The runtime retains `librga.so.2` and provides `librga2 (= 2.2.0)`. The plugin
package therefore keeps its `librga2` virtual dependency, also allowing the
legacy Radxa runtime for rollback; it does not depend on the provider's new name.

**R0 compatibility blocker:** the published runtime requires `libc6 (>= 2.38)`.
It cannot install on Debian Bookworm (libc6 2.36), so the required Bookworm build
gate currently blocks this pin. Trixie satisfies that dependency; this does not
waive Bookworm compatibility or authorize a forced install.

```bash
bash ci/install-build-deps.sh
meson setup build --prefix=/usr \
  -Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled
meson compile -C build
meson test -C build --print-errorlogs
```

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
side-by-side PbP presets, and raw custom pad rectangles, with per-pad alpha and
z-order. A two-input frame uses one primary copy/scale and one geometry-aware
librga composite pass; a lone primary is passed through without an RGA or CPU
pixel operation. Both factories remain discoverable without hardware, but
NULL→READY fails with a typed error when `/dev/rga` does not pass the shared
driver-version trial.

The H.264/H.265 encoder sinks accept linear NV12 `video/x-raw(memory:DMABuf)`
alongside their existing plain raw caps. This lets the compositor negotiate
through a queue/tee into the encoder's existing FD-import path without a CPU
converter or feature-stripping adapter. Host regressions cover all six presets
and both codecs; real-source composition endurance and per-frame FD identity
still require board qualification. The separate ordinary 1080p30 PLAYING failure
remains unresolved in the [runtime contract](docs/ENCODER-RUNTIME-CONTRACT.md).

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

The strict d5 matrix records four PASS cells, five expected U3 chroma failures,
and three rotation submission failures. Same-kernel A/B reproduces all three
rotations identically on baseline: they are pre-existing, tracked separately,
and not waived as U3. The owner authorizes `1.14.4+ceralive.3` with these explicit
limitations; this is not a claim of complete board qualification. See the
[board results](tests/board/DRILL-RESULTS.md#2026-09-08--u1-fixation-candidate-orange-pi-5-partial)
for the tested packages, PSNR cells, and proof limits.

## License

This project is free software under the **GNU Lesser General Public License,
version 2.1**. See [`COPYING`](COPYING). CeraLive modifications remain under the
same license; upstream copyright and license notices are preserved.
