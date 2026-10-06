<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

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

**Release ledger, current at 2026-09-21.** Seven releases exist, one arm64 `.deb`
plus `.sha256` each: `.1` (nine factories), `.2` (`rgaconvert`/`rgacompositor`,
PR #27), `.3` (colorimetry fixation, PR #30), `.4` (sRGB transfer VUI mapping,
PR #32 — pruned from the APT index by its own reindex run and never
re-published, see its release notes), `.5` (compositor DMA-BUF caps accepted by
the encoder, PR #33), `.6` (suite-matched release builds + librga R1 pins,
PR #39/#41), `.7` (C6b explicit colour and im2d compatibility boundary PR #42,
bounded stop with fence quarantine PR #47, bounded DMA-BUF handle cache PR #46,
delayed-primary composition start PR #45, real RKVENC fault attribution PR #44,
teardown admission PR #43). `image-building-pipeline` master pins **`.7`**
(image PR #183, `.6` retained commented above it as the rollback), and that pin
has been booted, not only configured: on 2026-09-21 the Rock 5B+ and the Orange
Pi 5+ each promoted the image built from it to RAUC slot A, came up with
`systemctl --failed` empty and `ceralive-healthcheck.service` self-marking the
slot good, and read `gstreamer1.0-rockchip-ceralive 1.14.4+ceralive.7` back
through `dpkg-query` on the booted slot (alongside `librga2-ceralive
1.10.5+ceralive.1`, `cerastream 2026.9.6` and the island `v2026.9.5` kernel).
Earlier bench runs of `.3` through `.6` on island candidate images are named by
the 2026-09-19 Rock composition teardown and encoder-restart rows and the OPi
`RUN-30-R3` composition row in root `docs/COMPLETENESS-MATRIX.md` §2.2. So
"devices ship it" is the honest wording for `.7` from that date; "pinned but no
device image has passed it" is history for every release. A clean boot with the
plugin installed is not a per-element pass: `tests/board/DRILL-RESULTS.md`
remains the per-drill verdict record and none of its limits are lifted here.

The `1.14.4+ceralive.3` release scope is the omitted-colorimetry fixation fix,
not complete media-stack qualification. The OPi hardware rerun proves that fix;
explicit BT.709 CSC and the remaining d5 failures are documented limitations
accepted by the owner for this release, not silently passing board gates.
[`tests/board/DRILL-RESULTS.md`](../../tests/board/DRILL-RESULTS.md) retains the actual
drill verdicts and their limits. A passing build or install smoke does not erase
those limits or establish a working 4K59.94 capture-to-encode stream.

Release through `.github/workflows/publish-release.yml` on `main` only, after
the merged commit's Build Check passes. Dispatch with `release_type=stable`
and `dry_run=false` for publication; `dry_run=true` rehearses without publishing.
The workflow derives the next upstream-style version from existing tags, gates
both suite builds and their suite-matched install smokes, publishes exactly one arm64 `.deb` plus
its `.sha256`, then dispatches `apt-reindex` to `CERALIVE/apt-worker`. Do not
pre-create the tag. Independently download and checksum the release archive
before image pinning, and verify the stable arm64 APT index and package bytes
after reindexing; a successful dispatch alone is not serving proof.

Production defaults to **Trixie / glibc 2.41 / GStreamer 1.26** in
`ci/target-suite.env`. Bookworm builds explicitly set `TARGET_SUITE=bookworm`
and select the existing `RGA_COMPAT_SUITE=bookworm` legacy pair. Both release
builds run all four gates and package contracts; each smoke starts in a fresh
matching container and verifies the package's `X-CeraLive-Build-Suite` field.
Only Trixie's `release-assets` is published. Bookworm's `~bookworm` package is
an internal `portability-bookworm` artifact, never a GitHub release asset or APT
input. Its green smoke is not a claim that the Trixie/R1 binary runs on Bookworm.

