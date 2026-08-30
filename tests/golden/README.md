# Rockchip MPP parity baselines

`fork-baseline/` is the active runtime contract for the fork's H.264 and H.265
encoders. `radxa-1.14-4/` remains the historical capture of the plugin currently
installed on the Rock 5B+ and remains active for the two decoder elements.

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
every non-comment baseline line and permits additional properties.
