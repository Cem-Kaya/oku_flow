# Accessibility and UI follow-ups

Baseline: Windows 0.3.1; reviewed 2026-10-24.

Accessible names, dynamic label synchronization, keyboard routes, live status,
translations and dependent-control synchronization exist. The old claims that
the app has no labels, no shortcuts or almost no assistive support are obsolete.

## Remaining work

- **System-theme consistency:** finish replacing hardcoded popup/overlay colors
  where they bypass the current palette helpers. Check Windows contrast themes
  and light/dark changes without overriding a user's chosen reading colors.
  Theme support is partial, not wholly absent.
- **Compact reading view:** a keyboard-accessible, always-on-top magnifier for
  taking notes in another app. This is a new viewport mode, distinct from the
  existing floating Assistant and corner windows; see [29](29-lecture-workflow.md).
- **On-demand spoken status:** one action summarizing recording, source,
  available space and relevant faults. Existing individual status labels and
  announcements are not this aggregate query.
- **Source-aware reading navigation:** preserve reading place across explicit
  rereads, provide useful line navigation and expose uncertainty without
  pretending OCR output is guaranteed correct. See [55](55-uct-quality-and-reading.md).
- **Feature-specific polish:** add native keyboard and accessible-state behavior
  alongside each new PDF, bookmark, notes-index or camera-control feature.

## Coverage, not missing implementation

Real NVDA/Narrator output, mixed-DPI layouts, RTL and wording quality need their
own evidence. Preserve existing results and actual defects; do not present an
unperformed manual pass as a missing labeling implementation. The validation
redesign belongs to [24](24-test-build-gates.md).

Current contracts: [UI modes](../docs/ui_modes_design.md),
[live languages](../docs/localization.md) and
[positioned UCT text](../docs/uct_screen_text_accessibility.md).
