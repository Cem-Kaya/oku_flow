# Current improvement backlog

Reviewed against Windows **0.3.1**, released from `3f7481af45639db4731395f7adb9175b4038a7a5`, on 2026-10-24.

Start with [status and priorities](00-status-and-priority.md). This directory
contains remaining proposals and investigations, not an archive of completed
implementation plans.

| Topic | Current note |
| --- | --- |
| Performance investigations and optional deployment | [03](03-performance-gpu-cpu.md) |
| Remaining accessibility and UI work | [04](04-accessibility-ux.md) |
| AMD/Intel GPU processing | [10](10-vendor-independent-gpu.md) |
| Product-name decision and owner follow-ups | [17](17-project-rename-plan.md) |
| Less disruptive, behavior-focused validation | [24](24-test-build-gates.md) |
| Lecture workflow features | [29](29-lecture-workflow.md) |
| Searchable saved captures | [34](34-searchable-captured-notes.md) |
| Open images and PDFs | [35](35-pdf-image-source-mode.md) |
| Phone/camera integration | [51](51-okuflow-camera-android.md) |
| Assistant provider follow-ups | [52](52-assistant-follow-ups.md) |
| Architecture work | [53](53-architecture-follow-ups.md) |
| OCR quality and reading experiments | [55](55-uct-quality-and-reading.md) |

## Maintenance

- Check the released source and current implementation docs before calling
  something missing. A missing hardware measurement is not missing code.
- Keep remaining work specific. Label suggestions, research and intentionally
  parked ideas; do not treat them as approved implementation tasks.
- Remove completed, superseded or disproved items. Move durable implementation
  contracts into `docs/` instead of retaining an old implementation backlog.
- Older plans, audit verdicts, failed experiments and the former `done/`
  directory remain in [the immutable release history](https://github.com/Cem-Kaya/oku_flow/tree/3f7481af45639db4731395f7adb9175b4038a7a5/improvement_ideas).
  References to their evidence use that snapshot. Removing a report is not a
  claim that every historical test passed.
- Source or workflow changes still follow [the contributor guide](../agents.md).
  This cleanup changes documentation only; it authorizes no feature changes.
