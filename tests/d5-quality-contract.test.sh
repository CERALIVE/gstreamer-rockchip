#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$root/tests/board/d5-quality-contract.sh"
[[ ${#EXPECTED_FAIL_CELLS[@]} == 0 ]]
for operation in csc scale crop rotate; do
  for pair in 'NV12->NV16' 'NV16->NV12' 'BGR->NV12'; do
    cell="$operation:$pair"
    [[ $(d5_quality_verdict "$cell" 1) == PASS ]]
    if verdict=$(d5_quality_verdict "$cell" 0); then exit 1; fi
    [[ "$verdict" == FAIL ]]
  done
done
printf 'D24 all 12 cells: PASS and below-threshold negative controls: PASS\n'
