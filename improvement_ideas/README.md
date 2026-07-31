# Improvement Ideas

Actionable improvement backlog for OpenZoom, written for AI agents (or humans) picking up
future work. Produced from a full-codebase analysis on 2026-07-21 at commit `9e069d9`
("Add assistive runtime and harden Windows builds"). All `file:line` references are
relative to that commit — re-locate by symbol name if the files have since changed.

## How to use this directory

- **Read [`00-status-and-priority.md`](00-status-and-priority.md) first.** It is the
  single ordered view: what is done, half-done, blocked, and what to pick up next.
  This README is only a map of the directory.
- Completed plans live in [`done/`](done/README.md). They are kept for provenance —
  why a design is the way it is — and must not be picked up as work.
- Each file groups related ideas by theme. Ideas are self-contained: problem, evidence,
  concrete fix, priority, effort.
- **Verification status matters.** Ideas marked `Confirmed` were verified against the
  source during this analysis. Ideas marked `Reported` came from subsystem analysis and
  are plausible but should be re-verified against the code before implementing.
- Read [`verified-non-issues.md`](verified-non-issues.md) **before** starting your own
  analysis — it lists plausible-looking "bugs" that were investigated and refuted, so you
  don't waste time rediscovering them.
- Follow `agents.md` at the repo root: update `docs/code_reference.md`, `CHANGELOG.md`,
  and related docs in the same commit as any code change.
- When you implement an idea, delete it from its file (or mark it `DONE <date>` with a
  commit reference) so this backlog stays truthful.

## Files

Status column is a summary of [`00-status-and-priority.md`](00-status-and-priority.md);
that document is authoritative.

| File | Theme | Status |
|---|---|---|
| [`00-status-and-priority.md`](00-status-and-priority.md) | **Ordered status of everything** | Start here |
| [`03-performance-gpu-cpu.md`](03-performance-gpu-cpu.md) | Frame latency and throughput | STALE / PARTIAL |
| [`04-accessibility-ux.md`](04-accessibility-ux.md) | Accessibility (the product mission) | PARTIAL |
| [`05-robustness-validation.md`](05-robustness-validation.md) | Input validation, error handling, I/O | PARTIAL (V1/V4 → plan 23) |
| [`06-build-tooling-docs.md`](06-build-tooling-docs.md) | Build system, CI, docs hygiene | PARTIAL (mostly → plan 24) |
| [`07-text-clarity-plan.md`](07-text-clarity-plan.md) | GPU document and text enhancement | PARTIAL — only item 13 open (parked) |
| [`08-ml-text-sr-options.md`](08-ml-text-sr-options.md) | ML text super-resolution research | REFERENCE |
| [`10-vendor-independent-gpu.md`](10-vendor-independent-gpu.md) | Cross-vendor GPU direction | REFERENCE (not scheduled) |
| [`14-stabilization-v2.md`](14-stabilization-v2.md) | Robust camera stabilization | ACTIVE |
| [`15-aspect-safe-high-refresh-viewport.md`](15-aspect-safe-high-refresh-viewport.md) | Viewport geometry and motion | VERIFY (owner acceptance) |
| [`16-review-findings-2026-07-23.md`](16-review-findings-2026-07-23.md) | Batch C/D + plan 15 review verdict | PARTIAL (P1/P2 open) |
| [`17-project-rename-plan.md`](17-project-rename-plan.md) | Project rename (FrontRow blocked) | BLOCKED (owner) |
| [`18-annotation-mode-plan.md`](18-annotation-mode-plan.md) | Draw-on-lecture annotation mode | **VERIFY — R2 implemented, owner hardware pass pending** |
| [`19-external-review-triage-2026-07-24.md`](19-external-review-triage-2026-07-24.md) | External review verdicts; index for 20-24 | REFERENCE |
| [`20-capture-recording-integrity.md`](20-capture-recording-integrity.md) | Capture and recording correctness | **P0-P4 implemented — hardware verification/P6 remain** |
| [`21-user-data-locations.md`](21-user-data-locations.md) | Where the user's work lives | READY |
| [`22-threading-performance.md`](22-threading-performance.md) | UI-thread offload, latency, throughput | **PARTIAL — percentiles/notes/recording/capture landed** |
| [`23-security-privacy-release.md`](23-security-privacy-release.md) | Secrets, privacy, robustness, signing | **PRIVATE/TEAM READY — PUBLIC GATED** |
| [`24-test-build-gates.md`](24-test-build-gates.md) | Test and packaging gates | **VERIFY COMMIT CONTENTS — implementation/tests green** |
| [`25-stabilization-research.md`](25-stabilization-research.md) | How others stabilize; what we do wrong | REFERENCE / ACTIVE |
| [`26-virtual-tripod-strength.md`](26-virtual-tripod-strength.md) | Virtual Tripod: weakness analysis + 2026-07-26 defect report | PARTIAL / DEFECTIVE |
| [`27-advanced-panel-redesign.md`](27-advanced-panel-redesign.md) | Advanced inspector: scopes, sub-menus, real camera modes | **VERIFY — owner/hardware pass** |
| [`28-dxva-zero-copy-capture.md`](28-dxva-zero-copy-capture.md) | DXVA + zero-copy capture (plan 20 P6, staged with owner gates) | **Stages 0-4 + long soak passed — camera churn remains** |
| [`29-idea-inbox-2026-07-29.md`](29-idea-inbox-2026-07-29.md) | New idea seeds: product gaps (A-K), lecture-day journey (L-Q), dev/deploy/maintain (R-U) | **Audio seed A implemented; remaining seeds await selection** |
| [`30-external-memory-capture-bridge.md`](30-external-memory-capture-bridge.md) | Gen-2 CUDA external-memory capture bridge: API names, as-built recipe, remaining validation | **IMPLEMENTED — R1 hardware validation + fence upgrade remain** |
| [`31-lecture-transcript.md`](31-lecture-transcript.md) | Archived lecture-transcript proposal; microphone recording remains | **DROPPED — owner decision 2026-07-30** |
| [`32-live-accessible-status.md`](32-live-accessible-status.md) | Dynamic status text reaches screen readers with truthful names, events, and severity policy | **VERIFY — owner NVDA/Narrator pass** |
| [`33-multilingual-support.md`](33-multilingual-support.md) | Live-switchable English/Türkçe/Deutsch: UI, accessibility layer, AI prompts, TTS voice, flag picker | **IMPLEMENTED — owner language/a11y review remains** |
| [`verified-non-issues.md`](verified-non-issues.md) | Refuted findings — do not "fix" these | Read before analyzing |
| [`done/`](done/README.md) | Completed plans (01, 02, 09, 11, 12, 13) | Provenance only |

## Implementation order

See [`00-status-and-priority.md`](00-status-and-priority.md#recommended-order).
The short version: close the test/commit-content gate (24), verify recording
P0-P4 on real cameras (20), close user-data-location commit contents (21),
reconcile the stale docs (03/02), then public-release follow-through (23), then
stabilization architecture (14 Tier 3 / 25). Recording P6 DXVA/zero-copy is
implemented under plan 28; its long no-tear soak passed, with camera-switch and
device-removal recovery still awaiting physical-hardware acceptance.
