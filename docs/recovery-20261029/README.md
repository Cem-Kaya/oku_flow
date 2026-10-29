# Pre-cleanup recovery drafts — 29 October 2026

These are recovery records, not production changes. The application tree outside
this documentation directory is identical to main at c0e33a22bcdd9e711a114d3c155134b36242534d.
Nothing here has been accepted as a current-main feature or a language review.

The five draft folders retain exact binary Git patches and original base SHAs.
`combined.patch` restores the tracked draft onto its recorded base. Separate staged
and unstaged patches retain index intent; untracked files are stored separately.
All five combined patches passed `git apply --cached --check` at their recorded
base. Full byte-exact copies, all original branch/checkpoint refs, the stash and
raw experiment evidence are additionally preserved in the owner's verified
private cleanup archive, outside the old workspace folders.

- Wording/translations: potentially useful copy polish based on older main;
  requires contextual language review and source-key parity before integration.
- Two geometry drafts: retain original tests and projection/proof experiments;
  compare with the later integrated UCT implementation rather than overwrite it.
- Overlay/proof draft: early lifecycle/validation research, not a ready replacement.
- Transport header fragment: incomplete declarations; do not integrate alone.
- Stashed TensorRT cache/batching policy: historical draft; compare with current
  model installation/cache identity contracts before use.

No application behavior, dependencies, model defaults or runtime settings change
on this archive branch. No builds or runtime acceptance are claimed. Private
credentials, raw captures, benchmark inputs and user documents are not published.
