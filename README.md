# CeraLive gstreamer-rockchip

GStreamer plugins for Rockchip MPP hardware encode/decode on RK3588 devices.
This public CeraLive fork preserves the complete nine-factory plugin set while
maintaining and validating the H.264/H.265 encoder and decoder paths used by the
CeraLive streaming stack.

## Maintainer notice

This fork is maintained by **CeraLive** at
<https://github.com/CERALIVE/gstreamer-rockchip>. Issues and pull requests for the
CeraLive package belong there, not on an upstream project. The Debian package is
`gstreamer1.0-rockchip-ceralive`; its upstream-style release series begins at
`1.14.4+ceralive.1`.

The fork keeps the plugin filename `libgstrockchipmpp.so` and replaces the
historical `gstreamer1.0-rockchip1` and `belabox-gstreamer1.0-rockchip` packages.
Its four engine-critical elements are `mpph264enc`, `mpph265enc`,
`mppvideodec`, and `mppjpegdec`. Five additional upstream factories remain part
of the package and registration contract.

## Build

The RK3588 build requires GStreamer development headers, Rockchip MPP, librga,
libdrm, and X11 development files. The repository's CI scripts install the pinned
MPP/RGA development packages used by the device contract.

```bash
bash ci/install-build-deps.sh
meson setup build --prefix=/usr \
  -Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled
meson compile -C build
meson test -C build --print-errorlogs
```

Build the arm64 Debian package with:

```bash
bash packaging/build-deb.sh
bash packaging/package-contract.sh
```

Hardware-independent tests use the mock MPP seam. RK3588-only acceptance is in
`tests/board/`; those scripts are explicitly gated and record their own verdicts.
See [`AGENTS.md`](AGENTS.md) for the exact proof boundary, frozen contracts, and
contribution rules.

## Upstream lineage and credits

This repository descends from the Rockchip plugin code through the JeffyCN,
BELABOX, and irlserver trees. CeraLive thanks:

- **Rockchip** and its contributors for the original MPP GStreamer plugins;
- **Jeffy Chen / JeffyCN** for the maintained `gstreamer-rockchip` line and fixes;
- **BELABOX** for carrying and rebasing the downstream plugin tree; and
- **Thomas “datagutt” Lekanger / irlserver** for the streaming-control additions
  inherited at the CeraLive fork point.

The shipped source also contains copyrighted work by **Rockchip Electronics Co.,
Ltd.**, **Collabora Ltd.**, **Igalia**, and **Julien Moutte**. Igalia and Julien
Moutte's notices apply to the `gst/rkximage/` subtree. The machine-readable,
file-scoped attribution is [`packaging/copyright`](packaging/copyright); source
headers remain authoritative.

## License

This project is free software under the **GNU Lesser General Public License,
version 2.1**. See [`COPYING`](COPYING). CeraLive modifications remain under the
same license; upstream copyright and license notices are preserved.
