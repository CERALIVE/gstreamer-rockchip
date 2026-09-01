#!/usr/bin/env bash
# Print the version the NEXT release of this fork should carry.
#
# The scheme is upstream-style and deliberately NOT CalVer:
#
#     <meson project version>+ceralive.<N>
#
# It mirrors the CeraLive `srt` fork's `1.5.6+ceralive.N`. The base tracks the
# upstream release this plugin set is a fork of and is FROZEN — packaging's
# package-contract.sh refuses a meson `project()` version other than 1.14.4 —
# so only the counter ever moves. Reading the base out of meson.build rather
# than restating it here is what keeps the two from drifting: there is one
# number, in one file, and this script quotes it.
#
# N is derived from the tags that already exist, not from a workflow input, so
# a release can neither reuse nor skip a counter by typo. Tags that are not
# exactly `<base>+ceralive.<digits>` are ignored — an upstream `mpp-dev-*` tag,
# or a hypothetical suffixed pre-release tag, must not move the counter.
#
# usage: next-release-version.sh [<repo-root>]
#
# Requires the tags to be present locally: in CI that means checking out with
# `fetch-depth: 0`. A shallow checkout would see no tags and silently propose
# `+ceralive.1` over an existing release, so the caller's fetch depth is part
# of this script's contract.
set -euo pipefail

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

meson_build="${root}/meson.build"
[ -f "${meson_build}" ] || { echo "next-release-version: no meson.build at ${root}" >&2; exit 1; }

base="$(sed -n "s/^[[:space:]]*version[[:space:]]*:[[:space:]]*'\([0-9][0-9.]*\)'.*/\1/p" \
	"${meson_build}" | head -1)"
if [ -z "${base}" ]; then
	echo "next-release-version: could not read project() version from meson.build" >&2
	exit 1
fi

highest=0
while IFS= read -r tag; do
	[ -n "${tag}" ] || continue
	counter="${tag##*+ceralive.}"
	# `git tag --list` uses fnmatch, so its `*` would also accept `1+ceralive.2-rc1`.
	# Re-check the whole tag against an anchored regex before trusting the counter.
	[[ "${tag}" =~ ^${base//./\\.}\+ceralive\.[0-9]+$ ]] || continue
	if [ "${counter}" -gt "${highest}" ]; then
		highest="${counter}"
	fi
done < <(git -C "${root}" tag --list "${base}+ceralive.*")

printf '%s+ceralive.%s\n' "${base}" "$((highest + 1))"
