# Remaining work after 0.3.1

Baseline: released source `3f7481af45639db4731395f7adb9175b4038a7a5`.
Reconciled 2026-10-24 against source, current implementation docs and owner
decisions. These are suggested priorities, not new execution grants.

## Suggested next work

| Order | Remaining outcome | State |
| --- | --- | --- |
| 1 | Open a saved image or PDF and use the magnifier/UCT on it | [35](35-pdf-image-source-mode.md): selected earlier; file-source UI/PDF rendering missing |
| 2 | Associate readable text with saved captures and navigate/search notes | [34](34-searchable-captured-notes.md): association/index missing; saving and browser transcription exist |
| 3 | Save view positions, use a compact always-on-top view, add chapter markers | [29](29-lecture-workflow.md): product proposals |
| 4 | Connect viewport zoom to phone zoom and add capability-based camera working sets | [51](51-okuflow-camera-android.md): transport exists; application integration remains |
| 5 | Reduce laptop power use and offer a smaller base download | [29](29-lecture-workflow.md), [03](03-performance-gpu-cpu.md): proposals |
| 6 | Improve difficult-page reading and character-change detection | [55](55-uct-quality-and-reading.md): research; current BF16 preference stays |
| 7 | Reduce validation interruptions and isolate large modules | [24](24-test-build-gates.md), [53](53-architecture-follow-ups.md): engineering work |

[Accessibility follow-ups](04-accessibility-ux.md) accompany each feature.
[AMD/Intel effects](10-vendor-independent-gpu.md) are a separate large project.
[Claude integration](52-assistant-follow-ups.md) is still a future provider.

## Already implemented: do not restart

- Ultra Clear Text with separate CPU/GPU detector and recognizer choices.
- BF16 preference for both selected GPU roles, same-tier FP32 fallback and
  CPU fallback. Perfect text or BF16/FP32 agreement is not an app startup gate.
- Exact merge improvements, batched OCR helpers, bulk copies, two-slot
  look-ahead and GPU integral work. Some alternative-width/regional paths remain
  optional; see [03](03-performance-gpu-cpu.md).
- Deadline-armed text expiry and one bounded probe admission credit after OCR
  completion. The released controller contains `ArmExpiry` and `CreditCompletion`.
- Freeze, saved-video playback and up to five minutes of **video-only** live
  RAM recap. Audio recap and clip export are separate missing pieces.
- Embedded Assistant Send preparation, turn-scoped current-view observations,
  UCT controls and external MCP V1.
- Dynamic accessible-name updates, 25 UI languages, Setup Assistant, annotation,
  persistent user paths, fixed-reference stabilization and zero-copy capture.
- Async recording/media work, timestamp/silence repairs, tracked test evidence,
  CI, MSI/portable packaging and exact payload/checksum/SBOM staging.

## Decisions that remain in force

- **Fast live model / accurate Freeze model:** feedback-only proposal. The
  owner explicitly said not to implement automatic switching yet.
- OCR errors are expected. Quality comparisons are diagnostic; do not revive
  perfect-golden-text or equality requirements for normal BF16 startup.
- Desktop transcription was retired in favor of the browser workflow. Do not
  restore the old separate recognition/transcription setup.
- Ordinary settings persistence, replay Resume and a crash-recovery session
  journal are different features; the first two already exist.
- Native/device/screen-reader coverage is tracked with its actual evidence.
  Pending coverage alone does not put an implemented feature back in this list.
