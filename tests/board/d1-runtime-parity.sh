#!/usr/bin/env bash
# Installs the CeraLive package, proves every factory the board's SoC can carry
# registers, then runs the four-element normalized golden contract against the
# live board.
#
# `mppvp8enc` is the one factory whose absence is not a failure. It needs a
# VEPU2 VP8 encoder block, which RK3588 does not have; the historical Radxa
# package is absent in exactly the same way on the same board, so scoring it as
# a registration failure would fail the drill for silicon. It is therefore
# EXPECTED-ABSENT when the board's device-tree `compatible` names rk3588, and
# PRESENT-REQUIRED on every other SoC — which keeps the criterion honest rather
# than merely permissive: an rk3588 board that DOES register it is a surprise
# worth failing on, because it means the element is registering without the
# hardware it claims to need.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tests/board/board-lib.sh
source "$ROOT/tests/board/board-lib.sh"

: "${FORK_DEB:?FORK_DEB must name the gstreamer1.0-rockchip-ceralive arm64 .deb}"
new_report_dir d1-runtime-parity
exec > >(tee "$REPORT_DIR/transcript.log") 2>&1

board_preflight
install_deb "$FORK_DEB"
board_ssh "dpkg-query -W -f='package=\${Package}\nversion=\${Version}\narchitecture=\${Architecture}\n' gstreamer1.0-rockchip-ceralive"

readonly -a FACTORIES=(
	mpph264enc mpph265enc mppjpegenc mppvideodec mppjpegdec
	mppvpxalphadecodebin kmssrc rkximagesink rgaconvert
)
readonly SOC_GATED_FACTORY=mppvp8enc

soc_compatible=$(board_ssh "tr '\\0' '\\n' </proc/device-tree/compatible | paste -sd, -")
printf 'soc_compatible=%s\n' "$soc_compatible"
if [[ "$soc_compatible" == *rk3588* ]]; then
	vp8_expectation=EXPECTED-ABSENT
else
	vp8_expectation=PRESENT-REQUIRED
fi
printf 'factory=%s expectation=%s\n' "$SOC_GATED_FACTORY" "$vp8_expectation"

board_ssh 'gst-inspect-1.0 rockchipmpp; gst-inspect-1.0 rockchiprga; gst-inspect-1.0 kmssrc; gst-inspect-1.0 rkximage' \
	>"$REPORT_DIR/plugin-inventories.txt"
for factory in "${FACTORIES[@]}"; do
	if ! board_ssh "GST_DEBUG=0 gst-inspect-1.0 '$factory'" >"$REPORT_DIR/$factory.inspect" 2>&1; then
		echo "FAIL: factory did not register: $factory"
		seal_report FAIL || true
		exit 1
	fi
	printf 'factory=%s registered=yes\n' "$factory"
done

vp8_registered=no
board_ssh "GST_DEBUG=0 gst-inspect-1.0 '$SOC_GATED_FACTORY'" \
	>"$REPORT_DIR/$SOC_GATED_FACTORY.inspect" 2>&1 && vp8_registered=yes
printf 'factory=%s registered=%s expectation=%s\n' \
	"$SOC_GATED_FACTORY" "$vp8_registered" "$vp8_expectation"
if [[ "$vp8_expectation" == PRESENT-REQUIRED && "$vp8_registered" == no ]]; then
	echo "FAIL: factory did not register: $SOC_GATED_FACTORY (this SoC declares VP8 encode silicon)"
	seal_report FAIL || true
	exit 1
fi
if [[ "$vp8_expectation" == EXPECTED-ABSENT && "$vp8_registered" == yes ]]; then
	echo "FAIL: $SOC_GATED_FACTORY registered on an rk3588 board, which has no VEPU2 VP8 block"
	seal_report FAIL || true
	exit 1
fi

if ! BOARD_IP="$BOARD_IP" BOARD_SSH_USER="$BOARD_SSH_USER" \
	BOARD_SSH_PASS="$BOARD_SSH_PASS" bash "$ROOT/tests/parity-check.sh"; then
	echo 'FAIL: detailed four-element golden contract failed'
	seal_report FAIL || true
	exit 1
fi

seal_report PASS
