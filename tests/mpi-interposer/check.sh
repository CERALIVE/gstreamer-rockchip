#!/usr/bin/env bash
set -euo pipefail

here=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
root=$(realpath "$here/../..")
plugin_build=$(realpath "${1:?usage: bash tests/mpi-interposer/check.sh <plugin-build>}")
case "$plugin_build/" in
  "$root/"*) ;;
  *) printf 'plugin build must be repo-local\n' >&2; exit 2 ;;
esac
plugin="$plugin_build/gst/rockchipmpp/libgstrockchipmpp.so"
test -f "$plugin"
mkdir -p "$root/test-results"
work=$(mktemp -d "$root/test-results/mpi-interposer.XXXXXX")
printf 'MPI test evidence: %s\n' "$work"
command -v ffmpeg
ffmpeg -version > "$work/ffmpeg-version.txt"
pkg-config --modversion gstreamer-1.0 > "$work/gstreamer-version.txt"

for mode in default debugoptimized release minsize; do
  options=()
  if [[ "$mode" != default ]]; then
    options=(-Dtest-only=true "--buildtype=$mode")
  fi
  if meson setup "$work/reject-$mode" "$here" "${options[@]}" > "$work/reject-$mode.log" 2>&1; then
    printf 'FAIL: %s enabled MPI injection\n' "$mode" >&2
    exit 1
  fi
  grep -F 'MPI interposer requires standalone' "$work/reject-$mode.log"
done
if meson setup "$work/reject-ndebug" "$here" -Dtest-only=true --buildtype=debug \
    -Db_ndebug=true > "$work/reject-ndebug.log" 2>&1; then
  printf 'FAIL: NDEBUG enabled MPI injection\n' >&2
  exit 1
fi
grep -F 'MPI interposer requires standalone' "$work/reject-ndebug.log"

read -r -a compiler <<< "${CC:-cc}"
read -r -a includes <<< "$(pkg-config --cflags glib-2.0 rockchip_mpp)"
if "${compiler[@]}" "${includes[@]}" -fsyntax-only "$here/interposer.c" \
    > "$work/reject-unguarded.log" 2>&1; then
  printf 'FAIL: unguarded source compilation succeeded\n' >&2
  exit 1
fi
grep -F 'MPI fault injection is available only in the standalone test build' "$work/reject-unguarded.log"

meson setup "$work/build" "$here" --buildtype=debug -Dtest-only=true -Db_ndebug=false
meson compile -C "$work/build" -j 6
meson test -C "$work/build" --print-errorlogs
test "$(meson introspect --installed "$work/build")" = '{}'
meson introspect --targets "$plugin_build" > "$work/production-targets.json"
nm -D --defined-only "$plugin" > "$work/production-symbols.txt"
objdump -p "$plugin" > "$work/production-dynamic.txt"
if grep -E 'mpi-test-interposer|mpi_test_|fixture-mpp|fake-mpi' \
    "$work/production-targets.json" "$work/production-symbols.txt" "$work/production-dynamic.txt"; then
  printf 'FAIL: production build contains MPI test instrumentation\n' >&2
  exit 1
fi
printf 'release-exclusion: PASS (no install entries or production linkage)\n'

for codec in h264 h265; do
  if [[ "$codec" == h264 ]]; then
    muxer=h264
    codec_args=(-c:v libx264 -threads 1 -preset ultrafast -tune zerolatency)
  else
    muxer=hevc
    codec_args=(-c:v libx265 -threads 1 -preset ultrafast \
      -x265-params pools=none:frame-threads=1:log-level=error)
  fi
  for colour in black white; do
    ffmpeg -nostdin -hide_banner -loglevel error -f lavfi \
      -i "color=$colour:s=320x240:r=30" -frames:v 1 "${codec_args[@]}" \
      -f "$muxer" "$work/$colour.$codec"
  done
  for mode in disarmed armed; do
    for memory in copy zero-copy; do
      log="$work/$codec-$mode-$memory.log"
      timeout 60s env \
        LD_PRELOAD="$work/build/libmpi-test-interposer.so:$work/build/libfixture-mpp.so" \
        GST_PLUGIN_PATH="$plugin_build/gst/rockchipmpp" \
        GST_REGISTRY="$work/registry-$codec-$mode-$memory.bin" GST_REGISTRY_FORK=no \
        GST_DEBUG=mppenc:3 GST_DEBUG_NO_COLOR=1 GST_MPP_ALLOW_CPU_COPY=1 \
        MPP_MOCK_LOG="$work/mock-$codec-$mode-$memory.log" \
        "$work/build/plugin-test" "mpp${codec}enc" "$work/black.$codec" \
        "$work/white.$codec" "$work/$codec-$mode-$memory.$codec" "$mode" "$memory" \
        > "$log" 2>&1 || { cat "$log"; exit 1; }
      expected=0
      [[ "$mode" != armed ]] || expected=1
      count=$(grep -c 'op=inject-put-before-submit ret=-1004 ' "$log" || :)
      test "$count" = "$expected"
      count=$(grep -c 'restarted MPP encoder after mpp_encode_put_frame error -1004 (1/3)' "$log" || :)
      test "$count" = "$expected"
      grep -F 'HOST-MOCK-PASS' "$log"
    done
  done
done
printf 'MPI interposer host gate PASS. Not hardware/cerastream-session proof; island-knob row remains OPEN.\n'
