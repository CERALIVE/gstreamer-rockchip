#!/usr/bin/env bash
# Encoders use the fork baseline; decoders retain the Radxa 1.14-4 baseline.
# The fork inherited cf155b3's bps* -> bitrate* rename before the fork point.
# Cerastream migrates separately, and the image pin must not move before it does.
# New properties are allowed; existing property/rank/caps lines remain frozen.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly ROOT
readonly RADXA_GOLDEN_DIR="$ROOT/tests/golden/radxa-1.14-4"
readonly FORK_GOLDEN_DIR="$ROOT/tests/golden/fork-baseline"
readonly FORK_NO_NV16_GOLDEN_DIR="$ROOT/tests/golden/fork-no-nv16-10le40"
readonly ELEMENTS=(mpph264enc mpph265enc mppvideodec mppjpegdec)

normalize() {
  local element=$1
  local input
  local rc=0
  input=$(mktemp)
  cat > "$input"
  python3 - "$element" "$input" <<'PY' || rc=$?
import re
import sys

element = sys.argv[1]
with open(sys.argv[2], encoding="utf-8") as source:
    lines = source.read().splitlines()


def compact(value: str) -> str:
    return " ".join(value.split())


rank = None
caps: dict[str, list[str]] = {"sink": [], "src": []}
active_pad = None
in_caps = False
properties: dict[str, list[str]] = {}
property_name = None
in_properties = False

for line in lines:
    rank_match = re.match(r"\s*Rank\s+.*\((\d+)\)\s*$", line)
    if rank_match:
        rank = rank_match.group(1)

    pad_match = re.match(r"\s*(SINK|SRC) template:", line)
    if pad_match:
        active_pad = pad_match.group(1).lower()
        in_caps = False
        continue
    if active_pad and re.match(r"\s*Capabilities:\s*$", line):
        in_caps = True
        continue
    if in_caps:
        if not line.strip():
            in_caps = False
            active_pad = None
        else:
            caps[active_pad].append(compact(line))
        continue

    if line == "Element Properties:":
        in_properties = True
        continue
    if not in_properties:
        continue
    heading = re.match(r"^  ([A-Za-z0-9][A-Za-z0-9-]*)\s*:", line)
    if heading:
        property_name = heading.group(1)
        properties[property_name] = []
    elif property_name is not None:
        properties[property_name].append(line.strip())

if rank is None:
    raise SystemExit(f"{element}: gst-inspect output has no numeric rank")

print(f"element={element}")
print(f"rank={rank}")
for pad in ("sink", "src"):
    print(f"{pad}_caps={compact(' '.join(caps[pad]))}")

for name in sorted(properties):
    block = properties[name]
    joined = " ".join(block)
    type_name = None
    default = None
    value_range = None
    enum_nicks: list[str] = []

    enum_match = re.search(r'Enum "([^"]+)" Default:\s*([^,\s]+),\s*"([^"]+)"', joined)
    flags_match = re.search(r'Flags "([^"]+)" Default:\s*([^,\s]+),\s*"([^"]+)"', joined)
    scalar_match = re.search(
        r'(Unsigned Integer64|Unsigned Integer|Integer64|Integer|Boolean|Double|Float|String)\.'
        r'(?: Range:\s*([^\s]+)\s*-\s*([^\s]+))?\s*Default:\s*(.+?)(?:\s{2,}|$)',
        joined,
    )
    object_match = re.search(r'Object of type "([^"]+)"', joined)
    array_match = re.search(r'GstValueArray of GValues of type "([^"]+)"', joined)

    if enum_match:
        type_name = f"Enum({enum_match.group(1)})"
        default = f"{enum_match.group(2)}:{enum_match.group(3)}"
        enum_nicks = re.findall(r"\([^)]*\):\s*([^\s]+)\s+-", joined)
    elif flags_match:
        type_name = f"Flags({flags_match.group(1)})"
        default = f"{flags_match.group(2)}:{flags_match.group(3)}"
        enum_nicks = re.findall(r"\([^)]*\):\s*([^\s]+)\s+-", joined)
    elif scalar_match:
        type_name = scalar_match.group(1)
        if scalar_match.group(2) is not None:
            value_range = f"{scalar_match.group(2)}..{scalar_match.group(3)}"
        default = scalar_match.group(4).strip().strip('"')
    elif object_match:
        type_name = f"Object({object_match.group(1)})"
    elif array_match:
        type_name = f"GstValueArray({array_match.group(1)})"
        default_match = re.search(r'Default:\s*"([^"]*)"', joined)
        if default_match:
            default = default_match.group(1)
    else:
        type_name = "Unknown"

    print(f"property.{name}.type={type_name}")
    if default is not None:
        print(f"property.{name}.default={default}")
    if value_range is not None:
        print(f"property.{name}.range={value_range}")
    if enum_nicks:
        print(f"property.{name}.enum_nicks={','.join(enum_nicks)}")
PY
  rm -f "$input"
  return "$rc"
}

emit_inspect() {
  local element=$1
  if [[ -n "${PARITY_INSPECT_DIR:-}" ]]; then
    cat "$PARITY_INSPECT_DIR/$element.inspect"
  elif [[ -n "${BOARD_IP:-}" ]]; then
    : "${BOARD_SSH_USER:?BOARD_SSH_USER is required when BOARD_IP is set}"
    : "${BOARD_SSH_PASS:?BOARD_SSH_PASS is required when BOARD_IP is set}"
    command -v sshpass >/dev/null || {
      echo "parity: sshpass is required for board inspection" >&2
      return 1
    }
    sshpass -p "$BOARD_SSH_PASS" ssh \
      -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 \
      "$BOARD_SSH_USER@$BOARD_IP" "GST_DEBUG=0 gst-inspect-1.0 '$element'"
  else
    gst-inspect-1.0 "$element"
  fi
}

golden_for() {
  local element=$1
  case "$element" in
    mpph264enc | mpph265enc) printf '%s\n' "$FORK_GOLDEN_DIR/$element.golden" ;;
    mppvideodec | mppjpegdec)
      if [[ -n "${PARITY_CONFIG_H:-}" ]] && ! config_has HAVE_NV16_10LE40; then
        printf '%s\n' "$FORK_NO_NV16_GOLDEN_DIR/$element.golden"
      else
        printf '%s\n' "$RADXA_GOLDEN_DIR/$element.golden"
      fi
      ;;
  esac
}

config_has() {
  local macro=$1
  grep -Eq \
    "^[[:space:]]*#[[:space:]]*define[[:space:]]+${macro}[[:space:]]+1([[:space:]]|$)" \
    "$PARITY_CONFIG_H"
}

check_decoder_capabilities() {
  local element=$1 actual=$2
  local macro format caps

  [[ -n "${PARITY_CONFIG_H:-}" ]] || return 0
  [[ "$element" == mppvideodec || "$element" == mppjpegdec ]] || return 0
  if ! caps=$(grep '^src_caps=' "$actual"); then
    echo "parity: $element has no normalized src caps" >&2
    return 1
  fi

  while read -r macro format; do
    if config_has "$macro"; then
      if [[ "$caps" != *"(string)$format"* ]]; then
        echo "parity: $element omits $format although $macro is defined" >&2
        return 1
      fi
    elif [[ "$caps" == *"(string)$format"* ]]; then
      echo "parity: $element advertises $format although $macro is not defined" >&2
      return 1
    fi
  done <<'EOF'
HAVE_NV12_10LE40 NV12_10LE40
HAVE_NV16_10LE40 NV16_10LE40
EOF
}

check_golden_capabilities() {
  local element=$1 golden=$2
  local macro format caps

  [[ -n "${PARITY_CONFIG_H:-}" ]] || return 0
  [[ "$element" == mppvideodec || "$element" == mppjpegdec ]] || return 0
  if ! caps=$(grep '^src_caps=' "$golden"); then
    echo "parity: $golden has no normalized src caps" >&2
    return 1
  fi

  while read -r macro format; do
    if config_has "$macro"; then
      if [[ "$caps" != *"(string)$format"* ]]; then
        echo "parity: $golden omits $format although $macro is defined" >&2
        return 1
      fi
    elif [[ "$caps" == *"(string)$format"* ]]; then
      echo "parity: $golden contains $format although $macro is not defined" >&2
      return 1
    fi
  done <<'EOF'
HAVE_NV12_10LE40 NV12_10LE40
HAVE_NV16_10LE40 NV16_10LE40
EOF
}

check_reference_golden_hashes() {
  if ! (cd "$RADXA_GOLDEN_DIR" && sha256sum -c decoder.sha256 >/dev/null); then
    echo "parity: immutable Radxa decoder golden hash mismatch" >&2
    return 1
  fi
}

check_source_contract() {
  local contract=$1 golden_dir=$2 label=$3
  local element expected golden key needle
  while IFS=$'\t' read -r element expected; do
    [[ -z "$element" || "$element" == \#* ]] && continue
    golden="$golden_dir/$element.golden"
    if [[ "$expected" == *.contains=* ]]; then
      key=${expected%%.contains=*}
      needle=${expected#*.contains=}
      if ! grep -F "${key}=" "$golden" | grep -Fq "$needle"; then
        echo "parity: source-derived contract missing from $element golden: $expected" >&2
        return 1
      fi
      continue
    fi
    if ! grep -Fqx "$expected" "$golden"; then
      echo "parity: source-derived contract missing from $element golden: $expected" >&2
      return 1
    fi
  done < "$contract"
  echo "PASS source-derived contract ($label)"
}

compare_element() {
  local element=$1 raw actual expected golden line rank
  raw=$(mktemp)
  actual=$(mktemp)
  expected=$(mktemp)
  trap 'rm -f "$raw" "$actual" "$expected"' RETURN

  emit_inspect "$element" > "$raw"
  normalize "$element" < "$raw" > "$actual"
  golden=$(golden_for "$element")
  check_golden_capabilities "$element" "$golden"
  grep -v '^#' "$golden" | grep -v '^$' > "$expected"

  rank=$(grep '^rank=' "$actual" | cut -d= -f2)
  if (( rank < 64 )); then
    echo "parity: $element rank $rank is below GST_RANK_MARGINAL (64)" >&2
    return 1
  fi
  if [[ "$element" == mppjpegdec && "$rank" != 257 ]]; then
    echo "parity: mppjpegdec rank is $rank, expected exactly 257" >&2
    return 1
  fi
  if [[ "$element" == mppjpegdec ]] && ! grep -F 'src_caps=' "$actual" | grep -Fq 'NV12'; then
    echo "parity: mppjpegdec src caps no longer contain NV12" >&2
    return 1
  fi
  check_decoder_capabilities "$element" "$actual"

  while IFS= read -r line; do
    if ! grep -Fqx "$line" "$actual"; then
      echo "parity: $element removed or changed baseline line:" >&2
      echo "  $line" >&2
      return 1
    fi
  done < "$expected"
  echo "PASS $element (additive properties allowed)"
}

if [[ "${1:-}" == "--normalize" ]]; then
  [[ $# -eq 2 ]] || { echo "usage: $0 --normalize ELEMENT" >&2; exit 2; }
  normalize "$2"
  exit 0
fi

if [[ -n "${PARITY_CONFIG_H:-}" ]]; then
  if [[ ! -r "$PARITY_CONFIG_H" ]]; then
    echo "parity: PARITY_CONFIG_H is not readable: $PARITY_CONFIG_H" >&2
    exit 1
  fi
  nv12_10le40=no
  nv16_10le40=no
  config_has HAVE_NV12_10LE40 && nv12_10le40=yes
  config_has HAVE_NV16_10LE40 && nv16_10le40=yes
  echo "parity: decoder build capabilities from $PARITY_CONFIG_H: HAVE_NV12_10LE40=$nv12_10le40 HAVE_NV16_10LE40=$nv16_10le40"
fi

check_reference_golden_hashes
check_source_contract \
  "$RADXA_GOLDEN_DIR/source-contract.tsv" \
  "$RADXA_GOLDEN_DIR" \
  "historical radxa-pkg/gstreamer-rockchip@a89cf9c"
check_source_contract \
  "$FORK_GOLDEN_DIR/source-contract.tsv" \
  "$FORK_GOLDEN_DIR" \
  "active fork encoder baseline"

if [[ "${PARITY_SOURCE_ONLY:-0}" == 1 ]]; then
  exit 0
fi

if [[ -z "${BOARD_IP:-}" && -z "${PARITY_INSPECT_DIR:-}" ]] && ! command -v gst-inspect-1.0 >/dev/null; then
  echo "SKIP runtime parity: no board credentials and gst-inspect-1.0 is unavailable" >&2
  exit 77
fi

for element in "${ELEMENTS[@]}"; do
  if ! emit_inspect "$element" >/dev/null 2>&1; then
    if [[ -z "${BOARD_IP:-}" && "$element" == mpph26*enc ]]; then
      echo "SKIP runtime parity: MPP encoders require a Rockchip board" >&2
      exit 77
    fi
    echo "parity: unable to inspect $element" >&2
    exit 1
  fi
  compare_element "$element"
done

echo "PASS fork encoder and capability-aware Radxa 1.14-4 decoder runtime parity"
