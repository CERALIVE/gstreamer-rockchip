# RK3588 board-drill results

This file records the latest executed hardware verdicts for the board suite. It
preserves the same outcomes as the retained raw transcripts; a command finishing
or a failure later being classified as pre-existing does not turn that drill
into a pass.

## 2026-09-19 — C6b: d5 green on Rock, perf BLOCKED, async one-board

Rock 5B+ `ceralive2`, `7.2.0-ceralive-rk3588`, `librga2-ceralive
1.10.5+ceralive.1`, RGA api `v1.10.5_[11]`. The plugin under test is the CI
candidate `.deb` from this branch's Build Check run
(`bd6f0cbe8080400216f870ec355f0d5f622aff486d95e5f168fd3ba3dbd2304d`), staged
under `/tmp` and resolved through a private registry; `gst-inspect-1.0` confirms
`Filename` inside that staging directory, so the installed package was never the
subject.

| Drill | Verdict | Recorded finding |
|---|---|---|
| d5 rgaconvert matrix | **PASS** | All 12 cells PASS against the BT.709 reference with an empty expected-FAIL list. Worst chroma 33.72 dB (`rotate-BGR-to-NV12`) against the unchanged 30 dB floor; `fallback=0 dropped=0 layout_rejections=0` on every cell. With the existing Orange Pi 5+ result this makes the matrix green on **both** boards. |
| R1 `improcessOpt` lookup + Opt entry point, in-element | **PASS** | Read out of the d5 logs above, which already run at `mpprgabackend:6`: `improcessOpt resolved` in 12/12 cells and `submitting synchronous improcessOpt` once per cell, 12/12, with zero `improcess` failures. `gst-inspect-1.0 rgaconvert` reports `interpolation … Default: 0, "default"`. |
| Older-Radxa-runtime fallback | **PASS** | Pinned R0 `librga.so.2.1.0` selected for one process via `LD_LIBRARY_PATH` (confirmed by `LD_DEBUG=libs`); nothing installed. `gst-inspect-1.0 rgaconvert` exit 0 with the fallback warning emitted **exactly once** and `improcessOpt resolved` absent. The same `dmabuf-rgaconvert` binary and the same NV16 720p source convert on both runtimes with `0/0/0` counters and **byte-identical** output `a18fbf01…`; R1 takes 1 Opt submission, R0 takes 0 and warns once. |
| d6 C6b-perf gate | **BLOCKED** | Handle-described `improcess()` fails 440/440 at both 4K and 1080p, on three independent runs. An RGBA 256×256 single-plane control on the *same* buffers passes by fd and fails by handle, so the refusal is the handle mechanism, not format or geometry. Driver reports `This handle[2073600] is illegal` — librga R1 leaves a non-handle `v_addr` in handle mode and the island validates every non-zero address field. Not a plugin defect and not fixable here. |
| d6 C6b-async gate | **ADOPT-RECOMMENDED (one board)** | Depth-1 `IM_ASYNC`: 4K 207.0→243.8 fps (**+17.8 %**), 1080p 758.7→971.8 fps (**+28.1 %**) against a ≥5 % gate; p95 per-call latency +388 µs at 4K, far inside one 60 fps frame period. Three runs agree: 4K +17.5/+17.1/+17.8 %, 1080p +26.0/+28.7/+28.1 %. Frames alternate between two destination buffers, as a pooled element would, so the gain is not a shared-buffer overlap artefact; the two bracketing synchronous runs agree to within 1 %, so it is not drift either. |

**The Orange Pi 5+ leg of d6 was NOT RUN in this session** — the board was held
by another session throughout. It ran on 2026-09-19 and is recorded below.
Nothing was installed, no engine session ran, no capture device was opened, and
every fence was polled to a terminal state before its buffers were released, so
the known `rga_job_commit` use-after-free path was not exercised.

## 2026-09-19 — d6 C6b second board, Orange Pi 5 Plus: async ADOPTED, perf still BLOCKED

Orange Pi 5 Plus (`Xunlong Orange Pi 5 Plus`, machine-id
`e661616a6cf24ca48cec5843124efc0f`, `7.2.0-ceralive-rk3588`, `librga2-ceralive
1.10.5+ceralive.1`, `librockchip-mpp1 1.5.0-1`,
`gstreamer1.0-rockchip-ceralive 1.14.4+ceralive.6`), reached at the literal
`192.168.78.150` under the canonical board lock. **Nothing was installed and no
element was instantiated**: d6 is a standalone im2d harness against the board's
own librga runtime.

| Drill | Verdict | Recorded finding |
|---|---|---|
| d6 C6b-async gate | **ADOPT-RECOMMENDED** | Depth-1 `IM_ASYNC`: 4K 202.2→239.4 fps (**+18.4 %**), 1080p 755.4→967.2 fps (**+28.0 %**) against a ≥5 % gate. A second independent run agrees to within 0.1 pp: **+18.4 %** / **+28.1 %**. `ASYNC_DST_BUFFERS=2` in both runs, i.e. the corrected two-destination shape. The two bracketing synchronous measurements agree to within 0.8 % (4K 4985 µs before every other mode, 4945 µs after), so no clock or thermal drift explains the gain. |
| d6 C6b-perf gate | **BLOCKED** | Reproduced independently on the second board: 440/440 handle-described submissions fail at both geometries across both runs, with the RGBA single-plane fd control passing (`status=1`) and the handle control failing (`status=0 errno=22`) on the *same* buffers. The kernel printed `This handle[2073600] is illegal` — byte-for-byte the value Rock produced, and `2073600` is `1920 × 1080`. Same librga/island disagreement, not a new finding and not fixable in this plugin. |

With Rock's result this satisfies the **both-board adoption rule for C6b-async**,
so `rgaconvert` now carries an `async-depth` property — **default `0`**, i.e.
shipped behaviour is unchanged. The gate's subject is this harness, not the
element; an in-element board measurement is what a default flip would need.
C6b-perf remains BLOCKED on both boards.

Board hygiene after both runs: remote scratch removed, zero
`KASAN|BUG:|use-after-free|Oops` lines in `dmesg`, no failed systemd units, SoC
44.4 °C, unit states unchanged from before the drill, board left idle.

**Harness build note.** The local `localhost/gstrk-trixie-arm64` image carried
the legacy Radxa `librga-dev 2.2.0-1` (im2d api 1.10.1), which declares no
`improcessOpt`, so `c6b-im2d-bench.c` failed to compile against it. The run used
a derived image with the SHA-pinned R1 `librga-ceralive-dev`/`librga2-ceralive`
`1.10.5+ceralive.1` pair from `ci/mpp-pin.env` installed on top — the same inputs
`ci/install-build-deps.sh` selects on Trixie. Nothing on the board changed.

## 2026-09-16 — PiP pattern repair, Orange Pi B: BLOCKED on librga R0

The bounded HDMI + BRIO run captured real `improcess=-1`, `errno=0` and
`imStrError`: `Blend mode background layer unsupport non-RGB format, dst format
= 0xa00(nv12)`. R0 rejects the destination before validating pattern size.
The plugin's separate full-size-pattern/inset defect was repaired with a
whole-image BGRA pre-scale. On the same board, that scale succeeds and the blend
receives equal active 960×540 pattern/destination dimensions, but R0 still
returns the same error. Thirty pre-failure HEVC frames decoded; the final one
has no visible inset. **No composition pass.**

The [full scoped receipt and source citations](../../docs/RGA-MPP-INTERACTION.md#compositor-pattern-contract)
record the exact kernel/packages, temporary-plugin procedure, diagnostic and
mutation evidence. Engine service restored, installed libraries unchanged,
temporary staging removed, B good and rollback A untouched/good. Neither the
600-second soak nor the 20-cycle teardown drill ran; items 30/31 remain open.

## 2026-09-01 — mainline 7.2 track

The suite ran on the reachable Rock 5B+ named `ceralive2`, running
`7.2.0-ceralive-rk3588`, with
`gstreamer1.0-rockchip-ceralive 1.14.4+ceralive.1 arm64` installed.

| Drill | Verdict | Recorded finding |
|---|---|---|
| d1 runtime parity/registration | **FAIL** | `mppvp8enc` did not register, so the all-nine criterion failed. Triage reproduced the same absence with the historical Radxa package on the same board: **PRE-EXISTING**. |
| d2 Radxa/fork A/B | **FAIL** | Both variants delivered 300/300 H.264 and H.265 access units with zero element errors, but both recorded nonzero RGA entry/error counts. The identical Radxa/fork result was triaged **PRE-EXISTING**. |
| d3 Main10 stride A/B | **INCONCLUSIVE** | The software reference decoded 10/10 frames; both hardware variants produced zero frames with negotiation errors, so neither stride formula matched the clean checksum oracle. No stride source changed. |
| d4 136-second allocation soak | **FAIL** | Repeated failure to open the vendor RGA interface stopped the run after the 30-second and 60-second transitions, before the full window completed. The same mainline/userspace RGA mismatch was triaged **PRE-EXISTING**. |

The d1, d2, and d4 triage classifications describe regression provenance only;
their acceptance criteria still failed and their verdicts remain **FAIL**. d3
remains **INCONCLUSIVE** and does not authorize either stride implementation.

## Criteria revision — the librga backend and `rgaconvert`

The acceptance criteria for d1, d2 and d4 were reworked, and d5 was added, after
the trial-verified librga backend and the `rgaconvert` element landed. **A
criteria change does not carry a verdict forward.** Every row below is therefore
**NOT-RUN**: the results in the section above were produced by the previous
criteria and say nothing about the current ones.

| Drill | Verdict under the current criteria | What the run must establish |
|---|---|---|
| d1 runtime parity/registration | **NOT-RUN** | Ten board-required factories register, including both `rgaconvert` and `rgacompositor`, and `mppvp8enc` matches its per-SoC expectation: EXPECTED-ABSENT on an `rk3588` board, PRESENT-REQUIRED elsewhere. |
| d2 Radxa/fork A/B | **NOT-RUN** | H.265 primary and H.264 secondary each deliver 300/300 access units, zero `RGA_BLIT fail` journal lines, and `conversion-fallback-frames = 0` from the fork encoder's own counter summary. |
| d3 Main10 stride A/B | **INCONCLUSIVE**, carried forward unchanged | Criteria unchanged; see the verification note below. |
| d4 136-second allocation soak | **NOT-RUN** | The full 136 s window on a trial-verified librga backend, four applied changes, no pipeline errors, and all three conversion counters zero. |
| d5 `rgaconvert` conversion matrix | **NOT-RUN** | Twelve cells — {CSC, scale, crop, rotate} × {NV12→NV16, NV16→NV12, BGR→NV12} — each at or above the PSNR floor against its software reference, with no cell leaving the silicon path. |

### d5 cell matrix

Twelve cells, no verdicts. These are filled by a board that actually executes
`d5-rgaconvert-matrix.sh`; the script writes the same table to `matrix.tsv` in
its report directory. Recording a measured dB here without a transcript behind
it is the one thing this file exists to prevent.

| Operation | NV12 → NV16 | NV16 → NV12 | BGR → NV12 |
|---|---|---|---|
| CSC (1280×720) | NOT-RUN | NOT-RUN | NOT-RUN |
| Scale (→ 640×480) | NOT-RUN | NOT-RUN | NOT-RUN |
| Crop (→ 1024×576) | NOT-RUN | NOT-RUN | NOT-RUN |
| Rotate 90° (→ 720×1280) | NOT-RUN | NOT-RUN | NOT-RUN |

The default PSNR floor is 30 dB, overridable through `D5_PSNR_MIN`. Luma and
chroma are scored separately and the cell takes the worse of the two; a cell
that cannot negotiate the silicon path fails outright rather than being scored.

### d3 verification note

d3 was re-examined against this revision and is **unaffected**, deliberately
rather than by omission. It patches a single stride expression in
`gst/rockchipmpp/gstmpp.c`, which still occurs exactly once and was last touched
by the upstream `31ee8bd8` port, not by any RGA work. Its subject is 10-bit
Main10 decode through `mppvideodec`, and 10-bit formats are outside the RGA
scope entirely — `rgaconvert` refuses them at caps. Its scratch build now also
compiles the `rockchiprga` plugin, but it copies and loads only
`libgstrockchipmpp.so`, so the element under test is unchanged. Its criteria and
its **INCONCLUSIVE** verdict therefore stand as recorded, and it remains
report-only: no stride edit follows from it.

## Untested hardware scope

The vendor-6.1 kernel-track drill was never executed. No vendor-6.1 board exists
in the fleet; only the mainline 7.2 track above was tested. These results must
not be generalized to that untested kernel track.

## 2026-09-04 — multi_rga BOARD GATE A (Orange Pi 5+)

The reachable Orange Pi 5+ ran `7.2.0-ceralive-rk3588 #3` from slot A with
`rk3588-media-island v2026.9.2` / patch-series commit `365b2463294019e3d4e2d5718a787c2055f4ab59`.
The installed package reported `gstreamer1.0-rockchip-ceralive
1.14.4+ceralive.1`; its source includes AUTO fix `38dadefa`, DMA-BUF allocation
fix `e95c9174`, and d5 harness fix `b724da59`. The board's post-release island
rotation-validation fix is `97b7b1c`; reference colorimetry fix `77b3bcb0`
aligns the software oracle with librga's default BT.601 matrix. Rock 5B+
remained BLOCKED-ON-OPERATOR at 100% ICMP loss and produced no result.

| Drill | Verdict under current criteria | Measured result |
|---|---|---|
| d2 Radxa/fork A/B | **PASS** | Radxa and fork each delivered H.265 300/300 then H.264 300/300 AUs. Fork summaries for both codecs were fallback=0, dropped=0, layout-rejections=0; journal counts were `RGA_BLIT fail=0`, `rga_api version=0`. Raw transcript: `test-results/board/d2-radxa-fork-ab-20260904T110439Z/`. |
| d4 136-second allocation soak | **FAIL — pre-existing live-TSVC defect** | Exact pre-flip geometry execution cannot pass the first transition because mainline supplies no `/dev/rga`, and CPU-copy debug mode cannot scale. A same-geometry, RGA-disabled control isolated the crash: tsvc2 applied at 30 s, tsvc3 at 60 s, then libmpp logged `h264e_dpb: find_cpb_frame can not find match frm 0` and SIGABRTed. Draining around geometry/ref-cfg changes did not repair libmpp's internal CPB state and was reverted. This code predates the RGA effort; the new driver only made the previously unrun d4 geometry path reachable. Raw hardware reruns: `test-results/board/d4-allocation-soak-20260904T132648Z/` and `test-results/board/d4-allocation-soak-20260904T133029Z/`; isolated control: root evidence `tmp/todo26-orange/d4-temporal-only-control.txt`. |
| d5 `rgaconvert`, strict floor 40 dB | **FAIL — 6/12 quality cells** | The corrected helper uses explicit dma-heap/appsrc input, verifies DMA-BUF on both DUT pads, and every final cell reports fallback=0, dropped=0, layout-rejections=0. Island fix `97b7b1c` restored quarter-turn validation. Pixel inspection disproved BGR/RGB byte-order and stride faults: BGR interpreted correctly scores 56.58/38.53 dB (Y/UV), while treating the same bytes as RGB collapses to 18.36/7.66 dB. The original ~22 dB result was mostly an oracle color-matrix mismatch (software BT.709 versus librga BT.601); `77b3bcb0` corrects that. Six cells remain below 40 solely on chroma, with sparse boundary/resampling outliers. Raw transcript: `test-results/board/d5-rgaconvert-matrix-20260904T142527Z/`. |

### Executed d5 matrix (`D5_PSNR_MIN=40`)

| Operation | NV12 → NV16 | NV16 → NV12 | BGR → NV12 |
|---|---|---|---|
| CSC (1280×720) | PASS — inf | PASS — inf | FAIL — 38.53 dB (Y 56.58) |
| Scale (→ 640×480) | FAIL — 39.71 dB (Y 48.55) | FAIL — 39.71 dB (Y 48.55) | FAIL — 38.28 dB (Y 49.52) |
| Crop (→ 1024×576) | PASS — inf | PASS — 45.24 dB | FAIL — 34.30 dB |
| Rotate 90° (→ 720×1280) | PASS — 40.80 dB | PASS — 45.24 dB | FAIL — 33.79 dB (Y 56.58) |

These are real final verdicts. d4 is a libmpp live-TSVC crash independent of
RGA conversion. d5's former DMA-BUF/fallback and rotate-submit failures are
fixed; the six remaining failures are confirmed zero-fallback hardware quality
results. Their luma is exact or 48.55–56.58 dB; the strict failures come from
fixed-function chroma conversion/resampling differences, including sparse edge
outliers up to 94 levels. They must not be softened into harness failures or a
channel-order defect.

## 2026-09-08 — U1 fixation candidate, Orange Pi 5+ [PARTIAL]

Current `origin/main` `aa92bdd460bdc5454f16a70ebd7173dc63db78f9` includes
`3c1272bc`, but still reproduces U1. A fresh arm64 package was built from that
source and installed before testing. HDMI negotiated NV16 3840×2160 at
60000/1001 with `colorimetry=2:4:7:1`; all three tests requested DMA-BUF NV12
output and 60 input buffers. The local fixation candidate was then installed
as `1.14.4+ceralive.2+u1candidate.1` on the same kernel and libraries.

| NV12 output request | Current main | Fixation candidate |
|---|---|---|
| Colorimetry omitted | FAIL, exit 1, `Not support full csc mode [300]` | PASS, EOS, exit 0; output retains `2:4:7:1` |
| Explicit `2:4:7:1` | PASS, EOS, exit 0 | PASS, EOS, exit 0 |
| Explicit `bt709` | FAIL, exit 1, `Not support full csc mode [300]` | FAIL, same refusal; the explicit request is not overwritten |

The candidate corrects metadata loss, not real BT.601→BT.709 conversion.
The initial all-three-PASS gate was not met. The owner subsequently authorized
`.3` with the explicit-BT.709 case recorded as a **KNOWN LIMITATION**, not a
release blocker: librga rejects full CSC mode 300, a genuine capability gap
whose fix is deferred to convergence todo 49 after librga R1 `1.10.5+ceralive.1`.
The explicit request is neither dropped nor relabelled to pretend conversion
succeeded. The two passing cases qualify the omitted-colorimetry fix.
Neither CSC selection nor librga defaults changed. Rock rows are **NO-SOURCE**:
the owner authorized this OPi-only run while Rock awaits physical inspection;
no Rock contact was attempted.

### Candidate d5, unchanged strict 40 dB floor

Board: `7.2.0-ceralive-rk3588 #ceralive1 SMP PREEMPT @1788765300`,
`librga2 2.2.0-1` (`rga_api version 1.10.1_[4]`), `librockchip-mpp1 1.5.0-1`.
The existing d5 script and DMA-BUF helper ran unchanged. The software reference
uses BT.709 for HD YUV and BT.601 for SD output, matching the helper's
GstVideoInfo defaults; it is not the historical always-BT.601 oracle.
Only orchestration changed: locked strict-host-key transport, Docker in place
of Podman for the same helper compile, and three-second SSH pacing.

| Operation | NV12 → NV16 | NV16 → NV12 | BGR → NV12 |
|---|---|---|---|
| CSC | PASS — inf/inf | PASS — inf/inf | EXPECTED-FAIL U3 — inf/38.54 |
| Scale | EXPECTED-FAIL U3 — 47.69/34.12 | EXPECTED-FAIL U3 — 47.69/34.12 | EXPECTED-FAIL U3 — 49.52/38.28 |
| Crop | PASS — 49.29/44.97 | PASS — 50.30/44.83 | EXPECTED-FAIL U3 — inf/34.30 |
| Rotate | **FAIL — request rejected, EINVAL** | **FAIL — request rejected, EINVAL** | **FAIL — request rejected, EINVAL; U3 quality not measured** |

Values are luma/chroma PSNR in dB. The five scored expected-fail cells match
the exact historical U3 identities and fail only the chroma floor, with
fallback=0, dropped=0 and layout-rejections=0. Their changed numeric scores
are retained, not claimed equivalent to the old reference. The sixth U3 cell,
BGR rotation, could not be quality-scored and must not be waived as U3.
All three rotations report `OUTPUT_SEEN=0`, dropped=1, and the kernel logs
`request validation failed before mapping`. Two of those cells previously
passed historically; the d5 result remains **FAIL**, not expected-fail-only.
The same-kernel A/B below establishes that the rotation submission failures are
pre-existing, rather than introduced by this fix. The owner accepts them as
separate, non-blocking follow-up work for `.3`; they are not U3 waivers.

Raw full matrix: `test-results/board/d5-rgaconvert-matrix-20260908T041507Z/`.
The earlier `20260908T041211Z` attempt lost SSH partway through and is retained
as transport-incomplete, not merged into these results. U1/build/restore
transcripts: `test-results/board/todo27-u1-transcripts.tar.gz`.
After the failed acceptance gate, the checksum-verified published
`1.14.4+ceralive.2` package was restored. No kernel, image pin, reboot, release,
or merge was performed.

### Same-kernel rotation A/B and release disposition

Baseline `aa92bdd460bdc5454f16a70ebd7173dc63db78f9` was installed from the
archived `1.14.4+ceralive.2+u1baseline.aa92bdd4` package (SHA-256
`ac4676253e8779616e04d9bb3cf4fe563d36f8dc45e53c31197e74210dcebe05`). Only the
three rotation cells ran, with the d5 script, helper source and 40 dB floor
unchanged. All three reproduce the candidate's EINVAL/no-output failure and
fallback=0, dropped=1, layout-rejections=0. The same boot journal contains both
legs' `request validation failed before mapping` entries.

| Pair | Candidate request | Baseline request | Provenance verdict |
|---|---|---|---|
| NV12 → NV16 | 3984 | 3987 | FAIL-ON-BASELINE-TOO — identical |
| NV16 → NV12 | 3985 | 3988 | FAIL-ON-BASELINE-TOO — identical |
| BGR → NV12 | 3986 | 3989 | FAIL-ON-BASELINE-TOO — identical |

Raw baseline run: `test-results/board/d5-rgaconvert-matrix-20260908T043811Z/`.
Both current legs use kernel `#ceralive1 SMP PREEMPT @1788765300`, slot B,
`librga2 2.2.0-1` / API `1.10.1_[4]`. The historical September-4 PASS rows
belong to kernel `#3 SMP PREEMPT Thu Sep 3 20:11:22 -05 2026`, slot A, and
the earlier `.1` development plugin. Historical Radxa records identify the
same librga lineage, but no run-specific historical dpkg receipt was recovered;
no librga version change is claimed. The older-to-current rotation failure
is a separate kernel-stack investigation, not a regression from this patch.
Published `.2` was restored and checksum-verified after the A/B.

**Owner-approved `.3` acceptance:** omitted colorimetry FIXED (FAIL→PASS),
explicit `2:4:7:1` PASS, explicit BT.709 documented/deferred; d5 **4 PASS,
5 expected U3 chroma FAIL, 3 independently reproduced pre-existing rotation
FAIL**. Hardware failures retain those verdicts; release authorization does
not turn them green. No CSC implementation, librga default, RGB bridge, kernel
fix or image pin belongs to this change.

### Commit-hook audit and scoped exception

The owner explicitly authorized `git commit --no-verify` for the three U1
commits, not a hook edit or a general exemption. The unchanged upstream-style
hook formats every C/header file in `gst/rockchipmpp`, including files outside
the staged diff. A non-mutating audit using GNU indent 2.2.12 and the hook's
exact flags and two passes found these unrelated baseline mismatches:

| File | Formatter pass exits | Finding |
|---|---|---|
| `gst/rockchipmpp/gstmppenc.h` | 0 / 0 | Formatting differs |
| `gst/rockchipmpp/gstmppallocator.c` | 0 / 0 | Formatting differs |
| `gst/rockchipmpp/gstmppenc.c` | 2 / 2 | Formatting differs; unmatched-else, statement-nesting and unexpected-EOF diagnostics |

The audit stopped at the third mismatch. Raw audit output is retained locally
as `test-results/t27-final/style-audit-results.log`, with per-file diff/stderr
under `test-results/t27-final/style-audit.5wt9fW/`. None of those three source
files or the hook is changed by U1. The exception avoids accepting unsafe
formatter output for unrelated code; it does not bypass the RED/GREEN tests,
hosted CI, independent review or merge-commit requirement.
