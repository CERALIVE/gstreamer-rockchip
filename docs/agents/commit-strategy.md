<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## Commit strategy

Three tiers preserve provenance and reviewability:

1. **Tier (a), ported upstream fixes.** Clean ports use `git cherry-pick -x`,
   preserving the original Author and message. Adapted ports use the adapter's
   authorship and credit the owner plus full source SHA in the message. Never
   squash either form.
2. **Tier (b), first-party bug fixes.** One commit per bug, titled for the defect
   mechanism rather than implementation trivia. Never squash these commits.
3. **Tier (c), CI, packaging, docs, and mechanical work.** These may be squashed
   under the normal CeraLive Rule C convention.

The integration PR contains tier-(a)/(b) history and therefore merges with
**Rebase and merge** or **Create a merge commit**. Squash-merge is forbidden for
that PR because it destroys the provenance the first two tiers exist to retain.
No commit may carry a `Co-authored-by` or AI/tool attribution trailer. A clean
cherry-pick's real upstream Author field is provenance, not a trailer.

