<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## Pre-commit formatting

`hooks/pre-commit.hook` runs GNU indent 2.2.12 with the upstream parameter set
against the index contents of every C/H file under `gst/rockchipmpp/`. The
encoder source and header intentionally retain inherited CRLF line endings;
GNU indent 2.2.12 misparses those carriage returns as input, producing
misleading unmatched-`else`, statement-nesting, and unexpected-EOF errors.
The hook strips CR only in its temporary checker input, so it can validate
those files without changing their stored line endings. It also leaves the
working tree untouched when a style diff is found.

The two genuinely non-compliant LF files (`gstmppallocator.c` and
`gstmpph265enc.c`) are kept at the hook's two-pass output. The encoder's
failure was introduced by commit `dd3ce32c` restoring inherited CRLF; the
underlying C was valid and the original file passed GNU indent before that
line-ending-only change.

