#!/usr/bin/env bash
# Regenerate the two committed GLIBC-floor fixtures.
#
# usage: make-glibc-stubs.sh            # writes into this directory
#
# These are the only binaries in the repository, so they are reproducible from
# this script rather than being mystery bytes. Run it, diff the output against
# the committed files, and you have audited them.
#
# WHY they are committed instead of generated at test time: a bookworm container
# ships glibc 2.36, so NO symbol above the 2.36 floor exists in it to link
# against. The over-floor case is therefore unbuildable in the very environment
# that has to prove the gate fails red. The trick below sidesteps that entirely
# — a throwaway stub library declares the version node `GLIBC_2.99` under the
# SONAME `libc.so.6`, so the linker writes a genuine Verneed entry naming a
# glibc version no real glibc has ever shipped. The fixture is a real ELF with a
# real version requirement; only its provider was fake, and the provider is not
# committed.
#
# aarch64 because that is the architecture the CI matrix runs on. `objdump -T`
# is cross-architecture, but a fixture that matches the runner removes the
# question of whether the local binutils was built with --enable-targets=all.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cc="${CC_AARCH64:-aarch64-linux-gnu-gcc}"

command -v "${cc}" >/dev/null 2>&1 || {
  echo "ERROR: ${cc} not found (apt-get install gcc-aarch64-linux-gnu)" >&2
  exit 1
}

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

cat >"${work}/provider.c" <<'EOF'
int over_floor_call(void) { return 0; }
int under_floor_call(void) { return 0; }
EOF

# `-s -Wl,--build-id=none` keeps the fixtures small and byte-stable: without
# them each rebuild embeds a fresh build-id and the committed file churns.
build_stub() {
  local node="$1" symbol="$2" out="$3" dir="${work}/$1"
  mkdir -p "${dir}"
  printf '%s { global: %s; local: *; };\n' "${node}" "${symbol}" >"${dir}/version.map"
  "${cc}" -shared -nostdlib -fPIC -o "${dir}/libc.so.6" "${work}/provider.c" \
    -Wl,--version-script="${dir}/version.map" -Wl,-soname,libc.so.6
  printf 'extern int %s(void);\nint stub_entry(void) { return %s(); }\n' \
    "${symbol}" "${symbol}" >"${dir}/stub.c"
  "${cc}" -shared -nostdlib -fPIC -s -Wl,--build-id=none -Wl,-z,norelro \
    -o "${out}" "${dir}/stub.c" -L"${dir}" -l:libc.so.6
}

build_stub GLIBC_2.99 over_floor_call "${here}/over-floor-stub.so"
build_stub GLIBC_2.17 under_floor_call "${here}/under-floor-stub.so"

for stub in over-floor-stub.so under-floor-stub.so; do
  echo "--- ${stub} ---"
  objdump -T "${here}/${stub}" | grep GLIBC_
done
