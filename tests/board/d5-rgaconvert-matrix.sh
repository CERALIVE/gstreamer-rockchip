#!/usr/bin/env bash
# rgaconvert conversion matrix: {CSC, scale, crop, rotate} x representative
# format pairs, each cell scored as PSNR against a software reference that
# performs the SAME operation on the SAME source frame.
#
# Both legs generate their own frame from `videotestsrc pattern=smpte100`, which
# is deterministic for a given pattern, caps and buffer index, so the two legs
# start from identical pixels without shipping a fixture.
#
# Every geometry in the matrix is a multiple of 16 on BOTH axes. That is load
# bearing, not tidiness: the element's DMA-BUF pool rounds width and height up
# to 16, so a non-aligned geometry would pad the hardware buffer, filesink would
# write the padding, and the byte-for-byte comparison against the unpadded
# software reference would measure the padding instead of the conversion.
#
# GStreamer 1.22 videotestsrc does not honour a downstream DMA-BUF allocation
# proposal. The board helper therefore imports the generated source bytes into a
# dma-heap allocation and pushes one explicitly annotated DMA-BUF through appsrc.
# It also observes the DUT output buffer and refuses non-DMA-BUF memory.
#
# PSNR is a fidelity measure, not an equality test: RGA's fixed-function chroma
# resampling does not match libgstvideo's, so an exact match is not expected on
# any converting cell. Luma and chroma are scored separately and the cell takes
# the worse of the two, because a chroma-only defect is invisible in a whole
# buffer average.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tests/board/board-lib.sh
source "$ROOT/tests/board/board-lib.sh"

: "${FORK_DEB:?FORK_DEB must name the CeraLive arm64 .deb}"
readonly PSNR_MIN="${D5_PSNR_MIN:-30}"
readonly BUILD_IMAGE="${BOARD_BUILD_IMAGE:-localhost/gstrk-bookworm-arm64}"
readonly SRC_WIDTH=1280
readonly SRC_HEIGHT=720
readonly SRC_CAPS="width=$SRC_WIDTH,height=$SRC_HEIGHT,framerate=30/1"

command -v python3 >/dev/null 2>&1 || {
	echo 'FAIL: python3 is required to score the PSNR matrix' >&2
	exit 1
}
command -v podman >/dev/null 2>&1 || {
	echo 'FAIL: podman is required for the arm64 DMA-BUF helper build' >&2
	exit 1
}

new_report_dir d5-rgaconvert-matrix
exec > >(tee "$REPORT_DIR/transcript.log") 2>&1
board_preflight
install_deb "$FORK_DEB"

remote_scratch="/tmp/ceralive-rgaconvert-matrix-$$"
helper_dir=$(mktemp -d)
# shellcheck disable=SC2317
cleanup() {
	board_ssh "rm -rf '$remote_scratch'" >/dev/null 2>&1 || true
	rm -rf "$helper_dir"
}
trap cleanup EXIT INT TERM
board_ssh "mkdir -p '$remote_scratch'"
podman run --rm --platform linux/arm64 --userns=keep-id \
	-v "$ROOT:/src:ro" -v "$helper_dir:/out" "$BUILD_IMAGE" bash -lc \
	'gcc -std=c11 -Wall -Wextra -Werror /src/tests/board/dmabuf-rgaconvert.c -o /out/dmabuf-rgaconvert $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0)'
board_scp "$helper_dir/dmabuf-rgaconvert" "$BOARD_TARGET:$remote_scratch/dmabuf-rgaconvert"
board_ssh "chmod 0755 '$remote_scratch/dmabuf-rgaconvert'"

printf 'psnr_min_db=%s\n' "$PSNR_MIN"
board_ssh 'test -c /dev/rga' || {
	echo 'FAIL: /dev/rga is not a character device on this board'
	seal_report FAIL || true
	exit 1
}
for element in rgaconvert videotestsrc rawvideoparse videoconvert videoscale videocrop videoflip filesink; do
	board_ssh "GST_DEBUG=0 gst-inspect-1.0 '$element' >/dev/null" || {
		echo "FAIL: required element unavailable on the board: $element"
		seal_report FAIL || true
		exit 1
	}
done

psnr_script="$REPORT_DIR/psnr.py"
cat >"$psnr_script" <<'PY'
import math
import sys

hardware, reference, width, height = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])

with open(hardware, "rb") as handle:
    produced = handle.read()
with open(reference, "rb") as handle:
    expected = handle.read()

if not produced or not expected:
    print("EMPTY produced=%d expected=%d" % (len(produced), len(expected)))
    raise SystemExit(2)
if len(produced) != len(expected):
    print("SIZE_MISMATCH produced=%d expected=%d" % (len(produced), len(expected)))
    raise SystemExit(2)

luma = width * height
if len(produced) < luma:
    print("SHORT_LUMA produced=%d luma=%d" % (len(produced), luma))
    raise SystemExit(2)


def psnr(left, right):
    if not left:
        return None
    error = 0
    for a, b in zip(left, right):
        delta = a - b
        error += delta * delta
    mse = error / len(left)
    if mse == 0:
        return math.inf
    return 10 * math.log10(255 * 255 / mse)


def render(value):
    if value is None:
        return "n/a"
    if value == math.inf:
        return "inf"
    return "%.2f" % value


luma_psnr = psnr(produced[:luma], expected[:luma])
chroma_psnr = psnr(produced[luma:], expected[luma:])
scored = [value for value in (luma_psnr, chroma_psnr) if value is not None]
print("PSNR luma=%s chroma=%s worst=%s" % (
    render(luma_psnr), render(chroma_psnr), render(min(scored) if scored else None)))
PY

matrix="$REPORT_DIR/matrix.tsv"
printf 'operation\tin_format\tout_format\tout_width\tout_height\tverdict\tpsnr_luma_db\tpsnr_chroma_db\tfallback\tdropped\tnote\n' >"$matrix"

record_cell() {
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$matrix"
}

# Prints the element's own end-of-run counters, or nothing when it emitted no
# summary at all — an absence the caller scores as a failed cell.
read_conversion_counters() {
	local log=$1 summary
	summary=$(grep -E '^DUT_FALLBACK=[0-9]+ DUT_DROPPED=[0-9]+ DUT_LAYOUT_REJECTIONS=[0-9]+$' "$log" | tail -1 || true)
	[[ -n "$summary" ]] || return 1
	sed -nE 's/DUT_FALLBACK=([0-9]+) DUT_DROPPED=([0-9]+) DUT_LAYOUT_REJECTIONS=([0-9]+)/\1 \2 \3/p' \
		<<<"$summary"
}

run_cell() {
	local operation=$1 in_format=$2 out_format=$3
	local parse_format=${in_format,,}
	local input_colorimetry=",colorimetry=bt601"
	local output_colorimetry=bt601
	local label="$operation-$in_format-to-$out_format"
	local reference_filter="videoconvert" out_w out_h
	local hw_log="$REPORT_DIR/$label.hw.log"
	local ref_log="$REPORT_DIR/$label.ref.log"
	local hw_raw="$REPORT_DIR/$label.hw.raw"
	local ref_raw="$REPORT_DIR/$label.ref.raw"
	local remote_input="$remote_scratch/$label.input.raw"
	local counters fallback dropped rejections psnr_line luma chroma worst
	(( SRC_HEIGHT > 576 )) && input_colorimetry=",colorimetry=bt709"
	[[ "$in_format" == BGR ]] && input_colorimetry=""

	case "$operation" in
		csc)
			out_w=$SRC_WIDTH out_h=$SRC_HEIGHT
			;;
		scale)
			out_w=640 out_h=480
			reference_filter="videoconvert ! videoscale add-borders=false"
			;;
		crop)
			out_w=1024 out_h=576
			reference_filter="videocrop left=64 right=192 top=48 bottom=96 ! videoconvert"
			;;
		rotate)
			out_w=$SRC_HEIGHT out_h=$SRC_WIDTH
			reference_filter="videoflip method=clockwise ! videoconvert"
			;;
		*)
			echo "FAIL: unknown matrix operation: $operation"
			return 1
			;;
	esac
	(( out_h > 576 )) && output_colorimetry=bt709
	if [[ "$in_format" == NV16 && ("$operation" == crop || "$operation" == rotate) ]]; then
		reference_filter="videoconvert ! video/x-raw,format=I420 ! $reference_filter"
	fi

	printf '\n== cell %s -> %s (%sx%s) ==\n' "$label" "$out_format" "$out_w" "$out_h"

	if ! board_ssh "timeout 60 gst-launch-1.0 -e videotestsrc num-buffers=1 pattern=smpte100 ! video/x-raw,format=$in_format,$SRC_CAPS ! filesink location='$remote_input'" \
		>"$REPORT_DIR/$label.source.log" 2>&1; then
		record_cell "$operation" "$in_format" "$out_format" "$out_w" "$out_h" \
			FAIL n/a n/a n/a n/a 'source frame generation failed'
		return 1
	fi

	if ! board_ssh "GST_DEBUG_NO_COLOR=1 GST_DEBUG=rgaconvert:5 timeout 60 '$remote_scratch/dmabuf-rgaconvert' '$remote_input' '$in_format' '$remote_scratch/$label.hw.raw' '$out_format' '$out_w' '$out_h' '$operation'" \
		>"$hw_log" 2>&1; then
		record_cell "$operation" "$in_format" "$out_format" "$out_w" "$out_h" \
			FAIL n/a n/a n/a n/a 'hardware pipeline failed'
		return 1
	fi

	if ! grep -q '^INPUT_DMABUF=1 INPUT_MEMORIES=1$' "$hw_log" ||
		! grep -q '^OUTPUT_SEEN=1 OUTPUT_DMABUF=1$' "$hw_log"; then
		record_cell "$operation" "$in_format" "$out_format" "$out_w" "$out_h" \
			FAIL n/a n/a n/a n/a 'DUT input/output was not one DMA-BUF'
		return 1
	fi

	if ! board_ssh "timeout 60 gst-launch-1.0 -e filesrc location='$remote_input' ! rawvideoparse format=$parse_format width=$SRC_WIDTH height=$SRC_HEIGHT framerate=1/1 ! video/x-raw,format=$in_format,width=$SRC_WIDTH,height=$SRC_HEIGHT$input_colorimetry ! $reference_filter ! video/x-raw,format=$out_format,width=$out_w,height=$out_h,colorimetry=$output_colorimetry ! filesink location='$remote_scratch/$label.ref.raw'" \
		>"$ref_log" 2>&1; then
		record_cell "$operation" "$in_format" "$out_format" "$out_w" "$out_h" \
			FAIL n/a n/a n/a n/a 'software reference pipeline failed'
		return 1
	fi

	board_scp "$BOARD_TARGET:$remote_scratch/$label.hw.raw" "$hw_raw" >/dev/null
	board_scp "$BOARD_TARGET:$remote_scratch/$label.ref.raw" "$ref_raw" >/dev/null

	if ! counters=$(read_conversion_counters "$hw_log") || [[ -z "$counters" ]]; then
		record_cell "$operation" "$in_format" "$out_format" "$out_w" "$out_h" \
			FAIL n/a n/a n/a n/a 'rgaconvert emitted no counter summary'
		return 1
	fi
	read -r fallback dropped rejections <<<"$counters"
	printf 'fallback=%s dropped=%s layout_rejections=%s\n' \
		"$fallback" "$dropped" "$rejections"

	if ! psnr_line=$(python3 "$psnr_script" "$hw_raw" "$ref_raw" "$out_w" "$out_h"); then
		printf 'psnr_error=%s\n' "$psnr_line"
		record_cell "$operation" "$in_format" "$out_format" "$out_w" "$out_h" \
			FAIL n/a n/a "$fallback" "$dropped" "${psnr_line:-psnr comparison failed}"
		return 1
	fi
	printf '%s\n' "$psnr_line"
	luma=$(sed -nE 's/.*luma=([^ ]+).*/\1/p' <<<"$psnr_line")
	chroma=$(sed -nE 's/.*chroma=([^ ]+).*/\1/p' <<<"$psnr_line")
	worst=$(sed -nE 's/.*worst=([^ ]+).*/\1/p' <<<"$psnr_line")

	if [[ "$fallback" -ne 0 || "$dropped" -ne 0 ]]; then
		record_cell "$operation" "$in_format" "$out_format" "$out_w" "$out_h" \
			FAIL "$luma" "$chroma" "$fallback" "$dropped" 'conversion left the silicon path'
		return 1
	fi

	if [[ "$worst" != inf ]] &&
		! awk -v worst="$worst" -v floor="$PSNR_MIN" 'BEGIN { exit !(worst >= floor) }'; then
		record_cell "$operation" "$in_format" "$out_format" "$out_w" "$out_h" \
			FAIL "$luma" "$chroma" "$fallback" "$dropped" "worst PSNR ${worst} dB below ${PSNR_MIN} dB"
		return 1
	fi

	record_cell "$operation" "$in_format" "$out_format" "$out_w" "$out_h" \
		PASS "$luma" "$chroma" "$fallback" "$dropped" "worst ${worst} dB"
}

result=0
for operation in csc scale crop rotate; do
	for pair in NV12:NV16 NV16:NV12 BGR:NV12; do
		run_cell "$operation" "${pair%%:*}" "${pair##*:}" || result=1
	done
done

printf '\n== conversion matrix ==\n'
cat "$matrix"

if [[ "$result" -ne 0 ]]; then
	seal_report FAIL || true
	exit 1
fi
seal_report PASS
