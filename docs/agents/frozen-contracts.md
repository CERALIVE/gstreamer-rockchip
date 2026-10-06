<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

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
  contract is `DTS = PTS`. The read-only `encoder-restarts` counter is additive,
  and so is the read-only `kernel-faults` counter the fault bridge publishes.
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
  `librga-ceralive-dev` and `librga2-ceralive` R1 `1.10.5+ceralive.1` assets from
  `CERALIVE/librga` in `ci/mpp-pin.env`; `ci/install-build-deps.sh` installs both.
  The runtime owns `librga.so.2` and provides `librga2 (= 2.2.0)`, so the plugin's
  `Depends: librga2` remains unchanged. The staged package contract accepts only
  `librga2-ceralive` (with its virtual Provides) or legacy Radxa `librga2` as the
  SONAME owner. Build Check runs the provider regressions and real staged package
  contract on both suites.
  **Suite boundary:** published R0/R1 require `libc6 (>= 2.38)`;
  they cannot run on Bookworm's glibc 2.36.
  Build Check and Publish Release set `RGA_COMPAT_SUITE=bookworm` only for that leg's
  installer, selecting the prior SHA-pinned Radxa `librga2`/`librga-dev`
  `2.2.0-1` pair. The installer rejects that selection outside Debian Bookworm.
  Trixie uses released R1; the default headers/runtime pins move together. Both required
  legs retain every test and the staged provider contract. Bookworm green means
  plugin portability on GStreamer 1.22, **not R0/R1-on-Bookworm support**. The
  `ci/rga-suite-pins.test.sh` gate pins both pairs and rejects unknown selectors.
  No Bookworm librga build is planned: our APT publishes Trixie only, so the
  legacy pair is the permanent Bookworm input (see **Release publishing
  policy**). Do not force-install R0/R1, lower their dependency floor or drop the
  compatibility leg.
- **C6b im2d color conversion:** complete colorimetry is retained from video
  info; directional CSC is applied with `imsetColorSpace`, never OR-ed into
  transform usage. Unexpressible rows use D29's default-matrix fallback, a
  one-time warning and `csc-fallback-frames`, distinct from CPU-copy accounting.
  Only SUCCESS/NOERROR succeed; other statuses reach typed flow failures.
  MPP blits use synchronous im2d; `GST_MPP_RGA_LEGACY_BLIT=1` retains rollback.
  Build against ≥1.10.5 headers, retain the explicit GModule handle and resolve
  Opt dynamically, never link it. See `docs/RGA-MPP-INTERACTION.md`; both-board
  qualification remains NOT-RUN.
- **C6b-perf is implemented, default-off and hardware-unqualified.**
  `GST_MPP_RGA_HANDLE_CACHE=1` at element construction enables rgaconvert's
  fd-keyed, identity-checked LRU (32 total imports, including in-flight leases).
  A retained duplicate fd pins the inode; each lookup checks device/inode/size
  and import geometry, so recycled fd numbers never identify old storage.
  Entries retain GstMemory, not GstBuffer/pool references. Pending/ready/
  quarantine records own leases until terminal completion; never evict a lease
  to satisfy the bound. Import failure or saturation uses FD mode for BOTH
  channels. Stop/dispose drains frames before releasing every import once.
  Cache logic stays in `gstrgaconvert.c`; backend fields only transport borrowed
  handles. librga #25 fixed the handle-plane defect on main, NOT in the pinned
  released R1 archive. Opt-in validation requires those fixed library bytes.
  Rock's direct-API +48.9%/+66.7% measurements justify implementation, not
  in-element or both-board adoption. See the C6b-perf contract below.
- **C6b-async is ADOPTED on both boards, and it ships DEFAULT-OFF.** Depth-1
  `IM_ASYNC` pipelining cleared the ≥5 % gate on Rock (+17.8 % 4K / +28.1 %
  1080p) and on Orange Pi (+18.4 % / +28.0 %, reproduced +18.4 % / +28.1 % on a
  second run), so `rgaconvert` carries an `async-depth` property (uint, `0`-`1`,
  **default `0`**). The default is 0 because the measurement's subject is the
  standalone `tests/board/d6-c6b-measurement.sh` im2d harness, not the element:
  no in-element board measurement exists yet, and depth 1 costs one frame of
  latency. Flipping the default needs an in-element measurement, not another
  harness run. `tests/board/d6-c6b-measurement.sh` remains the sole source of
  these numbers.
- **Async fence lifetime is terminal-state-owned.** Pending and ready records
  retain both input and output; a 100 ms timeout drops the frame logically,
  increments `conversion-dropped-frames`, and disables async until the next
  start. The record remains quarantined until its fence is terminal. FLUSH_START
  forwards immediately without waiting, while other serialized events drain
  older output before the parent's event handler. The two-second error stays;
  after a separate five-second stop deadline, unresolved frame references move
  to an element-independent reaper with a counted error. Only sole-owned,
  terminal frames are released, even after element finalization. Truly stuck
  fences remain retained forever; this bounds waiting, not aggregate memory
  across faulting sessions. `GST_DEBUG=rgaconvert:2` exposes the cumulative
  `quarantine-escapes`/outstanding counts and 30-second retention reminders.
  The property stays zero with a warning when Opt is unavailable. Debug staging
  remains synchronous; a rejected submission remains a typed failure, not an
  unreported synchronous retry. See `docs/ASYNC-FENCE-LIFETIME.md` for the
  unsignalled-fence tests and process-isolated Rock fault-injection boundary.
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
  | `interpolation` | enum `GstRgaInterpolation` | `default` | `default`, `linear`, `cubic`; older runtimes use default with a warning |
  | `crop-x` / `crop-y` | uint | `0` | `0`–`G_MAXUINT`, input crop origin |
  | `crop-w` / `crop-h` | uint | `0` | `0`–`G_MAXUINT`; **zero means "the remaining extent"**, not "an empty crop" |
  | `async-depth` | uint | `0` | `0`–`1`; `0` submits synchronously (the shipped behaviour), `1` enables depth-1 `IM_ASYNC` pipelining and adds one frame of latency |

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

  **Missing-primary intervals are skipped, not fatal.** `create_output_buffer`
  returns OK/NULL when no usable primary covers the current interval, before
  allocating DMA memory. GstVideoAggregator advances time without rendering or
  pushing, so a later primary can start normally. Do not substitute NEED_DATA
  from `aggregate_frames`: it retries the same interval and can spin. Do not
  return OK with an allocated, unwritten surface either. Failed latency queries
  alone do not force a zero deadline; queue readiness and video-frame selection
  are distinct. The Rock BRIO-primary/Osmo-secondary startup repair, host RED/GREEN
  and same-run output controls are recorded in
  [`docs/notes/compositor-primary-startup.md`](../notes/compositor-primary-startup.md).
  Permanent primary absence still yields no output; secondary-only fallback is
  not implemented by this rule.

  **Output color selection [EXISTS].** `find_best_format` selects `sink_0`'s
  NV12 video info before the parent constructs preferred caps. The generic base
  selector compares plain-memory possible caps with DMA-BUF downstream caps and
  can fall back to BT.601 even for a BT.709 primary. Do not copy only geometry
  from the primary after that fallback. The BGRA overlay is not output-color
  authority. Keep the parent's downstream-alternative negotiation; the regression
  uses unconstrained output caps rather than pre-forcing the expected color.

  **Allocation-query ownership [EXISTS].**
  `gst_query_parse_nth_allocation_pool()` returns an owned reference, including
  during inspection and reordering. `rgaconvert` must release every parsed pool,
  whether selected or rejected, after installing replacement query references.
  A NULL pool remains a valid proposal. Returning DMA buffers at stop does not
  prove the pool/config/allocator objects were freed. Weak-reference regressions
  and the OPi allocation profile are recorded in
  [`docs/notes/allocation-pool-lifetime.md`](../notes/allocation-pool-lifetime.md).

  **Compositor allocator lifetime [EXISTS].** Its allocation callback finishes
  with the configured DMA-BUF pool and explicit 16-byte allocation alignment.
  Do not re-enter the generic video-aggregator allocation callback: its allocator
  alignment loop leaks a transfer-full parser reference on the tested GStreamer
  1.22/1.26 versions. The regression independently weak-watches the compositor
  and its allocator through real composed-buffer teardown. This is separate from
  the converter pool-reference fix and from the RSS acceptance criterion.

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

