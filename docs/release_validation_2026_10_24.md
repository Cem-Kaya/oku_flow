# Windows release preparation, 2026-10-24

This records candidate validation, not a published release or complete acceptance.
The candidate includes the magnifier, both-role BF16 OCR preference and MCP V1.

## Completed checks

- Cleanup reproduction at `5e4bf7f`: actual CUDA application/fixture compilation
  and four scoped contracts passed. Work and controller exited zero. After a
  bounded natural drain, the sole compiler telemetry survivor matched the
  independently pinned VCTIP image through held process/Job/file identities.
  Exact-Job cleanup reached zero members and verified the runner lock free.
  Historical exit-125 receipts remain failures; their unresolved identities
  have not been retrospectively classified.
- The 47-file desktop OCR payload staged and verified 2,394,527,371 bytes,
  including all six original and six converted models, private runtimes and
  notices. Staging and metadata contracts passed. An isolated SM89 cold run at
  `5955885` built and selected BF16 for both tiny roles from staged files, kept
  tiny recognition N1, saved only private plans and retired cleanly. No OCR
  accuracy or FP32-agreement gate ran. Other GPU architectures remain untested.
- MCP at `61cc850`: scoped **7/244 PASS**, zero skipped Qt rows, skipped cases or
  unresolved results. Five matching passes were retained and the repaired
  inventory/affected bridge fingerprints ran freshly. The selection covers
  bridge, complete control inventory, native imports, MCP bundle, bundle layout,
  actual-target offscreen and shared-source equivalence contracts.
- Official SDK 1.32.1: **17/17 synthetic independent-client scenarios** and
  **9/9 production AppControlTarget offscreen scenarios** passed with catalog
  `v1` and deployed sibling identity. Both use copied, hashed private fixtures;
  no owner app, settings, camera or provider participated. The enclosing child
  and controller exited zero, with zero Job members and a free runner lock.

## Preserved failures and repairs

The first MCP configure reset an existing cache after a compiler-path spelling
change; the identity check stopped before target compilation. A fresh private
CPU cache and normalized compiler path resolved that environment issue. A
subsequent resume request refused the cache because it had no prior ledger;
fresh execution then exposed the inventory's obsolete zoom minimum of `1`.
Commit `61cc850` corrects that runtime inventory input to production's `0.01`,
retaining every strict schema assertion. The failed fingerprint and all logs
remain archived; no passing result replaces the historical failure.

## Remaining acceptance

The tracked shipping/CPU/CUDA and bundle gates, focused native hardware and
provider checks through both adapters, and candidate-specific disposable-VM
install/upgrade/uninstall acceptance remain required. Headless SDK success does
not establish camera/media outputs, native approval accessibility, provider
behavior or clean-machine installation. Signing is not configured; the existing
unsigned-preview channel must remain explicit if used.

Private evidence is under
`Z:/Projects/OkuFlow/local-work-data/release-acceptance-20261024/`:
`cleanup-closure-r4`, `payload-stage-r2`, `payload-runtime-r3`, and `mcp-v1-r1`
through `mcp-v1-r4`. These paths identify local receipts, not shipped payload.
