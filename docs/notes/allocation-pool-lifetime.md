# Allocation-query pool lifetime [EXISTS]

## Root cause

`gst_query_parse_nth_allocation_pool()` returns a **transfer-full** pool reference,
not a borrowed pointer. `rgaconvert` discarded those references while scanning
candidate pools, swapping the selected pool into the first position and reading
its allocator configuration. The query owned additional references independently;
destroying the query could not release the converter's discarded references.

This retains `GstVideoBufferPool`, its configuration and allocator metadata after
the pipeline returns to NULL. The pool can already have returned its DMA buffers,
so zero DMA-BUF occupancy and MPP IOVA ranges do not prove host-object release.
Weak callbacks on the encoder and capture elements also cannot see a leaked pool
that outlives those elements.

The fix releases each parsed reference after use, preserving query-owned refs
through pool reordering. Nullable proposals use `gst_clear_object`. The allocator
read from a pool configuration remains borrowed and is not unreffed separately.
No allocation size, pool selection policy, hardware request or teardown timing
changes; no `malloc_trim` or allocator environment tuning is shipped.

## Regression and mutation evidence

`test_allocation_query_pool_references_are_released` executes the production
allocation callback in four cases: selected-first DMA-BUF pool, rejected system
pool followed by DMA-BUF, converter-created pool, and NULL-first followed by
DMA-BUF. A positive control requires the selected pool to be alive before
teardown; every pool's weak reference must be empty after the query and converter
are destroyed. The existing allocation-contract test now also releases its own
two parser references; none of its assertions was removed or weakened.

Before the fix, the new test fails with `variant 0 pool 0 retained after query
and converter teardown`. All four variants pass after the fix. Replacing the
reordered selected-pool unref with a discarded pointer makes variant 1 fail.
Run the `rockchiprga rgaconvert negotiation and DMA-BUF transform` Meson test;
both full Bookworm/GStreamer 1.22 and Trixie/GStreamer 1.26 suites pass.

## OPi allocation evidence and RSS limits [PARTIAL]

The 2026-09-16 ten-cycle baseline allocation profile resolves retained pool
instances to compositor allocation proposals and converter-created pools: twenty
proposal pools plus ten primary-conversion and ten decoder-side-conversion
pools. Forty associated pool recursive-mutex allocations survive. With the fixed
plugin, the same ten-cycle profile has **no retained pool instances or pool
recursive mutexes**. The remaining 595 bytes on filtered pool-construction
stacks are one-time GType/class/option metadata, not forty surviving pools.

The profiler was checked with a deliberate test-only 1 MiB retention; it detects
that allocation and attributes it to the observer's `watch` stack. That control
ran one stream, not the planned three: the receiver exited naturally at stop
before the harness's strict kill. Restoration still ran. Some unrelated host
libraries were unavailable to the symbolizer; the exact loaded candidate RGA
stacks resolve, and no whole-process leak-freedom claim is based on missing symbols.

This fixes a real per-cycle leak, but it does **not** establish that all of the
original +792 KiB/cycle RSS fit came from those objects. A forty-cycle no-trim
baseline flattened from 41,732 KiB initially to 63,120 KiB in its last three
samples; glibc statistics show substantial freed heap retained in arenas.
Diagnostic trimming releases pages but was not adopted as a software fix.
The strict non-positive RSS acceptance criterion remains a separate measurement;
the general RGA IOMMU census and link timing are not covered here.

The subsequent fixed-plugin, no-trim twenty-cycle run still grows from **41,660
to 58,804 KiB**, with an all-cycle fitted slope of **+770.917 KiB/cycle** and a
last-ten slope of **+184.606 KiB/cycle**. Therefore the reported RSS defect is
**PARTIAL / NOT RESOLVED against its strict criterion**. The proven pool leak is
repaired, but it is not a sufficient explanation or fix for the RSS series.
Do not present these runs as a controlled percentage improvement against the
original acceptance run: their observation durations differ. Further work must
attribute remaining arena/cache retention before proposing reclamation policy.

All board candidates were per-process overrides confirmed by loader logs. No
APT transaction, flash, slot modification, RGA driver reload or release occurred.

## Separate compositor allocator leak [EXISTS]

The follow-up found another ownership defect, not a reopening of the converter
pool fix. A 60-cycle profile of the pool-fixed plugin retains exactly one
`GstRgaDmaHeapAllocator` per composition cycle: **232 bytes and one instance,
then 464 bytes and two instances, through 13,920 bytes and sixty instances**.
Its sixty object names retain another 1,358 bytes. These allocations survive
subsequent cycles; they are not one-time GType registration metadata.

The allocation stack is `gst_rga_compositor_get_allocator()` →
`gst_rga_dma_heap_allocator_new()` → `g_object_new()` →
`g_type_create_instance()` → `g_malloc0()`. The reference is lost later:
`GstVideoAggregator`'s generic `decide_allocation` alignment loop calls
`gst_query_parse_nth_allocation_param()` and replaces the query entry without
releasing the parser's **transfer-full** reference. Its separately parsed outer
allocator reference is released correctly; that does not release the loop's
reference. See the
[GStreamer 1.26.2 implementation](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gst-plugins-base/gst-libs/gst/video/gstvideoaggregator.c).
The lifetime regression reproduces the parent-call defect on both the Bookworm
GStreamer 1.22 and Trixie GStreamer 1.26 build legs.

`rgacompositor` has already installed and configured its custom DMA-BUF pool,
including padded video geometry, allocator and video metadata. It now returns
success at that boundary instead of asking the generic parent to negotiate the
same allocation again. The query explicitly retains the parent's 16-byte minimum
allocation alignment (`align = 15`). Backend negotiation and pool-configuration
failures still return failure; no CPU-memory fallback is introduced. No compensating
unref is applied to a borrowed pointer, so this does not depend on a future
GStreamer release continuing to leak.

### Failing-first and mutation proof

`test_allocator_finalizes_after_composed_teardown` obtains an actual composed
output buffer from the existing two-input harness. It proves that output memory
uses the watched allocator and that the negotiated allocator/alignment are
preserved. Positive weak-reference observations establish both live objects.
After output and harness teardown, the compositor must disappear independently
of the allocator, and the allocator must disappear too.

The allocator assertion fails before the fix and passes after it. Restoring only
the generic parent call makes that assertion fail again on both supported build
suites, while the compositor-destruction assertion passes. Both complete Meson
suites then pass **12/12**, with ABI, glibc-floor, suite-pin and provider-contract
checks green. No existing test was removed or weakened.

### Allocation-profile discriminator

The same instance filter reports **1…60** surviving allocators before the fix and
**zero after every one of sixty stops** with the candidate. This is not an empty
trace: the factory's instance-allocation call site records **300 creations / 240
frees** before the fix and **300 creations / 300 frees** afterward, across the
main process's negotiation paths. A separate three-cycle
positive control deliberately retains one MiB per cycle; the raw allocation
replay reports exactly **1,048,576 / 2,097,152 / 3,145,728 bytes** at that observer
stack. Those injected allocations are never part of the candidate or its normal
measurement runs.

Across the fixed profile, outstanding malloc payload remains within
**5,134,046–5,155,005 bytes**. The all-cycle live-payload fit is **+24.127
bytes/cycle**, versus **+286.864 bytes/cycle** before the fix; the fixed final
twenty-cycle fit is **−128.668 bytes/cycle**. The recurring 254/255-byte survivor
cohorts disappear. Small bounded MPP/TLS caches and asynchronous audio-meter
allocations remain visible rather than being filtered out of whole-process totals.
One-time class metadata stays counted as well.

These are **live-payload**, not RSS, measurements. Raw pointer replay used each
post-stop trace-file byte offset, avoiding an inferred wall-clock alignment.
Both new sixty-cycle traces have two unmatched startup frees and **zero** reused
addresses missing a preceding free. An older trace did contain reuse gaps; it
was used only for exploration, not these acceptance measurements. Symbolization
used copied loaded libraries and the exact candidate plugin. Unknown frames do
not remove allocations from totals.

All sixty fixed-profile stops report **62 descriptors total**, including **four
dma-heap descriptors**, without per-cycle accumulation. Four is the measured
post-stop state, not a claim of zero open heap devices. Every cycle has a positive
PLAYING/nonzero-output-buffer status snapshot; those counters are not video FPS.
The final recording independently decodes to 182 HEVC 1920×1080 frames with
BT.709 primaries, transfer and matrix. No installed binary or package was changed.

## Long-run RSS: bounded retention on the measured tuple [PARTIAL]

The non-heaptrack candidate run completed **320** identical HDMI + BRIO PiP
start/stop cycles, with **4,159 seconds between the first and last post-stop
samples**. All cycles are retained. No explicit trim, allocator tuning, baseline
restart or warmup exclusion was used. Loader logs identify the per-process
candidate plugin and librga; the installed engine remained unchanged.

| Measurement | Result |
|---|---:|
| First → last post-stop RSS | 41,596 → 62,412 KiB |
| Maximum over all 320 cycles | **64,304 KiB / 62.80 MiB** |
| All-cycle RSS fit | **+20.898 KiB/cycle** |
| Cycles 1–20 RSS fit | +649.847 KiB/cycle |
| Cycles 161–320 RSS range | **62,352–64,304 KiB** |
| Cycles 161–320 first → last | 62,716 → 62,412 KiB |
| Cycles 161–320 RSS fit | **+4.930 KiB/cycle** |
| Cycles 301–320 RSS fit | −90.589 KiB/cycle |
| First → last allocator free arena space | 9,197.609 → 27,072.688 KiB |
| All-cycle free-arena fit | **+20.807 KiB/cycle** |
| First → last allocator used + mmap space | 6,150.391 → 7,059.312 KiB |

The dominant rise is retained allocator space, not the small allocator-object
leak. The positive full-run RSS fit closely follows free-arena growth, while the
separate allocation profile proves release of the actual objects. The final
160-cycle RSS envelope is approximately 61–63 MiB and ends below its first
sample; automatic allocator releases remain visible. The earlier, separate
160-cycle pool-fixed run peaked at 63,900 KiB. Doubling the measured run length
does not double the growth; the observed ceiling stays near 63 MiB.

This is an **empirical bounded-retention diagnosis for this fixed workload and
allocator**, not a universal upper bound or proof about arbitrarily long runs.
The late fit is still positive and is reported, not rounded to zero. The new
20/160/320-cycle fits must not be compared as if their difference measured the
small reference fix's effect: increasing the fit horizon dilutes startup growth.
All 320 cycles have positive PLAYING/output-buffer observations and exactly
**58 descriptors after every stop, including four dma-heap descriptors**. The
profiled process has four additional instrumentation descriptors; compare
within-process stability, not raw totals between the two configurations.

`mallinfo2`'s used space is not synonymous with live application payload: freed
tcache chunks can remain counted there, alongside allocator overhead. Its free
space is not necessarily all resident or reclaimable. The one-second allocator
sampler is paired with post-stop RSS within 1.2 seconds; exact page-by-page
attribution is not claimed. Heaptrack also adds its own memory overhead, so its
roughly 78 MiB RSS maximum is not substituted for this non-profiled ceiling.

### Proposed acceptance correction — not adopted

The literal startup-inclusive **RSS slope ≤ 0 remains FAIL**. A process that
acquires a finite allocator working set can have a positive full-run fit despite
a bounded later envelope. Conversely, a seemingly flat RSS trace can conceal the
small reference leak found here. RSS slope alone is therefore the wrong leak
detector for this path.

Propose a two-part, owner-approved replacement:

1. **Ownership gate:** no cycle-proportional surviving allocation cohort; every
   per-session allocator/pool is released after teardown. Keep known-positive
   retention controls and require the observer to see actual creations, not
   merely zero survivors. Report one-time registrations and bounded caches
   separately without deleting them from whole-process totals.
2. **Resident-budget gate:** retain a cold-start campaign of at least 320 cycles,
   report every sample and the full-run fit, and independently qualify the RSS
   ceiling and later envelope for the exact input tuple/build. For this measured
   tuple, **64 MiB is a proposed budget**, above the observed 62.80 MiB maximum;
   it is not an approved threshold or a promise for other tuples. Report the
   second 160 cycles separately as a diagnostic comparison, never as a replacement
   for the full series. Confirm the budget on the released/installed candidate
   before using it as an acceptance gate.

No plan row is discharged by this proposal. Item 31's RSS row remains open until
the owner approves an amended criterion and its required qualification is met.
The source fix itself is independently proven. The board was restored to the
installed executable, hashes and saved geometry, idle on good B with good
inactive A; all owned overrides and board staging were removed.
