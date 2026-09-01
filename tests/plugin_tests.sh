#!/bin/sh
set -eu
export GST_PLUGIN_PATH="$MESON_BUILD_ROOT/gst/rockchipmpp"
export MPP_MOCK_LOG="$MESON_BUILD_ROOT/mpp-mock-plugin.log"
: > "$MPP_MOCK_LOG"
export LD_PRELOAD="$MESON_BUILD_ROOT/gst/rockchipmpp/libmppmock.so"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
gst-inspect-1.0 mpph264enc >"$tmpdir/mpph264.inspect"
gst-inspect-1.0 mpph265enc >"$tmpdir/mpph265.inspect"
gst-inspect-1.0 mppjpegdec >"$tmpdir/mppjpeg.inspect"
grep -q 'Factory Details' "$tmpdir/mpph264.inspect"
grep -q 'profile' "$tmpdir/mpph264.inspect"
grep -q 'tier' "$tmpdir/mpph265.inspect"
grep -q 'format' "$tmpdir/mppjpeg.inspect"
test "$(grep -c '^mpp_create' "$MPP_MOCK_LOG")" -ge 2
test "$(grep -c '^mpp_init' "$MPP_MOCK_LOG")" -ge 2
grep -q 'video/x-raw' "$tmpdir/mppjpeg.inspect"
grep -q 'NV12' "$tmpdir/mppjpeg.inspect"
