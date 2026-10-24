# Argon report verification — 2026-10-17

This reconciles every item in [Argon's original report](https://github.com/Cem-Kaya/oku_flow/blob/3f7481af45639db4731395f7adb9175b4038a7a5/improvement_ideas/54-argon-codebase-analysis-2026-10-17.md)
against development commit `8f4567c` and the follow-up changes. The original
review used `7dd90ac`. Meta Muse Wonderful independently read all items against
the committed baseline; Opus verified and implemented advisory backpressure,
then reviewed integration and identified the recovery-completion issue below. Neither a report's “Confirmed” label nor structural
review proves device, GUI or release acceptance.

| Item | Verified result and disposition |
| --- | --- |
| 2.1 Extra Stable persistence | The omission is real but intentional: `parameter_registry.cpp` declares `image.stabilization.bumpHold` session-only, and the developer guide says it is never saved in a preset. Keep it explicit and transient because it can hide moving content. Do not silently add it to `AdvancedConfig`, persistence or mode badges. Stabilization off clears it; a mode retaining stabilization may retain this session choice. Decision-time native-state polling already protects agent user-wins revisions. Muse suggested persistence as a possible product change; that proposal was not adopted as a defect repair. |
| 2.2 Owned adb executable | The documented lookup accepts trusted same-user override/SDK/PATH tools. It is not a remote executable-selection boundary; imposing borrow-only on all external tools would break development trees with no adjacent runtime. Preserve the lookup and test hooks; log the exact absolute image at every actual owned launch. Bundled-manifest integrity at runtime remains optional hardening, distinct from mandatory packaging pins and the much stronger authority needed to reclaim another running process. |
| 2.3 Advisory metadata overflow | Confirmed runtime defect; repaired by the production `OutboundQueue`. Control remains 128 queued packets / 512 KiB including in-flight bytes. Advisory admission stops at 96 packets / 384 KiB, reserving 32 packets / 128 KiB for reliable ACK/STATE/ERROR. Dropped metadata never disconnects a session. Reliable overflow, video overflow and the 3-second stalled-write watchdog still retire it. FIFO and exact host/generation gating remain. |
| 2.4 Silent in-call camera stall | Confirmed detection gap; repaired with an atomic pending-read watchdog polled through the existing idle `ConsumeDeviceLost` path. It measures actual time inside `ReadSample`, including the first read, and clears on every return including STREAMTICK. Budget: 10 working seconds or four negotiated frame periods, capped at 60 seconds. No driver call or release occurs on the polling thread. A late return cannot deliver pixels. Truly wedged drivers may still require restarting the app after the existing 1.5-second stop quarantine; detection does not make such a driver safely reusable. Repeated empty samples/STREAMTICKs that return within the allowance are not classified as an in-call wedge. |
| 3.1 State duplication | Confirmed architectural debt, not a standalone proven defect. Preserve side-effect handlers, validation, persistence and agent journaling. A descriptor/single-state migration needs a separate incremental design. |
| 3.2 Repeated UI/save work | Confirmed bounded redundancy. Remove the extra badge refresh in `UpdateControlEnabledStates` and `ApplyConfigToUI`; final config sync owns it. Propagate `SaveStatus` through storage injection so save refusal does not serialize again just to classify size. Retain the fresh disk read/validation before backup rotation: it prevents corrupt/future/oversized primary documents from replacing the valid backup. No measured throughput claim. |
| 3.3 Controller decomposition | Architectural follow-up. Existing PipelineOrchestrator, SettingsController, UIStateManager, InteractionController, RecordingManager and UCT controller already own substantial responsibilities. No broad class/UI rewrite is included in this defect repair. |
| 3.4 CMake module targets | Long-term design option; the proposed blanket replacement is not equivalent. Fixtures intentionally compile production code under different feature/fixture contracts with executable-owned MOC/RCC. Preserve preprocessing-equivalence, PCH and variant gates; ordinary shared module libraries cannot simply replace all groups. |
| 4.1 Localization index | Confirmed stale documentation; replace the en/tr/de-only and absent-RTL claims with the 25-language registry, system/manual preference and current structural/wording/native-acceptance limits. No translated UI strings change. |
| 4.2 Root TODO | Confirmed obsolete entrypoint; redirect to the idea index and status/priorities rather than restating old unverified open/closed claims. |
| 4.3 Root artifacts | Local `*.obj`/`*.log` files exist only in the owner's root checkout and are already ignored. They are absent from this implementation worktree and release inventory. Do not delete owner files as a code repair; no branch change is needed. |

## Review reconciliation

The pending-read stamp uses an atomic timeout claim (`-1`), so a returning read
wins by clearing its stamp or observes the watchdog's claim and exits. CPU/GPU
frame conversion is outside the armed interval. Phone sessions and explicit
stop are excluded. Existing split-stop ownership, exact-resource retention and
refusal after abandonment remain unchanged.

System sleep and hibernation must not spend the driver's allowance. The clock is
[`QueryUnbiasedInterruptTime`](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryunbiasedinterrupttime),
not `GetTickCount64` (which includes suspended time). This corrects that detail
of Muse's suggested clock while retaining its nonblocking/atomic constraint.
The fake source reader executes the production loop without a real camera;
this bounded seam tests a late return and cancellation rather than asserting
source-code wording or manipulating a physical driver.

## Opus integration-review repair

Opus found that the watchdog would expose an existing premature-success path:
reader adoption announced reconnection before any frame arrived. A silent but
openable source could then reset its retry window forever. Completion now waits
for the successful CPU/CUDA presentation hook. An adopted reader is not reopened
on each tick; another failed attempt retains the original window, and physical
recovery expiry precedes pending/active guards and cancels reconnect-owned work.
The opt-in actual-app fixture starts recovery with backoff already due, so
repeated idle drives exercise the active-reader guard. A successful presentation
completes exactly once; a zero-frame active failure stops and schedules retry
without replacing the deadline, and a later silent adoption expires. Additional
Fallback/Manual rows prove working replacement video silently retires the old
recovery owner, avoiding a false timeout over a working picture. Only the
Reconnect origin announces that its original camera returned. A separate orchestrator case
checks the unchanged original deadline across backoff and repeated Begin calls.

## Validation

- Scoped `agent_check.ps1` settings/capture cases passed. After the working-time
  clock refinement, `capture_shutdown` passed again; the retry-policy case also
  passed. These are scoped checks, not release acceptance.
- The tracked desktop-excluded matrix passed its shipping CUDA 12.9 Release
  compile, 83 selected CPU/background cases and eight CUDA delta cases. One
  explicitly optional development MCP Qt row was skipped; no selected CTest
  case was skipped and no failure remained. GUI cases were excluded.
- After Opus's final fallback/manual-owner refinement, the shipping Release
  target was rebuilt successfully and the actual-app offscreen fixture was
  rebuilt and passed. Its due-reader guard and active-failure/replacement-video
  rows cover the final recovery code. The earlier background/kernel results
  are recorded separately; they are not a claim of a complete final native
  GUI/release gate.
- Android debug assembly, all 100 JVM tests (including 12 outbound-queue cases),
  lint and 25-language resource parity passed. The small subsequent fixture
  warning cleanup passed incremental assembly/JVM/lint validation too. No APK
  was installed; physical LocalSocket/USB behavior remains unverified.
- Translation parity passed for all 24 non-English Qt catalogs, with 807
  complete keys each. No translated UI wording changed.

Raw logs, the private Android build and review task ids are retained under
`%LOCALAPPDATA%/OkuFlow/argon54-review-20261017/`; executable/runtime fingerprints
and per-case results remain in the tracked test-evidence ledgers. The owner's
desktop, phone display and running app/adb processes were untouched. Native GUI
checks remain with the hosted Windows workflow. No installer or APK was
published, no V1 promotion occurred, and physical-device/provider/clean-VM
acceptance remain separate. The later independent Maxine producer is now
implemented with its [own policy and acceptance scope](maxine_async_producer_design.md);
the Argon validation counts above predate that continuation.
