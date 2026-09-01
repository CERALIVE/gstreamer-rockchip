#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  printf 'usage: %s BUILD_ROOT SOURCE_ROOT\n' "$0" >&2
  exit 2
fi

build_root=$1
source_root=$2
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

export GST_PLUGIN_PATH="$build_root/gst/rockchipmpp"
export GST_REGISTRY_1_0="$tmpdir/registry.bin"
export MPP_MOCK_LOG="$tmpdir/mpp-mock-parity.log"
: > "$MPP_MOCK_LOG"
export LD_PRELOAD="$build_root/gst/rockchipmpp/libmppmock.so"
export PARITY_CONFIG_H="$build_root/config.h"

bash "$source_root/tests/parity-check.sh"
