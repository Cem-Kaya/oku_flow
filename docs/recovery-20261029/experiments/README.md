# Historical experiment results retained before storage cleanup

These are historical diagnostics and design reviews, not new acceptance claims.
The commit also retains the original unpublished experiment heads as parents;
`branches.json` maps names to original commits. This is an archive graph, not a
merge of those implementations into main. The application tree stays unchanged.

Results retain measured timing/counts, configurations, hashes, comparisons and
recorded limitations. Captured OCR text payloads, hardware IDs, local account
names and email addresses have been removed from machine-readable results.
Duplicate reports are represented in `results-index.json`. Human review reports
may quote public sample text to explain findings. Measurements are not rerun.

The cleanup removes regenerable builds, vendor downloads, obsolete installers,
raw test recordings/images, and redundant source snapshots. Those raw inputs and
per-attempt logs are not part of this archive; a later reproduction must reacquire
its documented inputs and run the tracked diagnostic. Current model packs remain
downloadable through pinned setup recipes. Private signing/Play credentials and
user notes/documents are intentionally outside Git and remain on this machine.

Recovery: clone the repository, fetch `archive/cleanup-20261029`, and inspect this
directory. Original experiment source can be inspected at the commits listed in
`branches.json`; `git show <sha>:<path>` avoids applying obsolete code to main.
The sibling `drafts` directory retains unvalidated unfinished patches and stash.
