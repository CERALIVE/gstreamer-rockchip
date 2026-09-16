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
