#!/usr/bin/env bash
set -euo pipefail

root=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
checker=${PARITY_CHECKER:-$root/parity-check.sh}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin"
cat > "$work/bin/grep" <<'EOF'
#!/bin/sh
echo 'per-line grep must not be launched by the normalized comparator' >&2
exit 95
EOF
chmod +x "$work/bin/grep"

check() {
  local name=$1 expected_status=$2 actual=$3 expected=$4 status=0
  printf '%s' "$actual" > "$work/actual"
  printf '%s' "$expected" > "$work/expected"
  PATH="$work/bin:$PATH" bash "$checker" --compare-normalized synthetic \
    "$work/actual" "$work/expected" > "$work/output" 2>&1 || status=$?
  if [[ "$status" != "$expected_status" ]]; then
    cat "$work/output" >&2
    printf 'FAIL %s: status %s, expected %s\n' "$name" "$status" "$expected_status" >&2
    exit 1
  fi
  printf 'PASS comparator %s\n' "$name"
}

check additive 0 $'property.a=1\nproperty.extra=2\n' $'property.a=1\n'
check missing 1 $'property.extra=2\n' $'property.a=1\n'
check changed 1 $'property.a=10\n' $'property.a=1\n'
check literal 0 $'property.[a]*?=$(false)\n' $'property.[a]*?=$(false)\n'
check unterminated-actual 0 'property.a=1' $'property.a=1\n'
check blank-line 0 $'\nproperty.a=1\n' $'\nproperty.a=1\n'
check duplicate-property 0 $'property.a=1\nproperty.a=1\n' $'property.a=1\n'
check exact-caps 0 $'src_caps=NV12\nsink_caps=NV12\n' $'src_caps=NV12\nsink_caps=NV12\n'
check changed-caps 1 $'src_caps=NV16\n' $'src_caps=NV12\n'
check duplicate-caps 1 $'src_caps=NV12\nsrc_caps=NV12\n' $'src_caps=NV12\n'
check missing-caps 1 $'property.a=1\n' $'src_caps=NV12\n'
