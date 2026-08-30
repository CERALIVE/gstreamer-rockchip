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
5. **Reviewer verdict** — `confirmed`. Reviewer == author
   (self-review); no independent-agent verdict is claimed. Self-review confirms
   the test has both controls (three buffer-full responses followed by success,
   and a persistent non-full error), and the source diff contains no frame-cap,
    encoder, property, or production-code change. Independently reviewed (oracle): confirmed 7ffd7f4 was already an ancestor of the fork baseline and no frame cap was reintroduced; the commit adds regression evidence, not new production logic.

### 5f45bd4 — decoder input-packet ownership snapshot and reset cleanup

1. **Provenance SHA and resolution diff** —
   `5f45bd4c3868e323d40eda744cb7aa92b430df09`, JeffyCN/mirrors, author
   `Jeffy Chen <jeffy.chen@rock-chips.com>`. The reset hunk ports literally:
   before `self->mpi->reset()` it deinitializes `self->mpp_frame` and clears the
   pointer. The ownership hunk ports the pre-send quote
   `packet_has_buffer = (mpp_packet_get_buffer (mpkt) != NULL);` and, after a
   successful send, transfers buffered packets to MPP while deinitializing
   copy-path packets in the base decoder.

   Resolution against the literal upstream diff: upstream `5f45bd4` sits after
   `ec14edc` moved video packet submission into the base decoder and after
   `dcbcd64` moved copy-packet cleanup there. This fork retains the older
   `GstMppDecClass::send_mpp_packet` callback, whose video implementation quoted
   `if (!ret) mpp_packet_deinit (&mpkt);`. Applying only the literal
   `gstmppdec.c` hunk would therefore double-deinitialize copy packets. The
   adaptation removes that subclass deinit and makes the base decoder the single
   success-path ownership authority; JPEG packets already carry `MppBuffer` and
   stay on the transfer branch. No encoder file or property is touched.
2. **Red/green outputs** — `meson test -C build --print-errorlogs
   "rockchipmpp decoder dmabuf caps and pending-frame bound"`, native aarch64
   bookworm container. RED at parent `d2f7874` produced both independent
   failures:

   ```
   reset cached-frame cleanup:
   assertion failed (mpp_mock_dec_frame_deinits () == before_reset + 1): (1 == 2)

   packet ownership before send:
   assertion failed (mpp_mock_dec_packet_buffer_queries () == 1): (0 == 1)
   ```

   GREEN at the adaptation:

   ```
   cached MPP frame deinits: 1 -> 2 on reset
   copy packet released once; buffered packet transferred without post-send access
   packet ownership before send: OK
   1/1 ... OK
   ```

   The mock is a logical ownership canary: it records buffer queries after send,
   duplicate deinitialization, and caller deinitialization after MPP ownership
   transfer. All three counters must stay zero. This catches the UAF/premature
   release class without depending on allocator address reuse.
3. **Hardware gate** — `hardware-independent`. Packet ownership transitions and
   cached-shell cleanup are fully observable through the MPP ABI seam; no DMA
   import or board-only output buffer is required.
4. **MPP ABI closure** — before: 67 MPP symbols referenced and present, empty
   diff. After: 67, empty diff against pinned
   `librockchip-mpp1_1.5.0-1_arm64.deb`. The adapted layout adds no new MPP ABI
   requirement.
5. **Reviewer verdict** — `confirmed`. Reviewer == author
   (self-review); no independent-agent verdict is claimed. Self-review checked
   both callback implementations: video copy packets now have exactly one base
   deinit, buffered JPEG packets are transferred, and every error path retains
   the existing base-level `mpp_packet_deinit()` cleanup. Because this is a
   cross-layer ownership adaptation, the branch is intentionally left open for
    independent review rather than self-merged. Independently reviewed (oracle): traced the ownership handoff against the pinned MPP 1.5.0-1 source (`Mpp::put_packet()` branches on `mpp_packet_get_buffer()`) — confirmed exactly one release authority on every success/error path across `gstmppvideodec.c` and `gstmppjpegdec.c`, no double-free, no leak.

### 892f662 — selective DMA_DRM caps negotiation

1. **Provenance SHA and hunk resolution** —
   `892f662465b54de9fe10b9c691f64d3dbe047248`, kelvinlawson, author
   `Kelvin Lawson <kelvin.a.lawson@gmail.com>`. The complete two-hunk diff was
   inspected before adaptation. Only the negotiation block was ported; the
   fork's output-state construction, FBC flags, `self->info` assignment,
   negotiation call, and stride alignment remain unchanged.

   | Upstream hunk | Decision | Resolution |
   | --- | --- | --- |
   | Include `gst/video/video-info-dma.h` | include, adapted | Guarded with `GST_CHECK_VERSION(1, 24, 0)` because the bookworm/GStreamer 1.22 build does not ship this header. |
   | Replace the `dma_feature` caps block with peer-selected legacy DMABuf + DMA_DRM offers | include, corrected selective port | On GStreamer 1.24+, modern DMA_DRM construction is now restricted to `!afbc && !rfbc`; compressed output keeps legacy `memory:DMABuf` caps so `arm-afbc=1` / `rfbc=1` survives. Only `output_state->caps` can be replaced for linear output; `output_state->info` remains the fork's existing state. Empty/ANY/unready peers retain sysmem caps for a later RECONFIGURE. GStreamer 1.22 keeps the exact legacy DMABuf feature path. |
   | Any output-state/FBC/stride behavior outside the two source hunks | exclude | Not present in the upstream diff and intentionally untouched; this is the auditor's selective-port boundary. |

2. **Red/green outputs** — `meson test -C build --verbose
   "rockchipmpp decoder dmabuf caps and pending-frame bound"`. The new case
   supplies a DMA_DRM-only downstream peer through GstHarness. On the aarch64
   trixie/GStreamer 1.26 build, with only the pre-existing GCC-14 crop-meta cast
   applied to the disposable container copy so the test could execute:

   RED at parent `de535020`:

   ```
   DMA_DRM-only peer -> negotiated caps video/x-raw(memory:DMABuf), format=(string)NV12, ...
   assertion failed (format == "DMA_DRM"): ("NV12" == "DMA_DRM")
   expected_dma_drm_red_exit=1
   ```

   GREEN after the selective port:

   ```
   DMA_DRM-only peer -> negotiated caps video/x-raw(memory:DMABuf),
     format=(string)DMA_DRM, ..., drm-format=(string)NV12
   DMA_DRM peer caps negotiation: OK
   1/1 rockchipmpp decoder dmabuf caps and pending-frame bound OK
   ```

   Independent review then found that the first submission also applied modifier
   zero to AFBC/RFBC `output_state->info`. `gst_video_info_dma_drm_to_caps()` did
   not serialize the fork's private compression field, producing this second RED
   on the first-submission commit `05f191c`:

   ```
   AFBC frame with DMA_DRM query peer -> negotiated caps
     video/x-raw(memory:DMABuf), format=(string)DMA_DRM, ...,
     drm-format=(string)NV12
   assertion failed (format != "DMA_DRM"): ("DMA_DRM" != "DMA_DRM")
   expected_afbc_red_exit=1
   ```

   GREEN after restricting modern DMA_DRM construction to linear output:

   ```
   AFBC frame with DMA_DRM query peer -> negotiated caps
     video/x-raw(memory:DMABuf), format=(string)NV12, ...,
     arm-afbc=(int)1
   AFBC output rejects linear DMA_DRM caps: OK
   ```

   The aarch64 bookworm/GStreamer 1.22 leg prints the explicit version skip for
   DMA_DRM and keeps both existing controls green: `dma-feature=true` negotiates
   legacy `video/x-raw(memory:DMABuf), format=NV12`, while
   `dma-feature=false` negotiates plain `video/x-raw`.
3. **Hardware gate** — `hardware-gated`, drill id
   `d4-dma-drm-allocation-soak`. Todo 26 must run the mandatory F19 136-second
   allocation drill on the vendor-6.1 board and record both
   `RGA_BLIT failures == 0` and `rga_api version failures == 0`, including the
   1080-to-1088 alignment/import path. CI proves caps selection, not the physical
   DMA allocation/import contract.
4. **MPP ABI closure** — before (`de535020`): 67 MPP symbols referenced and
   present, empty diff. After: 67, empty diff against pinned MPP 1.5.0-1. The
   adaptation calls GStreamer video/caps APIs only and adds no MPP entry point.
5. **Reviewer verdict** — `needs-human-review`. The first independent oracle
   review returned `needs-fix`: it found the silent AFBC/RFBC-as-linear
   misadvertisement that the original linear-only test missed. The corrected
   submission gates zero-modifier DMA_DRM caps to linear output and adds the
   AFBC regression above. Reviewer != correction author for the finding, but a
   second independent review of the correction has not run yet. The PR remains
   open and is not self-merged.

### b93ecb6 — RGA DMA32 and used-path hunk adjudication

1. **Provenance SHA and hunk resolution** —
   `b93ecb63b0e50f4b2220cf5219d88d93ad855a21`, kelvinlawson, author
   `Kelvin Lawson <kelvin.a.lawson@gmail.com>`, derived upstream from
   BoxCloudIRL `3b58acf`. The full diff was inspected. Its required DMA32 and
   used shared-core error behavior already predate this fork baseline as
   independent commits, so this adaptation adds a regression lock rather than
   duplicating production code.

   | Upstream hunk | Include/exclude | Current-tree resolution |
   | --- | --- | --- |
   | `gstmppallocator.c`: request `MPP_BUFFER_FLAGS_DMA32` for the internal DRM group | include | Already present at `gst_mpp_allocator_new()` from inherited `e53dca6`; pinned MPP 1.5.0-1's installed `mpp_buffer.h` defines the flag as `0x00200000`. Added a mock-MPP assertion that records the real group type and requires the flag. |
   | `gstmpp.c`: reject unknown MPP-to-RGA formats | include (used decoder RGA path) | Already present from inherited `e4c76e0`; no duplicate edit. |
   | `gstmpp.c`: reject unknown GstVideoInfo-to-RGA formats | include (used encoder and decoder RGA path) | Already present from inherited `e4c76e0`; no duplicate edit. |
   | `gstmpp.c`: add YUYV/YVYU/UYVY/VYUY and ARGB-family RGA table entries | exclude from this port | These specific formats are not exercised by cerastream's NV12 encoder/used-decoder paths. The entries already exist from inherited `508af8a` and are left unchanged; this task neither claims nor deep-reviews unused-format quality. |
   | `gstmpp.c`: route RGB16/BGR16 through swapped RGA formats | exclude from this port | Cerastream does not use these formats. The inherited `b3b7924` behavior is retained unchanged; no unused-element/format quality work was performed. |

2. **Red/green outputs** — `meson test -C build --verbose
   "rockchipmpp decoder dmabuf caps and pending-frame bound"`, aarch64
   bookworm/GStreamer 1.22. In a disposable parent-tree copy, removing only the
   inherited DMA32 flag produced:

   ```
   internal MPP buffer group type flags: 0x3
   assertion failed (group_types & MPP_BUFFER_FLAGS_DMA32 ==
     MPP_BUFFER_FLAGS_DMA32): (0 == 2097152)
   expected_red_exit=1
   ```

   With the inherited production code intact:

   ```
   internal MPP buffer group type flags: 0x200003
   decoder allocator DMA32 request: OK
   1/1 rockchipmpp decoder dmabuf caps and pending-frame bound OK
   ```

   This is a regression lock for an already-present required hunk, not evidence
   that this task introduced the allocator behavior.
3. **Hardware gate** — `hardware-gated`, drill id
   `d5-rga-dma32-allocation-soak`. Todo 26 must run the mandatory F19 136-second
   allocation drill on the vendor-6.1 board and record both
   `RGA_BLIT failures == 0` and `rga_api version failures == 0`; the mock proves
   the requested flag, while only the board proves the allocated address is
   RGA2-accessible throughout the soak.
4. **MPP ABI closure** — before and after: 67 MPP symbols referenced and
   present, empty diff against pinned MPP 1.5.0-1. No production hunk was needed;
   the mock-only recorder adds no undefined symbol to the plugin.
5. **Reviewer verdict** — `confirmed`. Independent oracle review checked the
   full upstream `b93ecb63b0e50f4b2220cf5219d88d93ad855a21` diff against the
   inherited source SHAs, confirmed `MPP_BUFFER_FLAGS_DMA32` in pinned MPP
   1.5.0-1, and confirmed the mock assertion is substantive. No correction was
   requested for this row.

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
