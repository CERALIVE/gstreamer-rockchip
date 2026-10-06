<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## Repository map

| Area | Location |
|---|---|
| MPP encoder/decoder plugin | `gst/rockchipmpp/` |
| RGA 2D converter/compositor plugin | `gst/rockchiprga/` |
| RGA backend, tuple health, conversion counters | `gst/rockchipmpp/gstmpprga*.c`, `gstmppconversionstats.c` |
| Cross-layer kernel fault bridge (tracefs/procfs → restart) | `gst/rockchipmpp/gstmppfaultbridge.{c,h}` |
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

