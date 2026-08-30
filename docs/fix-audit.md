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
5. **Reviewer verdict** — `confirmed`. The first independent oracle
   review returned `needs-fix`: it found the silent AFBC/RFBC-as-linear
   misadvertisement that the original linear-only test missed. The corrected
   submission gates zero-modifier DMA_DRM caps to linear output and adds the
   AFBC regression above. Reviewer != correction author for the finding. The PR
    remains open and is not self-merged. Independently reviewed (oracle) in TWO
    passes: pass 1 found a real defect (AFBC/RFBC-compressed output could be
    silently advertised as linear `format=DMA_DRM`, since
    `gst_video_info_dma_drm_to_caps()` doesn't serialize this fork's private
    `arm-afbc`/`rfbc` caps fields); fixed by gating the modern DMA_DRM path behind
    `!afbc && !rfbc` and preserving the legacy `memory:DMABuf` caps path with
    `arm-afbc`/`rfbc` intact for compressed output. Pass 2 confirmed complete
    branch coverage (linear/AFBC/RFBC/both/failure/disabled all reach exactly one
    caps outcome) and verified the new regression test genuinely exercises the
    AFBC path via `start_afbc_decoder()`/`dec_new_frame()`.

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

## Frozen JeffyCN extras audit (`a0d45af`)

The audit was limited to the 15 named commits below. The transient
`jeffycn-mirror` remote resolved `gstreamer-rockchip` to
`a0d45af504099b4b82f3d3377019a63d357e7cef`, and every full SHA below is an
ancestor of that frozen tip. All 15 touch a used decoder/encoder or shared core;
none qualifies for `SKIP-OUT-OF-SCOPE`. None adds an MPP entry point, so no row
is `BLOCKED-MPP-VERSION` against pinned MPP 1.5.0-1.

| Candidate | Baseline defect and current-code evidence | Used/shared scope | Pick trial | MPP ABI | Verdict |
| --- | --- | --- | --- | --- | --- |
| `27ba1c798f320ebe40c4f58cbb2fd8940baa97c9` — clear pending frames after reset | Already fixed: the commit is an ancestor, and `gstmppdec.c:267-272` quotes `frames = gst_video_decoder_get_frames (decoder);` followed by `gst_video_decoder_release_frame (decoder, f);`. | `mppvideodec`/`mppjpegdec` shared decoder core | conflict in `gstmppdec.c` | no new call; 67-symbol closure unchanged | `SKIP-ALREADY-PRESENT` |
| `65098a9b3ae9180be5f1b361f576c9ee1434da2a` — EOS without buffer | Already fixed: ancestor; `gstmppdec.c:1126-1135` rejects `!mpp_frame_get_buffer (mframe)` before ordinary frame-info-change processing, while the dedicated info-change frame remains handled at `:1113-1116`. | shared decoder core | conflict in `gstmppdec.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `0489caabc314cc167a87545a14109b2bd2dda36a` — seek after finish | Already fixed: ancestor; allocator attachment is deferred to `gst_mpp_dec_handle_frame()` (`gstmppdec.c:1287-1296`), getters reject a missing allocator, and clear detaches `MPP_DEC_SET_EXT_BUF_GROUP` at `:320`. | shared decoder core | clean, but the only staged delta duplicated the existing `if (!self->allocator) return NULL;` guard | no new call | `SKIP-ALREADY-PRESENT` |
| `6c90d19b502e36f6228b2d3d31230363bb690b2d` — stop-time frame leak | Already fixed: ancestor; stop calls `mpp_frame_deinit (&self->mpp_frame)` at `gstmppdec.c:345`. | shared decoder core | clean empty/no-op | no new call | `SKIP-ALREADY-PRESENT` |
| `c5601186c8c07c24845e805094a35b1045911bf7` — leftover encoder packets after reset | Present at baseline: reset called `mpi->reset` and immediately zeroed accounting without polling output. The adapted post-state at `gstmppenc.c:797-804` now drains `gst_mpp_enc_poll_packet_locked()` before clearing `pending_frames`. | H.264/H.265 shared encoder core | conflict in `gstmppenc.c`; manually adapted | no new external call; static helper only | `PORT-WITH-ADAPTATION` (`5b450adc`) |
| `973fd0e6815dbcc37e134724f05eebdc44fc4c7e` — return MppBuffer before group clear | False-positive mechanism: GStreamer destroys mini-object qdata and runs `gst_mpp_mem_destroy()` before entering the allocator `free()` vfunc, so the MppBuffer is already returned in both orderings. The clean pick was reviewed and reverted by `f3b8775d`. | allocator shared by all used elements | clean, then reverted after oracle review | no new call; 67 symbols before/after | `SKIP` — reviewer verdict `false-positive` |
| `dcbcd6454ef892e385b3a782600369eb6c0719db` — input packet leak | Already fixed by the stricter `5f45bd4` adaptation: `gstmppdec.c:1317-1336` snapshots `packet_has_buffer` before send, transfers buffered packets to MPP, and deinitializes copy packets exactly once. | shared decoder core | conflict in `gstmppdec.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `ed10c9135ff5bbc80c4fb2d714687192ae0ac7e1` — encoder DMA-fd leak typo | Already fixed: ancestor; both failure and packet-return paths pass addresses to `mpp_frame_deinit (&mframe)` (`gstmppenc.c:1403,1432`). | H.264/H.265 shared encoder core | conflict in `gstmppenc.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `ed9f828600994b174a65130f27a5f48ad1d81228` — inverted frame-init result | Already fixed: ancestor; `gstmppenc.c:1375` returns on nonzero `mpp_frame_init (&mframe)`, preserving MPP's zero-is-success convention. | H.264/H.265 shared encoder core | conflict in `gstmppenc.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `bbd1fdaf804ce3b4b15e8f429c2c17ff4e84876a` — RGA hstride units | Superseded, not absent: current `gstmpp.c:210-221` uses byte stride then divides by `format->pixel_stride0`, the later `31ee8bd` post-state already adjudicated above. Reapplying `bbd1fda` would restore the removed MPP accessor. No stride code changed. | shared RGA conversion core | clean mechanical reversion of the later fix | no new call | `SKIP-ALREADY-PRESENT`; frozen H4-B3 remains drill `d3-hevc10bit-stride` |
| `e811521f3cf76550d61d842a0fc62a6f3e587f0b` — unaligned encoder input | Already fixed: ancestor; `gstmpp.c:410-415` rejects undersized alignment, and `gstmppenc.c:1268-1306` imports only matching aligned layouts before falling back to conversion. | H.264/H.265 shared encoder/core | conflict in `gstmppenc.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `cce848d65143ad817f4b4f6cd54e4ed6ac622809` — apply strides from new buffers | Already fixed: ancestor; `gst_mpp_enc_apply_strides()` is called from set-format, runtime-resolution, and imported-buffer paths (`gstmppenc.c:1063,1144,1287`). The incidental JPEG-encoder hunk is irrelevant to the already-present shared-core fix. | shared encoder/core (plus one unused `mppjpegenc` hint-only hunk) | conflict in `gstmppenc.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `f9c7186c5f7eb04667978384258c112d17bfa4be` — clear cached buffers when unused | Already fixed: ancestor; allocator `cacheable` state and group clears remain at `gstmppallocator.c:85-92,276-281`, decoder clear disables caching, and encoder start does likewise. | allocator + used encoder/decoder core | conflicts in decoder and encoder | no new call | `SKIP-ALREADY-PRESENT` |
| `bbe50f15ba4ee41a48114bf87e455dfe625261eb` — MPP warning on stop | Already fixed: ancestor; `gstmppdec.c:342-349` clears the allocator before `mpp_destroy (self->mpp_ctx)`. | shared decoder core | conflict in `gstmppdec.c` | no new call | `SKIP-ALREADY-PRESENT` |
| `a910efebc5f438eacbe8eda66a0ed1c01a43f787` — JPEG packet-send timeout | Present at baseline: JPEG ignored `poll()` and base decoder treated timeout as an unknown fatal error. Ported code checks poll at `gstmppjpegdec.c:366-371`; base retry accepts `MPP_ERR_TIMEOUT` at `gstmppdec.c:1256-1264`. | `mppjpegdec` + shared decoder core | clean | no new call; 67 symbols before/after | `PORT` (`a1de3d17`) |

### c560118 — encoder reset output-queue drain

1. **Provenance SHA** — `c5601186c8c07c24845e805094a35b1045911bf7`,
   JeffyCN/mirrors, author Jeffy Chen. The conflict was only surrounding fork
   evolution; the adapted production delta preserves the literal reset-time poll
   loop. The original poll-only test was rejected by independent oracle review;
   substantive replacement seam: `1f2609e4`, hardened against reset-generation
   and packet-release mutations by `754e12e5`, then expanded to the encoder's
   maximum 16-packet backlog by `61ad9c2d`.
2. **Red/green outputs** — native aarch64 bookworm mock-MPP test
   `rockchipmpp GstHarness factories and caps`. With `gstmppenc.c` restored to
   pre-port `gstmppenc.c` from `5b450adc^`, RED:

   ```
    test_encoder_reset_drains_old_packets_before_new_session:
      mpp_mock_enc_dequeued_packets() (5) is not equal to 16
   FAIL
   ```

   At `5b450adc` plus the complete test series, GREEN:

   ```
   reset generation 1 drained old packets: dequeued=16 depth=0 empty-polls=1
     deinits=16 double-deinits=0 live=0 duplicates=0
   new session output: packet=17 pts=3333333300 dequeued=17 depth=0
     deinits=17 live=0
   reset generation 3 variable backlog: dequeued=19 depth=0 empty-polls=1
     deinits=19 live=0
   1/1 ... GstHarness factories and caps OK
   ```

   The mock fills the encoder's configured `max-pending=16` capacity, releases
   packet 1 to force a downstream error and pause the task, withholds packets
   2-16 until `mock_reset()`, and retains the matching GstVideoCodecFrames. The
   first reset generation must perform exactly one empty poll and release all 16
   packets exactly once. Packet 17 then carries the fresh-session PTS and is also
   released. A second, one-withheld-packet reset uses a different generation and
   requires exactly one empty poll; this makes over-polling fixed-count mutants
   observable instead of letting a hardcoded 16 accidentally match the max case.

    Four disposable mutation checks bound these claims:

   ```
   # reset loop replaced by exactly two poll calls
   dequeued_packets() (5) is not equal to 16
   FAIL

   # mpp_packet_deinit(&mpkt) removed
   mpp_mock_enc_packet_deinits() (0) is not equal to 16
   FAIL

   # reset loop replaced by exactly three poll calls
   dequeued_packets() (7) is not equal to 16
   FAIL

   # reset loop replaced by exactly sixteen poll calls
   variable reset empty_polls (15) is not equal to 1
   FAIL
   ```
3. **Hardware gate** — `hardware-independent`. The mock exercises MPP packet
   retention plus real GstVideoEncoder frame accounting and state transitions;
   no codec device is required.
4. **MPP ABI closure** — before and after: 67 symbols referenced and present,
   empty diff against pinned MPP 1.5.0-1. The port calls an existing static helper.
 5. **Reviewer verdict** — `Confirmed across FOUR independent review passes. Round 1:
    production code confirmed correct (encoder-reset ordering), initial test found
    insubstantial. Round 2 (mutation testing): found test didn't scope 'empty' to the
    correct reset generation, didn't verify deinit counts. Round 3 (mutation testing):
    found a hardcoded-3-poll mutant survived. Round 4: expanded to a 16-packet
    max-backlog scenario plus a second, differently-sized reset generation, which
    mathematically rules out any single fixed poll count satisfying both — confirmed
    no further credible mutant found after an active adversarial search.`

### 973fd0e — allocator release ordering

1. **Provenance SHA** — `973fd0e6815dbcc37e134724f05eebdc44fc4c7e`,
   JeffyCN/mirrors, author Jeffy Chen. It was applied exactly with `git cherry-pick
   -x` as `ad1930e7`, independently reviewed, found false-positive on its claimed
   mechanism, and reverted by `f3b8775d`.
2. **Red/green outputs** — none: there is no defect to reproduce. GStreamer's
   actual sequence is `gst_mini_object_unref()` → private-data/qdata destruction →
   `gst_mpp_mem_destroy()`/`mpp_buffer_put()` → `_gst_memory_free()` → allocator
   virtual `free()`. Therefore the MppBuffer has already been returned before
   `gst_mpp_allocator_free()` begins, whether parent `free()` is written before or
   after the group clears. The upstream reorder is a no-op, not a bug fix.
3. **Hardware gate** — none. The port is reverted; the destroyed-qdata ordering is
   a GStreamer lifecycle fact, not an unresolved MPP hardware behavior.
4. **MPP ABI closure** — before and after: 67 symbols referenced and present,
   empty diff. The commit only reorders existing calls.
5. **Reviewer verdict** — `false-positive`. This is the first genuine independent
   verdict for the candidate. The earlier self-directed explore-agent review did
   not satisfy F27 and incorrectly accepted the commit-message mechanism.

### a910efe — JPEG input timeout retry

1. **Provenance SHA** — `a910efebc5f438eacbe8eda66a0ed1c01a43f787`,
   JeffyCN/mirrors, author Jeffy Chen. Applied with `git cherry-pick -x`; author
   and message preserved. Regression seam: `27e46bea`.
2. **Red/green outputs** — native aarch64 bookworm test
   `rockchipmpp decoder dmabuf caps and pending-frame bound`. RED before the pick:

   ```
   == jpeg input timeout retry ==
   assertion failed (gst_harness_push (h, buffer) == GST_FLOW_OK): (-5 == 0)
   FAIL
   ```

   GREEN after the pick: `one MPP_ERR_TIMEOUT retried; accepted on poll 2` and
   `1/1 ... decoder dmabuf caps and pending-frame bound OK`.
3. **Hardware gate** — `hardware-independent`. The mock returns one input-port
   timeout, withholds a dequeue task for that timed-out poll, then succeeds; both
   the JPEG subclass classification and shared base retry are exercised.
4. **MPP ABI closure** — before and after: 67 symbols referenced and present,
   empty diff. Only existing `MppApi` function pointers and an existing enum value
   are used.
5. **Reviewer verdict** — `confirmed`. The orchestrator-dispatched independent
   oracle confirmed both production hunks and the timeout-withheld-dequeue test;
   no correction was requested.

### e72392d parent — encoder runtime-property snapshot synchronization

1. **Provenance SHA** — `e72392daa32404793417685a0e14d09b32179e29`, the
   `integration/verified-fix-ledger` parent on which the race was reproduced. The
   fix is self-authored on `fix/encoder-property-runtime-sync`; the row-bearing
   commit cannot embed its own content-derived SHA.
2. **Red/green outputs** — native aarch64 bookworm test `rockchipmpp GstHarness
   factories and caps`, case
   `test_runtime_property_snapshot_is_coherent_and_eventually_applied`. The mock
   pauses after writing `rc:bps_target`, changes the public `bitrate` property from
   another thread, and records the three bitrate fields at every
   `MPP_ENC_SET_CFG` boundary. RED at the untouched parent:

   ```
   minimum (3000000) is not equal to target * 15 / 16 (1500000)
   85%: Checks: 7, Failures: 1, Errors: 0
   ```

   GREEN with the dedicated property lock and coherent base/codec snapshots:

   ```
   1/3 rockchipmpp GstHarness factories and caps               OK
   2/3 rockchipmpp decoder dmabuf caps and pending-frame bound OK
   3/3 rockchipmpp plugin registration and properties          OK
   Ok: 3  Fail: 0
   ```

   A delayed-clear mutant moved `prop_dirty = FALSE` after
   `MPP_ENC_SET_CFG`; the same test rejected it with `the final quiescent bitrate
   was never applied`. The correct locked handoff was then restored. **TSAN
   execution: NOT DONE.** The aarch64 TSAN binary failed to start under QEMU
   emulation (`unsupported VMA range`, found 47 bits). This is an environment
   limitation, not a test result, and no sanitizer conclusion can be drawn. The
   deterministic mock-sequencing test reproduces both the torn-read and
   lost-update failure modes under controlled thread barriers and independently
   satisfies the plan's stated `TSAN OR mock-sequencing` acceptance criterion.
   `PARITY_SOURCE_ONLY=1 bash tests/parity-check.sh` passed both source-derived
   contracts. A full mock-host invocation then passed `mpph264enc` and
   `mpph265enc` runtime parity (including every frozen property line) before the
   established, parent-identical Radxa decoder-caps mismatch stopped the script.
3. **Hardware gate** — `hardware-independent`. The defect is GObject/threading
   state handoff, and both the torn snapshot and lost-final-dirty mutant are
   deterministically sequenced at the mock MPP boundary. No board behavior is
   needed to prove the fix.
4. **MPP ABI closure** — before (`e72392d`): 67 symbols referenced and present,
   empty diff. After: 67 symbols referenced and present, empty diff. The property
   mutex, snapshots, and mock recorder add no MPP entry point. `nm -D
   --defined-only` also confirms the internal snapshot helper is hidden from the
   plugin's dynamic ABI.
5. **Reviewer verdict** — Confirmed by independent oracle review: torn-read prevention verified (single coherent snapshot under `prop_mutex`, covering every plan-cited field), the exact lost-update race the falsifier identified (concurrent prop_dirty TRUE/FALSE race) verified closed via full trace, no lock-order inversion found, lock never held across `mpi->control`, coalescing behavior preserved as intended (not a regression). Unused-element (`gstmppjpegenc.c`/`gstmppvp8enc.c`) touches confirmed to be a legitimate mechanical consequence of the shared property-lock macro, not a scope violation. TSAN could not execute (QEMU VMA limitation on aarch64, environment issue not a test result) — the deterministic mock-sequencing test satisfies the plan's stated 'TSAN OR mock-sequencing' criterion independently.

### Follow-up list

- No later JeffyCN commit was inspected or added; work after frozen `a0d45af` is a
  separate follow-up audit.

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
- a bounded encoder packet FIFO that can pause after one output, retain multiple
  packets until reset, attribute empty polls to a specific reset generation,
  count exact-once packet release/live state and duplicate dequeues, and expose
  packet ids as one-byte payloads for old/new-session association checks;
- JPEG task-port behavior with a scripted input-poll timeout and a withheld
  dequeue task, so timeout classification and retry are tested together.
- an encoder-config snapshot recorder plus one-shot barriers at
  `rc:bps_target` and `MPP_ENC_SET_CFG`, so concurrent property reads and dirty
  handoff ordering are asserted without relying on scheduler luck.

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

### F27 — crop meta removal pointer type

1. **Provenance SHA** — `fe23e6e` (full SHA recorded in repository history).
2. **Red/green outputs** — the parent fails in the trixie/GCC-14 compile at
   `gst_buffer_remove_meta (buffer, crop)` with `-Wincompatible-pointer-types`; the fix
   compiles both bookworm/GCC-12 and trixie/GCC-14 suites cleanly. PR #8 passed both
   blocking matrix legs.
3. **Hardware gate** — `hardware-independent` (the explicit embedded `GstMeta` pointer
   preserves runtime behavior and requires no device access).
4. **MPP ABI closure** — `bash ci/check-mpp-abi.sh`; before and after: 67 referenced
   symbols, empty diff. No MPP entry point changed.
5. **Reviewer verdict** — `confirmed`.
