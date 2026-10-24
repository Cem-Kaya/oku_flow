# Architecture follow-ups

Baseline: Windows 0.3.1; reviewed 2026-10-24.
Old audit severities, line counts and launch-time estimates have been removed.
Current watchdog, retained cleanup, settings bounds, source sharing and owned
child lifecycle repairs must not be reopened as missing implementation.

## Remaining structural work

- Split the large `MainWindow`, `OkuFlowApp` orchestration and assistive runtime
  into cohesive components with narrow interfaces. Prefer extraction around a
  concrete next feature, not a wholesale rewrite.
- Reduce friend/member reach across UI, source/media, reading and control
  services. Preserve one owner for persistence, active source and OCR authority.
- Extend common theme/palette tokens rather than adding another independent
  set of hardcoded styles.
- Reconcile remaining globbed source collection with explicit source-group
  ownership where that prevents accidental duplicate/stale compilation.
- Measure build-time and test-isolation benefit. Existing shared-source
  equivalence, executable-owned MOC/RCC and PCH contracts stay intact.

## Boundaries

This is maintainability work, not evidence of an unmeasured runtime speedup.
Keep GPU/COM ownership, async retirement, exact source stamps and native control
validation unchanged during extraction. Add no general abstraction merely to
replace a small concrete implementation.

See [code reference](../docs/code_reference.md),
[developer guide](../docs/developer_guide.md) and
[validation redesign](24-test-build-gates.md).
