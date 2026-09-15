# Encoder runtime contract

## Composition DMA-BUF boundary [EXISTS]

The H.264/H.265 sink templates retain their plain raw caps and add **linear
NV12** with `memory:DMABuf`. No DMA_DRM/modifier, deep-colour, new factory or
property contract is added. No allocation, alignment, conversion or pixel code
changes: accepted buffers still use `gst_mpp_enc_convert()`'s existing
`gst_mpp_allocator_import_gst_memory()` path. Incompatible memory/layouts retain
the existing fail-closed production behavior; advertising a feature does not
make every layout directly importable.

### Root cause and single-source contrast

The installed item-30 control refused `rgacompositor ! queue ! tee ! mpph265enc`
before opening a capture source. The owner is **element pad templates**, not
engine graph construction: the pre-fix `gstmpph265enc.c:176–182` and
`gstmpph264enc.c:117–123` advertised plain `video/x-raw` only, whereas
`gst/rockchiprga/gstrgacompositor.c:22–34` advertises DMA-BUF-only NV12 output.
Queue and tee propagate that incompatibility; a capsfilter cannot manufacture
an intersection between those features.

The successful ordinary HDMI control is not contradictory. In cerastream
`100e84e`, `crates/cerastream/src/engine/encode.rs` constructs plain raw
`leg_output_caps` for switching. Composition uses explicit DMA-BUF input caps
and the compositor's DMA-BUF-only src template. **Plain caps do not prove a CPU
copy:** the actual `GstMemory` can still be DMA-BUF and take the encoder's import
path. Conversely, explicit DMA-BUF caps alone are not an FD-identity proof.

`tests/check/enc-dmabuf.c` attempts actual `gst_pad_link()` calls through the
real compositor, queue, tee and encoder elements for all six presets and both
codecs. Before the template change, the H.265 row reports
`preview_tee -> mpph265enc refused: no common format`; an explicit-DMA-BUF frame
push returns `GST_FLOW_NOT_NEGOTIATED`. The plain-caps import control passes.
After the change, all links succeed with DMA-BUF-only queried caps.

Separate GstHarness cases push aligned NV12 at 1920×1080p30 (1088-row storage)
and 3840×2160p60000/1001 under both explicit and plain caps. They require mock
encoded output, the original FD at the real plugin's MPP import call, retained
negotiated features and zero `conversion-fallback-frames`,
`conversion-dropped-frames`, and `layout-rejections`, with CPU fallback and RGA
conversion disabled. The host memfd is marked non-mappable. Mock MPP records
the import argument but allocates its own stand-in storage: these checks prove
plugin dispatch, **not hardware FD identity or decoded video**.

Full arm64 builds and all **12/12 Meson suites** pass under host QEMU in
Bookworm/GStreamer 1.22.0 and Trixie/GStreamer 1.26.2, with no skipped suites.
Both MPP ABI-closure checks and GLIBC-gate self-tests pass; the declared
Bookworm target passes its GLIBC floor. The static package contract passes.

### Separate finding: ordinary 1080p30 PLAYING failure [PARTIAL]

Retained `RUN-30-OPI-20260915` evidence at root ledger commit `847318bd`
(`docs/media-island/ledger/phase7.md`, `job-005-observe.sh.log` and
`job-008-caps-controls.sh.log`) shows the non-composited 1080p30 request passed
linking but returned `pipeline did not reach PLAYING: Element failed to change
its state` at monotonic 41345.672456372. The later native 4K59.94 control decoded
1,156 frames over 19.285933 seconds with BT.709 primaries/transfer/matrix and
limited range. That is historical control evidence, not this patch's board QA.

The retained logs do not name the failing element or contain its GStreamer
ERROR/debug details. Lower output geometry/rate requires additional upstream
normalization relative to the native mode, but scaling, rate negotiation,
allocation, capture state and encoder activation are not distinguished by
these receipts. This is a separate **failure stage**, not a proven independent
root cause and not a failure this caps-only patch claims to fix. A future
authorized diagnostic needs caps/state tracing armed before the ordinary
1080p30 Start, the per-element bus error and conversion counters, followed by
the unchanged native control. No such board run was authorized for this fix.

Item 30 remains open until the real-source >=600-second composition, per-frame
FD trace, zero counters, VUI and IDR rows pass. Ten-minute UVC power endurance
was never exercised and remains **untested**, not power-failed. No board access,
installation, camera change, merge, tag or release is part of this software fix.

## Latency, recovery and colorimetry

The MPP H.264 and H.265 encoders report one frame of codec work plus the
currently tracked pending depth as fixed latency. The value is recalculated
after each successful `MPP_ENC_SET_CFG` application.

Runtime failures from frame submission or packet retrieval trigger a complete
MPP context restart unless MPP reports its non-blocking timeout/no-capacity
result. Recovery drains and destroys the old context, creates and initializes a
new one, and reapplies the complete retained configuration. Three restarts are
allowed in any ten-second window. A fourth failure posts
`GST_STREAM_ERROR_ENCODE` with `encoder restart budget exhausted`. The
read-only `encoder-restarts` property reports successful restarts over the
element lifetime.

### Hardware-error propagation gap [PARTIAL]

**The bounded restart exists, but real hardware task failures cannot trigger it
through the pinned library's configured async APIs.** This is a production
recovery/observability gap, not just a test-harness limitation. No propagation
fix or libmpp-pin change is implemented by the test-only interposer.

Verified against installed `.4` source
`f8960191ea14360347ceb9b96b6dce72f26b743d` and main
`f84526ae1264bf1d4975d0eb4f4e2f9002f87ba5`: both have encoder blob
`2d18d7ebfbb93f1359dd67577f00343e87230bd1`. The plugin sets nonblocking input
and a 1ms output timeout (`gstmppenc.c:1204–1214`), rejects NOK/TIMEOUT as restart
stimuli (`1315–1326`) and increments only after successful recreation (`1299`).
The public call sites are `2194–2198` and `2229–2232`.

Pinned MPP source `194af181db3a02a095c01db84e176d972e19b216` supplies the missing
cross-layer link:

- [`mpp/mpp.cpp:820–891`](https://github.com/tsukumijima/mpp-rockchip/blob/194af181db3a02a095c01db84e176d972e19b216/mpp/mpp.cpp#L820-L891):
  async put returns only OK/NOK; async get only OK/NOK/TIMEOUT. None triggers
  the plugin restart on this valid initialized path.
- [`mpp/codec/mpp_enc_impl.cpp:3258–3314`](https://github.com/tsukumijima/mpp-rockchip/blob/194af181db3a02a095c01db84e176d972e19b216/mpp/codec/mpp_enc_impl.cpp#L3258-L3314):
  a worker failure forces a future IDR and queues a zero-length packet. Get
  returns OK for that packet; plugin `2260–2269,2324–2329` drops it as though it
  were a rate-control drop. An empty packet does not uniquely identify a fault.
- [`hal_h265e_vepu580.c:3306–3319`](https://github.com/tsukumijima/mpp-rockchip/blob/194af181db3a02a095c01db84e176d972e19b216/mpp/hal/rkenc/h265e/hal_h265e_vepu580.c#L3306-L3319):
  H.265's non-split path overwrites the poll return with hardware-status checking,
  potentially swallowing the error even before worker completion.

The [standalone MPI test](../tests/mpi-interposer/README.md) proves a qualifying
public error causes the unchanged plugin to restart and continue synthetic host
output. **It does not satisfy item 30's island-knob requirement.** A separately
scoped cross-layer bridge or approved propagation change must preserve task-error
provenance through HAL, worker and public MPI, including H.265's earlier overwrite,
without confusing legitimate rate-control drops or breaking frame ownership.
Another kernel errno knob cannot extend the public async return set. MNH-27's
libmpp freeze remains intact; no hardware result follows from these host tests.

Negotiated BT.601, BT.709, and BT.2020 colorimetry is written to MPP's
`prep:colorspace`, `prep:colorprim`, `prep:colortrc`, and `prep:range` keys.
Full range maps to MPP's JPEG range and limited range maps to MPEG range. Caps
without an explicit `colorimetry` field write no color keys; renegotiation from
specified to unspecified starts from a fresh MPP config so old values cannot
leak into the new stream.

sRGB transfer is also supported: GStreamer `GST_VIDEO_TRANSFER_SRGB` (enum 7)
maps to `MPP_FRAME_TRC_IEC61966_2_1` (H.26x transfer code 13). The discriminating
`2:4:7:1` tuple writes `prep:colorspace=6`, `prep:colorprim=1`,
`prep:colortrc=13`, and `prep:range=MPP_FRAME_RANGE_MPEG`. Its expected SPS is
`video_full_range_flag=0`, `matrix_coefficients=6`,
`transfer_characteristics=13`, `colour_primaries=1`, with video-signal and
colour-description presence flags set. The range enum itself is not the SPS
full-range flag. This is metadata transport, not a pixel-conversion claim.

### Unmapped values (unchanged)

Each unsupported axis warns and returns before **any** of the four color-key
writes. An unsupported explicit tuple on an already configured encoder can
therefore leave prior color keys in place; "omitting VUI" describes skipped
configuration, not a guarantee of absent bits. This separate behavior is not
changed by adding sRGB.

- Matrix: UNKNOWN, RGB/identity, FCC, SMPTE240M.
- Primaries: UNKNOWN, BT470M, SMPTE240M, FILM, ADOBERGB, SMPTEST428,
  SMPTERP431, SMPTEEG432, EBU3213.
- Transfer: UNKNOWN, GAMMA10/18/20/22/28, SMPTE240M, LOG100/316, ADOBERGB,
  SMPTE2084/PQ, ARIB_STD_B67/HLG.
- Range: UNKNOWN; both defined full and limited ranges already map.

All-UNKNOWN colorimetry is an intentional no-op. The named `sRGB` preset uses
an RGB/identity matrix and is not interchangeable with YUV `2:4:7:1`; adding
the transfer case does not add an RGB-matrix mapping or broaden negotiation.

`tests/check/enc-colorimetry.c` checks the exact tuple from a cold start and
BT.709→sRGB→BT.709 color-only caps transitions on both encoder factories.
Before the fix, its two added cases failed with missing cold-start keys and
stale BT.709 keys respectively. These are MPP configuration assertions, not
hardware-generated bitstream evidence.

### 2026-09-14 — sRGB SPS serialization evidence [PARTIAL]

The plugin change is three added lines; inherited CRLF, factories, properties,
defaults, caps negotiation and package pins are unchanged. Full arm64 builds
and all **11/11 Meson suites** pass in bookworm/GStreamer 1.22.0 and
trixie/GStreamer 1.26.2 under host QEMU user emulation. No suites skipped.
Both MPP ABI-closure gates and GLIBC gate self-tests pass; the trixie plugin
passes the repository's declared GLIBC floor. Both changed C files pass clangd
with the bookworm target headers configured locally.

For a stronger check than captured config alone, a separate process called the
**real pinned library's software header writers**, not the mock's packet output:

1. The actual plugin negotiated `bt709`→`2:4:7:1`→`bt709` at unchanged geometry
   through GstHarness, once for each codec. The existing mock captured its four
   `prep:*` values after each transition.
2. An isolated serializer process, with **no `LD_PRELOAD`**, applied those
   captured integers through real `mpp_enc_cfg_set_s32`. It used
   `h264e_sps_update`/`h264e_sps_to_packet` for H.264 and
   `h265e_set_extra_info`/`h265e_get_extra_info` for H.265. No `mpp_create`,
   `mpp_init`, pixel buffers, frame submission or encoder device was used.
3. A native host executable independently parsed the written Annex-B bytes
   with GStreamer's H.264/H.265 codec parsers (1.28.7), asserting all presence
   flags plus each leg's expected color values. It imports no MPP headers or
   serializer code. Both pre-fix B files fail that oracle (exit 9); all six
   post-fix files pass.

The library is `librockchip-mpp1_1.5.0-1_arm64.deb`, package SHA-256
`fe839d41010def25b2c096581815fd26214680bf9720fc47ff2c7afe501f6bcd`.
Private struct declarations came from its release source,
[`194af181db3a02a095c01db84e176d972e19b216`](https://github.com/tsukumijima/mpp-rockchip/tree/194af181db3a02a095c01db84e176d972e19b216).
H.265's SoC query used the no-device-tree fallback, not an emulated RK3588.

Every parsed file has one SPS, with `vui_parameters_present_flag=1`,
`video_signal_type_present_flag=1`, `colour_description_present_flag=1`,
`video_full_range_flag=0`, and `colour_primaries=1`:

| Header input | H.264 transfer / matrix | H.265 transfer / matrix |
|---|---|---|
| Pre-fix B after A | **1 / 1 (wrong: stale BT.709)** | **1 / 1 (wrong: stale BT.709)** |
| Fixed A | 1 / 1 | 1 / 1 |
| Fixed B (`2:4:7:1`) | **13 / 6** | **13 / 6** |
| Fixed return to A | 1 / 1 | 1 / 1 |

Retained B artifacts:

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| H.264 SPS | 31 | `26851d20bd3e65cf03d52de6ee91e1a940bb3fd70958606f8b48aa353bd97075` |
| H.265 VPS/SPS/PPS | 85 | `10d82223f16769f211a3e9d1182a1b4a9af5c8ac4887349936ce7d6c614c1f2c` |

Local raw evidence, probe sources and logs are retained in the ignored
`test-results/u6-srgb/` directory. The initial environment setup needed a
root-capable isolated rootfs and correct temporary-directory permissions; an
ABI-gate attempt also failed DNS until the resolver was mounted. These setup
failures are not RED regression evidence. The actual RED cases are the two
config regressions and the independently parsed stale-B headers above.

**Proof boundary:** these are real MPP-generated parameter-set bytes, but no
encoded picture or muxed stream was produced. This removes the demonstrated
sRGB mapping obstruction and verifies SPS serialization, **not** hardware
encoding, header delivery across a live switch, receiver continuity, pixel
accuracy, or U6's ten cycles. A candidate package must still be deployed by the
authorized hardware lane and its received muxed output parsed per active leg.
U6 remains uncredited; no board, PR, tag or release operation was performed.

Both upstream and downstream `GstForceKeyUnit` events request
`MPP_ENC_SET_IDR_FRAME` for the next submitted frame. With
`header-mode=each-idr`, that access unit also carries `GST_BUFFER_FLAG_HEADER`.
The software test backend verifies the control call and parses an H.264 type-5
NAL from a `videotestsrc` pipeline. Real H.264/H.265 bitstream and SPS VUI
verification with `ffprobe -show_streams` remains the board-gated todo 36; this
contract does not claim that hardware result.

The encoders are **PTS-only (no B-frames); DTS = PTS**. Every output frame sets
DTS explicitly rather than relying on an unset or inherited timestamp.
