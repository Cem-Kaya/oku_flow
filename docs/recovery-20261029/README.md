# Pre-cleanup recovery records - 29 October 2026

This branch preserves useful unfinished drafts, original experiment history and
curated measured results. It does not merge the experiments into production.
The application tree outside this documentation directory matches the original
main baseline. No new builds, accuracy or runtime acceptance are claimed here.

`drafts/` holds five binary Git patches with original base SHAs and the cache
policy stash. They remain unvalidated research; do not apply them wholesale to
current main. `experiments/branches.json` maps 53 original research heads, all
retained as ancestors of this archive commit. `experiments/results/` preserves
timings, numeric receipts and design/review reports with captured text and machine
identifiers removed from JSON. `experiments/prototype-source/` deduplicates unique
intermediate native setup source not already in those histories.

Clone or fetch this branch from GitHub to recover this material. Local duplicate
Git mirrors/bundles, raw test captures and redundant source/install archives can
be removed after independent fetch/verification. Published portable/MSI release
assets remain on GitHub Releases. Reproduction may require reacquiring documented
inputs/models and rebuilding; the archive does not retain every disposable run.

Private signing/Play credentials, personal notes/documents and app preferences
are deliberately not published. The cleanup keeps them on the owner's machine.
They are separate from a fresh source-code checkout.
