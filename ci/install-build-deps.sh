#!/usr/bin/env bash
# Install everything `meson setup` needs for a full four-plugin build, inside a
# `debian:<suite>-slim` container on a native arm64 runner.
#
# Two classes of dependency, and the split matters:
#
#   * suite packages (meson, ninja, gcc, GStreamer dev, libdrm, libx11) come
#     from the container's OWN suite — that is the entire reason the build runs
#     in a suite container rather than on the runner's Ubuntu. Pinning them here
#     would defeat the parity the container exists to provide.
#   * MPP and RGA are NOT in Debian. They come from ci/mpp-pin.env, URL+SHA
#     pinned to exactly the packages the device image installs.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ci/mpp-pin.env
source "${here}/mpp-pin.env"

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
  build-essential \
  ca-certificates \
  ccache \
  curl \
  binutils \
  file \
  git \
  meson \
  ninja-build \
  pkg-config \
  python3 \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libglib2.0-dev \
  libdrm-dev \
  libx11-dev

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

bash "${here}/fetch-pinned-deb.sh" "${MPP_RUNTIME_URL}" "${MPP_RUNTIME_SHA256}" "${work}/${MPP_RUNTIME_DEB}"
bash "${here}/fetch-pinned-deb.sh" "${MPP_DEV_URL}" "${MPP_DEV_SHA256}" "${work}/${MPP_DEV_DEB}"
bash "${here}/fetch-pinned-deb.sh" "${RGA_RUNTIME_URL}" "${RGA_RUNTIME_SHA256}" "${work}/${RGA_RUNTIME_DEB}"
bash "${here}/fetch-pinned-deb.sh" "${RGA_DEV_URL}" "${RGA_DEV_SHA256}" "${work}/${RGA_DEV_DEB}"

# apt resolves the four together so their inter-dependencies settle in one pass.
apt-get install -y --no-install-recommends \
  "${work}/${MPP_RUNTIME_DEB}" \
  "${work}/${MPP_DEV_DEB}" \
  "${work}/${RGA_RUNTIME_DEB}" \
  "${work}/${RGA_DEV_DEB}"

# Fail loudly here rather than letting meson's `auto` features silently drop the
# rockchipmpp plugin — an absent MPP would build a green, EMPTY matrix leg.
pkg-config --modversion rockchip_mpp
pkg-config --modversion librga
pkg-config --modversion gstreamer-1.0
