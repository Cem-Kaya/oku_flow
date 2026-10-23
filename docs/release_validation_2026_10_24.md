# Windows release preparation, 2026-10-24

This records candidate validation, not a published release or complete acceptance.
The candidate includes the magnifier, both-role BF16 OCR preference and MCP V1.

## Owner-reported Assistant follow-up

The owner's Luna run reported tool support unavailable and answered the zoom
request without changing the view. This is failed live-control evidence, not a
successful control test. Read-only inspection of the installed 0.160.1 catalog
found `gpt-6-luna` uses `code_mode_only`; the selected package includes the host,
but the restricted launch profile disabled `code_mode_host`. The source repair
enables that transport and retains execution/provider exclusions, sandboxing
and the existing call/item checks. No stronger model is substituted.

Assistant action names, approval/lease labels, main corner captions and setup
actions have a separate accessibility synchronization repair with headless
name/state/language coverage authored. Both repairs are awaiting build/runtime
validation; they do not establish native screen-reader or live-provider passes.

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

The tracked background matrix at `3d2ab50` passed shipping compilation, the CPU
selection and the CUDA delta. CPU retained 161 matching results and finished
with **166 passes** (four containing skipped Qt rows), **nine optional skips**
and **zero unresolved**. The CUDA delta retained 14 matching passes and finished
with **14 passes, five optional skips and zero unresolved**. GUI was excluded;
this is explicitly partial validation, not the full release gate. Both child
and controller exited zero in 83.29 seconds, reached zero Job members and
verified runner release. Per-test ledgers, detailed logs and build inventories
are archived under `background-matrix-r4`.

The preceding matrix passed shipping compilation and its CUDA delta, but CPU
failed `bundle_toolchain_contract`: nested VS initialization grew PATH beyond
Windows command limits. `3d2ab50` restores Microsoft's pre-initialization PATH
before genuine installed-toolchain setup; its fixture stresses roughly 7000
characters while retaining forged-environment and refusal assertions. Source
review found no P1/P2 issues. The original failure and 1476 archived evidence
files remain under `background-matrix-r3`.

Earlier private release-cache attempts also preserve NVCC setup failures caused
by long/8.3 aliases of the same selected host compiler. A fresh cache without a
redundant forced CUDA host path, using the verified VS2022 environment, resolved
that setup issue. No installed compiler files or owner processes were changed.

The first MCP configure reset an existing cache after a compiler-path spelling
change; the identity check stopped before target compilation. A fresh private
CPU cache and normalized compiler path resolved that environment issue. A
subsequent resume request refused the cache because it had no prior ledger;
fresh execution then exposed the inventory's obsolete zoom minimum of `1`.
Commit `61cc850` corrects that runtime inventory input to production's `0.01`,
retaining every strict schema assertion. The failed fingerprint and all logs
remain archived; no passing result replaces the historical failure.

## Remaining acceptance

The final bundle configuration built at `2812fdf`, but its opt-in native codec
gate retained four failures among 182 passes. All four use
`NativeSparseVideoAudio`: its two-second sample gap is valid following-frame
evidence under the current native-duration policy, contrary to the negative
fixture's empty-neighbour assertion. The fixture now authors a three-second
gap beyond the 2500 ms ceiling, retaining every preservation assertion. This
repair is not yet built or executed; the original failed logs remain under
`bundle-background-r1`.

The serial desktop matrix at the same source passed shipping compilation and
64 of 66 GUI cases, with two native hit-test failures retained in the CPU
selection (230 passes, nine skips, two unresolved). The failures report a
foreign native hit for the chat attachment and UCT checkbox at 150% DPI.
Their cause remains under investigation; neither is counted as a desktop
interruption or passing acceptance. CUDA finished with 15 passes and five
optional skips. Both outer runs released their exact Jobs and runner locks.

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
