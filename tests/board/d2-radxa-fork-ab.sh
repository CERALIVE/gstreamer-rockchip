#!/usr/bin/env bash
# A/B the historical Radxa package and this fork with the same 60 s / 300-AU
# program-encode pipeline, H.265 first as the primary codec and H.264 second as
# the secondary. The fork is restored before exit.
#
# Three criteria, all required: 300/300 access units per codec, zero
# `RGA_BLIT fail` journal lines, and zero CPU-copy fallback frames.
#
# The third cannot be read from journal silence, because the CPU-copy path is
# silent by construction. It is scored instead on the encoder's own counter
# summary, emitted at READY_TO_NULL under GST_DEBUG=mppenc:5. An absent summary
# line FAILS: reading zero out of a line that is not there is fail-open, and
# would score a build carrying no counters at all as a clean run.
#
# That criterion is fork-only. The Radxa baseline has no such counters, so
# demanding the line from it would fail the A/B for the baseline behaving
# exactly as expected.

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
	board_ssh "GST_DEBUG_NO_COLOR=1 GST_DEBUG=mppenc:5 timeout 90 gst-launch-1.0 -v -e videotestsrc is-live=true num-buffers=300 pattern=ball ! video/x-raw,format=I420,width=1920,height=1080,framerate=5/1 ! tee ! queue ! videoconvert ! video/x-raw,format=NV12 ! mpp${codec}enc rc-mode=cbr ${property}=6000000 gop=30 qp-max=51 zero-copy-pkt=false ! identity name=auprobe silent=false ! $parser ! fakesink sync=false" \
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

# Prints `<fallback> <dropped> <layout-rejections>`, or nothing when the element
# emitted no summary at all — an absence the caller must score as a failure.
read_conversion_counters() {
	local log=$1 summary
	summary=$(grep -F 'conversion summary:' "$log" | tail -1 || true)
	[[ -n "$summary" ]] || return 1
	sed -nE 's/.*fallback=([0-9]+) dropped=([0-9]+) layout-rejections=([0-9]+).*/\1 \2 \3/p' \
		<<<"$summary"
}

score_fork_counters() {
	local codec=$1 counters fallback dropped rejections
	if ! counters=$(read_conversion_counters "$REPORT_DIR/fork-$codec.log") ||
		[[ -z "$counters" ]]; then
		printf 'variant=fork codec=%s conversion_summary=MISSING\n' "$codec"
		return 1
	fi
	read -r fallback dropped rejections <<<"$counters"
	printf 'variant=fork codec=%s fallback=%s dropped=%s layout_rejections=%s\n' \
		"$codec" "$fallback" "$dropped" "$rejections"
	[[ "$fallback" -eq 0 ]]
}

run_variant() {
	local variant=$1 deb=$2 property=$3 since rga_blit rga_api failed=0 codec
	install_deb "$deb"
	since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
	for codec in h265 h264; do
		run_codec "$variant" "$codec" "$property" || failed=1
	done
	rga_blit=$(journal_count "$since" 'RGA_BLIT fail')
	rga_api=$(journal_count "$since" 'rga_api version')
	printf 'variant=%s RGA_BLIT_fail=%s rga_api_version=%s\n' "$variant" "$rga_blit" "$rga_api"
	[[ "$rga_blit" -eq 0 && "$rga_api" -eq 0 ]] || failed=1
	if [[ "$variant" == fork ]]; then
		for codec in h265 h264; do
			score_fork_counters "$codec" || failed=1
		done
	fi
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
