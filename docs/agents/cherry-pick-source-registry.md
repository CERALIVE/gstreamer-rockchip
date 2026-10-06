<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

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

