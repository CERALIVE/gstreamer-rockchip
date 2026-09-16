# gstreamer-rockchip

CeraLive's public fork of the Rockchip MPP GStreamer plugins. The fork is based
on `irlserver/gstreamer-rockchip` at `755aeb9`, preserving the BELABOX and
datagutt streaming-control work, and carries CeraLive's independently reviewed
fix ledger for the RK3588 encode/decode path.

## Role

This repository supplies the RK3588 hardware elements used by `cerastream`:

```text
capture -> rgaconvert -> mpph264enc/mpph265enc -> cerastream transport
primary + secondary -> rgacompositor -> mpph264enc/mpph265enc
compressed input -> mppvideodec/mppjpegdec -> program graph
```

The package remains a complete plugin set. The factory contract contains
**eleven built entries** with no reserved slots — but read the release-state note
below the table before quoting that number as something a device carries:

| # | Factory | Plugin | Registration expectation |
|---|---|---|---|
| 1 | `mpph264enc` | `rockchipmpp` | Registers where `gst_mpp_enc_supported()` finds a VPU. |
| 2 | `mpph265enc` | `rockchipmpp` | Registers where `gst_mpp_enc_supported()` finds a VPU. |
| 3 | `mppvp8enc` | `rockchipmpp` | Registers only on SoCs with a VEPU2 VP8 encoder. **RK3588: EXPECTED-ABSENT.** |
| 4 | `mppjpegenc` | `rockchipmpp` | Registers where `gst_mpp_enc_supported()` finds a VPU. |
| 5 | `mppvideodec` | `rockchipmpp` | Registers unconditionally. |
| 6 | `mppjpegdec` | `rockchipmpp` | Registers unconditionally, rank 257. |
| 7 | `mppvpxalphadecodebin` | `rockchipmpp` | Registers on GStreamer ≥ 1.19. |
| 8 | `kmssrc` | `kmssrc` | Registers unconditionally. |
| 9 | `rkximagesink` | `rkximage` | Registers unconditionally. |
| 10 | `rgaconvert` | `rockchiprga` | Registers unconditionally at rank `NONE`; activation is gated at NULL→READY. |
| 11 | `rgacompositor` | `rockchiprga` | Registers unconditionally at rank `NONE`; activation is gated at NULL→READY. |

`mppvp8enc`'s absence on RK3588 is a **silicon fact, not a registration
failure**: the SoC has no VEPU2 VP8 block, the historical Radxa package behaves
identically on the same board, and `d1-runtime-parity.sh` scores it
EXPECTED-ABSENT rather than failing. Do not "fix" it and do not remove it — it
registers on the SoCs that do carry the block.

CeraLive deeply validates the four MPP factories used by the engine plus
`rgaconvert` and `rgacompositor`; the remaining five retain
build-and-registration coverage.

**RELEASE BOUNDARY — `1.14.4+ceralive.1` carries nine factories.** It predates
both `rockchiprga` elements. This tree adds `rgaconvert`, `rgacompositor` and
encoder hygiene for `1.14.4+ceralive.2`; use the
[published releases](https://github.com/CERALIVE/gstreamer-rockchip/releases)
to establish whether that package is available, not a branch name or CI result.
Publishing the package and pinning it in an image are separate from installing
and qualifying that exact image on hardware.

The `1.14.4+ceralive.3` release scope is the omitted-colorimetry fixation fix,
not complete media-stack qualification. The OPi hardware rerun proves that fix;
explicit BT.709 CSC and the remaining d5 failures are documented limitations
accepted by the owner for this release, not silently passing board gates.
[`tests/board/DRILL-RESULTS.md`](tests/board/DRILL-RESULTS.md) retains the actual
drill verdicts and their limits. A passing build or install smoke does not erase
those limits or establish a working 4K59.94 capture-to-encode stream.

Release through `.github/workflows/publish-release.yml` on `main` only, after
the merged commit's Build Check passes. Dispatch with `release_type=stable`
and `dry_run=false` for publication; `dry_run=true` rehearses without publishing.
The workflow derives the next upstream-style version from existing tags, gates
the build and both suite install smokes, publishes exactly one arm64 `.deb` plus
its `.sha256`, then dispatches `apt-reindex` to `CERALIVE/apt-worker`. Do not
pre-create the tag. Independently download and checksum the release archive
before image pinning, and verify the stable arm64 APT index and package bytes
after reindexing; a successful dispatch alone is not serving proof.

## Repository map

| Area | Location |
|---|---|
| MPP encoder/decoder plugin | `gst/rockchipmpp/` |
| RGA 2D converter/compositor plugin | `gst/rockchiprga/` |
| RGA backend, tuple health, conversion counters | `gst/rockchipmpp/gstmpprga*.c`, `gstmppconversionstats.c` |
| RGA ↔ MPP interaction reference | `docs/RGA-MPP-INTERACTION.md` |
| KMS source | `gst/kmssrc/` |
| Rockchip X11/KMS sink | `gst/rkximage/` |
| Hardware-independent tests | `tests/` |
| Board-gated drills | `tests/board/` |
| Runtime parity goldens | `tests/golden/` |
| Per-fix evidence ledger | `docs/fix-audit.md` |
| Encoder latency/recovery/color/VUI/IDR/DTS contract | `docs/ENCODER-RUNTIME-CONTRACT.md` |
| Debian package contract | `packaging/` |
| Target suite and MPP/RGA pins | `ci/` |

## Commit strategy

Three tiers preserve provenance and reviewability:

1. **Tier (a), ported upstream fixes.** Clean ports use `git cherry-pick -x`,
   preserving the original Author and message. Adapted ports use the adapter's
   authorship and credit the owner plus full source SHA in the message. Never
   squash either form.
2. **Tier (b), first-party bug fixes.** One commit per bug, titled for the defect
   mechanism rather than implementation trivia. Never squash these commits.
3. **Tier (c), CI, packaging, docs, and mechanical work.** These may be squashed
   under the normal CeraLive Rule C convention.

The integration PR contains tier-(a)/(b) history and therefore merges with
**Rebase and merge** or **Create a merge commit**. Squash-merge is forbidden for
that PR because it destroys the provenance the first two tiers exist to retain.
No commit may carry a `Co-authored-by` or AI/tool attribution trailer. A clean
cherry-pick's real upstream Author field is provenance, not a trailer.

## PR-TARGETING

Every PR targets `CERALIVE/gstreamer-rockchip`, never the fork parent. Working
clones retain only the CERALIVE `origin`; do not leave a remote named `upstream`
attached. If a source comparison is required, add a descriptively named,
temporary remote, fetch an explicit ref, verify the expected SHA, and remove the
remote before pushing or opening a PR.

Open the PR explicitly against the CERALIVE repository's canonical `main`
branch:

```bash
gh pr create --repo CERALIVE/gstreamer-rockchip \
  --base main
```

Before handoff, verify the PR URL starts with
`https://github.com/CERALIVE/gstreamer-rockchip/`. A PR carrying tier-(a)/(b)
commits is never self-merged; an independent reviewer must confirm its evidence
and merge method.

## Cherry-pick source registry

The upstream audit is frozen at JeffyCN/mirrors branch `gstreamer-rockchip` tip
`a0d45af504099b4b82f3d3377019a63d357e7cef`. Later JeffyCN work is a new audit,
not an implicit extension of this ledger.

| Source | Resolution in this fork |
|---|---|
| irlserver `755aeb9` | Fork base; includes BELABOX and irlserver-datagutt features. |
| JeffyCN `1ceaf42` | Clean `-x` port: decoder DMA-BUF caps. |
| kelvinlawson `d27ae92` | Clean `-x` port: unmatched-PTS pending-frame bound. |
| JeffyCN `7ffd7f4` | Already an ancestor; regression lock only. |
| JeffyCN `5f45bd4` | Adapted packet-ownership/reset cleanup for this fork's older decoder callback layout. |
| kelvinlawson `892f662` | Selective DMA_DRM negotiation port; linear output only, GStreamer 1.22 preserved. |
| kelvinlawson `b93ecb6` / BoxCloudIRL `3b58acf` | DMA32 and used-path RGA behavior already inherited; regression lock only. |
| JeffyCN `c560118` | Adapted encoder reset output-queue drain. |
| JeffyCN `a910efe` | Ported JPEG input timeout handling, subsequently corrected against pinned MPP timeout semantics. |
| kelvinlawson `44578bd` | Adapted into codec-aware no-output decoder accounting; broad size/PTS heuristics were rejected in review. |
| JeffyCN `31ee8bd` | `SKIP-ALREADY-PRESENT`; stride semantics remain board-gated. |
| radxa-pkg `3ccc1e3` | Rejected: packaging wrapper for already-present `31ee8bd`, no source delta. |
| JeffyCN `973fd0e` | Cherry-picked then reverted after independent review falsified its allocator-order premise. |

The complete red/green, MPP-ABI, hardware-gate, and independent-review record is
`docs/fix-audit.md`; this table is a routing index, not a replacement for it.

## Frozen contracts

The following are compatibility contracts, not cleanup opportunities:

- **Plugin filenames:** `libgstrockchipmpp.so` remains unchanged — the image's
  sysext exclusion globs and its MPP runtime-contract test key on that exact
  name. The RGA converter and compositor ship alongside it as
  `libgstrockchiprga.so`, in the same package and the same plugin directory;
  both names are frozen.
- **Package prefix/name:** Rockchip packages retain the
  `gstreamer1.0-rockchip` prefix; this fork ships
  `gstreamer1.0-rockchip-ceralive` and replaces
  `gstreamer1.0-rockchip1` plus `belabox-gstreamer1.0-rockchip`.
- **Factory set:** the eleven-entry table under **Role** is the contract. All
  eleven factories continue to register on the platforms named in their
  expectation column, with `mppvp8enc` EXPECTED-ABSENT on RK3588. Registering
  fewer factories than the SoC can carry is a defect; registering an unlisted
  factory is a contract change.
- **Encoder properties used by the engine:** `bitrate`, `bitrate-min`,
  `bitrate-max`, `zero-copy-pkt`, `rc-mode`, `qp-max`, `gop`, `width`, and
  `height` keep their names, types, defaults, ranges, and enum nicks. The
  inherited fork spelling is `bitrate`, not the historical Radxa `bps` spelling;
  consumer migration is a separate release prerequisite.
- **Decoder properties:** `mppvideodec` keeps `format`, `width`, and `height`;
  `mppjpegdec` keeps NV12 output. The four used elements remain at least
  `GST_RANK_MARGINAL`; `mppjpegdec` remains rank 257.
- **Caps and allocation:** existing golden caps are additive-only. The MPP
  encoder's DMA-BUF pool, 1080-to-1088 `GstVideoAlignment`, and DMA32 allocator
  request are runtime contracts.
  H.264/H.265 sinks additionally accept linear NV12 `memory:DMABuf`, so the
  DMA-BUF-only compositor can link through a queue and preview tee. Plain caps
  remain supported; caps features and actual buffer backing are distinct.
  `tests/check/enc-dmabuf.c` exercises real pad links and the existing FD-import
  path with CPU copying and RGA conversion disabled. This is host software
  coverage, not item-30 composition or per-frame board FD qualification; see
  `docs/ENCODER-RUNTIME-CONTRACT.md` for the separate 1080p30 finding.
- **Encoder runtime hygiene:** latency follows the tracked pending depth;
  non-timeout MPP failures get at most three context restarts per ten seconds;
  explicit sink colorimetry reaches MPP VUI config without guessing absent
  values; both force-key-unit directions request IDR; and the no-B-frame output
  contract is `DTS = PTS`. The read-only `encoder-restarts` counter is additive.
  The sRGB transfer mapping [EXISTS] preserves `2:4:7:1` as limited-range
  BT.601 matrix / IEC 61966-2-1 transfer / BT.709 primaries. GStreamer transfer
  enum 7 maps to MPP/H.26x code 13, not 7. Host config tests cover cold starts
  and color-only BT.709↔sRGB renegotiation for both codecs; hardware output and
  ten-cycle live-switch acceptance remain separate gates. See the encoder
  runtime contract for unmapped values and the bitstream evidence boundary.
- **RGA conversion:** `/dev/rga` must pass the driver-version ioctl before use;
  librga init alone is never sufficient. Blit health is isolated per operation
  and format pair. MPP/`rgaconvert` CPU staging remains debug-only behind
  `GST_MPP_ALLOW_CPU_COPY=1`; `rgacompositor` has no CPU pixel path at all.
  Normal operation fails negotiation instead. The three read-only conversion
  counters are additive element properties. The build pins matching
  `librga-ceralive-dev` and `librga2-ceralive` R0 `1.10.1+ceralive.1` assets from
  `CERALIVE/librga` in `ci/mpp-pin.env`; `ci/install-build-deps.sh` installs both.
  The runtime owns `librga.so.2` and provides `librga2 (= 2.2.0)`, so the plugin's
  `Depends: librga2` remains unchanged. The staged package contract accepts only
  `librga2-ceralive` (with its virtual Provides) or legacy Radxa `librga2` as the
  SONAME owner. Build Check runs the provider regressions and real staged package
  contract on both suites. R1 pinning waits for an actual R1 release.
  **Suite boundary:** published R0 imports `__isoc23_sscanf` and
  `__isoc23_strtol` at `GLIBC_2.38`; it cannot run on Bookworm's glibc 2.36.
  Build Check explicitly sets `RGA_COMPAT_SUITE=bookworm` only for that leg's
  installer, selecting the prior SHA-pinned Radxa `librga2`/`librga-dev`
  `2.2.0-1` pair. The installer rejects that selection outside Debian Bookworm.
  Trixie keeps R0; default pins and release callers remain R0. Both required
  legs retain every test and the staged provider contract. Bookworm green means
  plugin portability on GStreamer 1.22, **not R0-on-Bookworm support**. The
  `ci/rga-suite-pins.test.sh` gate pins both pairs and rejects unknown selectors.
  Bookworm-built librga packages are feasible but require separate producer
  packaging/publication work; see README's build/release boundary. Do not
  force-install R0, lower its dependency floor or drop the compatibility leg.
- **im2d color conversion:** negotiated `GstVideoInfo` matrix/range selects
  `rga_buffer_t.color_space_mode`; format/stride-only work leaves CSC unset.
  YUV-output composition explicitly requests both CSC directions. Unknown or
  unsupported conversions fail rather than assuming BT.601. Mapping, defaults,
  full-range limitations, and test scope: `docs/RGA-MPP-INTERACTION.md`.
- **Caps fixation [EXISTS]:** same-memory identity alternatives retain
  explicit colorimetry; raw-format fixation restores omitted colorimetry within
  the same YUV/RGB family. Explicit output requests are never overwritten. The
  OPi omitted-colorimetry U1 case passes. Explicit BT.709 remains a known CSC
  limitation deferred to convergence todo 49 after librga R1
  `1.10.5+ceralive.1`. The three d5 rotation failures reproduce identically on
  baseline in the same kernel boot; they are separate from U3 chroma limits.
  The owner authorizes `.3` with these documented limitations. See the
  2026-09-08 evidence and release disposition in `tests/board/DRILL-RESULTS.md`.
- **`rgaconvert` properties used by the engine:** the six transform properties
  keep their names, types, defaults, ranges, and enum/flag nicks. A consumer
  graph names them literally, so a rename is a cross-repository migration.

  | Property | Type | Default | Range / nicks |
  |---|---|---|---|
  | `rotation` | enum `GstRgaRotation` | `0` | nicks `0`, `90`, `180`, `270` — clockwise |
  | `hflip` | boolean | `FALSE` | — |
  | `vflip` | boolean | `FALSE` | — |
  | `core-mask` | flags `GstRgaCoreMask` | `auto` | nicks `auto`, `rga3-core0`, `rga3-core1`, `rga2` |
  | `priority` | int | `0` | `0`–`6` |
  | `crop-x` / `crop-y` | uint | `0` | `0`–`G_MAXUINT`, input crop origin |
  | `crop-w` / `crop-h` | uint | `0` | `0`–`G_MAXUINT`; **zero means "the remaining extent"**, not "an empty crop" |

  `crop-{x,y,w,h}` are four separate properties by design — an operator sets
  only the edges it wants and leaves the rest at the default. Zero width or
  height is therefore load-bearing: it selects the remainder of the input, so
  the default property set is a full-frame no-crop conversion.

  `rgaconvert` also carries the same three read-only counters as the MPP
  elements — `conversion-fallback-frames`, `conversion-dropped-frames`,
  `layout-rejections` — with identical semantics, because all three elements
  share `gstmppconversionstats.c`. Output geometry comes from negotiated src
  caps, including the width/height swap that 90°/270° rotation forces; it is
  never a property. The factory registers at rank `NONE` and never autoplugs;
  activation, not registration, is what a missing `/dev/rga` blocks.

- **`rgacompositor` element and pad properties:** the element is a
  `GstVideoAggregator` with request pads `sink_%u`, capped at `sink_0` and
  `sink_1` in v1. Its per-pad contract is:

  | Property | Type | Default | Range / meaning |
  |---|---|---|---|
  | `xpos` | int | `0` | `0`–`G_MAXINT`; custom-layout left edge |
  | `ypos` | int | `0` | `0`–`G_MAXINT`; custom-layout top edge |
  | `width` | int | `0` | `0`–`G_MAXINT`; custom requires a positive even value |
  | `height` | int | `0` | `0`–`G_MAXINT`; custom requires a positive even value |
  | `alpha` | double | `1.0` | `0.0`–`1.0`; global alpha when the pad is composited |
  | `zorder` | uint | request index | Lower values render first; ties use pad index |

  Element property `layout` is enum `GstRgaCompositorLayout`, default
  `pip-top-right`, with nicks `pip-top-right`, `pip-top-left`,
  `pip-bottom-right`, `pip-bottom-left`, `pbp-left-right`, `pbp-top-bottom`, and
  `custom`. PiP makes `sink_0` the full output and scales `sink_1` to half of
  each output axis (one-quarter area), inset by an even-aligned 5% margin. PbP
  splits the output into even-aligned left/right or top/bottom halves. `custom`
  uses each pad's four geometry properties verbatim and rejects zero, odd, or
  out-of-bounds rectangles.

  `sink_0` and output are progressive NV12 DMA-BUF; `sink_1` is progressive
  BGRA DMA-BUF. The asymmetric input contract is required by librga's
  NV12-output three-channel blend: the NV12 accumulator is the source/dst and
  the RGB overlay is the `pat` channel. Upstream `rgaconvert` supplies BGRA when
  a secondary source starts as YUV. Two inputs run one primary `improcess`
  copy/scale, a BGRA scale when the secondary dimensions differ from its target,
  then one geometry-aware composite pass. `pat` cannot scale: reducing its crop
  alone would discard part of the secondary. The intermediate uses a reusable
  16-aligned DMA-BUF pool, recreated on target-size changes and released at stop;
  equal-size secondaries need no intermediate. Both passes are synchronous and
  keep the intermediate alive through the blend. Output allocation
  uses the same 16-aligned dma-heap allocator as `rgaconvert`. With only
  `sink_0` connected and output caps unchanged, the input buffer is passed
  through by reference with no RGA or CPU pixel operation. The element exposes
  `conversion-fallback-frames`, `conversion-dropped-frames`, and
  `layout-rejections` through `gstmppconversionstats.c`; fallback remains zero
  because no CPU path exists. Its rank and READY failure contract match
  `rgaconvert`.

  **Composition remains hardware-blocked on librga R0.** The instrumented OPi-B
  run returns `improcess=-1` (`IM_STATUS_NOT_SUPPORTED`), `errno=0`, with R0's
  `Blend mode background layer unsupport non-RGB format, dst format = 0xa00(nv12)`.
  This separate userspace guard precedes pat geometry validation; the corrected
  geometry does not fix that library defect. See the source citations and bounded
  board receipt in `docs/RGA-MPP-INTERACTION.md#compositor-pattern-contract`.
  `GST_DEBUG=mpprgabackend:2` preserves composite status, errno, librga error text
  and descriptors, and distinguishes imconfig refusal. The backend is
  **driver-probed**, not trial-composite/pixel-verified. Host geometry tests now
  enforce a documented rectangle contract at the improcess seam and were proven
  RED by mutation; they do not substitute for real-library or decoded-pixel proof.

`tests/parity-check.sh`, `tests/golden/`, `packaging/package-contract.sh`, and
the board drills are the executable authorities. Update a frozen contract only
through an explicit cross-repository migration, never as incidental refactoring.

## Test and board-drill contract

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
[`tests/mpi-interposer/`](tests/mpi-interposer/README.md). Build Check runs its
separate debug-only gate after the existing tests; it is absent from the
production build graph and has no install targets. It injects one pre-submit
`MPP_ERR_STREAM` on an explicitly selected healthy context, never writes the
counter and never changes plugin recovery or libmpp. Its independently decoded
synthetic host-output test is **not** a live engine/session or hardware receipt,
and does **not** discharge item 30's literal island-knob row. The production
async-error propagation gap and H.265 poll-error overwrite are recorded in
[`docs/ENCODER-RUNTIME-CONTRACT.md`](docs/ENCODER-RUNTIME-CONTRACT.md#hardware-error-propagation-gap-partial).
Do not replace that finding with another kernel errno knob or ship this harness.

The board suite is deliberately outside Meson:

| Drill | Hardware claim |
|---|---|
| `d1-runtime-parity.sh` | Package installation, registration of every built factory the board's SoC can carry, four-element golden contract. `mppvp8enc` is scored EXPECTED-ABSENT on an `rk3588` board and PRESENT-REQUIRED elsewhere. |
| `d2-radxa-fork-ab.sh` | Radxa/fork encode A/B over **H.265 primary and H.264 secondary**: 300/300 AUs per codec, SPS geometry/profile/level, zero `RGA_BLIT fail`, and `conversion-fallback-frames = 0` on the fork variant. |
| `d3-main10-stride-ab.sh` | Report-only Main10 current-vs-`*8/pixel_stride0` frame-checksum experiment. |
| `d4-allocation-soak.sh` | 136 s DMA allocation soak with live bitrate, resolution, and temporal-SVC changes, run **on a trial-verified librga backend** and scored on the three conversion counters. |
| `d5-rgaconvert-matrix.sh` | `rgaconvert` conversion matrix — {CSC, scale, crop, rotate} × representative format pairs, each cell measured as PSNR against a software reference of the same operation. |

The latest executed verdicts and their hardware scope are recorded in
[`tests/board/DRILL-RESULTS.md`](tests/board/DRILL-RESULTS.md). That tracked
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

## Licensing and credits

The project remains LGPL-2.1. Keep `COPYING`, source headers, and
`packaging/copyright` intact. Copyright holders represented in the shipped tree
are Rockchip Electronics Co., Ltd.; Collabora Ltd.; Igalia; and Julien Moutte.
Igalia and Julien Moutte are scoped to `gst/rkximage/`, not the MPP plugin;
CERALIVE owns the new RGA backend, tuple-health, and conversion-counter files
and the whole of `gst/rockchiprga/`.

Provenance credits are distinct: Rockchip originated the plugin family, JeffyCN
maintains the audited upstream line, BELABOX rebased and carried the downstream
tree, and irlserver-datagutt added the streaming-control features inherited by
this fork. See `README.md` for the public maintainer notice.

## Pre-commit formatting

`hooks/pre-commit.hook` runs GNU indent 2.2.12 with the upstream parameter set
against the index contents of every C/H file under `gst/rockchipmpp/`. The
encoder source and header intentionally retain inherited CRLF line endings;
GNU indent 2.2.12 misparses those carriage returns as input, producing
misleading unmatched-`else`, statement-nesting, and unexpected-EOF errors.
The hook strips CR only in its temporary checker input, so it can validate
those files without changing their stored line endings. It also leaves the
working tree untouched when a style diff is found.

The two genuinely non-compliant LF files (`gstmppallocator.c` and
`gstmpph265enc.c`) are kept at the hook's two-pass output. The encoder's
failure was introduced by commit `dd3ce32c` restoring inherited CRLF; the
underlying C was valid and the original file passed GNU indent before that
line-ending-only change.

## Anti-patterns

- Do not rename `libgstrockchipmpp.so`, `libgstrockchiprga.so`, or the package
  prefix, and do not split the RGA elements into a second `.deb`: the release
  publishes exactly one archive.
- Do not remove unused factories to reduce the package.
- Do not extend `rgacompositor` beyond two sink pads or add a CPU compositor;
  v1 is deliberately one primary plus one secondary on librga.
- Do not treat `mppvp8enc`'s absence on RK3588 as a bug to fix or as a drill
  failure to suppress. It is silicon, it reproduces on the Radxa package, and
  d1 scores it explicitly.
- Do not rename `bitrate` back to `bps` or add a legacy alias here.
- Do not change Main10 stride semantics on static-analysis confidence alone.
- Do not treat plugin registration success as proof all factories registered;
  `plugin_init` historically swallows individual registration failures.
- Do not claim sanitizer coverage that the qemu-user environment cannot run.
- Do not let a board drill install a package without recording package, kernel,
  and final verdict, and do not infer PASS from a command merely completing.
- Do not run the pre-commit hook casually: it checks the whole MPP subtree and
  is intentionally baseline-wide. Review `git status` immediately if it runs.
- Preserve mixed line endings in inherited files; avoid text-mode whole-file
  rewrites and compare raw versus whitespace-ignored diffs.
