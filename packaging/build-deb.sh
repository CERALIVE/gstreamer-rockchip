#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=ci/target-suite.env
source "${root}/ci/target-suite.env"
build_suite="$(. /etc/os-release; printf '%s' "${VERSION_CODENAME}")"
[[ "${build_suite}" == "${TARGET_SUITE}" ]] || {
	printf 'build suite %s does not match TARGET_SUITE=%s\n' "${build_suite}" "${TARGET_SUITE}" >&2
	exit 1
}
arch="${DEB_ARCH:-$(dpkg --print-architecture)}"
version="${CERALIVE_GSTREAMER_ROCKCHIP_VERSION:-1.14.4+ceralive.1}"
triplet="$(dpkg-architecture -a "${arch}" -qDEB_HOST_MULTIARCH)"
build_dir="${BUILD_DIR:-${root}/build-deb-${arch}}"
stage_dir="${STAGE_DIR:-${root}/stage-deb-${arch}}"
out_dir="${OUT_DIR:-${root}/dist}"

# arm64 only, and deliberately so: the plugin links librockchip_mpp and librga,
# which exist for RK3588 boards and nowhere else. A build for any other
# architecture would produce a package with no hardware to drive.
case "${arch}" in
	arm64) ;;
	*) echo "unsupported Debian architecture: ${arch} (this plugin is RK3588/arm64 only)" >&2; exit 2 ;;
esac

rm -rf "${build_dir}" "${stage_dir}"

# Every plugin option is stated explicitly rather than left to meson's `auto`.
# With `auto`, a missing MPP or RGA drops the affected plugin and the build
# still succeeds — which would ship a green, silently element-less package.
#
# `-Drga=enabled` carries two meanings and both are load-bearing: it enables
# RGA conversion inside the MPP plugin, AND it is what builds the separate
# rockchiprga plugin at all (gst/meson.build descends into it only when librga
# is found and the feature is not disabled). Left on `auto` in a container
# without librga, the package would silently ship without either RGA factory.
meson setup "${build_dir}" "${root}" \
	--prefix=/usr \
	--libdir="lib/${triplet}" \
	-Drkximage=enabled \
	-Drockchipmpp=enabled \
	-Dkmssrc=enabled \
	-Drga=enabled
meson compile -C "${build_dir}"
DESTDIR="${stage_dir}" meson install -C "${build_dir}"

plugin_dir="${stage_dir}/usr/lib/${triplet}/gstreamer-1.0"

# The release publishes EXACTLY ONE .deb, so there is no -dev/-dbgsym package to
# route development artifacts into. meson installs only the plugin .so files
# today; these removals keep that true if a future target gains `install : true`,
# rather than letting headers or a pkg-config file ride silently into the
# runtime package.
rm -rf "${stage_dir}/usr/include" "${stage_dir}/usr/lib/${triplet}/pkgconfig"
find "${stage_dir}" -type f \( -name '*.pc' -o -name '*.a' -o -name '*.la' -o -name '*.h' \) -delete

# Same reason, for debug symbols: the project default buildtype is
# `debugoptimized`, so an unstripped stage would ship megabytes of DWARF in the
# runtime package. These are dh_strip's flags for a shared object — dynamic
# symbols survive, so the MPP ABI closure is unchanged.
find "${plugin_dir}" -name '*.so' -exec \
	strip --strip-unneeded --remove-section=.comment --remove-section=.note {} +

# LGPL distribution mechanics: the licence text, the machine-readable per-holder
# attribution, and a Debian changelog, at the path dpkg and licence auditors
# both expect.
doc_dir="${stage_dir}/usr/share/doc/gstreamer1.0-rockchip-ceralive"
install -Dm644 "${root}/packaging/copyright" "${doc_dir}/copyright"
install -Dm644 "${root}/COPYING" "${doc_dir}/COPYING"

if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
	changelog_date="$(date -R -u -d "@${SOURCE_DATE_EPOCH}")"
else
	changelog_date="$(date -R -u)"
fi
# -n omits the filename/mtime header so the same input gzips to the same bytes.
gzip -9n >"${doc_dir}/changelog.Debian.gz" <<EOF
gstreamer1.0-rockchip-ceralive (${version}) unstable; urgency=medium

  * CeraLive fork of the Rockchip RK3588 MPP GStreamer plugins, carrying the
    verified fix ledger recorded in docs/fix-audit.md.
  * Replaces gstreamer1.0-rockchip1 and belabox-gstreamer1.0-rockchip on the
    device image; the plugin filename libgstrockchipmpp.so is unchanged.
  * Ships the rockchiprga plugin (libgstrockchiprga.so) providing rgaconvert and
    the two-input rgacompositor PiP/PbP element, in this same package.

 -- CERALIVE <contact@ceralive.tv>  ${changelog_date}
EOF

installed_size="$(du -ks "${stage_dir}" | cut -f1)"
mkdir -p "${stage_dir}/DEBIAN"

# Depends is the ELF NEEDED closure of all FOUR shipped plugins, not the build
# flag list. Under-declaring here fails silently: a build container already has
# the missing library, so the install smoke passes and only a device breaks.
# package-contract.sh re-derives this closure from the staged .so files.
#
#   libgstreamer1.0-0               libgstreamer-1.0.so.0, libgstbase-1.0.so.0
#   libgstreamer-plugins-base1.0-0  libgstvideo/allocators/pbutils-1.0.so.0
#   libglib2.0-0                    libglib-2.0.so.0, libgobject-2.0.so.0
#   libdrm2                         libdrm.so.2          (kmssrc, rkximage)
#   libx11-6                        libX11.so.6          (rkximage)
#   librockchip-mpp1                librockchip_mpp.so.1 (rockchipmpp)
#   librga2                         librga.so.2          (rockchipmpp, rockchiprga)
#
# rockchiprga leaves Depends unchanged, but NOT because its NEEDED set is a
# subset of the MPP plugin's -- it links libgstbase-1.0.so.0, which the MPP
# plugin does not. That SONAME is already inside the declared closure, supplied
# by libgstreamer1.0-0 for kmssrc, so no new package is required.
# The staged closure check re-derives this from the built .so files instead of
# trusting the reasoning here.
#
# Only libc6 is versioned, and that floor IS the target-suite contract
# (GLIBC_FLOOR in ci/target-suite.env, gated by ci/check-glibc-floor.sh). The
# GStreamer names stay unversioned so the package keeps resolving on both the
# bookworm 1.22 and the trixie 1.26 runtime.
#
# libglib2.0-0 is the subtle one: on trixie that name does not exist as a real
# package — the time64 transition renamed it to libglib2.0-0t64, which carries
# `Provides: libglib2.0-0`. Depending on the OLD name unversioned resolves on
# BOTH suites. Naming the t64 package directly would break bookworm, where it
# does not exist at all.
#
# Provides is UNVERSIONED too: nothing in the image Depends on
# gstreamer1.0-rockchip1 at a version, so a version here would only invent a
# constraint to get wrong later.
cat >"${stage_dir}/DEBIAN/control" <<EOF
Package: gstreamer1.0-rockchip-ceralive
Version: ${version}
Architecture: ${arch}
X-CeraLive-Build-Suite: ${build_suite}
Maintainer: CERALIVE <contact@ceralive.tv>
Installed-Size: ${installed_size}
Depends: libgstreamer1.0-0, libgstreamer-plugins-base1.0-0, libglib2.0-0, libc6 (>= ${GLIBC_FLOOR}), libdrm2, libx11-6, librockchip-mpp1, librga2
Provides: gstreamer1.0-rockchip1
Conflicts: gstreamer1.0-rockchip1, belabox-gstreamer1.0-rockchip
Replaces: gstreamer1.0-rockchip1, belabox-gstreamer1.0-rockchip
Section: libs
Priority: optional
Homepage: https://github.com/CERALIVE/gstreamer-rockchip
License: LGPL-2.1
Description: CeraLive-hardened GStreamer plugins for Rockchip RK3588
 The RK3588 hardware encode/decode elements the CeraLive streaming engine runs
 on: mpph264enc, mpph265enc, mppvideodec and mppjpegdec, plus the rgaconvert 2D
 converter and rgacompositor PiP/PbP mixer, backed by Rockchip MPP and RGA.
 .
 This is the CeraLive fork of the Rockchip plugin set. It exists so the elements
 the device streams with are under first-party control: every change it carries
 beyond the packaged radxa build is an individually reviewed entry in the
 verified fix ledger, each one tied to the upstream commit it came from and to
 the test that proves it.
 .
 It replaces gstreamer1.0-rockchip1 and belabox-gstreamer1.0-rockchip, and
 installs the same libgstrockchipmpp.so filename, so nothing downstream has to
 learn a new plugin name.
EOF

mkdir -p "${out_dir}"
deb="${out_dir}/gstreamer1.0-rockchip-ceralive_${version}_${arch}.deb"
dpkg-deb --root-owner-group --build "${stage_dir}" "${deb}"
printf '%s\n' "${deb}"
