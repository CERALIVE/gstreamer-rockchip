#!/bin/sh
set -eu
export GST_PLUGIN_PATH="$MESON_BUILD_ROOT/gst/rockchipmpp"
export MPP_MOCK_LOG="$MESON_BUILD_ROOT/mpp-mock-plugin.log"
: > "$MPP_MOCK_LOG"
export LD_PRELOAD="$MESON_BUILD_ROOT/gst/rockchipmpp/libmppmock.so"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
export GST_REGISTRY_1_0="$tmpdir/registry.bin"
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
sh "$MESON_SOURCE_ROOT/tests/parity_with_mock.sh" \
  "$MESON_BUILD_ROOT" "$MESON_SOURCE_ROOT"

cat > "$tmpdir/parity.expected" <<'EOF'
element=synthetic
src_caps=video/x-raw format: NV12
property.required.type=Boolean
EOF
cat > "$tmpdir/parity.actual" <<'EOF'
element=synthetic
src_caps=video/x-raw format: NV12
property.required.type=Boolean
property.additive.type=Boolean
EOF
bash "$MESON_SOURCE_ROOT/tests/parity-check.sh" --compare-normalized \
  synthetic "$tmpdir/parity.actual" "$tmpdir/parity.expected"

cat >> "$tmpdir/parity.actual" <<'EOF'
src_caps=INJECTED
EOF
if bash "$MESON_SOURCE_ROOT/tests/parity-check.sh" --compare-normalized \
  synthetic "$tmpdir/parity.actual" "$tmpdir/parity.expected" \
  > "$tmpdir/duplicate-caps.log" 2>&1; then
  echo "parity comparator accepted duplicate src caps" >&2
  exit 1
fi
grep -Fq 'duplicated, or changed baseline caps' "$tmpdir/duplicate-caps.log"

cat > "$tmpdir/parity.actual" <<'EOF'
element=synthetic
src_caps=video/x-raw format: NV12, NV16
property.required.type=Boolean
EOF
if bash "$MESON_SOURCE_ROOT/tests/parity-check.sh" --compare-normalized \
  synthetic "$tmpdir/parity.actual" "$tmpdir/parity.expected" \
  > "$tmpdir/changed-caps.log" 2>&1; then
  echo "parity comparator accepted changed src caps" >&2
  exit 1
fi
grep -Fq 'duplicated, or changed baseline caps' "$tmpdir/changed-caps.log"

cat > "$tmpdir/no-nv16.config.h" <<'EOF'
#define HAVE_NV12_10LE40 1
EOF
cat > "$tmpdir/decoder.golden" <<'EOF'
src_caps=video/x-raw format: { (string)NV12_10LE40 }
EOF
cat > "$tmpdir/decoder.actual" <<'EOF'
src_caps=video/x-raw format: { (string)NV12_10LE40, (string)NV16_10LE40 }
EOF
if bash "$MESON_SOURCE_ROOT/tests/parity-check.sh" --check-capabilities \
  mppvideodec "$tmpdir/decoder.actual" "$tmpdir/decoder.golden" \
  "$tmpdir/no-nv16.config.h" > "$tmpdir/wrong-advertisement.log" 2>&1; then
  echo "parity capability check accepted unsupported NV16_10LE40" >&2
  exit 1
fi
grep -Fq 'advertises NV16_10LE40 although HAVE_NV16_10LE40 is not defined' \
  "$tmpdir/wrong-advertisement.log"
