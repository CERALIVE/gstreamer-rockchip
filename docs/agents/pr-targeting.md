<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## PR-TARGETING

Every PR targets `CERALIVE/gstreamer-rockchip`, never the fork parent. Working
clones retain only the CERALIVE `origin`; do not leave a remote named `upstream`
attached. If a source comparison is required, add a descriptively named,
temporary remote, fetch an explicit ref, verify the expected SHA, and remove the
remote before pushing or opening a PR.

Open the PR explicitly against the CERALIVE repository's canonical `main`
branch:

```bash
gh pr create --repo CERALIVE/gstreamer-rockchip \
  --base main
```

Before handoff, verify the PR URL starts with
`https://github.com/CERALIVE/gstreamer-rockchip/`. A PR carrying tier-(a)/(b)
commits is never self-merged; an independent reviewer must confirm its evidence
and merge method.

