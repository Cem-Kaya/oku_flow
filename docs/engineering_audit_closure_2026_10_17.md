# Engineering audit closure — 2026-10-17

This record maps all 36 findings in the supplied *OkuFlow Agent Engineering
Brief* to code changes and evidence. “Repaired” describes an implemented boundary
with targeted regressions; it does not imply a published release or complete
hardware/provider/installer acceptance. Original failed attempts remain in the
private evidence archive, not in Git or release payloads.

The work is on `fix/report-critical-20261017`; browser controller changes are in
the sibling website's `fix/report-transcription-cancel-20261017` branch. Public
v0.2.5 downloads have not been replaced by this work.

| Finding | Disposition | Implementation / evidence |
| --- | --- | --- |
| NP-001 | Repaired | Camera open/reopen/reconnect driver startup is worker-owned; real-target held-open/cancellation regressions. |
| NP-002 | Repaired | Microphone open, flush and final shutdown are worker-owned; late session completions cannot affect successors. |
| NP-003 | Repaired | Recording remains nonterminal until old-session cleanup completes; barrier regressions. |
| NP-004 | Repaired | Annotation PNGs use exclusive temporary files and atomic no-clobber publication. |
| NP-005 | Repaired | Glare-only Text Clarity no longer enables unchecked background flattening; CPU/CUDA fixtures. |
| NP-006 | Reproduced and repaired | Native AAC compacted a three-second gap. Bounded silence restores the marker to 3.999333 s versus video 3.999967 s, without changing normal PCM accounting. |
| NP-007 | Repaired | Every failed encoder attempt keeps its bytes; retries use exclusive GUID paths and notes use the selected finalized pair. |
| NP-008 | Repaired | Annotation intent retains displayed source/frame/processing revision, actual GPU output signature and transform; camera retirement revokes pending copies. |
| NP-009 | Repaired | Saved microphone None is distinct from actual audio capture; metadata follows real capture state. |
| NP-010 | Measured; async producer under review | One actual 60 s Maxine run measured host p95/p99 7.583/12.469 ms, max 80.766 ms and 201.780 ms model load. A bounded asynchronous producer is under review; no native pan/stale-frame speedup is claimed. |
| PW-001 | Repaired | One serial Android session owner fences restart, stop and disconnect with immutable host/session tokens. |
| PW-002 | Repaired, sibling website | Cancellation generation rechecked after every await and before worker/result publication. |
| PW-003 | Repaired | Completed segments are frozen before awaits; sidecar writes serialize by media identity. |
| PW-004 | Repaired across native/browser/website | Immutable UUID/content markers plus bounded fresh byte verification bind notes, File handoff, result cache and UUID-named transcript sidecars. |
| PW-005 | Repaired, sibling website | Audio decoding and worker results preserve the original media timeline origin. |
| PW-006 | Repaired, sibling website | Decoder/parser acquisition, feeding and flush use finally cleanup while preserving the primary error. |
| PW-007 | Repaired | Native and Android share one bounded startup deadline; candidate retries reserve useful time and CSD alone does not start HEVC output timing. |
| PW-008 | Repaired | Serialized field patches replace stale whole-settings writes; concurrency/barrier JVM fixtures. |
| PW-009 | Repaired | Zoom intent waits for its exact capture session and survives same-generation fallback, without modifying display settings. |
| AS-001 | Repaired at child launch boundary | Non-coding Codex launches use a constrained capability profile and replace the child after permission changes. Live provider/OS sandbox acceptance remains separate. |
| AS-002 | Repaired | Credential IDs bind immutable provider/endpoint/key tuples; failed settings commit and backup recovery preserve matching old generations. |
| AS-003 | Repaired | History commits before pruning; bounded durable overflow preserves previous conversations on write/cleanup failure. |
| AS-004 | Repaired | Authorized image observations retain native provenance and grant epochs through final HTTP/Codex/MCP egress checks. |
| AS-005 | Repaired | Exact approvals report pending until actual operation outcome; final receipt retains the original chat-turn identity. All 48 new translated entries reviewed. |
| AS-006 | Repaired | Wait compares the same enriched operation projection used by queries, avoiding unchanged-state false transitions. |
| UI-001 | Repaired | Stable message/block/UTF-16 anchors preserve duplicate occurrence, reverse selection and reading position; deleted messages cannot transfer selection. |
| ST-001 | Repaired | Quick Mode Save commits immediately and rolls back only the staged mode on failure. |
| TB-001 | Repaired | Fresh exact-byte hashes bracket every evidence attempt, including resume; metadata-preserving mutation invalidates reuse. |
| TB-002 | Repaired | Strict release-capability profiles reject missing/skipped required evidence; optional development skips stay explicit. |
| TB-003 | Repaired | Imported Android host probe participates in CI and scoped impact selection. |
| BR-001 | Repaired | CMake minimum is 3.25 for schema-6 presets; official 3.25.3 root/preset parsing checked. |
| RB-001 | Repaired | WiX executes only the exact pinned archive apphost/tree; ambient feeds and cached installation shims cannot substitute tools. |
| RB-002 | Repaired | Effective CMake target facts and successful-build facts/binary seals govern CUDA/compiler/runtime/license provenance; reconfigure-without-build is refused. |
| RB-003 | Repaired | Verified bundle generations exchange by same-parent rename with rollback. Locked/user-owned/unknown bundles remain intact; previous complete generations are retained. |
| RB-004 | Repaired | Bundle builds use Ninja with VS2022 x64 and compilation databases; unsupported generators/stale caches fail before mutation. |
| PF-001 | Measured and optimized | Eligible streaming tails update only the final root paragraph; stale resize callbacks no longer rebuild unchanged documents. Same-workload queued p99 improved from 36.062 to 10.610 ms with zero selection/anchor changes. |

## Ultra Clear Text

The experimental opt-in uses local Windows OCR on one latest-only worker, an
independent bounded probe worker and cached glyph reprojection on presentation.
Slow recognition never blocks presentation. Shared line type sizes/baselines
avoid per-word stretching. A live synthetic-camera review accepted all 44 printed
fixture words and exercised Show original. A held-OCR fixture measured presentation
hook p99 0.006 ms and 1080p Qt painting p99 3.115 ms; these exclude real camera,
GPU scanout and OCR accuracy beyond the fixture. High-risk/moving/composite text
fails closed. Curved pages, handwriting, reliable math/diagram replacement,
multimonitor/NVDA acceptance and localization of new UCT controls remain open.
See [UCT architecture and limits](ultra_clear_text.md).

## Validation evidence

Private archive: `%LOCALAPPDATA%/OkuFlow/audit-closure-20261017`.

- Earlier complete tracked gate at `fe3cbd4`: shipping compile, 144 CPU/native
  cases (one explicitly optional dev-full Qt row skipped), and seven CUDA delta
  cases passed. Later changes require the final gate below.
- Remaining boundary SCOPED gate: recording manager, native H.264/AAC, phone
  protocol, notes HTML and source equivalence: five passed, zero skipped.
- Policy SCOPED gate: recording integrity, deadline worker and complete headless
  Assistant panel fixture passed. Final chat parity/fallback slot also passed.
- Android tracked debug assembly, JVM tests and lint passed; 11 Python host
  protocol tests and eight new pure JVM budget cases passed. No APK was installed.
- Bridge Node contracts: 11 passed; sibling website check: 23 passed plus asset,
  14-page and language checks. Native Chrome folder-grant acceptance is separate.
- Private real WiX synthetic MSI compile/extract and provenance mutation/refusal
  fixtures passed; no installer was installed on the owner's PC.
- Native target fixture verifies annotation source/geometry/actual-output refusal,
  camera/microphone worker ownership, operation waiting and stop/undo. Refreshed native fixture passed after the actual SR/focus-output signature
  regressions. The final tracked Windows gate passed: shipping compile, all 146
  CPU/native cases (one optional dev-full Qt row skipped), and seven CUDA delta
  cases with no skips or unresolved failures. No saved results matched; all
  selected cases ran. PCH-OFF compilation plus chat, recording-integrity and
  source-equivalence checks also passed (three cases).

No phone display resolution, density, aspect ratio, rotation, scaling or font
setting was changed. No owner process or foreign adb server was stopped. There
is no new public installer, Play rollout, V1 catalog acceptance, or physical GTX
1070/microphone/camera/Maxine quality claim from these fixtures.

## Follow-up measurement

NP-010 now has actual host-tail evidence from one off/on synthetic-camera pair
using the installed NVIDIA runtime. Across 1,253 successful frame calls, p95/p99
caller wait was 7.583/12.469 ms and maximum 80.766 ms; model load took 201.780 ms.
There were no timeouts, rejected calls or SDK failures. Off/on camera-tick p95
was 1.889/6.411 ms and UI heartbeat-delay p95 was 1.914/5.010 ms. These are one
development-PC pair with a virtual source, not pan-to-present or real optics
acceptance. An independently scheduled, bounded SR producer is being reviewed
against GPU lifetime, capture pairing and exact presentation provenance before
any asynchronous replacement is accepted. The [performance diagnostic record](performance_diagnostics.md)
keeps the unsuccessful intermediate chat measurement as well as the final
improvement; controlled sleeps are not vendor SDK performance measurements.
