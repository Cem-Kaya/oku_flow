# Windows phone/GPU safeguard validation, 2026-10-13

The working tree implements decoder-progress recovery, visible codec aperture
handling, nonblocking retained GPU readback, GPU completion deadlines, bounded
NVIDIA host calls, coalesced strength updates and persistent fault reporting.
These changes are built locally; they have not been bundled or published.

## Automated checks

- Shipping CUDA/SuperRes configuration compiled successfully.
- Earlier complete CTest executions: CPU 79/79 (88.99 seconds), CUDA 84/84
  (90.45 seconds), no skips. Each includes 55 serial GUI entries.
- An earlier CUDA run failed the right-side docked-header held-pull assertion.
  Ten instrumented diagnostic repetitions passed. Annotation fixtures now verify
  native foreground ownership and log cancellation events; the following full
  CPU/CUDA executions passed. The original intermittent event was not captured,
  so native focus remains a suspected cause rather than a proven diagnosis.
- The evidence runner initially rejected the otherwise passing CUDA execution
  because it fingerprinted replay output BGRA/CSV bytes as inputs. Its diagnostic
  named those exact two files. Fixture `--output` and `--trace` arguments now
  declare generated outputs; command paths remain fingerprinted.
- The corrected runner passed 34 contract checks, including changing generated
  outputs across consecutive runs, and accepted the real CUDA replay result.
  Its selected-test ledger correctly reports the other 83 cases as unresolved.
  That selected execution was not a complete publishing gate. The later full
  matrix below regenerated both ledgers after the correction and scale fix.

## Short real-phone check

The connected, authorized Pixel 9 Pro Fold was detected through USB. No shared
adb server was stopped, user settings were changed, or user app was terminated.

- Eight-second protocol probe: HEVC 4000 × 3000 @ 30 requested, 226 frames,
  30.06 FPS measured over frame arrivals.
- Fresh Windows build, isolated settings, stabilization enabled: automatically
  connected to the phone, HEVC 4000 × 2256 @ 24 negotiated. Fifteen-second run:
  314 received, 311 presented, 23.99 arrival FPS, 23.88 viewport FPS, no expired
  GPU retries or camera errors, normal exit.
- A second independent ten-second launch reconnected: 196 received, 194
  presented, 23.91 arrival FPS, 24.15 viewport FPS, no expired GPU retries or
  camera errors, normal exit. This is session reconnection, not cable-unplug
  recovery or a long soak.
- UI pulse p95 was about 1.3 ms; maximum delay was about 1.0–1.14 seconds during
  startup. Capture-to-present figures are submission timings, not sensor-to-screen
  latency. Two ingress drops occurred in each run; the second had two missed
  presentation attempts. Tracking confidence was low, limiting stabilization
  quality conclusions for this scene.
- SuperRes was requested, but logs explicitly report the NVIDIA Video Effects
  runtime missing. Actual SuperRes inference and the original combined
  SuperRes/Extra Stable failure were **not** validated. Extra Stable is transient
  UI state; the attempted fixture JSON field does not enable it.

Private evidence: `%LOCALAPPDATA%/OkuFlow/test-evidence/`,
`%LOCALAPPDATA%/OkuFlow/camera-probes/20261013-053617-237987/`, and
`%LOCALAPPDATA%/OkuFlow/phone-final-check/`. These are excluded from publication.

At this earlier stage, installed-runtime and complete-gate acceptance remained
open. See the later checks below. Genuine USB removal/reattachment, longer soak,
then verified bundle/MSI staging and publication remain outstanding.

## Installed NVIDIA runtime check

NVIDIA Video Effects 0.7.6 for Ada was installed from the official NVIDIA
installer. Its SHA-256 matched the application's pinned value. The installed
runtime reports 0.7.6.0 and includes the Ada SuperRes model files.

A 15-second isolated profiling child loaded SuperRes in 639 ms and reported
`Active | AI stage 960x540 -> 1280x720 (1.33x)` with CUDA stabilization enabled.
The selected phone was unavailable and capture fell back to the Windows virtual
camera at 1280 x 720 / 30 FPS. This verifies runtime loading and inference, not
phone-camera quality or the reported SuperRes plus Extra Stable interaction.
The report recorded 30.03 arrival FPS, 21.26 viewport FPS, 123 ingress drops,
two missed presentation attempts, zero expired GPU retries and no camera error.
Startup's maximum UI delay was 1.895 seconds; no steady-state speed claim is
established by this short run.

The application wrote its report and exited, but the profiling PowerShell
wrapper remained waiting for redirected output. A newly started shared adb
server remained a child of that application, consistent with inherited pipe
handles preventing end-of-stream. Only the owned profiling wrapper was stopped;
the shared adb server was left running. Its wrapper did not finish validation,
so this is diagnostic evidence rather than a passing profiling-script gate.
Bounded output collection in `profile_startup.ps1` remains follow-up work.

Private evidence: `%LOCALAPPDATA%/OkuFlow/superres-installed-check/worker-1/`.

## Phone stress checks with the installed runtime

Four isolated 60-second sessions used the authorized USB phone at negotiated
HEVC 4000x2256 @ 24 FPS. Arrival rates stayed between 23.97 and 24.00 FPS;
viewport rates were 23.67 to 24.21 FPS. No GPU retry expired. These short runs
are not long-soak acceptance. User settings and the shared adb server were
preserved; profiling children exited at their configured deadline.

- SuperRes at maximum strength, stabilization and the actual Extra Stable UI
  checkbox ran together. Logs confirm `bump mode 1`; transient state was not
  inferred from JSON. Native controls, zoom and window resizing were exercised.
- Dragging zoom to 16x reproduced NVIDIA SDK resolution error `-16`: the
  selected 4x model received 1000x564, above its documented 540p input limit.
  The camera/view kept advancing instead of hanging. Scale selection now
  skips factors outside their documented input-height limit, so this stream
  can choose its valid 2000x1128 -> 4000x2256 2x stage while preserving 16x
  desktop zoom. Regression checks cover this size and the model boundaries.
- The 2x run recorded both original and processed AV1 files while SuperRes,
  stabilization and Extra Stable were active. Pan, Simple mode and maximizing
  the window were exercised during recording. Both files fully decoded with
  ffprobe without errors: 912 frames each, 4000x2256, matching 38.085917-second
  durations. Test audio was explicitly disabled. The files stayed private.
- The recording run measured 23.98 arrival FPS and 23.95 viewport FPS, with
  four ingress drops, zero missed presentation attempts and zero expired GPU
  retries. UI pulse p95 was 18.03 ms; maximum delay was 1002.68 ms. Maximum
  delay is included, rather than interpreting the p95 as a stall-free result.
- Android's historical process-exit report showed no new crash during these
  sessions; its latest exit was the earlier package update at 04:02.
- No physical unplug/reattach event was observed in these four sessions.
  The scene also limits conclusions about stabilization tracking quality.

Private evidence: `%LOCALAPPDATA%/OkuFlow/phone-heavy-check/`, sessions
`20261013-173938`, `20261013-174132`, `20261013-174504`, `20261013-174707`.
The observations above are explicitly pre-fix evidence.

## Post-fix verification

`scripts/agent_build.bat` completed the shipping release compile and full serial
matrix: CPU 79/79 (86.78 seconds), CUDA 84/84 (87.41 seconds), no skipped Qt rows
or unresolved cases. Both ledgers were accepted by the corrected evidence
runner. Each configuration includes 55 GUI cases. The bundle gate is separate
and has not been run for publication of this build.

- Session `20261013-175858` started at 16x and loaded the supported
  2000x1128 -> 4000x2256 2x model in 109 ms, with no resolution error.
  The phone app was deliberately force-stopped at 17:59:25 and reopened at
  17:59:40. Windows logged the lost USB connection, temporarily selected the
  available Windows camera, then automatically returned to the preferred phone
  at HEVC 4000x2256 @ 24 FPS without restarting the desktop app. SuperRes
  reloaded in 92 ms. This tests process/endpoint loss, not physical unplug.
  Six ingress drops and 16 missed presentation attempts were reported across
  source changes; there were no expired GPU retries or final camera error.
- Session `20261013-180042` verified the actual Extra Stable checkbox and
  `bump mode 1` with SuperRes at strength 1.0. The 16x viewport visibly responded
  to horizontal pan; reducing zoom to 3.48x also updated the image. It received
  1398 frames and presented 1396, with 23.99 arrival FPS and 24.22 viewport FPS
  (viewport redraws count independently of camera arrivals), four ingress
  drops, zero missed presentation attempts and zero expired GPU retries.
  UI delay p95 was 17.19 ms; maximum was 1007.67 ms. The test exited normally
  at 60 seconds. A final Simple-mode input arrived after that normal exit and
  is not counted as a successful UI action.
- Android's only new exit record is the deliberate force-stop. The app is
  running again. No new crash or ANR was observed. Physical cable recovery and
  long-soak/thermal behavior remain unverified by these short sessions.

Test executable (local, not deployed to the published distribution):
`%LOCALAPPDATA%/OkuFlow/build-no-transcription-release/cmake/cmake/oku_flow.exe`.
SHA-256: `3B9B00B01B14F16C64243992C62AEA239EF7F89A90DA54177550E2B82550688A`.
Matrix log: `%LOCALAPPDATA%/OkuFlow/phone-heavy-check/matrix-after-scale-fix.log`.

## Owner-performed cable cycle, 18:16 follow-up

The owner reported unplugging, waiting and reconnecting after the earlier
profiling child had exited. USB transport changed from ID 1 to ID 2; the phone
app retained PID 16398 across the cycle. Its exit history still ended at the
deliberate 17:59:25 force-stop, with no new crash/ANR recorded.

A new isolated 20-second Windows child automatically connected after replug,
negotiated HEVC 4000x2256 @ 24 FPS and activated the supported 2x SuperRes stage
with stabilization. It measured 23.97 arrival FPS and 24.32 viewport FPS, with
437 received frames, 432 presented, four ingress drops, zero missed presentation
attempts, zero expired GPU retries and no camera error. Exit code was 0. The
phone retained the same PID afterward. Evidence is in
`%LOCALAPPDATA%/OkuFlow/phone-heavy-check/replug-20261013-181651/`.

This confirms phone-process survival across that cable cycle and automatic
Windows startup connection afterward. The Windows receiver was not running
during the physical unplug, so live-stream cable-loss recovery remains open.

## Live physical cable recovery, 18:30 follow-up

Diagnostic duration now accepts up to ten minutes (default still 15 seconds),
allowing the owner time to perform the cable action. Ordinary app launches
remain untimed. The diagnostic build compiled; CPU passed 79/79. Its CUDA
execution passed 78/84, with six GUI cases unresolved: three explicitly marked
desktop interruptions and three remaining assertion failures (held dock pull,
minimum viewport chrome, unrelated-input idle fade). The owner confirmed using
the desktop during this execution. The failures remain preserved and are not
reclassified as passing. No GUI fixture is to run while the owner uses the PC;
background validation must use `OKUFLOW_AGENT_EXCLUDE_GUI=1` and report partial
coverage. Opus's in-progress localization changes also require their own gate.

The isolated Windows child PID 47044 streamed before the owner's physical
unplug. Read-only observation recorded USB transport 2 disappearing at
18:30:44.054 and returning as transport 3 at 18:30:49.238. The same Windows
process remained running and reported responsive throughout the observation.
Logs report `Phone USB connection lost`, automatic reconnection, temporary
1280x720 Windows-camera capture and automatic return to the preferred USB
phone at HEVC 4000x2256 @ 24 FPS. New frame processing/presentation diagnostics
continued after reconnect; SuperRes's supported 2x stage reloaded in 92 ms.

Android retained PID 16398 before and after the cable cycle, with no new exit,
crash or ANR in its historical exit report. No shared adb server was stopped.
The owner confirmed the five-second physical unplug/replug. Background
observation used no mouse/keyboard input. The test's normal close saved its
report after 201.545 seconds; it is no longer running.

The report's final phone-arrival rate is 23.99 FPS and viewport rate 24.25 FPS.
It records 38 ingress drops, 17 missed presentation attempts and six expired
GPU retry frames across the session/handover, with no final camera error.
Counts cross source changes, and per-source arrival counters reset, so they
must not be compared directly to global presented counts. Heartbeat evidence
is capped at the first 3000 samples; it does not cover the full session.
This establishes one live physical cable-recovery cycle on this Pixel, not
long-soak/thermal or other-device acceptance.

Private evidence: `%LOCALAPPDATA%/OkuFlow/phone-heavy-check/live-unplug-20261013-182903/`.
Build SHA-256: `BCA3F495EABF7E9DF43A738FB7F55119FD06B4410BEE1D478C96C526D2B2455B`.
Gate log: `%LOCALAPPDATA%/OkuFlow/phone-heavy-check/matrix-long-diagnostic.log`.

## Guarded adb transport recovery follow-up

The owner reports that ending adb in Task Manager cleared a stuck connection.
That observation does not distinguish a whole-server hang from a stale USB
transport or camera session. A later read-only raw host query found protocol
41 responding (31 ms version query, 16 ms device listing), with an empty
structured device list. No shared server was terminated or live hang induced.

Unreleased Windows code now checks the selected/preferred idle phone with a
separate device echo after non-protocol camera failures or failed preferred
discovery. Only a timeout with a responsive host and the same authorized USB
serial/transport before and after may send a targeted reconnect. One request
per phone/server per minute survives changing transport IDs and failed replies.
Camera pause/busy/problem codes, active ownership, cancellation and explicit
webcam choice keep their existing behavior. A whole hung server is retained;
this is not an automatic global adb restart.

`scripts/agent_build.bat` compiled release and passed CPU 25/25 and CUDA 30/30,
with no skipped Qt rows. All GUI cases were deliberately excluded while the
owner used the desktop; this is partial validation, not a publication gate.
The expanded private `phone_camera_contract` fixtures verify stalled selection,
healthy-device exclusion, global-server hangs, replug races, old listing IDs,
unsupported-service throttling, unauthorized/Wi-Fi exclusions, cancellation,
busy/problem ownership and recovery from startup/preferred-discovery failure.
All 11 Python host-protocol tests also passed. Natural live adb stalls and
long-soak hardware acceptance remain unverified.

Build SHA-256: `61018353E531BD45E7668F9DA65CAE4300941EA085FF8809D59C1E1E83E10D9F`.
Gate log: `%LOCALAPPDATA%/OkuFlow/language-work/windows-adb-recovery-validation.log`.
Private nested preview: `%LOCALAPPDATA%/OkuFlow/bundle-layout-preview/24d284e7591d4569b196a2be80fc77a0/`.
All 152 ZIP entries match the staged bytes and both EXEs match the compiled
outputs. ZIP SHA-256: `CFD82F106F8A5D3F199228A7B3AB022156A1F82CEB1E200AE3EC709E57F8F5BC`.
The preview is unsigned, uses the dirty working tree and has not been published.

## Grouped-bug checkpoint before owned-adb lifecycle work

The RTX 5070 Ti report exposed missing native kernel targets: v0.2.3 presets
listed only `75;86;89`, and local shipped binaries contained those three cubins
with no PTX. Copy-only frames could succeed while an effect or YUV converter
kernel failed. The new shipping binary passed `cuobjdump` coverage for
`75;80;86;89;90;100;103;120;121`. This confirms embedded code, not execution on
an RTX 5070 Ti laptop or ARM/Spark acceptance. GTX 10-series remains outside
the CUDA 13 toolkit target set.

Preset identity, light-theme contrast and stricter USB recovery were integrated.
Recovery now requires two 2-second shell timeouts, a fresh non-claiming companion
timeout, and observed disappearance/new ID after acknowledgment. The D3D11
shutdown query is bounded; unfinished resources remain retained. Discovery
distinguishes a globally unresponsive server and presents localized manual
guidance without killing it. Cooldowns remain per-process, not persistent.

The tracked `scripts/agent_build.bat` checkpoint compiled release and passed CPU
25/25 and CUDA 31/31 with no skipped Qt rows. GUI cases were explicitly excluded
while the owner used the PC; native light/dark/preset interactions and laptop
acceptance remain pending, so this is not release-publication validation.

Shipping Qt EXE SHA-256:
`B79E0921FD153BEAD489448B371247D5D8176ECC3FB65B49C1F63EE221CD6A05`.
Log: `%LOCALAPPDATA%/OkuFlow/bug-batch-20261013/windows-validation.log`.
CUDA ledger: `%LOCALAPPDATA%/OkuFlow/test-evidence/C798FBB6FE42EEC9F3FA8F3131724760CC113B82C03ADCD69E2D441FDC363CB0/46DE9B6FC8A87C84EE4A9D015D24888BD3C9CD8AADF17BAF36904DE398A264C8.json`.

The owner subsequently requested an OkuFlow-owned child server that exits with
the app and can restart after a fault. That is a new lifecycle change; the
checkpoint above does not validate it. A borrowed external/shared server must
retain independent ownership. The website copy/files-card updates were pushed
to its existing repository and verified live independently of Windows builds.
