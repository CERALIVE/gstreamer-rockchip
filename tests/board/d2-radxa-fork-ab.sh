#!/usr/bin/env bash
# A/B the historical Radxa package and this fork with the same 60 s / 300-AU
# H.264 and H.265 program-encode pipeline. The fork is restored before exit.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tests/board/board-lib.sh
source "$ROOT/tests/board/board-lib.sh"

: "${RADXA_DEB:?RADXA_DEB must name the gstreamer1.0-rockchip1 1.14-4 arm64 .deb}"
: "${FORK_DEB:?FORK_DEB must name the CeraLive arm64 .deb}"
new_report_dir d2-radxa-fork-ab
exec > >(tee "$REPORT_DIR/transcript.log") 2>&1
board_preflight

restore_fork() {
	install_deb "$FORK_DEB" >/dev/null 2>&1 || true
}
trap restore_fork EXIT INT TERM

run_codec() {
	local variant=$1 codec=$2 property=$3 parser log remote_log aus errors caps
	parser="${codec}parse"
	log="$REPORT_DIR/$variant-$codec.log"
	remote_log="/tmp/ceralive-$variant-$codec.log"
	board_ssh "timeout 90 gst-launch-1.0 -v -e videotestsrc is-live=true num-buffers=300 pattern=ball ! video/x-raw,format=I420,width=1920,height=1080,framerate=5/1 ! tee ! queue ! videoconvert ! video/x-raw,format=NV12 ! mpp${codec}enc rc-mode=cbr ${property}=6000000 gop=30 qp-max=51 zero-copy-pkt=false ! identity name=auprobe silent=false ! $parser ! fakesink sync=false" \
		>"$log" 2>&1 || true
	board_scp "$log" "$BOARD_TARGET:$remote_log" >/dev/null
	aus=$(grep -c 'GstIdentity:auprobe: last-message = chain' "$log" || true)
	errors=$(grep -cE 'ERROR|CRITICAL|not-negotiated|Internal data stream error' "$log" || true)
	caps=$(grep -E "Gst${codec^^}Parse:.*GstPad:src: caps =" "$log" | tail -1 || true)
	printf 'variant=%s codec=%s aus=%s/300 errors=%s\n' "$variant" "$codec" "$aus" "$errors"
	printf 'parsed_caps=%s\n' "${caps:-missing}"
	[[ "$aus" -eq 300 && "$errors" -eq 0 ]] || return 1
	grep -q 'width=(int)1920' <<<"$caps" && grep -q 'height=(int)1080' <<<"$caps" || return 1
	grep -q 'profile=(string)' <<<"$caps" && grep -q 'level=(string)' <<<"$caps" || return 1
}

run_variant() {
	local variant=$1 deb=$2 property=$3 since rga_blit rga_api failed=0
	install_deb "$deb"
	since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
	run_codec "$variant" h264 "$property" || failed=1
	run_codec "$variant" h265 "$property" || failed=1
	rga_blit=$(journal_count "$since" 'RGA_BLIT fail')
	rga_api=$(journal_count "$since" 'rga_api version')
	printf 'variant=%s RGA_BLIT_fail=%s rga_api_version=%s\n' "$variant" "$rga_blit" "$rga_api"
	[[ "$rga_blit" -eq 0 && "$rga_api" -eq 0 ]] || failed=1
	return "$failed"
}

result=0
run_variant radxa "$RADXA_DEB" bps || result=1
run_variant fork "$FORK_DEB" bitrate || result=1
trap - EXIT INT TERM

if [[ "$result" -ne 0 ]]; then
	seal_report FAIL || true
	exit 1
fi
seal_report PASS
