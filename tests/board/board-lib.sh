#!/usr/bin/env bash

set -euo pipefail

if [[ "${CERALIVE_BOARD_TEST:-0}" != 1 ]]; then
	echo "SKIP: RK3588 hardware required; re-run with CERALIVE_BOARD_TEST=1" >&2
	exit 77
fi

: "${BOARD_IP:?BOARD_IP is required}"
: "${BOARD_SSH_USER:?BOARD_SSH_USER is required}"
: "${BOARD_SSH_PASS:?BOARD_SSH_PASS is required}"

for tool in sshpass ssh scp; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "FAIL: required tool '$tool' is unavailable" >&2
		exit 1
	}
done

readonly BOARD_TARGET="${BOARD_SSH_USER}@${BOARD_IP}"
readonly -a SSH_OPTS=(-o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)

board_ssh() {
	sshpass -p "$BOARD_SSH_PASS" ssh "${SSH_OPTS[@]}" "$BOARD_TARGET" "$@"
}

board_scp() {
	sshpass -p "$BOARD_SSH_PASS" scp "${SSH_OPTS[@]}" "$@"
}

board_sudo() {
	local command=$1
	printf '%s\n' "$BOARD_SSH_PASS" | sshpass -p "$BOARD_SSH_PASS" ssh \
		"${SSH_OPTS[@]}" "$BOARD_TARGET" "sudo -S -p '' bash -lc $(printf '%q' "$command")"
}

board_preflight() {
	printf 'board_ip=%s\nboard_user=%s\n' "$BOARD_IP" "$BOARD_SSH_USER"
	board_ssh "printf 'hostname=%s\\nkernel=%s\\narch=%s\\n' \"\$(hostname)\" \"\$(uname -r)\" \"\$(uname -m)\""
}

install_deb() {
	local deb=$1 remote
	remote="/tmp/$(basename "$deb")"
	[[ -f "$deb" ]] || {
		echo "FAIL: package not found: $deb" >&2
		return 1
	}
	board_scp "$deb" "$BOARD_TARGET:$remote"
	board_sudo "apt-get install -y '$remote' && rm -f /root/.cache/gstreamer-1.0/registry.*.bin /home/*/.cache/gstreamer-1.0/registry.*.bin"
}

journal_count() {
	local since=$1 pattern=$2
	board_sudo "journalctl --since '$since' --no-pager -o cat | grep -cF '$pattern' || true"
}

new_report_dir() {
	local drill=$1 repo_root
	repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
	REPORT_DIR="$repo_root/test-results/board/$drill-$(date -u +%Y%m%dT%H%M%SZ)"
	mkdir -p "$REPORT_DIR"
	readonly REPORT_DIR
	printf 'report_dir=%s\n' "$REPORT_DIR"
}

seal_report() {
	local verdict=$1
	{
		printf 'verdict=%s\n' "$verdict"
		printf 'finished_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} >"$REPORT_DIR/RUN_COMPLETE"
	printf 'VERDICT: %s\n' "$verdict"
	[[ "$verdict" == PASS ]]
}
