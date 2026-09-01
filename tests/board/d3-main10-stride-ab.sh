#!/usr/bin/env bash
# REPORT-ONLY H4-B3 investigation. Builds two scratch trees and compares their
# board decode against software reference frames. No installed plugin is changed.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tests/board/board-lib.sh
source "$ROOT/tests/board/board-lib.sh"

readonly FIXTURE="$ROOT/tests/fixtures/main10-318x178-10frames.hevc"
readonly BUILD_IMAGE="${BOARD_BUILD_IMAGE:-localhost/gstrk-trixie-arm64}"
[[ -f "$FIXTURE" ]] || { echo "FAIL: fixture missing: $FIXTURE" >&2; exit 1; }
command -v podman >/dev/null 2>&1 || { echo 'FAIL: podman is required for scratch arm64 builds' >&2; exit 1; }
new_report_dir d3-main10-stride-ab
exec > >(tee "$REPORT_DIR/transcript.log") 2>&1
board_preflight

local_scratch=$(mktemp -d)
remote_scratch="/tmp/ceralive-main10-stride-$$"
# shellcheck disable=SC2317
cleanup() {
	rm -rf "$local_scratch"
	board_ssh "rm -rf '$remote_scratch'" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

mkdir -p "$local_scratch/current/src" "$local_scratch/alternative/src"
tar --exclude=.git --exclude='build*' --exclude='stage*' --exclude=dist \
	-C "$ROOT" -cf - . | tar -xf - -C "$local_scratch/current/src"
cp -a "$local_scratch/current/src/." "$local_scratch/alternative/src/"
python3 - "$local_scratch/alternative/src/gst/rockchipmpp/gstmpp.c" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()
old = "hstride = hstride / format->pixel_stride0;"
new = "hstride = hstride * 8 / format->pixel_stride0;"
if s.count(old) != 1:
    raise SystemExit(f"expected exactly one stride expression, found {s.count(old)}")
p.write_text(s.replace(old, new))
PY

for variant in current alternative; do
	podman run --rm --platform linux/arm64 -v "$local_scratch:/work" "$BUILD_IMAGE" \
		bash -lc "meson setup '/work/$variant/build' '/work/$variant/src' --prefix=/usr -Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled && meson compile -C '/work/$variant/build'"
done

board_ssh "mkdir -p '$remote_scratch/current' '$remote_scratch/alternative' '$remote_scratch/results'"
board_scp "$FIXTURE" "$BOARD_TARGET:$remote_scratch/fixture.hevc"
for variant in current alternative; do
	board_scp "$local_scratch/$variant/build/gst/rockchipmpp/libgstrockchipmpp.so" \
		"$BOARD_TARGET:$remote_scratch/$variant/libgstrockchipmpp.so"
done

run_decode="set -euo pipefail
base='$remote_scratch/results'; fixture='$remote_scratch/fixture.hevc'
decode() { label=\$1 decoder=\$2 plugin=\${3:-}; out=\"\$base/\$label\"; rm -rf \"\$out\"; mkdir -p \"\$out\"; registry=\"\$base/registry-\$label.bin\"; GST_REGISTRY=\"\$registry\" GST_PLUGIN_PATH=\"\$plugin\" GST_DEBUG_NO_COLOR=1 GST_DEBUG=rockchipmpp:4,rga:4 gst-launch-1.0 -v filesrc location=\"\$fixture\" ! h265parse config-interval=-1 ! video/x-h265,stream-format=byte-stream,alignment=au ! \"\$decoder\" ! videoconvert ! video/x-raw,format=I420 ! multifilesink location=\"\$out/frame-%03d.raw\" >\"\$base/\$label.log\" 2>&1; sha256sum \"\$out\"/*.raw | sed \"s#\$out/##\" >\"\$base/\$label.sha256\"; }
decode reference avdec_h265
for v in current alternative; do plugin='$remote_scratch/'\"\$v\"; GST_REGISTRY=\"\$base/inspect-\$v.bin\" GST_PLUGIN_PATH=\"\$plugin\" gst-inspect-1.0 mppvideodec | grep -E 'Filename|Version' >\"\$base/\$v.inspect\"; grep -q \"\$plugin/libgstrockchipmpp.so\" \"\$base/\$v.inspect\"; decode \"\$v\" mppvideodec \"\$plugin\" || true; done
for label in reference current alternative; do printf '%s frames=%s errors=%s rga_mpp_errors=%s\\n' \"\$label\" \"\$(find \"\$base/\$label\" -name 'frame-*.raw' 2>/dev/null | wc -l)\" \"\$(grep -cE 'ERROR|CRITICAL|not-negotiated' \"\$base/\$label.log\" || true)\" \"\$(grep -ciE 'RGA_BLIT fail|rga_api version|mpp.*(fail|error)|failed.*mpp' \"\$base/\$label.log\" || true)\"; done"
board_ssh "$run_decode" | tee "$REPORT_DIR/counts.txt"
board_ssh "tar -C '$remote_scratch/results' -cf '$remote_scratch/results.tar' ."
board_scp "$BOARD_TARGET:$remote_scratch/results.tar" "$REPORT_DIR/results.tar"
mkdir -p "$REPORT_DIR/results"
tar -xf "$REPORT_DIR/results.tar" -C "$REPORT_DIR/results"

matches() {
	local label=$1
	[[ -s "$REPORT_DIR/results/$label.sha256" ]] &&
		cmp -s "$REPORT_DIR/results/reference.sha256" "$REPORT_DIR/results/$label.sha256" &&
		! grep -qE 'ERROR|CRITICAL|not-negotiated|RGA_BLIT fail|rga_api version' "$REPORT_DIR/results/$label.log"
}

current_ok=0 alternative_ok=0
matches current && current_ok=1
matches alternative && alternative_ok=1
case "$current_ok:$alternative_ok" in
	1:0) verdict=CURRENT_CORRECT ;;
	0:1) verdict=ALTERNATIVE_CORRECT ;;
	*) verdict=INCONCLUSIVE ;;
esac
printf 'STRIDE_VERDICT: %s current_matches=%s alternative_matches=%s\n' \
	"$verdict" "$current_ok" "$alternative_ok" | tee "$REPORT_DIR/verdict.txt"
{
	printf 'verdict=%s\n' "$verdict"
	printf 'finished_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >"$REPORT_DIR/RUN_COMPLETE"

# Completion means the report is sealed; the scientific result is the enum.
exit 0
