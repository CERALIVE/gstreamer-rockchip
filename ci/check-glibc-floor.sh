#!/usr/bin/env bash
# Fail the build if a shipped ELF imports a versioned GLIBC symbol above the
# device's floor.
#
# usage: check-glibc-floor.sh <elf> [<elf> ...]
#
# The floor is DERIVED from GLIBC_FLOOR in ci/target-suite.env — never spelled
# here, and never approximated by a regex over "versions we happen to know are
# too new". A pattern like `GLIBC_2.3[89]` is wrong twice over: it passes
# GLIBC_2.40 (which did not exist when the pattern was written) and it hardcodes
# a floor that has to be edited in two places the day the target suite moves.
# Every comparison below is numeric against the one declared value.
#
# Why a plugin needs this at all: `libgstrockchipmpp.so` is `dlopen`'d by the
# device's GStreamer. An over-ceiling import there produces no apt warning and
# no dpkg error — it surfaces as an element that silently fails to register, and
# the operator sees a board with no hardware encoder.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ci/target-suite.env
source "${here}/target-suite.env"

if (($# == 0)); then
  echo "usage: check-glibc-floor.sh <elf> [<elf> ...]" >&2
  exit 2
fi

if [[ -z "${GLIBC_FLOOR:-}" ]]; then
  echo "ERROR: GLIBC_FLOOR is not declared in ci/target-suite.env" >&2
  exit 1
fi

if [[ -n "${OBJDUMP:-}" ]]; then
  dump_tool="${OBJDUMP}"
elif command -v objdump >/dev/null 2>&1; then
  dump_tool="objdump"
else
  echo "ERROR: objdump is not available (install binutils)" >&2
  exit 1
fi

# Numeric dotted-version compare: `glibc_exceeds A B` is true when A > B.
# Pure bash so the gate needs nothing beyond binutils — in particular it does
# not depend on dpkg, which is not guaranteed wherever an ELF gets checked.
glibc_exceeds() {
  local -a left right
  IFS='.' read -r -a left <<<"$1"
  IFS='.' read -r -a right <<<"$2"
  local i len=${#left[@]}
  ((${#right[@]} > len)) && len=${#right[@]}
  for ((i = 0; i < len; i++)); do
    local l="${left[i]:-0}" r="${right[i]:-0}"
    ((10#$l > 10#$r)) && return 0
    ((10#$l < 10#$r)) && return 1
  done
  return 1
}

status=0
for elf in "$@"; do
  if [[ ! -f "${elf}" ]]; then
    echo "ERROR: ${elf} does not exist" >&2
    status=1
    continue
  fi

  if ! symbols="$("${dump_tool}" -T "${elf}")"; then
    echo "ERROR: failed to inspect ${elf} with ${dump_tool} -T" >&2
    status=1
    continue
  fi

  versions="$(printf '%s\n' "${symbols}" | grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu || true)"
  if [[ -z "${versions}" ]]; then
    # An ELF with no versioned GLIBC import at all is not a pass — it means the
    # inspection looked at the wrong file (a stripped stub, a script, the wrong
    # build output), and silently returning 0 would make the gate decorative.
    echo "ERROR: ${elf} has no GLIBC version references" >&2
    status=1
    continue
  fi

  highest="$(tail -n 1 <<<"${versions}")"
  echo "${elf}: highest GLIBC import ${highest}; device floor GLIBC_${GLIBC_FLOOR}"

  while IFS= read -r version; do
    [[ -z "${version}" ]] && continue
    if glibc_exceeds "${version#GLIBC_}" "${GLIBC_FLOOR}"; then
      echo "ERROR: ${elf} imports ${version}, which exceeds GLIBC_${GLIBC_FLOOR}" >&2
      offenders="$(printf '%s\n' "${symbols}" |
        awk -v v="${version}" '$0 ~ ("\\(" v "\\)") { print $NF }' | sort -u)"
      [[ -z "${offenders}" ]] && offenders='<symbol unavailable>'
      while IFS= read -r symbol; do
        printf '  %s: %s\n' "${version}" "${symbol}" >&2
      done <<<"${offenders}"
      status=1
    fi
  done <<<"${versions}"
done

exit "${status}"
