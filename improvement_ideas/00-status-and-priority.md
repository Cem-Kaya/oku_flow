# 00 — Status and Priority (2026-08-07)

**Start here.** This is the single ordered view of every plan: what is done,
what is half-done, what is blocked, and what to work on next. Audited on
2026-07-24 by reading each document and cross-checking it against the code and
CHANGELOG rather than trusting its own status line.

Completed plans have moved to [`done/`](done/README.md) — kept for provenance,
not as work. Refuted findings live in
[`verified-non-issues.md`](verified-non-issues.md); read it before analyzing.

## Status vocabulary

| Tag | Meaning |
|---|---|
| **DONE** | Nothing actionable left. Archived in `done/`. |
| **PARTIAL** | Real work shipped, specific items remain. |
| **VERIFY** | Implementation complete; only owner/hardware acceptance is outstanding. |
| **ACTIVE** | Being worked on right now. |
| **READY** | Specified, unblocked, nobody has started. |
| **BLOCKED** | Cannot start — waiting on an owner decision or another plan. |
| **REFERENCE** | Research/direction. Not a work item until adopted. |
| **STALE** | Document no longer matches reality; needs reconciliation before use. |

## Source review notes added 2026-09-10

[37 — Code review notes](37-code-review-notes-2026-09-10.md) records ten findings
and their individual implementations, with original evidence, impact, and
actual automated coverage. Status: **IMPLEMENTED AND VERIFIED**. The owner
authorized Astra implementations followed by root integration, compilation,
and tests. Release compilation passed; CPU passed 23/23 and CUDA passed 27/27,
with no skips. Live camera performance and real driver hangs were not measured.

## The list

[39 — Startup and UI latency](39-startup-ui-latency-2026-09-10.md) records the
follow-up performance implementation, real-camera measurements, and final
validation. It separates advertised camera rate from observed delivery and
presentation, and includes independent camera startup plus nonblocking GPU
admission. The earlier review-37 statement about unmeasured camera performance
describes that review's original validation only.

| # | Plan | Status | What actually remains |
|---|---|---|---|
| 03 | Performance GPU/CPU | **STALE / PARTIAL** | 7 of 15 marked done, but P4, P6, P8 and P15 were completed by plan 11 Waves 1&3 and never marked back. Genuinely open: **P5** (cache Gaussian weights), **P7** (CPU pipeline allocations), **P9** (launch-config tuning), **P10** (verify no dead in-place zoom kernel). Reconcile the doc first — most of its "open" list is a lie. |
| 04 | Accessibility / UX | **PARTIAL** | 1 of 7 done. Open: **U2 system High Contrast** (mission-critical, cheap), U3 keyboard-only audit, U4 dependent-control desync, U5 surface pipeline errors, U6 assistive cadence, U7 reject degenerate frames before spending VLM quota. |
| 05 | Robustness / validation | **PARTIAL** | 4 of 8 done. Open: V1 settings versioning, V4 VLM HTTPS/timeout/response limits, V7 readback error reporting, V8 slider conversion. **V1 and V4 are now restated with more detail in plan 23** — do them there. |
| 06 | Build / tooling / docs | **PARTIAL** | Nothing marked done. B2 (test infrastructure) partly exists now. **Superseded in priority by plan 24**, which is sharper about what is broken. Keep for B1 warnings, B3 CI, B4 clang-format, B5 path dedup, B7 script robustness. |
| 07 | Text clarity | **PARTIAL (parked)** | Items 1-12 and 15 shipped. Only item 13 (ML text SR) is open and is **blocked on licensing/latency**, tracked by plan 08. No item 14 exists. Effectively done. |
| 08 | ML text SR options | **REFERENCE** | Research complete; its recommendation became plan 09 (shipped). Remains the reference for plan 07 item 13. |
| 10 | Vendor-independent GPU | **REFERENCE** | Not started. Strategic direction (D3D12 compute for AMD/Intel). Large. Revisit only after correctness work lands. |
| 14 | Stabilization v2 | **ACTIVE** | Tier 1/2 shipped and being retuned continuously. Tier 3 Screen Lock is owner-approved and **plan 25 argues it is the correct primary algorithm, not an extra mode**. |
| 15 | Aspect-safe viewport | **VERIFY** | Implemented and smoke-verified. Outstanding: multi-monitor/DPI acceptance pass and the 2560x1440 timing gate. Owner/hardware task only. |
| 16 | Review findings (Batch C/D) | **PARTIAL** | P0s (F1-F4 fences) fixed. Open P1/P2: M2 AssistiveFeatureManager disconnect, M3 dual ownership, release null-guards (the plan-02 A5 remnant), F3 degraded-mode stalls, swallowed photo capture, F6 no re-present when camera off, unused generation tag, color-migration/LUT tests. |
| 17 | Project rename | **BLOCKED (owner)** | FrontRow failed clearance. Needs the owner to pick a name (OpenLoupe recommended) and run the Phase 0 checklist. Everything after is written and name-agnostic. |
| 18 | Annotation mode | **VERIFY — R2 IMPLEMENTED** | R2 code landed 2026-07-28: transient tool flyout, right action rail, checked swatches, Shape and dashed styles, eight functional scale handles, cross-window zoom/pan forwarding, Advanced action bar, persistence, contrast/text fixes, and CPU model/settings coverage. Release build and automated tests pass; owner hardware/trackpad/screen-reader/notes acceptance remains. |
| 19 | External review triage | **REFERENCE** | The verdict list and index for 20-24. Read before starting any of them. |
| 20 | Capture / recording integrity | **P0-P4 IMPLEMENTED — VERIFY HARDWARE** | Implemented 2026-07-28: timestamp/sequence identity, signed stride, exact fractional-rate VFR, checked asynchronous finalization, explicit states/drop reporting, fixed Source/1080p/1440p/2160p canvas, `_partN` mode-change pairs, bounded encoder worker queue, and fail-closed CUDA/DXGI adapter matching. Release build, CPU 6/6, CUDA 8/8 pass. Remaining: P5 reconnect/device hardware matrix; eliminating preview-slot loss needs plan 22; P6 DXVA/zero-copy is now **plan 28**. |
| 21 | User data locations | **IMPLEMENTED** | One configurable folder under Documents, dated artifact categories, Open Folder shortcut, and bundle isolation. The legacy `output/` copy-migration was removed by owner decision 2026-07-31 (it silently skipped categories with any existing destination file, then wrote a suppress-forever marker; owner chose removal over repair). |
| 22 | Threading / performance | **PARTIAL** | Recording worker + async finalize, zero-copy capture, capture counters, UI/capture-to-present p50/p95/p99, refresh-aware warnings, and O(1) lecture-note append are implemented. Remaining: detailed GPU/readback/encoder metrics, photo/OCR/VLM encode-and-write offload, bounded blocking-call removal, GPU-fed hardware recording encoder, and measured buffer-pool policy. |
| 23 | Security / privacy / release | **PRIVATE/TEAM READY — PUBLIC GATED** | Credential Manager secrets (with no unnecessary legacy plaintext migration), visible privacy state, assistant watchdog/buffer limits, settings recovery, stale temp cleanup, checksums/manifest/SBOM, and optional signing are implemented. Stable app-server still lacks a full per-turn tool allow-list; public distribution additionally needs a signed installer/update channel. |
| 24 | Test / build gates | **VERIFY COMMIT CONTENTS** | Implemented and Windows-validated 2026-07-28: release compile, CPU 4/4, CUDA 6/6, in-memory replay CTest, no false-green presets, and tested/hash-verified staged packaging all pass. Remaining: include the currently untracked test sources and `scripts/agent_build.bat` in the eventual commit/PR. Generated Y4M/MP4 media stays ignored by owner decision. |
| 25 | Stabilization research | **REFERENCE / ACTIVE** | Research complete, first quality correction implemented. The reference-frame (tripod) design is the open architectural item, feeding plan 14 Tier 3. |
| 27 | Advanced panel redesign | **VERIFY** | Phases 0–5 implemented 2026-07-26: scope separation, collapsible/searchable inspector, centralized enabled state, real camera mode requests with negotiated-format reporting, and precision geometric Ctrl+scroll. Automated builds/tests pass; owner/hardware camera negotiation and accessibility passes remain. |
| 28 | DXVA / zero-copy capture | **STAGES 0-4 IMPLEMENTED — VERIFY CAMERA CHURN** | Production now retains validated MF DXGI samples, converts them into a reusable BGRA D3D11 texture, and maps that texture in CUDA with zero CPU copy. The ladder persists direct GPU -> accelerated copy -> compatibility per camera; raw capture reads back only while recording/photos are active. The stage-4 shutdown AV (legacy `cudaGraphicsUnregisterResource`, NVIDIA driver defect) was resolved by porting the bridge to CUDA external memory — **plan 30** records the as-built recipe. Release/CPU/CUDA gates pass and the owner completed the long moving-picture no-tear soak on 2026-07-29. Only camera switching and device-removal recovery remain in the hardware gate. |
| 30 | External-memory capture bridge | **IMPLEMENTED — R1 VALIDATION + R3 FENCE UPGRADE REMAIN** | Gen-2 CUDA External Resource Interoperability replaces the crashing Gen-1 graphics-interop API in capture: NT-shared D3D11 texture → D3D12 `OpenSharedHandle` + `GetResourceAllocationInfo` → `cudaImportExternalMemory` (dedicated) → mapped array reused per frame; per-frame map/unmap eliminated; teardown deterministic. Remaining: R1 owner cycle/soak matrix, R2 legacy-crash repro loop for the NVIDIA report, R3 shared-fence async handoff (only after a week of synchronous soak). |
| 29 | Idea inbox 2026-07-29 | **SEEDS — A implemented; B selected; F/W promoted** | Audio seed A is implemented. Owner-selected B now combines comprehensive capability-probed camera hardware controls, focus/exposure/white-balance anti-hunt locks, and per-camera mode/rotation/profile/view working sets. Searchable-notes seed F is promoted to plan 34 and live-transcript seed W to plan 36. Remaining seeds: C view bookmarks, D session resume, E battery saver, G acceptance day, H diagnostics, I CI, J onboarding; lecture-day L framing assistant, M always-on-top compact view, N freeze-to-read, O change alerts, P status hotkey, Q chapter markers; **V live rewind**; maintenance R main_window split, S version identity, T crash capture/logs, U CUDA graphs. K multilingual support shipped as plan 33. |
| 31 | Lecture transcript | **REFERENCE — superseded 2026-08-07** | Historical record of the 2026-07-30 drop decision and Codex 0.145 probe. The owner's narrower reopened transcription goal is specified by plan 36. |
| 32 | Live accessible status | **VERIFY — implementation/tests complete** | `SetLiveText` now synchronizes dynamic visible text and role-qualified accessible names, invalidates cached UIA names, applies silent/polite/assertive policy, deduplicates announcements, and coalesces diagnostics. Pipeline/camera, recording, Setup Assistant, negotiated-format, Codex, and dynamic value surfaces were converted; duplicate manual announcements were removed. The Qt accessibility event regression test passes. Remaining: the P4 owner pass with NVDA and Narrator on the real rig. |
| 33 | Multilingual support | **IMPLEMENTED — OWNER LANGUAGE/A11Y REVIEW REMAINS** | Live-switchable English/Türkçe/Deutsch now covers UI, accessible names/announcements, AI response language, locale-formatted display values, and TTS voice preference. Embedded Qt catalogs contain 616/616 completed entries per language; release, CPU 11/11, and CUDA 13/13 gates pass. Remaining: Turkish owner wording review, German native-speaker review, and NVDA/Narrator live verification. CJK is an incremental catalog addition; RTL requires a separate mirroring/QA phase. |
| 34 | Searchable captured notes | **READY — OWNER SELECTED** | Paired original/processed photo saving and notes embedding already ship. Add bounded local OCR associated with each saved photo/annotation, processed-first with original fallback, stable section ids, and a semantic keyboard/screen-reader index while retaining O(1) live note appends. Share the section/index contract with plan 36. |
| 35 | PDF / image source mode | **READY — OWNER SELECTED** | Open a saved image or PDF page as a static magnifier source; reuse colour/Text Clarity/sharpening, OCR, annotation, VLM, and TTS; save a recoverable original/processed render pair without modifying the source; retain PDF page provenance in notes. |
| 36 | Recording transcription to notes | **IMPLEMENTED 2026-08-08 — Carrier D native stack, live gate PASSED** | Live user-only transcription ships end to end behind the opt-in on the native WebRTC stack (libdatachannel v0.24.5 + opus v1.5.2 + MbedTLS 3.6.7 LTS, all commit-pinned and statically linked; no browser, no proprietary bits, no extra runtime files). Every WebView2 artifact is deleted. The live WAV gate produced correct partials and finalized user segments against real Codex (Windows codex-cli 0.147.0); loopback RTP/Opus tests run in ctest. Remaining owner acceptance: real microphone plus NVDA/Narrator pass, and quota behavior over a full lecture. |
| 26 | Virtual Tripod strength | **PARTIAL / DEFECTIVE** | Phases A/B implemented. Saved-camera replay on 2026-07-26 proved the synthetic 97.3% gate overstates real performance: accumulated-reference alignment left 0.5-8 Hz vertical vibration almost unchanged and worsened horizontal motion. Production now uses the sharp single keyframe, which improves the saved clip by 24-38% on its main axes, but stronger real-scene correspondence/reacquisition remains open before Phase C. |

## Recommended order

Rationale: unblock verification first, then stop losing user data, then fix what
silently corrupts recordings, then what blocks distribution, then speed.

**Now — this week**

1. **20 — run the recording hardware matrix.** P0-P4 are implemented and the
   automated gates pass. Verify duration against wall clock, resize/monitor
   independence, induced-drop reporting, mode-change `_partN` pairs, disk-full
   behavior, and reconnect on the owner's real phone/webcam set. P6
   DXVA/zero-copy is now **plan 28**. Stages 0-4 and the long no-tear soak are
   complete; camera switching and device-removal recovery are the remaining
   hardware checks.
2. **24 — close commit-content verification.** Implementation and Windows
   validation are complete. Ensure the untracked test sources and build script
   enter the eventual PR while generated fixture media remains ignored; then
   archive the plan as done.
3. **21 — close commit contents.** User-data locations are implemented.
   Ensure `user_data_paths.{hpp,cpp}` and their tests enter the eventual PR,
   then archive the plan.
4. **03 + 02 reconciliation.** Half an hour of bookkeeping: mark the items plan
   11 completed. A backlog that lies about its own state is worse than no
   backlog, and this one currently overstates its remaining work.

**Next — correctness and distribution**

6. **23 — public-release follow-through only.** Private/team security work is
   implemented. Before arbitrary public distribution, close the app-server
   allow-list gap when the protocol supports it and ship a signed
   installer/update channel.
7. **16 P1 items.** Teardown robustness and release null-guards — cheap, and
   they protect everything above.

**Then — experience and speed**

8. **26 — fix the two Virtual Tripod defects.** Validation on 2026-07-26 found
   the tripod dies permanently after a ~40 source-pixel bump, and mis-tracks
   past 0.45x frame because the clamped measurement is reused as the tracker
   seed. Seed-before-clamp is one statement and is verified; the re-acquisition
   ladder is the real work. Do not start Phase C until both land.
9. **14 Tier 3 / 25 — reference-frame stabilization.** The owner's most-felt
   problem. Now well understood: drift is structural, so this is an
   architecture change, not more tuning. Plan 26 supersedes it for the
   already-shipped tripod path.
10. **04 U2 — system High Contrast.** Small, and it serves the core mission.
    U4 is absorbed by plan 27 Phase 0; U3 shortcuts pair with plan 27.
11. **18 §R2 — Draw UI redesign verification.** R2-A through R2-E are
    implemented with automated gates passing. Complete the live D3D,
    mouse/trackpad, screen-reader, contrast, and notes-output checklist.
    **21** should still land before more notes-path work, since annotation
    snapshots write into the folder 21 moves.
12. **22 — threading and performance remainder.** The percentile instrument
   and several largest wins are shipped. Continue only from measured p95/p99:
   photo/OCR/VLM offload, blocking waits, then a GPU-fed recording encoder.
13. **34 then 35 — searchable captures and saved-source reading.** First give
    saved captures local OCR/index semantics, then reuse that notes contract
    when images and PDF pages become magnifier sources.
14. **36 — recording transcription to notes.** Implemented behind the opt-in
    on the native Carrier D stack with the live gate passed; what remains is
    the owner acceptance pass (real microphone plus signed-in Codex, quota
    behavior over a full lecture, NVDA/Narrator) and
    recording-independence spot checks on every failure path.

**Owner decisions blocking work**

- **17** — pick a name and run the Phase 0 clearance checklist.
- **15** — run the multi-monitor/DPI and 1440p timing acceptance pass.
- **12** (archived) — a final screenshot/screen-reader look at the colour picker.
- **23** — lawyer review of CLA/§7 and public code-signing/installer ownership
  before real commercial contracts or arbitrary public distribution.

**Deliberately not scheduled**

- **10** vendor-independent GPU — strategic, large, premature.
- **07 item 13 / 08** ML text SR — blocked on licensing and latency budget.
- **06** — mostly superseded by 24; pick up B1/B3/B4 opportunistically.
- Full-frame neural stabilization (plan 25) — wrong tool for a zoomed magnifier.

## Health note

Two documents (02, 03) had drifted badly out of date because work was completed
under plan 11's wave structure without marking the originating items. The
directory's own rule already covers this — *"when you implement an idea, delete
it from its file or mark it DONE so this backlog stays truthful"* — it just was
not followed across documents. When a wave-style plan finishes, close the items
in the plans it drew from, then move it to `done/`.
