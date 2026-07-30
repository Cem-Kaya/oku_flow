# Plan 23 — Security, Privacy, Robustness, and Release Integrity

Status: **IMPLEMENTED FOR PRIVATE/TEAM DISTRIBUTION (2026-07-29); PUBLIC
DISTRIBUTION REMAINS OWNER-GATED.** Verdicts and evidence: plan 19.

## 2026-07-29 implementation record

- **A implemented without legacy migration.** OpenZoom has only lived on the
  owner's and a few teammates' machines and no deployed build ever persisted
  a VLM key. AI Settings now stores the secret in Windows Credential Manager
  under `OpenZoom/VLM API Key`; JSON stores only `vlmCredentialId` and ignores
  a plaintext `vlmApiKey` field. The environment override remains
  process-only. There is deliberately no migration path for a deployment that
  never existed.
- **B, D, E, and F implemented.** Requests surface a deduplicated privacy
  summary suitable for the status area and screen reader. Codex turns have
  idle/maximum/interrupt watchdogs and bounded protocol, answer, and
  transcript storage. Model-catalog errors name the server compatibility
  problem. Settings classify first run/corruption/future schema, preserve bad
  files, restore a valid backup, and surface save/recovery failures. Temporary
  OCR/Codex frames are process-named, cleaned on all normal exits, and stale
  dead-process files are swept at startup.
- **C is implemented to the strongest stable protocol boundary, with one
  explicit residual limitation.** Simple Explain sends read-only,
  no-network, approval-never turns and denies every server approval,
  permission, and elicitation request. Unexpected MCP/dynamic/collaboration
  and other forbidden tool items are interrupted. The current stable
  app-server `turn/start` schema does not expose a complete per-turn tool
  allow-list, so OpenZoom cannot yet prove that an unexpected tool item is
  rejected before the server reports it as started. The reactive path remains
  defense in depth and this plan must be revisited if app-server adds that
  capability.
- **G implemented for the actual private/team release model.** Bundle staging
  emits SHA-256 checksums, a release manifest, and an SPDX SBOM. An installed
  code-signing certificate can be selected by thumbprint. Unsigned private
  bundles remain permitted; setting `OPENZOOM_PUBLIC_RELEASE=1` fails closed
  without signing. A signed installer/update channel is still required before
  distributing to arbitrary public users.

## A. API keys must leave settings.json

`vlmApiKey` is serialized in cleartext (`settings_store.cpp:480`, read back at
`:510`). Anyone who can read the profile — or receives a copied settings file,
a backup, or a support archive — gets the key.

**Fix.** Store the secret in Windows Credential Manager (or DPAPI-protected
blob); keep only a credential identifier in settings.json. Because no
plaintext-key build was deployed, ignore plaintext fields instead of carrying
an unnecessary compatibility path. Keep an
environment-variable override for development. Keys must never appear in logs,
diagnostics exports, crash metadata, or the assistant transcript — add an
explicit redaction pass on any future support-bundle feature.

This is the single most important item in this plan, and it is small.

## B. Privacy the user can see

The OpenAI-compatible path base64-encodes a camera frame to a configured
endpoint; the Codex path can attach a frame too. For a camera app used in
lecture halls — where other students and their work are in frame — the user
must be able to tell, at a glance and by screen reader, whether an image is
leaving the machine.

Surface, at request time and not only in a tooltip: whether a frame is
attached; which provider receives it; whether the endpoint is local or remote;
whether the conversation is persistent; and whether internet/coding access is
enabled. Announce state changes (announcements, never TTS — standing rule).

## C. Assistant capability enforcement

Current model: developer instructions + sandbox policy + reactive interruption
when a forbidden tool item is *reported as started*
(`codex_app_server_client.cpp:434-476, 828-869, 898-917`). Reacting after start
is a race — the item may read data before the interrupt lands.

For the restricted "explain what the camera sees" use case, disable command,
file, web, MCP, dynamic, and collaboration tools **at the protocol level** so a
forbidden action cannot begin, rather than relying on instructions plus
interruption. Keep the reactive path as defense in depth, and write a test that
proves a forbidden command cannot start.

## D. Assistant liveness and bounded buffers

- **No turn watchdog.** JSON control requests expire after 60 s
  (`codex_app_server_client.cpp:503-523`), but an acknowledged `turn/start`
  with a lost `turn/completed` leaves `activeThreadId_` populated and the UI
  busy forever. `InterruptTurn` sends an interrupt with no reply handler and no
  forced local completion (`:325-338`). Add: idle timeout, maximum turn
  duration, interrupt-acknowledgement timeout, and a local forced reset that
  always returns the UI to idle.
- **Unbounded growth.** `stdoutBuffer_`, `activeText_`, and the transcript have
  no documented limits (`:554-570`, `:627-631`; UI append at
  `app_bootstrap.cpp:668-679`). Add maximum line, turn, transcript, and history
  limits with visible truncation.
- **Model-catalog errors ignored.** The `model/list` callback drops its error
  object (`:721-750`); the owner's own run logged a models-cache schema error
  (`missing field supports_reasoning_summaries`) while the app continued with an
  empty/stale catalog. Surface it as a compatibility warning naming the
  app-server version.

## E. Settings robustness

- **Silent failures.** The loader returns `std::nullopt` identically for
  missing file, permission error, invalid JSON, and unsupported structure
  (`settings_store.cpp:627-750`), and the controller then silently loads
  defaults (`settings_controller.cpp:18-32`). A student who loses every preset
  after a bad shutdown gets no explanation. Distinguish first run from
  corruption: preserve the bad file as `settings.json.corrupt-<timestamp>`,
  restore the newest valid backup if present, and say what happened.
- **Save result ignored** (`app_settings.cpp:235-253`) — surface failures in
  the status area.
- **Future versions accepted.** Only legacy version 1 is special-cased
  (`settings_store.cpp:651-659`); a newer-than-current version is read as if it
  were current. Reject with a clear message or define a compatibility mode.
- Add backup, restore, and reset-by-category (the existing Reset Tuning is a
  good precedent).

## F. Temporary camera frames

OCR and Codex temp images use `setAutoRemove(false)`
(`assistive_runtime.cpp:672-688, 863-875`); normal paths delete them, crashes do
not — and the owner's `%TEMP%` already contains a pile of
`openzoom_ocr_*.png` files, each a picture of a lecture. Sweep stale
`openzoom_ocr_*` / `openzoom_codex_*` at startup, delete on cancellation and
shutdown, and prefer memory-backed transport where the consumer allows it.

## G. Release integrity

Missing today: code signing, signed installer, update signature verification,
published checksums, SBOM, crash-symbol manifest, reproducible release
metadata. This matters more than usual because the app launches configured
executables and can download external runtimes through Setup Assistant.

Minimum before public distribution: signed binaries and installer, published
SHA-256 checksums, an SBOM covering Qt/CUDA/Tesseract/Maxine/Codex CLI, and
signature verification on anything Setup Assistant downloads (the pinned
SHA-256 bootstrap is a good precedent to extend). Pair with plan 21 — an
installer cannot ship while user data is written under the install directory —
and with plan 17, since the trademark and product name land on the certificate.

## Acceptance

No secret appears anywhere in `%APPDATA%`, logs, or exports; a plaintext JSON
field is ignored. A stalled assistant turn always returns to idle within the
configured timeout. A corrupt settings file produces a named preserved copy, a
restored or default profile, and a screen-reader-friendly explanation.
`%TEMP%` contains no stale `openzoom_*` frames after a crash-then-restart
cycle. Every private/team bundle verifies against its generated checksum.
Public acceptance additionally requires a valid signature and the
installer/update work intentionally left outside the current deployment scope.
