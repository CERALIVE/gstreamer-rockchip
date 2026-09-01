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

## Untested hardware scope

The vendor-6.1 kernel-track drill was never executed. No vendor-6.1 board exists
in the fleet; only the mainline 7.2 track above was tested. These results must
not be generalized to that untested kernel track.
