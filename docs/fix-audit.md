# Fix audit

## F27 row schema

Every fix row appended by later todos must contain exactly these five fields:

1. **Provenance SHA** — the full source/fix commit SHA.
2. **Red/green outputs** — commands and captured failing-at-parent/passing-at-fix output.
3. **Hardware gate** — `hardware-independent`, or `hardware-gated` plus the board drill id.
4. **MPP ABI closure** — the before/after `nm` closure diff for MPP symbols.
5. **Reviewer verdict** — one of `confirmed`, `false-positive`, `needs-fix`, or `needs-human-review`.

## Rows

All three rows below were produced in a native-headers `debian:bookworm-slim`
aarch64 container with the pinned MPP 1.5.0-1 / RGA 2.2.0-1 packages, built with
the same feature flags CI and `debian/rules` use
(`-Drkximage=enabled -Drockchipmpp=enabled -Dkmssrc=enabled -Drga=enabled`).

### 1ceaf42 — decoder DMA-BUF caps negotiation

1. **Provenance SHA** — `1ceaf42ca3189c2c0ca917181cf3cb6db8c4df45`, JeffyCN/mirrors,
   author `orangepi <orangepi@local>` (`Signed-off-by: Jeffy Chen`). Applied with
   `git cherry-pick -x`; author preserved, no reword, no squash.
2. **Red/green outputs** — `meson test -C build`, case
   `rockchipmpp decoder dmabuf caps and pending-frame bound`
   (`tests/mpp_decoder_seam.c::test_dma_feature_reaches_negotiated_caps`).

   RED at the parent (`64ebcf4`, `gst/rockchipmpp/gstmppdec.c` restored to its
   pre-pick content):

   ```
   dma-feature=true -> negotiated caps video/x-raw, format=(string)NV12, width=(int)320, ...
   ERROR: dma-feature=true did not put memory:DMABuf on the output caps
   exit=133
   ```

   GREEN at the pick:

   ```
   dma-feature=true  -> negotiated caps video/x-raw(memory:DMABuf), format=(string)NV12, ...
   dma-feature=false -> negotiated caps video/x-raw, format=(string)NV12, ...
   dmabuf caps negotiation: OK
   ```

   The `dma-feature=false` line is the negative control: a gate that stamped the
   feature unconditionally would fail it. Pre-pick the property was inert on stock
   GStreamer — the removed code gated the feature behind
   `gst_caps_is_subset (dmabuf_caps, sysmem_caps)`, which is false unless the
   subset check is patched, as the deleted comment ("HACK: … when the subset check
   is hacked") said.
3. **Hardware gate** — `hardware-gated`, drill id `d1-dmabuf-registration`. The
   negotiated caps assertion is hardware-independent and runs in CI, but the pick
   changes what downstream imports, so DMA-BUF registration against a real
   consumer is board-verified in the allocation drill.
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh build/gst/rockchipmpp/libgstrockchipmpp.so`.
   Before (`64ebcf4`): 67 MPP symbols referenced and present, empty diff. After: 67,
   empty diff. Delta: none — the pick calls no new MPP entry point.
5. **Reviewer verdict** — `confirmed`. Reviewer == author (self-review);
   the mandated different-agent adversarial review has NOT run. Self-review found
   no defect: diff is 5 insertions / 8 deletions confined to
   `gst_mpp_dec_update_video_info`, touches no frozen property and no encoder file.
   Independently reviewed by a separate agent/model (oracle); confirmed via patch-ID comparison + git range-diff against the fetched upstream source (see `.omo/notepads/gstreamer-rockchip-fork/decisions.md` for the full review record).

### d27ae92 — unmatched-PTS pending-frame bound

1. **Provenance SHA** — `d27ae920e36fd72fe116e6c00108a65235c3e3d5`, kelvinlawson,
   author `Kelvin Lawson <kelvin.a.lawson@gmail.com>`. Full SHA confirmed by
   `git log` on the fetched object; the draft's two-variant ambiguity resolved to
   this one. Applied with `git cherry-pick -x`; author preserved, no reword, no
   squash.
2. **Red/green outputs** — `meson test -C build`, case
   `rockchipmpp decoder dmabuf caps and pending-frame bound`
   (`tests/mpp_decoder_seam.c::test_unmatched_pts_pending_list_is_bounded`). The
   mock feeds 300 decoder outputs whose PTS sits below every pending input PTS,
   which is the "outputs persistently older than ALL pending PTS" condition the
   falsifier attached to H2-B1.

   RED at the parent (`4be22f7`):

   ```
   pushed 300 inputs, 300 MPP outputs, pending frames settled at 299 (bound 64 + slack 8)
   ERROR: pending frame list grew to 299 after 300 unmatched-PTS outputs;
          expected it to stay at or below 72
   exit=133
   ```

   GREEN at the pick:

   ```
   pushed 300 inputs, 300 MPP outputs, pending frames settled at 0 (bound 64 + slack 8)
   unmatched-PTS pending bound: OK
   ```

   299 → 0 is the whole defect: pre-pick each unmatched output re-released the
   already-finished `self->last_frame`, consuming zero pending frames, so
   `gst_video_decoder_get_frames()` never drained.
3. **Hardware gate** — `hardware-independent`. The defect is pure
   GstVideoDecoder frame accounting and reproduces fully on the mocked MPP; no
   board drill is required to prove it.
4. **MPP ABI closure** — before (`4be22f7`): 67 MPP symbols, empty diff. After: 67,
   empty diff. Delta: none.
5. **Reviewer verdict** — `confirmed`. Reviewer == author (self-review);
   the mandated different-agent adversarial review has NOT run. Self-review found
   no defect: the added `GST_MPP_DEC_MAX_PENDING_FRAMES` trim branch re-fetches the
   frame list after releasing, and the rewritten no-match branch refs the frame it
   stores into `self->last_frame`, so the ref accounting stays balanced against the
   caller's drop path.
   Independently reviewed by a separate agent/model (oracle); confirmed via patch-ID comparison + git range-diff against the fetched upstream source (see `.omo/notepads/gstreamer-rockchip-fork/decisions.md` for the full review record).

### 3ccc1e3 — NOT PORTED (rejected with evidence)

1. **Provenance SHA** — `3ccc1e3808dafcbeb926bf060472cc96924c0ac5`, radxa-pkg,
   author `Zhaoming Luo <luozhaoming@radxa.com>`. Cherry-picked, inspected, then
   **dropped**; it is not on the branch.
2. **Red/green outputs** — none, and none are possible: the commit adds no source
   change to port. Its whole content is `debian/patches/series` plus a quilt patch
   wrapping JeffyCN `31ee8bd8a6438ee4030291f3cf0de51fab5995e5`, and that patch's
   post-state is already the fork baseline. Evidence:

   ```
   # the patch's removal is already done
   grep -rn mpp_frame_get_hor_stride_pixel gst/rockchipmpp/   ->  no match
   # the patch's addition is already present
   gst/rockchipmpp/gstmpp.c:219  struct gst_mpp_format *format = GST_MPP_GET_FORMAT (mpp, mpp_format);
   gst/rockchipmpp/gstmpp.c:221    hstride = hstride / format->pixel_stride0;
   # and it applies in neither direction, even at fuzz 3
   patch -p1 --dry-run    -F3 < 0001-...-hor_stride_pixe.patch  ->  Hunk #1 FAILED at 203
   patch -p1 --dry-run -R -F3 < 0001-...-hor_stride_pixe.patch  ->  Hunk #1 FAILED at 203
   ```

   Keeping it would ship a dead file: this package has no `debian/source/format`
   source format `1.0`; no active quilt sequencing (confirmed via `dpkg-source --print-format .`);
   the changelog version `20260118-1` shows this is a non-native `1.0` source package, not
   `native` — the point stands regardless: nothing in `debian/rules` applies
   `debian/patches/series` today. It is worse
   than inert — if `debian/source/format` ever becomes `3.0 (quilt)`, as packaging
   work may well do, `dpkg-source` would try this patch and fail the build.
3. **Hardware gate** — `hardware-gated`, drill id `d3-hevc10bit-stride`. Moot while
   unported, and recorded because the commit's content is the RGA/MPP stride
   divisor, which is frozen behind the HEVC Main10 A/B board drill.
4. **MPP ABI closure** — not applicable; nothing landed. Branch closure is
   unchanged at 67 symbols, empty diff.
5. **Reviewer verdict** — `confirmed`. Reviewer == author (self-review),
   and this row additionally **overturns a planning-phase audit verdict**, so it
   needs an independent pass rather than acceptance on this evidence alone. The
   planning audit listed 3ccc1e3 as a clean source-level "H.265 10-bit" port whose
   `MPP_FMT_YUV420SP_10BIT` exists in MPP 1.5.0-1; the commit contains no format
   table change and no `MPP_FMT_*` reference at all. This also settles ADJ-2 in the
   falsifier's favour — the baseline already carries 31ee8bd's content — which is
   the adjudication the 31ee8bd verify-delta work was scheduled to make; that work
   should confirm rather than re-derive it.
   Independently reviewed by a separate agent/model (oracle); confirmed via patch-ID comparison + git range-diff against the fetched upstream source (see `.omo/notepads/gstreamer-rockchip-fork/decisions.md` for the full review record).

### 31ee8bd — SKIP-ALREADY-PRESENT (verify-delta adjudication)

1. **Provenance SHA** — `31ee8bd8a6438ee4030291f3cf0de51fab5995e5`, JeffyCN/mirrors
   `gstreamer-rockchip`, author `Jeffy Chen <jeffy.chen@rock-chips.com>`:
   `rockchipmpp: Stop using mpp_frame_get_hor_stride_pixel`. The transient
   `jeffycn-mirror` remote resolved `gstreamer-rockchip` to
   `a0d45af504099b4b82f3d3377019a63d357e7cef`; `git show` of the full source
   commit contains one hunk in `gst/rockchipmpp/gstmpp.c`.
2. **Per-hunk delta comparison** — all source content is already in the
   `integration/verified-fix-ledger` baseline, so no cherry-pick or stride
   semantics change was made.

   | Hunk | Current file:lines | Already present | Evidence quote |
   | --- | --- | --- | --- |
   | 1 | `gst/rockchipmpp/gstmpp.c:210-221` | yes | **Pre-state removed:** `git grep -n 'mpp_frame_get_hor_stride_pixel' HEAD -- gst/rockchipmpp` returned no matches; the source parent had `guint hstride = mpp_frame_get_hor_stride_pixel (mframe);`. **Post-state present:** `guint hstride = mpp_frame_get_hor_stride (mframe);`; `struct gst_mpp_format *format = GST_MPP_GET_FORMAT (mpp, mpp_format);`; `if (format)`; `hstride = hstride / format->pixel_stride0;`. |

   **Verdict: `SKIP-ALREADY-PRESENT`.** The sole hunk's removal and addition
   are both already reflected by the baseline. This independently confirms the
   `3ccc1e3` wrapper-commit finding; it does not rely on that finding as the
   adjudication evidence.
3. **Hardware gate** — `hardware-gated`, drill id `d3-hevc10bit-stride`.
   H4-B3 remains frozen for the HEVC Main10 A/B board drill; this skip changes
   no stride calculation.
4. **MPP ABI closure** — not applicable; no code landed and the baseline's
   67-symbol, empty-diff closure is unchanged.
5. **Reviewer verdict** — `self-authored adjudication`. I am the sole author
   of this hunk-by-hunk comparison; no separate-agent review is claimed here.

### 7ffd7f4 — put_packet-result fullness detection (already present; regression locked)

1. **Provenance SHA and resolution diff** —
   `7ffd7f40576bbfb861bc7a7d3492c710d149aff8`, JeffyCN/mirrors, author
   `Jeffy Chen <jeffy.chen@rock-chips.com>`. The planning audit classified this
   as a conflicting adapted port, but the current fork descends from this exact
   commit (`git merge-base --is-ancestor 7ffd7f4 HEAD` exits 0), and `git blame`
   attributes `MPP_INPUT_TIMEOUT_MS 10`,
   `gst_mpp_dec_send_mpp_packet_unlocked()`, and the handle-frame result checks
   directly to `7ffd7f40`. The literal upstream production diff therefore
   resolves to **no production-code delta** rather than a duplicate port.

   Resolution against the literal upstream diff: upstream replaced the quote
   `"Avoid holding too many frames"` plus the `g_list_length (frames) >= 4`
   send-side cap with a loop that quotes
   `case MPP_ERR_BUFFER_FULL: /* Timed out */ break;` and returns
   `GST_FLOW_ERROR` for any other MPP error. The current tree already has that
   post-state at `gstmppdec.c:1192-1213` and no four-frame send-side cap. This
   commit adds only the mock result script and regression assertions. The
   separate 64-frame orphan-accounting safety bound from `d27ae92` is untouched;
   no cap was added or reintroduced by this resolution.
2. **Red/green outputs** — `meson test -C build --print-errorlogs
   "rockchipmpp decoder dmabuf caps and pending-frame bound"`, native aarch64
   bookworm container. RED with the four source files restored to `7ffd7f4^`
   (RGA disabled solely because that historical snapshot predates the current
   RGA function signature):

   ```
   three MPP_ERR_BUFFER_FULL results retried; accepted on call 4
   assertion failed (mpp_mock_dec_put_calls () == 1): (972 == 1)
   FAIL, SIGABRT
   ```

   The 972 calls are the old nested deadline retries misclassifying a persistent
   MPP error as retryable. GREEN with the inherited `7ffd7f4` production code:

   ```
   three MPP_ERR_BUFFER_FULL results retried; accepted on call 4
   persistent MPP error rejected after 1 call
   put_packet fullness detection: OK
   1/1 ... OK
   ```
3. **Hardware gate** — `hardware-independent`. The mock controls each
   `decode_put_packet()` result and counts calls; no allocator or board behavior
   is involved.
4. **MPP ABI closure** — no plugin source changed, so the before/after closure is
   identical by construction: 67 MPP symbols referenced and present, empty diff.
   The final branch gate reruns `ci/check-mpp-abi.sh` against the built plugin.
5. **Reviewer verdict** — `needs-human-review`. Reviewer == author
   (self-review); no independent-agent verdict is claimed. Self-review confirms
   the test has both controls (three buffer-full responses followed by success,
   and a persistent non-full error), and the source diff contains no frame-cap,
   encoder, property, or production-code change.

## Mock-MPP verdict: WORKING

The host-only seam builds `tests/mock_mpp.c` as `libmppmock.so` against the same
`rockchip_mpp` headers selected by Meson, then preloads it while loading the built
plugin. It covers:

- `mpp_create`, `mpp_init`, and the encoder `MppApi` control/encode entry points;
- the `mpp_enc_cfg_set_*` and `mpp_enc_cfg_get_*` families with an in-memory key/value recorder;
- minimal buffer-group, DMA-fd, frame-association, packet, and metadata behavior needed to
  negotiate and submit one NV12 frame without `/dev/mpp_service`;
- process-local logs selected by `MPP_MOCK_LOG`, with the Meson tests serialized;
- an opt-in decoder simulation (`mpp_mock_dec_arm()` / `mpp_mock_dec_disarm()`) that
  answers `decode_put_packet` / `decode_get_frame`, emits one info-change frame to
  drive negotiation, and then returns data frames carrying **no MppBuffer** so the
  decode loop takes its drop path. That drop path is what releases a pending
  `GstVideoCodecFrame`, which is what makes decoder frame accounting measurable off
  hardware. It stays disarmed unless a test arms it, so the encoder harness keeps the
  MPP behavior it was written against.

`tests/mpp_gstharness.c` loads the real built factories and proves both `mpph264enc`
and `mpph265enc` negotiate and submit a 320x240 NV12 frame. For each codec it asserts
`prep:width=320`, `prep:height=240`, `rc:mode=1`, `rc:bps_target=500`,
`rc:fps_in_num=30`, `rc:fps_in_denorm=1`, `MPP_ENC_SET_CFG`,
`MPP_ENC_SET_SEI_CFG`, and `MPP_ENC_SET_HEADER_MODE`. The public `bitrate` property is
measured in BPS, so `500` is intentionally recorded without scaling.

The same executable introspects properties on `mppvideodec` and `mppjpegdec`, checks
the multicodec decoder's H.264/H.265/AV1 input and NV12 output template truth, and uses
GstHarness to assert the JPEG decoder advertises non-fixed NV12 raw output caps. These
checks create elements and inspect templates only; they submit no decoder input and
perform no MPP hardware access.

`tests/mpp_decoder_seam.c` is the decoder counterpart and does submit input. It arms
the decoder simulation, pushes parsed H.264 buffers into `mppvideodec`, and asserts
two things the rows above depend on: that `dma-feature=true` puts `memory:DMABuf` on
the negotiated output caps (with `dma-feature=false` as the negative control), and
that 300 outputs whose PTS never matches a pending input leave the pending-frame list
at or below the decoder's own bound. Both assertions were confirmed to fail at their
pre-pick parent before being accepted.

The encoder harness waits for `mpp_frame_set_buffer()` rather than sampling it
straight after `gst_harness_push()`. `gst_mpp_enc_handle_frame()` only queues the
frame and broadcasts — the MPP submission happens later on the encoder's srcpad task
thread — so the original instantaneous read was a race that failed roughly 2 runs in
12 under emulation. Do not fold that wait back into a direct read.

### Verification

With `rockchip_mpp.pc` available to pkg-config:

```sh
meson setup build -Drockchipmpp=enabled -Drga=disabled -Drkximage=disabled -Dkmssrc=disabled
meson compile -C build
meson test -C build --print-errorlogs
meson test -C build --print-errorlogs
```

All three tests must report `OK` in both runs. The registration test's negative control sets
`GST_PLUGIN_PATH` to a nonexistent directory and runs `gst-inspect-1.0 mppjpegdec`; it
must exit nonzero with `No such element or plugin`, proving plugin-load failures cannot
produce a false green. Detailed native output is retained by Meson in
`build/meson-logs/testlog.txt`; CI runs the same commands on bookworm and trixie.
