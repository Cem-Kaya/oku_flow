# Vision reading cleanup — 2026-09-10

User-requested removal of the separate local text-recognition path in favor of
the existing Codex vision assistant. These notes record the implemented changes.

## 1. Duplicate recognition engine and setup

Removed the recognition process, watchdog, executable discovery, managed
download/install/uninstall row, language/path settings, and optional release
dependency metadata. Setup & Downloads now covers Codex CLI and NVIDIA effects.

## 2. Obsolete controls and presets

Removed the recognition toggle, dedicated preset, settings serialization,
accessibility labels, and translated strings. Read remains an explicit camera
action through the selected vision provider. New Codex configurations select
Luna with low reasoning; existing explicit model choices are preserved.

## 3. Reading behavior differs from scene explanation

Read requests verbatim text in reading order, preserves the source language,
and marks unclear portions. Explain retains its scene-description prompt.
Interface-language directives must not turn a reading into a translation.
Read keeps the complete result within the existing display safety limit so
manual Read Aloud can speak it; notes retain the returned text and camera image.

## 4. Concurrent submissions

An in-progress frame capture or vision request prevents a second Read/Explain
submission from replacing the active request or its pending notes image.
Codex camera analysis remains user initiated; enabling the overlay does not
start recurring Codex requests.

## 5. Verification

The local fake app-server checks the Read and Explain request contracts: Luna
and low reasoning, an existing camera-image attachment, temporary conversation,
restricted permissions, distinct prompts, complete reading text, and notes.
It makes no real account request and does not measure recognition quality on a
live camera feed. Six scenarios cover Read and Explain across English, Turkish,
and German interface settings.

Final validation passed on 2026-09-10:

- `scripts/agent_build.bat`: release compile, 24/24 CPU tests, 28/28 CUDA tests.
- `scripts/build_release_bundle.bat`: 28/28 release tests; published
  `dist/OpenZoom/open_zoom.exe` with matching release-build SHA-256.
- Translation integrity: 664 source keys, complete Turkish/German parity.
- Source/documentation and published app-metadata scans contain no references
  to the retired recognition engine.

Published executable SHA-256:
`653187E9B0AF5F587A2E3D5C4945AE39ABC97BEB97C18A211641913315887816`.

## 6. Startup/setup follow-up audit

Traced `OpenZoomApp::Initialize` through `SetupAssistantDialog::NeedsSetup`
and `OpenSetupAssistant`. Automatic prompting respects the saved decline
preference and checks only Codex CLI and NVIDIA Video Effects on supported
hardware. The dependency enum, rows, callbacks, download selection, and install
completion paths contain only those two tools. The Codex action shows Install
when missing and Update when present. Its translated description now explicitly
includes Read as well as Explain and Assistant.

A second search included ignored files and found stale setup tutorials and
campaign copy. These were updated to show the selected vision provider and
manual Read Aloud. The current code, UI catalogs, documentation, campaign files,
and published app metadata are clear of retired-engine references. The release
executable was also checked for the former ASCII and UTF-16 names/settings.

Historical matches remain only in `local_evidence/zero_copy_smoke/` snapshots
and one old generated `build-cuda-tests/.../moc_setup_assistant.cpp` file. Those
records are not current application sources or inputs to the shipping bundle;
their historical contents were preserved.

The follow-up build passed the release/CPU/CUDA gate and all 28 packaged-release
tests. The updated `dist/OpenZoom/open_zoom.exe` was built at 17:38:06 +03:00 on
2026-09-10 and its hash matches the validated release output. Follow-up logs:
`build/setup-removal-recheck-build.log` and
`build/setup-removal-recheck-bundle.log`.
