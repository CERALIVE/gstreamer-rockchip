#!/usr/bin/env bash
# D24: only measured chroma-quality limits, never pipeline/submission errors.
EXPECTED_FAIL_CELLS=(
  'csc:BGR->NV12'
  'scale:NV12->NV16'
  'scale:NV16->NV12'
  'scale:BGR->NV12'
  'crop:BGR->NV12'
)

d5_quality_verdict() {
  local cell=$1 quality_pass=$2 known=0 expected
  for expected in "${EXPECTED_FAIL_CELLS[@]}"; do
    [[ "$cell" != "$expected" ]] || known=1
  done
  if [[ "$quality_pass" == 0 && "$known" == 1 ]]; then
    printf 'EXPECTED-FAIL\n'
  elif [[ "$quality_pass" == 1 && "$known" == 0 ]]; then
    printf 'PASS\n'
  else
    printf 'FAIL\n'
    return 1
  fi
}
