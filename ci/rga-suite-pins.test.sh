#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fail() { printf 'rga-suite-pins: FAIL: %s\n' "$1" >&2; exit 1; }

check_pair() (
  export RGA_COMPAT_SUITE="$1"
  # shellcheck source=ci/mpp-pin.env
  source "${here}/mpp-pin.env"
  [[ "${RGA_HEADER_DEB}" == librga-ceralive-dev_1.10.5+ceralive.1_arm64.deb ]] || fail 'C6b header identity'
  [[ "${RGA_HEADER_SHA256}" == 8dd35334ed1022ff8e64a86b3ac426abb847bf36f3ccff2746a658ccc0d8577a ]] || fail 'C6b header checksum'
  [[ "${RGA_RUNTIME_DEB}" == "$2" ]] || fail "$1 runtime"
  [[ "${RGA_DEV_DEB}" == "$3" ]] || fail "$1 headers"
  [[ "${RGA_RUNTIME_SHA256}" == "$4" ]] || fail "$1 runtime checksum"
  [[ "${RGA_DEV_SHA256}" == "$5" ]] || fail "$1 header checksum"
  [[ "${RGA_RUNTIME_URL}" == "$6/${RGA_RUNTIME_DEB}" ]] || fail "$1 runtime URL"
  [[ "${RGA_DEV_URL}" == "$6/${RGA_DEV_DEB}" ]] || fail "$1 header URL"
  [[ "${MPP_RUNTIME_SHA256}" == fe839d41010def25b2c096581815fd26214680bf9720fc47ff2c7afe501f6bcd ]] || fail 'MPP runtime changed'
  [[ "${MPP_DEV_SHA256}" == aa38d6476ff4798623b37f09845436ab80d730f42c17e305f43ba7a892ee34fe ]] || fail 'MPP headers changed'
)

check_pair '' \
  librga2-ceralive_1.10.5+ceralive.1_arm64.deb \
  librga-ceralive-dev_1.10.5+ceralive.1_arm64.deb \
  5f8ea1f259b95d5bf6fbe68edf03bf08820ab7bc8d4d17bfc1fc4a00344c7bb3 \
  8dd35334ed1022ff8e64a86b3ac426abb847bf36f3ccff2746a658ccc0d8577a \
  https://github.com/CERALIVE/librga/releases/download/1.10.5+ceralive.1
check_pair bookworm \
  librga2_2.2.0-1_arm64.deb librga-dev_2.2.0-1_arm64.deb \
  ca4f18666f6c5d5290c7e41e5901350ecf76530f24364e37b81fa6be4ab5f344 \
  49885e2423bff2cdcfd319c0e76259e46d6d13e3d26c057cf988653600a4043d \
  https://radxa-repo.github.io/rk3588s2-bookworm/pool/main/libr/librga

for invalid in trixie typo; do
  if RGA_COMPAT_SUITE="${invalid}" bash -c 'source "$1"' _ "${here}/mpp-pin.env"; then
    fail "accepted unknown compatibility selector ${invalid}"
  fi
done
printf 'rga-suite-pins: all regression cases passed\n'
