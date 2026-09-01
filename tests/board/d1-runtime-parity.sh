#!/usr/bin/env bash
# Installs the CeraLive package, proves all nine factories register, then runs
# the four-element normalized golden contract against the live board.

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
	mpph264enc mpph265enc mppvp8enc mppjpegenc mppvideodec mppjpegdec
	mppvpxalphadecodebin kmssrc rkximagesink
)

board_ssh 'gst-inspect-1.0 rockchipmpp; gst-inspect-1.0 kmssrc; gst-inspect-1.0 rkximage' \
	>"$REPORT_DIR/plugin-inventories.txt"
for factory in "${FACTORIES[@]}"; do
	if ! board_ssh "GST_DEBUG=0 gst-inspect-1.0 '$factory'" >"$REPORT_DIR/$factory.inspect" 2>&1; then
		echo "FAIL: factory did not register: $factory"
		seal_report FAIL || true
		exit 1
	fi
	printf 'factory=%s registered=yes\n' "$factory"
done

if ! BOARD_IP="$BOARD_IP" BOARD_SSH_USER="$BOARD_SSH_USER" \
	BOARD_SSH_PASS="$BOARD_SSH_PASS" bash "$ROOT/tests/parity-check.sh"; then
	echo 'FAIL: detailed four-element golden contract failed'
	seal_report FAIL || true
	exit 1
fi

seal_report PASS
