# Task ledger — Opus end-to-end latency + shadow tracker audit (2026-10-23)

Source frozen: Z:/Projects/T3-worktrees/oku_flow/t3-f93349e5 @ 9bdd5d13f2352342860ba146ad164c7e46645d05
(worktree HEAD a7d4757 differs only in CHANGELOG.md, docs/code_reference.md, tests/uct_surface_test_cases.hpp).

| # | specialist | provider/model/options | clientRequestId | taskId | scope | status |
|---|---|---|---|---|---|---|
| A | read-only source | codex / gpt-6.1-sol / reasoningEffort=high | opus-e2e-shadow-audit-20261023-specA-r1 | node:delegated-task:command%3Amcp%3Acfef0c2e-94ee-4446-a824-fb9448e48b97%3Adelegate-task%3Aopus-e2e-shadow-audit-20261023-specA-r1 | app capture/present/lease/controller/overlay/a11y path | completed; report read and reconciled |
| B | read-only source | codex / gpt-6.1-sol / reasoningEffort=high | opus-e2e-shadow-audit-20261023-specB-r1 | node:delegated-task:command%3Amcp%3Acfef0c2e-94ee-4446-a824-fb9448e48b97%3Adelegate-task%3Aopus-e2e-shadow-audit-20261023-specB-r1 | region planner, scheduler/worker, current-input validation | completed; report read and reconciled |

No watchers spawned; completion notifications are the only wake signal.

## Parent scope note (received mid-audit)
Public-prep seam issues already routed (candidate-deadline gating ordinary FP32 with EMPTY catalog; FindPlan builder loop; directory-pin attribute DELETE weakness confirmed by root native probe). 9bdd contains them; NOT counted as new here. Public publication-lifetime Qt unhandled exception attribution unknown; out of scope. Audit stays on latency/tracking.
Correction found by this audit: `git merge-base --is-ancestor 618ee9a 9bdd5d1` = true, so the 618 Resume fix is inside the audited source.

## Reconciliation
- A completed: specialists/specA_app_path.md. Promoted A1 (merged into P2-2), A3 (P2-1), A2 (I8), A4 (I9), G1 PresentationReceipt-unemitted (verified by grep). All cited lines spot-checked by Opus.
- B completed: specialists/specB_shadow_planner.md. B-P2 downgraded to P3-1 (verified 1514-1565, 4024-4093; resident path always checks >=64 lines). B-P3s kept as P3-3/P3-4. B ideas 2-4 listed as not-new/low.
- Opus-own: P3-2 orphan live capture/dual throttle, D1 regional cannot shorten new-text latency, I1/I4/I5/I6/I7/I10.
- Final report: uct_end_to_end_shadow_audit_opus_2026_10_23.md. No repo edits, no execution.
