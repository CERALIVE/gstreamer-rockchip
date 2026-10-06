<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## Anti-patterns

- Do not rename `libgstrockchipmpp.so`, `libgstrockchiprga.so`, or the package
  prefix, and do not split the RGA elements into a second `.deb`: the release
  publishes exactly one archive.
- Do not publish the Bookworm `~bookworm` package, widen the "exactly one
  `.deb`" assertion to admit it, or remove the Bookworm build/smoke legs. The
  first two break the Trixie-only APT policy and apt-worker's reindex; the
  third deletes the GStreamer 1.22 coverage. Do not start a Bookworm librga
  build on the strength of the legacy pin either; it is permanent by policy.
- Do not remove unused factories to reduce the package.
- Do not extend `rgacompositor` beyond two sink pads or add a CPU compositor;
  v1 is deliberately one primary plus one secondary on librga.
- Do not treat `mppvp8enc`'s absence on RK3588 as a bug to fix or as a drill
  failure to suppress. It is silicon, it reproduces on the Radxa package, and
  d1 scores it explicitly.
- Do not rename `bitrate` back to `bps` or add a legacy alias here.
- Do not change Main10 stride semantics on static-analysis confidence alone.
- Do not treat plugin registration success as proof all factories registered;
  `plugin_init` historically swallows individual registration failures.
- Do not arm the kernel fault bridge by default, and do not describe it as
  detecting IOMMU faults. It is off until a board has driven a real injected
  hardware fault through it end to end, and the IOMMU path emits no tracepoint
  at all — what arrives is the ~500 ms timeout consequence, as a timeout.
- Do not resolve an ambiguous `task_id` in the fault bridge by picking the
  newest, the oldest, or the nearest-in-time candidate. `task_id` is per
  taskqueue, so a wrong pick restarts a healthy encoder on another process's
  fault. Losing detection is the safe failure; abandoning both candidates is
  deliberate.
- Do not re-resolve fault-bridge session ownership by matching TIDs on a timer,
  and do not make the re-resolve after a restart replace the cached set. The
  `pid` in `sessions-summary` is the creating thread's TID and that thread can
  die while the session lives, which is why ownership is resolved at `start()`,
  cached, and only ever unioned.
- Do not claim sanitizer coverage that the qemu-user environment cannot run.
- Do not let a board drill install a package without recording package, kernel,
  and final verdict, and do not infer PASS from a command merely completing.
- Do not run the pre-commit hook casually: it checks the whole MPP subtree and
  is intentionally baseline-wide. Review `git status` immediately if it runs.
- Preserve mixed line endings in inherited files; avoid text-mode whole-file
  rewrites and compare raw versus whitespace-ignored diffs.
