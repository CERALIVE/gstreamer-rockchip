# RK3588 board-drill results

This file records the latest executed hardware verdicts for the board suite. It
preserves the same outcomes as the retained raw transcripts; a command finishing
or a failure later being classified as pre-existing does not turn that drill
into a pass.

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
| d1 runtime parity/registration | **NOT-RUN** | Nine unconditional factories register, `rgaconvert` among them, and `mppvp8enc` matches its per-SoC expectation: EXPECTED-ABSENT on an `rk3588` board, PRESENT-REQUIRED elsewhere. |
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
rotation-validation fix is `97b7b1c`. Rock 5B+ remained BLOCKED-ON-OPERATOR at
100% ICMP loss and produced no result.

| Drill | Verdict under current criteria | Measured result |
|---|---|---|
| d2 Radxa/fork A/B | **PASS** | Radxa and fork each delivered H.265 300/300 then H.264 300/300 AUs. Fork summaries for both codecs were fallback=0, dropped=0, layout-rejections=0; journal counts were `RGA_BLIT fail=0`, `rga_api version=0`. Raw transcript: `test-results/board/d2-radxa-fork-ab-20260904T110439Z/`. |
| d4 136-second allocation soak | **FAIL — pre-existing live-TSVC defect** | Exact pre-flip geometry execution cannot pass the first transition because mainline supplies no `/dev/rga`, and CPU-copy debug mode cannot scale. A same-geometry, RGA-disabled control isolated the crash: tsvc2 applied at 30 s, tsvc3 at 60 s, then libmpp logged `h264e_dpb: find_cpb_frame can not find match frm 0` and SIGABRTed. Draining around geometry/ref-cfg changes did not repair libmpp's internal CPB state and was reverted. This code predates the RGA effort; the new driver only made the previously unrun d4 geometry path reachable. Raw hardware reruns: `test-results/board/d4-allocation-soak-20260904T132648Z/` and `test-results/board/d4-allocation-soak-20260904T133029Z/`; isolated control: root evidence `tmp/todo26-orange/d4-temporal-only-control.txt`. |
| d5 `rgaconvert`, strict floor 40 dB | **FAIL — 6/12 quality cells** | The corrected helper uses explicit dma-heap/appsrc input, verifies DMA-BUF on both DUT pads, and every final cell reports fallback=0, dropped=0, layout-rejections=0. A driver-validation bug initially rejected quarter-turn destination axes; island fix `97b7b1c` restored both YUV rotation cells. Six cells pass and six execute on silicon but remain below 40 dB. Raw transcript: `test-results/board/d5-rgaconvert-matrix-20260904T133931Z/`. |

### Executed d5 matrix (`D5_PSNR_MIN=40`)

| Operation | NV12 → NV16 | NV16 → NV12 | BGR → NV12 |
|---|---|---|---|
| CSC (1280×720) | PASS — inf | PASS — inf | FAIL — 22.65 dB |
| Scale (→ 640×480) | FAIL — 22.69 dB | FAIL — 22.69 dB | FAIL — 22.66 dB |
| Crop (→ 1024×576) | PASS — inf | PASS — 45.24 dB | FAIL — 34.30 dB |
| Rotate 90° (→ 720×1280) | PASS — 40.80 dB | PASS — 45.24 dB | FAIL — 22.65 dB |

These are real final verdicts. d4 is a libmpp live-TSVC crash independent of
RGA conversion. d5's former DMA-BUF/fallback and rotate-submit failures are
fixed; the six remaining failures are confirmed zero-fallback hardware quality
results and must not be softened into harness failures.
