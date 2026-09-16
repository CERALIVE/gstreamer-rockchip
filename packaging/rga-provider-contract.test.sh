#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
cat >"${work}/dpkg-query" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "$1" in
  -S) printf '%s:arm64: %s\n' "${TEST_PROVIDER}" "$2" ;;
  -W)
    case "$2" in
      '-f=${Status}') printf '%s' "${TEST_STATUS}" ;;
      '-f=${Provides}') printf '%s' "${TEST_PROVIDES}" ;;
      *) exit 2 ;;
    esac ;;
  *) exit 2 ;;
esac
EOF
chmod +x "${work}/dpkg-query"
export PATH="${work}:${PATH}"
export TEST_PROVIDER=librga2-ceralive TEST_STATUS='install ok installed'
export TEST_PROVIDES='librga2 (= 2.2.0)'

check() { bash "${here}/rga-provider-contract.sh"; }
reject() {
  if check >"${work}/error" 2>&1; then
    printf 'FAIL: accepted %s\n' "$1" >&2
    exit 1
  fi
  grep -qF "$2" "${work}/error"
}

check
TEST_PROVIDER=librga2 TEST_PROVIDES='' check
TEST_PROVIDER=unrelated reject 'unknown owner' 'not an allowed librga.so.2 provider'
TEST_PROVIDES='' reject 'missing virtual Provides' 'does not provide librga2'
TEST_PROVIDES='not-librga2 (= 2.2.0)' reject 'substring Provides' 'does not provide librga2'
TEST_PROVIDES='librga2-extra' reject 'suffix Provides' 'does not provide librga2'
TEST_STATUS='deinstall ok config-files' reject 'removed provider' 'is not installed'
TEST_PROVIDES='other, librga2 (= 2.2.0), another' check
printf 'rga-provider-contract: all regression cases passed\n'
