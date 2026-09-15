# Rockchip MPP parity baselines

`fork-baseline/` is the active runtime contract for the fork's H.264 and H.265
encoders. `radxa-1.14-4/` remains the immutable reference capture for the two
decoder elements and is selected when the build defines `HAVE_NV16_10LE40`.
`fork-no-nv16-10le40/` is selected from the Meson-generated `config.h` when that
capability is absent, as it is in both current CI suites.
`radxa-1.14-4/decoder.sha256` pins the two immutable decoder captures and is
verified before either source or runtime parity runs.

The encoder lineages intentionally differ on three GObject property names:

| Radxa 1.14-4 | Fork baseline |
|---|---|
| `bps` | `bitrate` |
| `bps-min` | `bitrate-min` |
| `bps-max` | `bitrate-max` |

The fork inherited that string-only rename from upstream commit `cf155b3`, before
the CeraLive fork point. Values are still raw bits per second. The workspace plan
history records the decision to keep the fork names and migrate cerastream in a
separate release; the image-pipeline plugin pin must not move before that consumer
release exists. This repository neither adds a legacy `bps` alias nor controls the
external pin-swap prerequisite.

Golden files use normalized `key=value` records. Property records are split into
`type`, `default`, `range`, and `enum_nicks` subkeys. `parity-check.sh` requires
every non-comment baseline line and permits additional properties. It separately
asserts that `NV12_10LE40` and `NV16_10LE40` presence matches the same generated
feature macros used to compile the plugin, so variant selection cannot mask a
wrong capability advertisement.

The active encoder sink goldens add a linear NV12 `memory:DMABuf` alternative
for the composition boundary. The original plain-caps prefix remains unchanged;
the comparator still checks exact caps and rejects removals or arbitrary changes.
This addition is host-tested on the actual plugin, not a new board inspection
or a revision of the historical capture provenance in the golden headers.
