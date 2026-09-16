#!/usr/bin/env bash
# shellcheck disable=SC2016
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Given no portability override, production must match the Debian 13 device.
actual="$(env -u TARGET_SUITE bash -c 'source "$1"; printf "%s %s %s" "$TARGET_SUITE" "$GLIBC_FLOOR" "$GST_RUNTIME_MINOR"' _ "${here}/target-suite.env")"
[[ "${actual}" == 'trixie 2.41 1.26' ]] || {
  printf 'FAIL: production suite contract: %s\n' "${actual}" >&2
  exit 1
}

# Given an explicit suite, packaging and ELF checks must share its ABI floor.
for tuple in 'bookworm 2.36 1.22' 'trixie 2.41 1.26'; do
  actual="$(TARGET_SUITE="${tuple%% *}" bash -c 'source "$1"; printf "%s %s %s" "$TARGET_SUITE" "$GLIBC_FLOOR" "$GST_RUNTIME_MINOR"' _ "${here}/target-suite.env")"
  [[ "${actual}" == "${tuple}" ]] || { printf 'FAIL: %s != %s\n' "${actual}" "${tuple}" >&2; exit 1; }
done

if TARGET_SUITE=typo bash -c 'source "$1"' _ "${here}/target-suite.env"; then
  printf 'FAIL: unknown target suite accepted\n' >&2
  exit 1
fi
printf 'target-suite: all regression cases passed\n'
