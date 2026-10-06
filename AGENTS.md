# gstreamer-rockchip

Parent policy: [CeraLive workspace](https://github.com/CERALIVE/ceralive/blob/master/AGENTS.md).

<!-- workspace-hard-rules:begin -->
## Workspace hard rules (identical in every CeraLive AGENTS.md)
- Commits and PRs carry the human author only: no Co-authored-by, no AI attribution.
- Start from the updated canonical branch; rebase to update; never `reset --hard` or discard others' work.
- One focused PR per repo, opened against CERALIVE/<repo>; the root policy PR merges first.
- A repo is self-contained: no path above its root; consume @ceralive packages from the registry, never link:/file:.
- Never delete, skip or weaken a test; every behavior change ships with a test.
- A user-visible change updates docs.ceralive.tv in English and Spanish (es-419), and any ceralive.tv claim it touches, in the same release.
- AGENTS.md holds rules and routing only, within budget; contracts and history live in docs/agents/.
- Full canon: https://github.com/CERALIVE/ceralive/blob/master/AGENTS.md
<!-- workspace-hard-rules:end -->

## ROLE

CeraLive's public RK3588 MPP/RGA GStreamer fork. Supplies hardware encode/decode,
conversion and composition to cerastream; ships one arm64 plugin package.
Host software coverage and board qualification are distinct.

## STRUCTURE

- `gst/` — MPP/RGA elements, KMS source and X11/KMS sink.
- `tests/` — host suites, parity goldens, board drills and MPI interposer.
- `ci/` — target-suite resolution, pinned dependencies and ABI/GLIBC gates.
- `packaging/` — Debian build and package/provider contracts.
- `docs/` — runtime contracts, fix evidence and extracted agent contracts.
- `hooks/` — upstream C formatting checker.
- `.github/` — Build Check and release workflows.

## COMMANDS

Full CI runs both Debian Bookworm/GStreamer 1.22 and Trixie/1.26 on arm64.
Use `RGA_COMPAT_SUITE=bookworm` on the Bookworm dependency-install step only.

```bash
bash ci/install-build-deps.sh
export CC="ccache gcc"
meson setup build -Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled
meson compile -C build
meson test -C build --print-errorlogs
shellcheck tests/mpi-interposer/check.sh
CC="ccache gcc" bash tests/mpi-interposer/check.sh build
bash ci/check-glibc-floor.test.sh
bash ci/check-glibc-floor.sh build/gst/rockchipmpp/libgstrockchipmpp.so
bash ci/check-mpp-abi.sh build/gst/rockchipmpp/libgstrockchipmpp.so
shellcheck -x ci/install-build-deps.sh ci/rga-suite-pins.test.sh
shellcheck -x ci/target-suite.test.sh ci/install-smoke.sh packaging/build-deb.sh
bash ci/target-suite.test.sh
bash ci/rga-suite-pins.test.sh
shellcheck -x packaging/package-contract.sh packaging/rga-provider-contract{,.test}.sh
bash packaging/rga-provider-contract.test.sh
bash packaging/build-deb.sh
bash packaging/package-contract.sh stage-deb-arm64 dist
```

`.github/workflows/build-check.yml` also rejects undefined `improcessOpt` symbols
in both built MPP/RGA plugins. It is the complete gate authority.

## WHERE TO LOOK

| Code path or task | Contract |
|---|---|
| Before changing anything else here, open docs/agents/README.md and read the contract for the subsystem you touch | [Contract index](docs/agents/README.md) |
| Overview | [overview](docs/agents/overview.md) |
| Role | [role](docs/agents/role.md) |
| Release publishing policy | [release-publishing-policy](docs/agents/release-publishing-policy.md) |
| Repository map | [repository-map](docs/agents/repository-map.md) |
| Commit strategy | [commit-strategy](docs/agents/commit-strategy.md) |
| PR-TARGETING | [pr-targeting](docs/agents/pr-targeting.md) |
| Cherry-pick source registry | [cherry-pick-source-registry](docs/agents/cherry-pick-source-registry.md) |
| Frozen contracts | [frozen-contracts](docs/agents/frozen-contracts.md) |
| Test and board-drill contract | [test-and-board-drill-contract](docs/agents/test-and-board-drill-contract.md) |
| Licensing and credits | [licensing-and-credits](docs/agents/licensing-and-credits.md) |
| Pre-commit formatting | [pre-commit-formatting](docs/agents/pre-commit-formatting.md) |
| Anti-patterns | [anti-patterns](docs/agents/anti-patterns.md) |

## HARD RULES

- Freeze `gstreamer1.0-rockchip` package prefix and `libgstrockchipmpp.so` / `libgstrockchiprga.so`; ship one `.deb`, not split RGA.
- Never add this package to device-image REPOS; seed it only through the platform-layer URL+SHA pin swap.
- Release versions are upstream-style `1.14.4+ceralive.N`, never CalVer. Release only from main after merged-head Build Check passes.
- Our MPP/RGA elements are the production path; no generic fallback. CPU staging is debug-only; compositor has no CPU pixel path.
- Publish only Trixie. Keep both suite build/smoke legs; Bookworm is internal portability coverage, never an APT/release input.
- Wire `RGA_COMPAT_SUITE` per librga-fetch/install step; never force-install R0/R1 on Bookworm or lower their dependency floor.
- Keep all eleven factories and frozen properties/caps/ranks; changes require explicit cross-repository migration.
- Preserve clean upstream ports with `cherry-pick -x`; never squash tier-(a)/(b) commits or self-merge their integration PR.
- Upstream-sync merges use merge-commit merge, never squash; retain upstream ancestry. Tier-(c) mechanical work may squash.
- PRs target CERALIVE/gstreamer-rockchip main, never fork parents; retain only CERALIVE origin at rest. No AI attribution trailers.
- Never pre-create release tags; independently verify archive checksums before pinning and APT index/package bytes after reindex.
- Probe `/dev/rga` driver version before use; librga init alone is insufficient. Preserve per-operation/format-pair blit health.
- Resolve Opt dynamically with a retained GModule handle, never link it; build against >=1.10.5 headers and retain legacy rollback.
- Async/cache/fault-bridge opt-ins stay default-off; defaults require their own in-element/end-to-end hardware evidence.
- Keep buffers/leases until terminal fences, including quarantine; never evict live leases or free unresolved frames at stop.
- Fault bridge never guesses ambiguous task ownership; cache start-time TIDs and union after restarts, never replace the owned set.
- Reject late encoder input before property application; preserve bounded restarts, explicit VUI and no-B-frame DTS = PTS.
- Board drills require CERALIVE_BOARD_TEST=1 and explicit environment credentials; preserve failed/inconclusive/unreachable verdicts.
- Host/mock/build/registration evidence is not hardware qualification; never infer PASS or sanitizer coverage from unavailable tests.
- Keep LGPL notices, copyright/provenance and inherited mixed line endings; formatting checks must not rewrite working files.
