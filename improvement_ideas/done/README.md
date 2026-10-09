# Completed plans

Plans in this directory have **no actionable work left in them**. They are kept
for provenance — why a design is the way it is, what was measured, what was
deliberately refused — not as a to-do list. Nothing here should be picked up as
work.

Live work: [`../00-status-and-priority.md`](../00-status-and-priority.md).

| Plan | Completed | What it delivered |
|---|---|---|
| [`01-stability-threading.md`](01-stability-threading.md) | 2026-07-21 → 07-23 | All 7 items resolved: 5 fixed (camera-switch CUDA lifetime, COM leak, temporal-history sync, fence protocol S6b, mid-stream format changes), 2 investigated and **refuted** (S2 capture-loop race, S4 capture/UI state race — read the refutations before "fixing" either again). |
| [`02-architecture-app-decomposition.md`](02-architecture-app-decomposition.md) | 2026-07-23 | A1-A5 delivered by plan 11 Wave 4 + Batch D: `OkuFlowApp` is a composition root, `src/app/app.cpp` is 5 lines, managers extracted, `suspend_guard.hpp` replaced the manual suspend flags, `Initialize()` split from construction. *The doc itself was never updated and read as fully open — reconciled 2026-07-24.* One remnant (A5 release-build null-guards) is tracked in plan 16 P1. |
| [`09-maxine-superres-plan.md`](09-maxine-superres-plan.md) | 2026-07-23 → 07-24 | NVIDIA SuperRes shipped as a runtime-loaded optional plugin: Setup Assistant with hash-verified downloads, latency guard with inspectable measurement and override, Ultra quality (full-frame to 1440p), Faster 2x, per-profile persistence, NVIDIA attribution, and zero NVIDIA binaries in the GPL bundle. *The doc's acceptance checkboxes were never ticked; completion is evidenced by CHANGELOG entries and shipped behavior.* |
| [`11-hardening-refactor-plan.md`](11-hardening-refactor-plan.md) | 2026-07-23 | All four waves: Wave 1 frame-timing instrumentation (P8), Wave 2 stability remainder (S6b fence timeline, S4 audit), Wave 3 performance remainder (P11-P15, P4, P6 — pinned-host upload ring, kernel-weight upload fixes, shared-memory reductions), Wave 4 architecture decomposition. |
| [`12-color-picker-redesign.md`](12-color-picker-redesign.md) | 2026-07-23 | Visual two-color scheme picker, luma-LUT color system with generation-cached CUDA upload, custom scheme persistence, accessibility wiring; legacy modes 0-16 passed the 1-LSB migration gate. The popover auto-hide bug found in review (M1) was fixed the same day. |
| [`13-handoff-batches-bcd.md`](13-handoff-batches-bcd.md) | 2026-07-23 | Orchestration document for Batches B, C, D — all three completed, rebuilt, and Windows-smoke verified (45-second live-camera run, clean exit). Superseded as a work instruction; retained as the record of how the decomposition was sequenced. |

## Also worth keeping in mind

[`../verified-non-issues.md`](../verified-non-issues.md) stays in the live
directory on purpose: it is a *guard*, not a completed plan. Read it before
starting any analysis so you don't rediscover a refuted finding.
