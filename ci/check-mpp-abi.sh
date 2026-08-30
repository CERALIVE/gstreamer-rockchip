#!/usr/bin/env bash
# Fail the build if the plugin calls an MPP symbol the PINNED librockchip_mpp
# does not export.
#
# usage: check-mpp-abi.sh <libgstrockchipmpp.so>
#
# WHY this gate exists, and why an ordinary link is not it: the plugin is built
# against MPP headers and then `dlopen`'d on the device against MPP's *runtime*.
# A cherry-picked upstream fix that calls a newer `mpp_*` entry point links
# perfectly against a newer header set and then resolves to nothing on a board
# that ships 1.5.0-1 — the element registers and dies at the first call. The
# closure below is computed against the SAME URL+SHA-pinned package the device
# image installs (ci/mpp-pin.env), so "it built" and "the board can run it" stop
# being two different questions.
#
# The MPP-only symbol set is DERIVED, never guessed from a name prefix:
#
#   mpp_only  = plugin's strong undefined symbols
#               MINUS everything exported by its other resolved DT_NEEDED libs
#   missing   = mpp_only MINUS the pinned library's exports        -> must be EMPTY
#
# A prefix table (`mpp_*`, `mpi_*`, ...) would have to be maintained by hand and
# would silently stop covering any entry point upstream names differently.
set -euo pipefail
# `comm` compares bytes; `sort` compares by locale. Pin both to C so the set
# arithmetic below cannot come apart on a runner with a different LANG.
export LC_ALL=C

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ci/mpp-pin.env
source "${here}/mpp-pin.env"

plugin="${1:?usage: check-mpp-abi.sh <libgstrockchipmpp.so>}"

[[ -f "${plugin}" ]] || {
  echo "ERROR: ${plugin} does not exist" >&2
  exit 1
}

for tool in nm objdump ldd dpkg-deb; do
  command -v "${tool}" >/dev/null 2>&1 || {
    echo "ERROR: ${tool} is not available" >&2
    exit 1
  }
done

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# Strip nm's `@VERSION` / `@@VERSION` suffix so a versioned glibc import and a
# bare MPP export compare on the same axis.
bare_names() { sed 's/@.*//' | sort -u; }

# --- the pinned MPP runtime ------------------------------------------------
bash "${here}/fetch-pinned-deb.sh" "${MPP_RUNTIME_URL}" "${MPP_RUNTIME_SHA256}" \
  "${work}/${MPP_RUNTIME_DEB}"
dpkg-deb -x "${work}/${MPP_RUNTIME_DEB}" "${work}/mpp"

# No `-type f`: in this package the SONAME is a symlink (librockchip_mpp.so.1 ->
# librockchip_mpp.so.0, a deliberate upstream compatibility alias). Resolving it
# is the point — the loader follows the same link on the device.
mpp_lib="$(find "${work}/mpp" -name "${MPP_SONAME}" -print -quit)"
[[ -n "${mpp_lib}" ]] || {
  echo "ERROR: ${MPP_SONAME} not found inside ${MPP_RUNTIME_DEB}" >&2
  exit 1
}
mpp_lib="$(readlink -f "${mpp_lib}")"
[[ -f "${mpp_lib}" ]] || {
  echo "ERROR: ${MPP_SONAME} in ${MPP_RUNTIME_DEB} resolves to no regular file" >&2
  exit 1
}
echo "pinned MPP runtime: ${MPP_SONAME} -> $(basename "${mpp_lib}") from ${MPP_RUNTIME_DEB}"

nm -D --defined-only "${mpp_lib}" | awk 'NF>1 {print $NF}' | bare_names >"${work}/mpp-exports"

# --- what the plugin needs -------------------------------------------------
# `U` only. A `w` (weak undefined) symbol — __gmon_start__ and the ITM clone
# hooks — is allowed to resolve to nothing by definition; treating one as a
# missing import would fail every ELF gcc has ever emitted.
nm -D --undefined-only "${plugin}" |
  awk '$(NF-1) == "U" { print $NF }' | bare_names >"${work}/plugin-undef"

if ! objdump -p "${plugin}" | grep -q "NEEDED *${MPP_SONAME}\b"; then
  echo "ERROR: ${plugin} does not declare NEEDED ${MPP_SONAME}" >&2
  echo "       (built without the rockchipmpp plugin, or against a different SONAME)" >&2
  exit 1
fi

# --- what its OTHER libraries already provide ------------------------------
# An unresolvable DT_NEEDED would land its whole export set in `mpp_only` and be
# reported as a missing MPP symbol — a true failure with a false explanation.
if ldd "${plugin}" | grep -q 'not found'; then
  echo "ERROR: ${plugin} has unresolved shared-library dependencies:" >&2
  ldd "${plugin}" | grep 'not found' | sed 's/^/  /' >&2
  exit 1
fi

: >"${work}/other-exports"
resolved=0
while read -r soname path; do
  [[ "${soname}" == "${MPP_SONAME}" ]] && continue
  [[ -f "${path}" ]] || continue
  nm -D --defined-only "${path}" 2>/dev/null | awk 'NF>1 {print $NF}' >>"${work}/other-exports"
  resolved=$((resolved + 1))
done < <(
  ldd "${plugin}" | awk '
    /=>/ && $3 ~ /^\// { sub(".*/", "", $1); print $1, $3 }
    !/=>/ && $1 ~ /^\// { name = $1; sub(".*/", "", name); print name, $1 }'
)

if ((resolved == 0)); then
  echo "ERROR: ldd resolved no libraries for ${plugin} — the closure would be" >&2
  echo "       vacuously 'all symbols are MPP symbols'. Refusing to report a" >&2
  echo "       meaningless result." >&2
  exit 1
fi
sort -u -o "${work}/other-exports" "${work}/other-exports"
sed -i 's/@.*//' "${work}/other-exports"
sort -u -o "${work}/other-exports" "${work}/other-exports"

# --- the closure -----------------------------------------------------------
comm -23 "${work}/plugin-undef" "${work}/other-exports" >"${work}/mpp-only"
comm -12 "${work}/mpp-only" "${work}/mpp-exports" >"${work}/mpp-resolved"
comm -23 "${work}/mpp-only" "${work}/mpp-exports" >"${work}/missing"

resolved_count="$(wc -l <"${work}/mpp-resolved")"
missing_count="$(wc -l <"${work}/missing")"

echo "resolved against ${resolved} sibling librar$([[ ${resolved} -eq 1 ]] && echo y || echo ies)"
echo "MPP symbols referenced and present: ${resolved_count}"

if ((resolved_count == 0)); then
  # A plugin that declares NEEDED librockchip_mpp and then calls nothing in it
  # means the inspection is looking at the wrong ELF. An empty diff would be a
  # green light earned by measuring nothing.
  echo "ERROR: ${plugin} references ZERO symbols from ${MPP_SONAME}" >&2
  exit 1
fi

if ((missing_count > 0)); then
  echo "ERROR: ${missing_count} symbol(s) resolve to no sibling library and are" >&2
  echo "       absent from the pinned ${MPP_SONAME}:" >&2
  sed 's/^/  /' "${work}/missing" >&2
  exit 1
fi

echo "MPP ABI closure is empty-diff against the pinned ${MPP_RUNTIME_DEB}"
