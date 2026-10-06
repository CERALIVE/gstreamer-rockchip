# Agent contracts

Original instruction blocks preserved from `origin/main:AGENTS.md`. Read the applicable contract before editing its subsystem.

| Original heading | Contract | Governed paths / tasks |
|---|---|---|
| Overview | [overview.md](overview.md) | `irlserver/gstreamer-rockchip` |
| Role | [role.md](role.md) | `docs/COMPLETENESS-MATRIX.md`, `tests/board/DRILL-RESULTS.md`, `.github/workflows/publish-release.yml`, `CERALIVE/apt-worker`, `ci/target-suite.env` |
| Release publishing policy | [release-publishing-policy.md](release-publishing-policy.md) | `dist/`, `ci/target-suite.env`, `ci/mpp-pin.env`, `/usr/local/include/rga`, `ci/install-build-deps.sh` |
| Repository map | [repository-map.md](repository-map.md) | `gst/rockchipmpp/`, `gst/rockchiprga/`, `gst/rockchipmpp/gstmpprga*.c`, `gst/rockchipmpp/gstmppfaultbridge.{c,h}`, `docs/RGA-MPP-INTERACTION.md`, `gst/kmssrc/`, `gst/rkximage/`, `tests/` |
| Commit strategy | [commit-strategy.md](commit-strategy.md) | Commit strategy |
| PR-TARGETING | [pr-targeting.md](pr-targeting.md) | `CERALIVE/gstreamer-rockchip` |
| Cherry-pick source registry | [cherry-pick-source-registry.md](cherry-pick-source-registry.md) | `docs/fix-audit.md` |
| Frozen contracts | [frozen-contracts.md](frozen-contracts.md) | `tests/check/enc-dmabuf.c`, `docs/ENCODER-RUNTIME-CONTRACT.md`, `/dev/rga`, `CERALIVE/librga`, `ci/mpp-pin.env`, `ci/install-build-deps.sh`, `ci/rga-suite-pins.test.sh`, `docs/RGA-MPP-INTERACTION.md` |
| Test and board-drill contract | [test-and-board-drill-contract.md](test-and-board-drill-contract.md) | `tests/check/enc-teardown.c`, `docs/ENCODER-RUNTIME-CONTRACT.md`, `/proc/self/task/*`, `/proc/mpp_service/sessions-summary`, `tests/check/fault-bridge.c`, `tests/check/enc-fault-bridge.c`, `tests/parity-comparator.test.sh`, `tests/mpi-interposer/` |
| Licensing and credits | [licensing-and-credits.md](licensing-and-credits.md) | `packaging/copyright`, `gst/rkximage/`, `gst/rockchiprga/` |
| Pre-commit formatting | [pre-commit-formatting.md](pre-commit-formatting.md) | `hooks/pre-commit.hook`, `gst/rockchipmpp/` |
| Anti-patterns | [anti-patterns.md](anti-patterns.md) | Anti-patterns |
