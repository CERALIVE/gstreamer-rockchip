#!/usr/bin/env bash
# Fetch one URL-pinned .deb and refuse it unless its SHA-256 matches.
#
# usage: fetch-pinned-deb.sh <url> <sha256> <dest-file>
#
# The digest check is the whole point: these packages come from third-party
# GitHub releases and a vendor apt pool, neither of which is signed by anything
# this build trusts. A silent content change upstream would otherwise become a
# silent ABI change here.
set -euo pipefail

url="${1:?usage: fetch-pinned-deb.sh <url> <sha256> <dest-file>}"
expected="${2:?usage: fetch-pinned-deb.sh <url> <sha256> <dest-file>}"
dest="${3:?usage: fetch-pinned-deb.sh <url> <sha256> <dest-file>}"

mkdir -p "$(dirname "${dest}")"
curl --fail --silent --show-error --location --retry 3 --retry-delay 2 \
  --output "${dest}" "${url}"

actual="$(sha256sum "${dest}" | cut -d' ' -f1)"
if [[ "${actual}" != "${expected}" ]]; then
  echo "ERROR: digest mismatch for ${url}" >&2
  echo "  expected ${expected}" >&2
  echo "  actual   ${actual}" >&2
  rm -f "${dest}"
  exit 1
fi

echo "pinned ok: $(basename "${dest}") ${actual}"
