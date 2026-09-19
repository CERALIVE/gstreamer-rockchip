# Missing primary intervals are not negotiation failures

## Mechanism

`GstAggregator` queue readiness and `GstVideoAggregator` frame selection are
different predicates. Both pads can have queued buffers while the primary's
first timestamp lies after the current output interval. A live timeout can also
reach an interval for which no primary frame is selected. Neither means the
negotiated NV12/BGRA layout or RGA backend is invalid.

Previously `create_output_buffer()` allocated output in that state. With only
the secondary selected, `aggregate_frames()` posted `sink_0 has no primary
frame` and returned `GST_FLOW_NOT_NEGOTIATED`. GstAggregator then marked all
sink pads failed. With neither input selected, the callback instead returned
OK without writing the allocated output: an unwritten DMA surface could reach
the encoder. Those startup buffers were not evidence of working composition.

The repair lives before allocation: when input collection has no usable primary
(including an empty GAP buffer), return OK with a NULL output. The parent skips
rendering/pushing this interval but advances its output time, eventually reaching
the primary's first frame. It retains ownership of queuing, clock waits, EOS,
flushes and timestamps. No source-specific hint, added sleep, force-live setting,
engine property support, or MPP change is needed. Layout and hardware failures
with a real primary retain their existing error paths.

### Upstream contract, not a new scheduler

The following behavior is present in both GStreamer 1.22.0 and 1.26.2:

- [`gst_video_aggregator_do_aggregate`](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gst-plugins-base/gst-libs/gst/video/gstvideoaggregator.c#L2106-L2163)
  explicitly accepts `create_output_buffer()` returning OK/NULL as “sub-class
  doesn't want to generate output right now”. It never calls `aggregate_frames`
  on that branch.
- [`gst_video_aggregator_aggregate`](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gst-plugins-base/gst-libs/gst/video/gstvideoaggregator.c#L2357-L2405)
  increments `nframes` and advances the segment position even without output.
  Returning `GST_AGGREGATOR_FLOW_NEED_DATA` from the render callback would instead
  retry the same interval and can spin against unchanged queued future buffers.
- [`gst_aggregator_wait_and_check`](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gstreamer/libs/gst/base/gstaggregator.c#L855-L1018)
  uses a condition wait when latency is invalid, not a zero clock timeout.
  Otherwise the clock deadline is based on base time, next output time and
  latency. Queue readiness can independently admit aggregation.

This corrects the earlier interpretation of `live:false min:0`: a failed
`v4l2src` latency query is not by itself a base-class instruction to render
immediately, nor is it a fatal flow return. The fatal choice was this plugin's.
The host regression reproduces it with fixed caps and staggered timestamps,
without a failed latency query or real camera.

## Verification — Rock 5B+, 2026-09-19 19:54–19:58 UTC

Engine source was unchanged current main `f9596ef3331ccf3cec9478adecd72345527f0b9c`,
freshly cross-built with WebRTC enabled; reported `2026.9.5+f9596ef`, SHA-256
`5d372d82a461b59503364d125f314df98ecb2c1468c8420db8ac6818eebf50a3`.
The RGA baseline was built from plugin main `d894ab0c`; candidate changes only
the missing-primary allocation decision. Candidate RGA plugin SHA-256:
`b7fec21bf657b5d44da797ae74c84bb428f3ee89dbbb01d3c4018edab1b0bbb7`.
The board's MPP plugin and `librockchip-mpp1=1.5.0-1` were not replaced.
Kernel `7.2.0-ceralive-rk3588`, production slot B, librga R1
`1.10.5+ceralive.1`, boot ID `34e25598-99db-4075-b310-25d08e3fa258` throughout.

Fixture identities were resolved afresh for each start: Logitech BRIO MJPEG and
DJI Osmo Pocket 3 H.264 on distinct USB controllers. All rows requested
1920x1080, 30 fps, fixed 4.5 Mbit/s, audio none, passthrough off. Each successful
start ran for 10 seconds plus a 3-second status observation, then clean stop.
The local SRT receiver used `-a:no` and `tsbpdmode=0`, exiting on disconnect
rather than being killed. These ~13-second recordings are deliberately compared
with same-run controls, not the earlier 10-second/~5.2 MB receipt.

| Row | Bytes | Decoded frames | Video duration (s) | Strict decode exit |
|---|---:|---:|---:|---:|
| baseline BRIO primary #1 | 7,473,564 | 396 | 13.200 | 183 |
| baseline BRIO primary #2 | **7,896** | **23** | **0.767** | 0 |
| baseline Osmo primary | 7,693,336 | 402 | 13.400 | 0 |
| baseline BRIO single | 7,617,008 | 399 | 13.267 | 0 |
| fixed BRIO primary #1 | 7,585,424 | 397 | 13.467 | 0 |
| fixed BRIO primary #2 | 7,598,584 | 398 | 13.267 | 0 |
| fixed BRIO primary #3 | 7,556,472 | 398 | 13.267 | 0 |
| fixed Osmo primary | 7,594,636 | 397 | 13.233 | 0 |
| fixed BRIO single | 7,605,164 | 399 | 13.267 | 0 |
| fixed Osmo single | 7,556,472 | 397 | 13.233 | 0 |
| fixed BRIO primary H.264 | 7,592,004 | 398 | 13.267 | 0 |
| fixed BRIO primary PbP | 7,578,844 | 396 | 13.400 | 0 |

All unspecified codecs are H.265; all unspecified layouts are top-right PiP.
The failing baseline journal contains the exact primary-frame refusal. Its other
attempt delivered media but had a corrupt packet: the defect is timing-dependent,
not a claim that every baseline run fails identically. The initial harness attempt
selected the Osmo audio row by mistake, failed typed before capture, and was
discarded; the fixture selector was corrected to video nodes before this matrix.

`ffprobe -count_frames` established geometry, frame count and duration;
`ffmpeg -v error -xerror -i <recording> -map 0:v:0 -f null -` decoded each complete
recording. All six fixed composition recordings decoded with no diagnostic.
Single-source recordings retained null-muxer timestamp warnings also present in
the baseline single-source control; no timestamp fix is claimed here. Decoded
stills show the distinct BRIO cable view and Osmo monitor view in opposite PiP
roles and in PbP halves, not blank or duplicated inputs.

The fixed journal records skipped intervals followed by normal media (7 skips in
BRIO H.265 #1, 7 in the reverse pairing, 41 in H.264, 6 in PbP). No SEGV,
RKVENC timeout, kernel BUG/Oops/KASAN/UBSAN/Call Trace, or service restart occurred
in the matrix. All stop RPCs returned idle. Original engine and RGA bytes were
restored and checksum-verified; temporary debug drop-in and scratch removed;
services active, taint 0, DMA-BUF objects 0, boot order B A with 3/3 counters,
no reboot or slot change, coordinated lock released.

Local regression: unchanged code fails
`test_primary_starting_after_secondary_does_not_fail_negotiation` with a primary
PTS of 100 ms and an overlay covering time zero. Fixed code delivers composition
at/after 100 ms without earlier uninitialized output. A second regression checks
the entirely empty case does not allocate output. All 15 Meson suites pass in
the arm64/Trixie GStreamer 1.26.2 container. The RGA backend is mocked there;
hardware correctness comes from the matrix above, not the host seam. GStreamer
1.22 behavior was source-inspected, not executed in this session.

Scope: bounded Rock qualification of this startup repair, not an Orange Pi test,
long soak, package release or a secondary-only fallback design. A permanently
absent primary still produces no composition until it returns; no pixels are
invented to mask source loss.
