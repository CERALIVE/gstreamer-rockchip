#!/usr/bin/env bash
# Most patterns below are single-quoted on purpose: they are the literal text of
# build-deb.sh, `${triplet}` and all. Expanding them here would assert against
# this script's values instead of the builder's source, which is the opposite of
# what a static contract is for.
# shellcheck disable=SC2016
set -euo pipefail

# Package-contract test for gstreamer1.0-rockchip-ceralive.
#
# Two modes:
#
#   package-contract.sh                    STATIC — reads the packaging sources
#                                          and the meson build files, builds
#                                          nothing, runs anywhere.
#   package-contract.sh <stage> [<out>]    STATIC + the same contract re-checked
#                                          against a real staged tree and the
#                                          .deb output directory.
#
# The static half is what a PR can run. The staged half is what the release
# workflow runs after packaging/build-deb.sh, because a few claims — the plugin
# actually landing on the frozen path, the doc trio actually being installed,
# development artifacts actually being absent — are properties of the produced
# tree and cannot be proven by reading a script.

# The release version. Upstream-style, NOT CalVer: the base tracks the meson
# project version and only the +ceralive counter moves.
readonly EXPECT_DEB_VERSION="1.14.4+ceralive.1"
# The meson project() version, which this fork does not bump.
readonly EXPECT_MESON_VERSION="1.14.4"
readonly EXPECT_PACKAGE="gstreamer1.0-rockchip-ceralive"
# FROZEN. The image's sysext exclusion globs and the MPP runtime-contract test
# key on this filename and on the package-name prefix, by exact name.
readonly EXPECT_PLUGIN_SO="libgstrockchipmpp.so"
readonly EXPECT_ARCH="arm64"
readonly EXPECT_TRIPLET="aarch64-linux-gnu"
readonly EXPECT_PLUGIN_PATH="/usr/lib/${EXPECT_TRIPLET}/gstreamer-1.0/${EXPECT_PLUGIN_SO}"
readonly DEP5_FORMAT="https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
readonly SOURCE_URL="https://github.com/CERALIVE/gstreamer-rockchip"
# Every distinct copyright holder in the compiled sources under gst/. Derived
# from the tree, not assumed: the scan below fails if gst/ grows a holder that
# is not in this list, so a new upstream contributor cannot reach a release
# without a DEP-5 stanza.
readonly KNOWN_HOLDERS_RE='Rockchip Electronics|Collabora Ltd|Igalia|Julien Moutte'

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
builder="${root}/packaging/build-deb.sh"
copyright="${root}/packaging/copyright"

fail() { printf 'package-contract: FAIL: %s\n' "$1" >&2; exit 1; }

bash -n "${builder}"

# --- Version: upstream-style, and tied to the frozen meson project version ----
grep -qF "version=\"\${CERALIVE_GSTREAMER_ROCKCHIP_VERSION:-${EXPECT_DEB_VERSION}}\"" "${builder}" \
	|| fail "build-deb.sh default version must be ${EXPECT_DEB_VERSION}"
grep -qF "version : '${EXPECT_MESON_VERSION}'" "${root}/meson.build" \
	|| fail "meson.build project() version must stay frozen at ${EXPECT_MESON_VERSION}"
[[ "${EXPECT_DEB_VERSION}" =~ ^${EXPECT_MESON_VERSION//./\\.}\+ceralive\.[0-9]+$ ]] \
	|| fail "deb version ${EXPECT_DEB_VERSION} must be ${EXPECT_MESON_VERSION}+ceralive.N (not CalVer)"

# --- Control fields -----------------------------------------------------------
grep -qF "Package: ${EXPECT_PACKAGE}" "${builder}" \
	|| fail "control must declare Package: ${EXPECT_PACKAGE}"
grep -qF 'Architecture: ${arch}' "${builder}" \
	|| fail "control must declare Architecture from the resolved \${arch}"
grep -qF 'Maintainer: CERALIVE <contact@ceralive.tv>' "${builder}" \
	|| fail "control Maintainer must be CERALIVE <contact@ceralive.tv>"
grep -qF 'Depends: libgstreamer1.0-0, libc6 (>= 2.36), librockchip-mpp1, librga2' "${builder}" \
	|| fail "control Depends must be libgstreamer1.0-0, libc6 (>= 2.36), librockchip-mpp1, librga2"
grep -qF 'Provides: gstreamer1.0-rockchip1' "${builder}" \
	|| fail "control must Provides: gstreamer1.0-rockchip1"
grep -qF 'Conflicts: gstreamer1.0-rockchip1, belabox-gstreamer1.0-rockchip' "${builder}" \
	|| fail "control Conflicts must name both replaced packages"
grep -qF 'Replaces: gstreamer1.0-rockchip1, belabox-gstreamer1.0-rockchip' "${builder}" \
	|| fail "control Replaces must name both replaced packages"
grep -qF 'Section: libs' "${builder}"     || fail "control must declare Section: libs"
grep -qF 'Priority: optional' "${builder}" || fail "control must declare Priority: optional"
grep -qF "Homepage: ${SOURCE_URL}" "${builder}" \
	|| fail "control Homepage must be ${SOURCE_URL}"
grep -qF 'License: LGPL-2.1' "${builder}" || fail "control must declare License: LGPL-2.1"
grep -qE '^Description: .+' "${builder}"  || fail "control must carry a Description"

# --- The two relationship fields that are wrong the moment they gain a version
# Provides is unversioned because nothing in the image depends on
# gstreamer1.0-rockchip1 at a version; a version here invents a constraint.
if grep -E '^Provides:' "${builder}" | grep -q '('; then
	fail "Provides must be UNVERSIONED — no (= X) suffix"
fi
# libgstreamer1.0-0 is unversioned because the plugin must keep resolving on
# both the bookworm 1.22 and the trixie 1.26 runtime. A minor pinned here turns
# a package that works on both into one that installs on neither by accident.
if grep -E '^Depends:' "${builder}" | grep -q 'libgstreamer1.0-0 ('; then
	fail "Depends must NOT pin a GStreamer version on libgstreamer1.0-0"
fi

# --- Exactly one .deb ---------------------------------------------------------
# reindex.sh downloads every .deb in a release tag and hard-fails if any package
# name differs from the dispatched component, so a second artifact is not a
# cosmetic problem — it breaks publication.
build_calls="$(grep -cE 'dpkg-deb .*--build' "${builder}" || true)"
[ "${build_calls}" = "1" ] \
	|| fail "build-deb.sh must invoke dpkg-deb --build exactly once (found ${build_calls})"
deb_names="$(grep -cE '^deb=' "${builder}" || true)"
[ "${deb_names}" = "1" ] \
	|| fail "build-deb.sh must name exactly one output .deb (found ${deb_names})"

# --- Frozen plugin path -------------------------------------------------------
# Proven rather than asserted: the triplet is resolved live for arm64, meson's
# install dir is read out of the build files, and the two are composed into the
# path the image's sysext globs and MPP runtime contract expect.
grep -qF 'triplet="$(dpkg-architecture -a "${arch}" -qDEB_HOST_MULTIARCH)"' "${builder}" \
	|| fail "build-deb.sh must resolve the multiarch triplet via dpkg-architecture"
grep -qF -- '--libdir="lib/${triplet}"' "${builder}" \
	|| fail "meson setup must install libraries under lib/\${triplet}"
grep -qF 'plugin_dir="${stage_dir}/usr/lib/${triplet}/gstreamer-1.0"' "${builder}" \
	|| fail "build-deb.sh must stage plugins under usr/lib/\${triplet}/gstreamer-1.0"
grep -qF "plugins_install_dir = '@0@/gstreamer-1.0'.format(get_option('libdir'))" "${root}/meson.build" \
	|| fail "meson.build plugins_install_dir must resolve to <libdir>/gstreamer-1.0"
grep -qF "library('gstrockchipmpp'," "${root}/gst/rockchipmpp/meson.build" \
	|| fail "the MPP plugin library name is FROZEN as gstrockchipmpp (${EXPECT_PLUGIN_SO})"
live_triplet="$(dpkg-architecture -a "${EXPECT_ARCH}" -qDEB_HOST_MULTIARCH)"
[ "${live_triplet}" = "${EXPECT_TRIPLET}" ] \
	|| fail "${EXPECT_ARCH} must resolve to ${EXPECT_TRIPLET}, got ${live_triplet}"

# --- LGPL doc trio ------------------------------------------------------------
grep -qF 'doc_dir="${stage_dir}/usr/share/doc/gstreamer1.0-rockchip-ceralive"' "${builder}" \
	|| fail "docs must be staged under usr/share/doc/${EXPECT_PACKAGE}"
grep -qF '"${doc_dir}/copyright"' "${builder}"          || fail "build-deb.sh must install copyright"
grep -qF '"${doc_dir}/COPYING"' "${builder}"            || fail "build-deb.sh must install COPYING"
grep -qF '"${doc_dir}/changelog.Debian.gz"' "${builder}" || fail "build-deb.sh must install changelog.Debian.gz"
[ -f "${root}/COPYING" ] || fail "repository COPYING (LGPL-2.1 text) is missing"
grep -qF 'GNU LESSER GENERAL PUBLIC LICENSE' "${root}/COPYING" \
	|| fail "COPYING must contain the LGPL text"

# --- Development artifacts are stripped at staging (one runtime .deb, no -dev)
grep -qF 'rm -rf "${stage_dir}/usr/include" "${stage_dir}/usr/lib/${triplet}/pkgconfig"' "${builder}" \
	|| fail "build-deb.sh must remove headers and pkgconfig from the stage"
grep -qF -- "-name '*.pc' -o -name '*.a' -o -name '*.la' -o -name '*.h' \\) -delete" "${builder}" \
	|| fail "build-deb.sh must delete .pc/.a/.la/.h artifacts from the stage"

# --- DEP-5 copyright ----------------------------------------------------------
[ -f "${copyright}" ] || fail "packaging/copyright is missing"
grep -qxF "Format: ${DEP5_FORMAT}" "${copyright}" \
	|| fail "packaging/copyright must open with Format: ${DEP5_FORMAT}"
grep -qxF "Source: ${SOURCE_URL}" "${copyright}" \
	|| fail "packaging/copyright must declare Source: ${SOURCE_URL}"
grep -qxF 'License: LGPL-2.1' "${copyright}" \
	|| fail "packaging/copyright must declare License: LGPL-2.1"

# Every Files: stanza must carry both a Copyright: and a License: field —
# a stanza missing either is not attribution, it is decoration.
stanzas="$(awk 'BEGIN{RS="";FS="\n"} /^Files:/ {n++} END{print n+0}' "${copyright}")"
[ "${stanzas}" -ge 4 ] \
	|| fail "packaging/copyright needs a Files: stanza per holder group (found ${stanzas})"
incomplete="$(awk 'BEGIN{RS="";FS="\n"}
	/^Files:/ { if ($0 !~ /\nCopyright:/ || $0 !~ /\nLicense:/) n++ }
	END{print n+0}' "${copyright}")"
[ "${incomplete}" = "0" ] \
	|| fail "${incomplete} Files: stanza(s) lack a Copyright: or License: field"
# The standalone licence paragraph the Files: stanzas refer to, with its text.
awk 'BEGIN{RS="";FS="\n"} /^License: LGPL-2.1\n / {found=1} END{exit !found}' "${copyright}" \
	|| fail "packaging/copyright needs a standalone License: LGPL-2.1 paragraph with the licence text"

# --- Holder coverage, derived from the tree rather than assumed ---------------
unknown="$(grep -rhoE 'Copyright.*' --include='*.c' --include='*.h' "${root}/gst" \
	| grep -vE "${KNOWN_HOLDERS_RE}" | sort -u || true)"
[ -z "${unknown}" ] \
	|| fail "gst/ carries copyright holder(s) with no DEP-5 stanza: ${unknown}"
while IFS= read -r holder; do
	grep -qF "${holder}" "${copyright}" \
		|| fail "packaging/copyright has no stanza naming ${holder}"
done <<'HOLDERS'
Rockchip Electronics Co., Ltd
Collabora Ltd.
Igalia
Julien Moutte
HOLDERS

if [ "$#" -eq 0 ]; then
	printf 'package-contract: OK static (%s %s · %s)\n' \
		"${EXPECT_PACKAGE}" "${EXPECT_DEB_VERSION}" "${EXPECT_PLUGIN_PATH}"
	exit 0
fi

# --- Staged tree --------------------------------------------------------------
stage="$1"
out="${2:-${root}/dist}"
[ -d "${stage}" ] || fail "staged tree ${stage} does not exist"

staged_plugin="${stage}${EXPECT_PLUGIN_PATH}"
[ -f "${staged_plugin}" ] \
	|| fail "FROZEN plugin path missing: ${EXPECT_PLUGIN_PATH} (looked in ${stage})"

staged_doc="${stage}/usr/share/doc/${EXPECT_PACKAGE}"
for doc in copyright COPYING changelog.Debian.gz; do
	[ -f "${staged_doc}/${doc}" ] \
		|| fail "LGPL doc missing from the stage: /usr/share/doc/${EXPECT_PACKAGE}/${doc}"
done
gzip -t "${staged_doc}/changelog.Debian.gz" \
	|| fail "changelog.Debian.gz is not valid gzip"

leaked="$(find "${stage}" -type f \( -name '*.pc' -o -name '*.a' -o -name '*.la' -o -name '*.h' \) -print)"
[ -z "${leaked}" ] || fail "development artifacts leaked into the stage: ${leaked}"
[ ! -d "${stage}/usr/include" ] || fail "headers leaked into the stage: usr/include"

control="${stage}/DEBIAN/control"
[ -f "${control}" ] || fail "staged DEBIAN/control is missing"
grep -qxF "Package: ${EXPECT_PACKAGE}" "${control}" || fail "staged control Package is wrong"
grep -qxF "Architecture: ${EXPECT_ARCH}" "${control}" || fail "staged control Architecture must be ${EXPECT_ARCH}"
grep -qxF 'Provides: gstreamer1.0-rockchip1' "${control}" || fail "staged control Provides is wrong"
if grep -E '^Provides:' "${control}" | grep -q '('; then
	fail "staged control Provides gained a version"
fi

if [ -d "${out}" ]; then
	debs="$(find "${out}" -maxdepth 1 -name '*.deb' | wc -l)"
	[ "${debs}" = "1" ] \
		|| fail "the release must publish EXACTLY ONE .deb, ${out} holds ${debs}"
fi

printf 'package-contract: OK static + staged (%s · %s)\n' "${stage}" "${EXPECT_PLUGIN_PATH}"
