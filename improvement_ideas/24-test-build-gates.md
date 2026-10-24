# Validation workflow improvements

Baseline: Windows 0.3.1; reviewed 2026-10-24.

Tracked build/test gates, native fixtures, CI, per-test fingerprints, reusable
evidence, strict release capability profiles and transactional packaging exist.
The old "missing test infrastructure", "untracked tests" and empty-preset repair
plans are completed and have been removed.

## Future test redesign requested by the owner (2026-10-14)

**Still an implementation follow-up.** Reduce false failures, repeated unchanged
checks and disruption of the owner's desktop while retaining meaningful
behavior and lifecycle coverage.

1. Replace incidental translated-copy assertions with stable control identity,
   saved state, event, navigation, placeholder and accessibility behavior.
   Retain exact comparisons where bytes or protocol values are the contract.
2. Capture actual Windows, Android and website states for language/readability,
   contrast and clipping review. Small language-capable agents can review
   screenshots and every changed translation, with explicit coverage and
   uncertainty. A screenshot verdict does not prove behavior.
3. Isolate desktop-free contracts from native input tests. Prefer the existing
   disposable hosted workflow; explore a separate suitable desktop/hardware
   machine instead of repeatedly taking the owner's focus.
4. Save the relevant screenshot and native geometry on every visual failure.
   Diagnose unresolved geometry/focus causes; do not classify them as owner
   interference or weaken behavior assertions without evidence.
5. Improve affected-case selection and explain reuse/refusal decisions. Per-test
   fingerprint reuse already exists; do not rebuild it as a new feature.
6. Measure release time, desktop interruptions and false failures. Inject real
   state, translation and clipping defects to check that the redesigned workflow
   still catches them.

These proposals do not change current scripts or override the owner's desktop
instructions. Installer byte checks, guest installation, GPU/device operation,
and spoken accessibility are distinct evidence scopes.

Current mechanics: [tests](../tests/README.md),
[developer guide](../docs/developer_guide.md) and
[Windows distribution](../docs/windows_distribution.md).
