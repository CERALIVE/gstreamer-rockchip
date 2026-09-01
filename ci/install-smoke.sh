#!/usr/bin/env bash
# Clean-container install smoke for gstreamer1.0-rockchip-ceralive.
#
# Runs INSIDE a fresh `debian:<suite>-slim` arm64 container. It is the producer
# side of the two questions a device actually asks of this package:
#
#   1. is the dependency closure COMPLETE?  -> `ldd`, zero "not found"
#   2. does the plugin actually LOAD?       -> `gst-inspect-1.0 --plugin rockchipmpp`
#
# Both are needed, and neither substitutes for the other. `apt-get install`
# succeeding proves only that the DECLARED dependencies resolve, never that they
# are SUFFICIENT: a package with an under-declared Depends installs cleanly on a
# bare container and then shows five unresolved SONAMEs. That failure shipped
# once already (the todo-23 review round), and it was invisible because the first
# smoke ran in the BUILD container, where every missing library was already
# present. Hence the two rules this script exists to enforce:
#
#   * the container starts BARE. No build dependencies, no GStreamer, nothing
#     but the two pinned runtime libraries the device image itself installs
#     (librockchip-mpp1, librga2) -- and the baseline is ASSERTED, not assumed.
#   * `gstreamer1.0-tools` is installed only AFTER the `ldd` check has run. It
#     is the tool that answers question 2, but installing it earlier would pull
#     GStreamer libraries in and mask exactly the gap question 1 looks for.
#
# usage: install-smoke.sh <release .deb> <dir holding the pinned runtime .debs>
set -euo pipefail

deb_arg="${1:?usage: install-smoke.sh <deb> <runtime-deb-dir>}"
runtime_arg="${2:?usage: install-smoke.sh <deb> <runtime-deb-dir>}"

# apt treats a bare relative path as a package NAME; only an absolute or
# ./-prefixed path is read as a local archive.
deb="$(readlink -f "${deb_arg}")"
runtime_dir="$(readlink -f "${runtime_arg}")"

readonly PACKAGE="gstreamer1.0-rockchip-ceralive"
readonly TRIPLET="aarch64-linux-gnu"
readonly PLUGIN_SO="/usr/lib/${TRIPLET}/gstreamer-1.0/libgstrockchipmpp.so"
# The build enables rkximage + rockchipmpp + kmssrc, so the package ships three
# plugins and no more. Asserting the count turns a silently dropped plugin --
# meson's `auto` features fail that way -- into a red smoke.
readonly EXPECT_PLUGIN_COUNT=3
# Decoders register unconditionally (gst_mpp_video_dec_register / _jpeg_dec_).
# These are the F13 runtime proof: the package built on the target suite must
# LOAD on the suite under test, including trixie's GStreamer 1.26.
readonly REQUIRED_FACTORIES="mppvideodec mppjpegdec"
# Encoders gate themselves on gst_mpp_enc_supported(), which needs a Rockchip
# VPU. They are REPORTED here, never asserted -- CI has no board, and
# tests/parity-check.sh applies the same off-board rule.
readonly HARDWARE_GATED_FACTORIES="mpph264enc mpph265enc mppvp8enc mppjpegenc"

fail() { printf '\ninstall-smoke: FAIL: %s\n' "$1" >&2; exit 1; }
step() { printf '\n== %s ==\n' "$1"; }

export DEBIAN_FRONTEND=noninteractive

step "container identity"
grep -E '^(PRETTY_NAME|VERSION_CODENAME)=' /etc/os-release
suite="$(sed -n 's/^VERSION_CODENAME=//p' /etc/os-release)"
printf 'dpkg architecture: %s\n' "$(dpkg --print-architecture)"
printf 'package under test: %s\n' "${deb}"

# `grep -c` exits 1 for a legitimate count of zero and >1 for a real error, so a
# bare `|| true` would report a BROKEN package query as a clean container --
# turning the assertion below into a rubber stamp. Discriminate the two.
assert_no_gstreamer() {
	local when="$1" list count rc
	list="$(dpkg-query -W -f='${binary:Package}\n' 2>/dev/null || true)"
	count="$(printf '%s\n' "${list}" | grep -c gstreamer)" || rc=$?
	rc="${rc:-0}"
	[ "${rc}" -le 1 ] || fail "could not enumerate installed packages ${when} (grep exit ${rc})"
	printf 'gstreamer packages installed %s: %s\n' "${when}" "${count}"
	[ "${count}" = "0" ] \
		|| fail "${count} gstreamer package(s) present ${when}; the closure check below would be vacuous"
}

step "baseline: the container must carry NOTHING GStreamer"
apt-get update -qq
assert_no_gstreamer "at baseline"

step "install ONLY the two pinned runtime libraries the device image installs"
runtime_debs=()
for pattern in 'librockchip-mpp1_*.deb' 'librga2_*.deb'; do
	mapfile -t matches < <(find "${runtime_dir}" -maxdepth 1 -name "${pattern}" | sort)
	[ "${#matches[@]}" -eq 1 ] \
		|| fail "${pattern} matched ${#matches[@]} files in ${runtime_dir} (expected exactly 1)"
	runtime_debs+=("${matches[0]}")
done
apt-get install -y --no-install-recommends "${runtime_debs[@]}"

assert_no_gstreamer "after the pinned runtime libraries"

step "install the release .deb"
apt-get install -y "${deb}"
dpkg-query -W -f='installed: ${binary:Package} ${Version} ${Architecture}\n' "${PACKAGE}"

# SCOPE, stated rather than implied: apt installs the DECLARED Depends, so this
# oracle catches a dependency that is missing outright -- proven with a negative
# control, where the previously under-declared package installed cleanly here and
# then showed five unresolved SONAMEs. What it cannot catch is a library that is
# undeclared but arrives anyway as a transitive dependency of a declared one.
# That case is covered statically instead: packaging/package-contract.sh derives
# the plugins' NEEDED set from the staged tree and refuses any SONAME the
# declared Depends does not supply. The two gates are complementary; neither
# alone closes under-declaration.
step "oracle 1 -- dependency closure: ldd every installed plugin, zero 'not found'"
mapfile -t plugins < <(dpkg -L "${PACKAGE}" | grep -E '\.so$' | sort)
printf 'plugins installed: %s\n' "${#plugins[@]}"
[ "${#plugins[@]}" -eq "${EXPECT_PLUGIN_COUNT}" ] \
	|| fail "expected ${EXPECT_PLUGIN_COUNT} plugin .so files, dpkg -L lists ${#plugins[@]}: ${plugins[*]}"
printf '%s\n' "${plugins[@]}" | grep -qxF "${PLUGIN_SO}" \
	|| fail "the FROZEN plugin path ${PLUGIN_SO} is not installed"

unresolved=0
for so in "${plugins[@]}"; do
	printf -- '--- ldd %s\n' "${so}"
	ldd_out="$(ldd "${so}" 2>&1)"
	printf '%s\n' "${ldd_out}"
	if grep -q 'not found' <<<"${ldd_out}"; then
		unresolved=1
	fi
done
[ "${unresolved}" -eq 0 ] \
	|| fail "a shipped plugin has an unresolved SONAME -- Depends is under-declared, and this package would install on a device and never load"
printf 'closure complete: no unresolved SONAMEs across %s plugins\n' "${#plugins[@]}"

# Deliberately AFTER oracle 1. gstreamer1.0-tools drags in GStreamer libraries,
# which would have satisfied a missing Depends and hidden the gap above.
step "oracle 2 -- the plugin LOADS on this suite's GStreamer"
apt-get install -y --no-install-recommends gstreamer1.0-tools
printf 'runtime %s\n' "$(gst-inspect-1.0 --version | sed -n '2p')"

# A non-zero exit here IS the failure: gst-inspect refuses a plugin it cannot
# load, whatever the reason (missing symbol, ABI refusal, wrong path).
inspect_out="$(gst-inspect-1.0 --plugin rockchipmpp)"
printf '%s\n' "${inspect_out}"

# gst-inspect prints its plugin details column-aligned and WITHOUT a colon
# ("Filename    /usr/lib/..."), so field-extract and compare literally rather
# than pattern-matching a punctuation shape that does not exist.
loaded_from="$(awk '$1 == "Filename" { print $2; exit }' <<<"${inspect_out}")"
[ "${loaded_from}" = "${PLUGIN_SO}" ] \
	|| fail "gst-inspect loaded rockchipmpp from '${loaded_from}', not ${PLUGIN_SO}"

# Close the loop: the plugin GStreamer just loaded is the one THIS package owns.
owner="$(dpkg -S "${PLUGIN_SO}" | cut -d: -f1)"
[ "${owner}" = "${PACKAGE}" ] \
	|| fail "${PLUGIN_SO} is owned by ${owner}, not ${PACKAGE}"

for factory in ${REQUIRED_FACTORIES}; do
	grep -qE "^[[:space:]]+${factory}:" <<<"${inspect_out}" \
		|| fail "factory ${factory} did not register -- the plugin loaded but is element-less"
	printf 'factory registered: %s\n' "${factory}"
done

step "hardware-gated factories (reported, not asserted)"
for factory in ${HARDWARE_GATED_FACTORIES}; do
	if grep -qE "^[[:space:]]+${factory}:" <<<"${inspect_out}"; then
		printf '%s: registered\n' "${factory}"
	else
		printf '%s: absent -- gst_mpp_enc_supported() found no VPU (expected off-board)\n' "${factory}"
	fi
done

printf '\ninstall-smoke: OK (%s -- closure complete, plugin loads, %s registered)\n' \
	"${suite}" "${REQUIRED_FACTORIES}"
