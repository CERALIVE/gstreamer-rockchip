<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## Release publishing policy

**Our APT publishes Trixie variants only. Bookworm is compile and portability
coverage, never published.** Owner decision, recorded 2026-09-16: "we dont want
to publish bookworm variants but trixie's in our apt". This is a permanent
policy, not a staging state waiting on a Bookworm librga release, and it is why
the release matrix below is deliberately asymmetric.

**The matrix builds two, publishes one.** `publish-release.yml`'s `build` job is
a `[bookworm, trixie]` matrix (line 158). Each leg runs the full four-gate suite
and packaging, then uploads its `dist/` under a name chosen by whether the leg
matches the suite from `ci/target-suite.env`: the suite-matched leg uploads
`release-assets`, the other uploads `portability-<suite>` (line 323). The
`publish` job downloads `release-assets` by name and nothing else (line 384),
so the portability artifact is discarded at the end of the run. Only one `.deb`
is ever published. The Bookworm package additionally carries a `~bookworm`
version suffix (line 163), so it could never collide with the released name
even if it escaped.

**The "exactly one `.deb`" assertion is correct BY POLICY.** Both the `build`
job (line 268, "Assemble and verify the release assets") and the `publish` job
(line 390, "Assert exactly one .deb and its checksum") assert set equality
between `dist/` and `{<deb>, <deb>.sha256}`. Do not "fix" either to admit a
second artifact. The assertion is also load-bearing downstream: apt-worker's
reindex downloads every `.deb` attached to the tag and hard-fails when a package
name differs from the dispatched component, so a stray second archive is a
failed publish, not a cosmetic one.

**The Bookworm leg's real value is GStreamer 1.22 API coverage.** Bookworm
ships 1.22 and Trixie ships 1.26; `build-check.yml` asserts each leg's actual
`pkg-config` minor against its declared one (lines 175-184) so a base-image
change cannot quietly turn the 1.22 leg into a second 1.26 leg. This coverage is
live, not theoretical: PR #38's `GstVideoAggregator` allocator-ownership fix
(`a7b6fe37`) was validated against both 1.22 and 1.26, and the Frozen contracts
entry for it names both versions. Dropping the Bookworm leg would delete that
coverage. Not publishing Bookworm is a reason to keep the leg internal, not a
reason to remove it.

**The Bookworm leg links legacy Radxa librga, with R1 headers only.** Published
R0/R1 require `libc6 (>= 2.38)` and cannot
install on Bookworm's glibc 2.36, so the Bookworm leg selects the SHA-pinned
Radxa `librga2`/`librga-dev` `2.2.0-1` pair through `RGA_COMPAT_SUITE=bookworm`
in `ci/mpp-pin.env`. A Bookworm artifact would therefore not be a supported
configuration even if it were published. `build-check.yml`'s summary already
labels the leg "legacy Radxa 2.2.0-1 (plugin portability only; NOT R0/R1
support)" (line 267). No Bookworm librga is needed under this policy and none
is planned; the legacy runtime pin is permanent, not a placeholder. C6b extracts
only checksum-verified R1 headers into `/usr/local/include/rga`, never installs
R1 packages there. Both suites enforce the ≥1.10.5 header assertion; older
runtime loading uses dynamic Opt lookup.

**`RGA_COMPAT_SUITE` must be wired into every job that installs or fetches
librga, per leg.** The `1.14.4+ceralive.6` release run at `800f92a8` failed
(run 35094258487) in `Gate and package · debian:bookworm · arm64` at the
dependency-install step: the installer logged `compatibility suite: none; R0
release pair` and apt refused with `librga2-ceralive : Depends: libc6 (>= 2.38)
but 2.36-9+deb12u14 is to be installed`. Before PR #39, `build-check.yml`
already set `RGA_COMPAT_SUITE` on its install step (now line 158), but
`publish-release.yml` set it nowhere. PR #39 (`b0892d94`) wired it into all
three places the release workflow touches librga: the `build` job's install
step (line 181), the `install-smoke` job's runtime fetch (line 352), and the
`docker run` environment for the smoke itself (line 368). Every one uses the
same expression, `${{ matrix.suite == 'bookworm' && 'bookworm' || '' }}`. The
same file got this right in one job and wrong in another because the selector
is per-step `env:`, not job-level, so a new step or job that installs librga
does not inherit it. Any future step that sources `ci/mpp-pin.env` or runs
`ci/install-build-deps.sh` inside a suite matrix needs the selector added
explicitly, or the Bookworm leg will try to install the incompatible release pair.

