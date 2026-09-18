#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$root/tests/board/d5-quality-contract.sh"
[[ ${#EXPECTED_FAIL_CELLS[@]} == 5 ]]
for cell in 'csc:BGR->NV12' 'scale:NV12->NV16' 'scale:NV16->NV12' 'scale:BGR->NV12' 'crop:BGR->NV12'; do
  [[ $(d5_quality_verdict "$cell" 0) == EXPECTED-FAIL ]]
  if d5_quality_verdict "$cell" 1; then exit 1; fi
done
for cell in 'rotate:NV12->NV16' 'rotate:NV16->NV12' 'rotate:BGR->NV12' 'csc:NV12->NV16' 'csc:NV16->NV12' 'crop:NV12->NV16' 'crop:NV16->NV12'; do
  [[ $(d5_quality_verdict "$cell" 1) == PASS ]]
  if d5_quality_verdict "$cell" 0; then exit 1; fi
done
unset 'EXPECTED_FAIL_CELLS[0]'
if d5_quality_verdict 'csc:BGR->NV12' 0; then exit 1; fi
printf 'D24 exact-list and removed-row negative control: PASS\n'
