#!/usr/bin/env bash
# The ELF owner and the dependency name differ for the first-party provider.
set -euo pipefail

fail() { printf 'rga-provider-contract: FAIL: %s\n' "$1" >&2; exit 1; }
soname_path=/usr/lib/aarch64-linux-gnu/librga.so.2
owner="$(dpkg-query -S "${soname_path}")" \
  || fail "no installed package owns ${soname_path}"
provider="${owner%%:*}"
case "${provider}" in
  librga2-ceralive|librga2) ;;
  *) fail "${provider} is not an allowed librga.so.2 provider" ;;
esac
[[ "$(dpkg-query -W '-f=${Status}' "${provider}")" == 'install ok installed' ]] \
  || fail "${provider} is not installed"
if [[ "${provider}" == librga2-ceralive ]]; then
  provides="$(dpkg-query -W '-f=${Provides}' "${provider}")"
  [[ "${provides}" =~ (^|,)[[:space:]]*librga2([[:space:]]+\(=[[:space:]]+[^\)]+\))?[[:space:]]*(,|$) ]] \
    || fail "${provider} does not provide librga2"
fi
printf 'rga-provider-contract: %s supplies librga.so.2 and satisfies librga2\n' "${provider}"
