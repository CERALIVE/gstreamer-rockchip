#!/usr/bin/env bash
# D6: the C6b-perf / C6b-async go/no-go measurement.
#
# Both halves of C6b beyond colour are conditional on a measured gate, and the
# gate's subject is the librga seam itself, not a GStreamer pipeline. This drill
# therefore runs a standalone im2d harness against the board's own installed
# librga runtime: no plugin is installed, no element is instantiated, no capture
# device is opened and the engine is not touched. What it measures is exactly
# what the two proposals would change.
#
# C6b-perf asks whether pre-importing a DMA-BUF once per pool buffer
# (`importbuffer_fd` + `wrapbuffer_handle`) is cheaper than describing it by fd
# on every frame. The harness answers that by running the SAME conversion on the
# SAME two buffers both ways, plus a single-plane RGBA control that isolates a
# handle-path refusal from any chroma-plane descriptor question.
#
# C6b-async asks whether submitting frame N with IM_ASYNC while retiring frame
# N-1's release fence raises sustained throughput. The harness brackets the async
# run with two independent synchronous runs — one before every other mode, one
# after — so a drift in clocks or die temperature across the run shows up as a
# disagreement between those two rather than as an async gain.
#
# Submissions are sequential and single-threaded and every fence is polled to a
# terminal state before its buffers are released, so the known rga_job_commit
# use-after-free (reachable through crash/fault injection during live
# composition) is not exercised.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tests/board/board-lib.sh
source "$ROOT/tests/board/board-lib.sh"

readonly BUILD_IMAGE="${BOARD_BUILD_IMAGE:-localhost/gstrk-trixie-arm64}"
readonly ITERS="${D6_ITERS:-400}"
readonly WARMUP="${D6_WARMUP:-40}"
readonly ASYNC_GAIN_MIN_PCT="${D6_ASYNC_GAIN_MIN_PCT:-5}"

command -v podman >/dev/null 2>&1 || {
	echo 'FAIL: podman is required for the arm64 harness build' >&2
	exit 1
}
command -v python3 >/dev/null 2>&1 || {
	echo 'FAIL: python3 is required to score the gates' >&2
	exit 1
}

new_report_dir d6-c6b-measurement
exec > >(tee "$REPORT_DIR/transcript.log") 2>&1
board_preflight

remote_scratch="/tmp/ceralive-c6b-$$"
helper_dir=$(mktemp -d)
# shellcheck disable=SC2317
cleanup() {
	board_ssh "rm -rf '$remote_scratch'" >/dev/null 2>&1 || true
	rm -rf "$helper_dir"
}
trap cleanup EXIT INT TERM

board_ssh 'test -c /dev/rga' || {
	echo 'FAIL: /dev/rga is not a character device on this board'
	seal_report FAIL || true
	exit 1
}
board_ssh "dpkg-query -W -f='\${Package} \${Version}\n' librga2-ceralive librockchip-mpp1 gstreamer1.0-rockchip-ceralive" \
	>"$REPORT_DIR/board-packages.txt" 2>&1 || true

podman run --rm --platform linux/arm64 --userns=keep-id \
	-v "$ROOT:/src:ro" -v "$helper_dir:/out" "$BUILD_IMAGE" bash -lc \
	'gcc -O2 -std=gnu11 -Wall -Wextra -Werror /src/tests/board/c6b-im2d-bench.c -o /out/c6b-im2d-bench -lrga'
sha256sum "$helper_dir/c6b-im2d-bench" >"$REPORT_DIR/harness.sha256"

board_ssh "mkdir -p '$remote_scratch'"
board_scp "$helper_dir/c6b-im2d-bench" "$BOARD_TARGET:$remote_scratch/c6b-im2d-bench"
board_ssh "chmod 0755 '$remote_scratch/c6b-im2d-bench'"

for geometry in 3840x2160 1920x1080; do
	width=${geometry%x*}
	height=${geometry#*x}
	board_ssh "'$remote_scratch/c6b-im2d-bench' --width $width --height $height --iters $ITERS --warmup $WARMUP" \
		>"$REPORT_DIR/bench-$geometry.txt" 2>&1 || true
	grep -E '^(MODE|FAILURES|PROBE|GEOMETRY)' "$REPORT_DIR/bench-$geometry.txt" || true
done

python3 - "$REPORT_DIR" "$ASYNC_GAIN_MIN_PCT" <<'PY' | tee "$REPORT_DIR/verdicts.txt"
import pathlib
import re
import sys

report = pathlib.Path(sys.argv[1])
gate = float(sys.argv[2])
failed = False

for path in sorted(report.glob("bench-*.txt")):
    geometry = path.stem.removeprefix("bench-")
    modes = {}
    for line in path.read_text(errors="replace").splitlines():
        match = re.match(r"^MODE=(\S+) .*?mean_us=([\d.]+).*?fps=([\d.]+)", line)
        if match:
            modes[match.group(1)] = (float(match.group(2)), float(match.group(3)))
    handle_ok = "FAILURES_handle=0" in path.read_text(errors="replace")
    probe = "PROBE_rgba_handle_copy status=1" in path.read_text(errors="replace")

    sync = modes.get("fd_sync_sustained")
    async_ = modes.get("async_depth1_sustained")
    if not sync or not async_:
        print(f"{geometry} async=INCOMPLETE")
        failed = True
    else:
        gain = (async_[1] / sync[1] - 1.0) * 100.0
        verdict = "ADOPT-RECOMMENDED" if gain >= gate else "NO-MEASURABLE-GAIN"
        print(
            f"{geometry} async sync_fps={sync[1]:.1f} async_fps={async_[1]:.1f} "
            f"gain_pct={gain:+.1f} gate_pct={gate:.1f} verdict={verdict}"
        )

    if handle_ok and "handle_sync" in modes:
        fd = modes.get("fd_sync")
        handle = modes["handle_sync"]
        gain = (handle[1] / fd[1] - 1.0) * 100.0 if fd else 0.0
        verdict = "ADOPT-RECOMMENDED" if gain >= gate else "NO-MEASURABLE-GAIN"
        print(
            f"{geometry} perf fd_fps={fd[1]:.1f} handle_fps={handle[1]:.1f} "
            f"gain_pct={gain:+.1f} gate_pct={gate:.1f} verdict={verdict}"
        )
    else:
        print(
            f"{geometry} perf verdict=BLOCKED handle_path_usable=no "
            f"rgba_single_plane_control_passed={'yes' if probe else 'no'}"
        )

sys.exit(1 if failed else 0)
PY

board_ssh "rm -rf '$remote_scratch'"
seal_report PASS
