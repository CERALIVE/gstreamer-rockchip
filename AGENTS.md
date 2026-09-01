# gstreamer-rockchip

CeraLive's public fork of the Rockchip MPP GStreamer plugins. The fork is based
on `irlserver/gstreamer-rockchip` at `755aeb9`, preserving the BELABOX and
datagutt streaming-control work, and carries CeraLive's independently reviewed
fix ledger for the RK3588 encode/decode path.

## Role

This repository supplies the RK3588 hardware elements used by `cerastream`:

```text
capture -> mpph264enc/mpph265enc -> cerastream transport
compressed input -> mppvideodec/mppjpegdec -> program graph
```

The package remains a complete plugin set. All nine factories must continue to
build and register: `mpph264enc`, `mpph265enc`, `mppvp8enc`, `mppjpegenc`,
`mppvideodec`, `mppjpegdec`, `mppvpxalphadecodebin`, `kmssrc`, and
`rkximagesink`. CeraLive deeply validates the four factories used by the engine;
the other five retain build-and-registration coverage.

## Repository map

| Area | Location |
|---|---|
| MPP encoder/decoder plugin | `gst/rockchipmpp/` |
| KMS source | `gst/kmssrc/` |
| Rockchip X11/KMS sink | `gst/rkximage/` |
| Hardware-independent tests | `tests/` |
| Board-gated drills | `tests/board/` |
| Runtime parity goldens | `tests/golden/` |
| Per-fix evidence ledger | `docs/fix-audit.md` |
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

- **Plugin filename:** `libgstrockchipmpp.so` remains unchanged.
- **Package prefix/name:** Rockchip packages retain the
  `gstreamer1.0-rockchip` prefix; this fork ships
  `gstreamer1.0-rockchip-ceralive` and replaces
  `gstreamer1.0-rockchip1` plus `belabox-gstreamer1.0-rockchip`.
- **Factory set:** all nine factories listed under **Role** continue to register.
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

`tests/parity-check.sh`, `tests/golden/`, `packaging/package-contract.sh`, and
the board drills are the executable authorities. Update a frozen contract only
through an explicit cross-repository migration, never as incidental refactoring.

## Test and board-drill contract

Hardware-independent gates run in both bookworm/GStreamer 1.22 and
trixie/GStreamer 1.26 environments. The mock-MPP suites prove software state,
ownership, caps construction, and MPP ABI closure; they do not emulate RK3588 DMA
addresses, RGA2, or the encoder firmware.

The board suite is deliberately outside Meson:

| Drill | Hardware claim |
|---|---|
| `d1-runtime-parity.sh` | Package installation, all-nine registration, four-element golden contract. |
| `d2-radxa-fork-ab.sh` | Radxa/fork 60 s encode A/B, 300/300 AUs, SPS geometry/profile/level, no RGA entry. |
| `d3-main10-stride-ab.sh` | Report-only Main10 current-vs-`*8/pixel_stride0` frame-checksum experiment. |
| `d4-allocation-soak.sh` | 136 s DMA allocation soak with live bitrate, resolution, and temporal-SVC changes. |

The latest executed verdicts and their hardware scope are recorded in
[`tests/board/DRILL-RESULTS.md`](tests/board/DRILL-RESULTS.md). That tracked
summary preserves failed and inconclusive outcomes; it is not a substitute for
the retained raw transcripts.

Every script requires `CERALIVE_BOARD_TEST=1` and otherwise exits 77. Board
identity is supplied only through `BOARD_IP`, `BOARD_SSH_USER`, and
`BOARD_SSH_PASS`; repository files never locate credentials or reference a
workspace parent. d1/d2/d4 also take package paths through environment variables.

### The suite proves

- The exact package and kernel named in each transcript loaded on the reachable
  board used for that run.
- Registration/property/caps/rank behavior and the finite runtime observations
  scored by each completed drill.
- For d2/d4, zero matching `RGA_BLIT fail` and `rga_api version` journal lines in
  the measured window.
- For d3, only the enum written by its frame-count/checksum/error oracle:
  `CURRENT_CORRECT`, `ALTERNATIVE_CORRECT`, or `INCONCLUSIVE`.

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

## Licensing and credits

The project remains LGPL-2.1. Keep `COPYING`, source headers, and
`packaging/copyright` intact. Copyright holders represented in the shipped tree
are Rockchip Electronics Co., Ltd.; Collabora Ltd.; Igalia; and Julien Moutte.
Igalia and Julien Moutte are scoped to `gst/rkximage/`, not the MPP plugin.

Provenance credits are distinct: Rockchip originated the plugin family, JeffyCN
maintains the audited upstream line, BELABOX rebased and carried the downstream
tree, and irlserver-datagutt added the streaming-control features inherited by
this fork. See `README.md` for the public maintainer notice.

## Anti-patterns

- Do not rename `libgstrockchipmpp.so` or the package prefix.
- Do not remove unused factories to reduce the package.
- Do not rename `bitrate` back to `bps` or add a legacy alias here.
- Do not change Main10 stride semantics on static-analysis confidence alone.
- Do not treat plugin registration success as proof all factories registered;
  `plugin_init` historically swallows individual registration failures.
- Do not claim sanitizer coverage that the qemu-user environment cannot run.
- Do not let a board drill install a package without recording package, kernel,
  and final verdict, and do not infer PASS from a command merely completing.
- Do not run the pre-commit hook casually: its baseline-wide `gst-indent` pass is
  destructive on failure and can rewrite untouched source. Review `git status`
  immediately if it runs.
- Preserve mixed line endings in inherited files; avoid text-mode whole-file
  rewrites and compare raw versus whitespace-ignored diffs.
