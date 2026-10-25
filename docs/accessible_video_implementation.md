# Accessible video and UCT implementation

This development change implements the owner's UCT/accessible-video brief on
top of merged PR 8 (`d23f871`). The attached brief is a design input; its
reported defects are verified against production code before repair.

## Recording and source contract

The owner's correction supersedes the brief's concurrent foreign-file playback
rules. Opening a saved video first stops and finalizes an ongoing recording.
Starting a new recording leaves saved playback and returns to the live camera
only after the first successful Original-frame admission. Encoder startup
failure preserves the selected video, cursor and playback intent.
Same-session rewind remains available through the current Original archive.
Stopping that recording preserves the selected position and playback intent.

The bottom transport always names its source. Its forward button follows the
live camera for the current recording, or holds the last decodable frame of a
saved file. Resume continues from the held position. An ordinary live camera
without a recording now has a separate bounded silent RAM recap; see
[the later live recap contract](live_ram_recap.md). Recording archive ownership
and all historical acceptance results below remain distinct.

New video recordings contain one clean full-source Original, including optional
microphone audio. Necessary conversion and orientation are allowed; viewport
crop, reading colours, UCT glyphs, neural enhancement and annotation ink are not.
Paired photos and existing paired-video notes/transcripts stay compatible.

Saved videos with an audio track show a separate **Playback volume** slider
from 0 to 100%, initially 100%. It changes only playback output in this app
session; Mute remains independent. Arrow keys change five percentage points,
Page Up/Down ten, and Home/End select the limits. The visible percentage,
translated accessible name and native slider value stay synchronized during
keyboard, pointer, snapshot and language updates. The slider follows Mute in
the Tab sequence, with Return to camera last; it hides for silent sources,
recording review and collapsed controls. The muted speaker is yellow on dark
surfaces and contrast-adjusted amber on light surfaces; unmuting restores the
normal theme icon color. Existing recording feedback protection stays in force.

The 2026-10-25 audio handoff repair and silent decoder/output test scope are
documented in [the audio repair record](video_audio_preroll_repair.md).

## Ownership and review plan

- Separate Original admission, selected-scene presentation and UCT admission.
  Recorded bytes and accepted capture timestamps remain distinct from the
  displayed cursor, wall-clock playback position and verified seekable tail.
- Decode with one bounded native Media Foundation worker. Pending opens and
  seeks replace older requests. Retained frames own their pixels; old tickets
  cannot replace a newer source or seek. UI cancellation never joins a native
  encoder or decoder call.
- Observe the existing fMP4 writer, fence mutations before writes and index only
  complete proven sample payloads. Concurrent native read is a feasibility gate,
  rather than an assumption based on the file extension or requested GOP size.
- Give immutable UCT selected-frame authority, a separate job deadline and
  distinct-neighbour evidence. Completed unchanged frames need no periodic OCR;
  pan and zoom reproject the retained scene artifact.
- Preserve rejected OCR spans as ordered omissions and protect their geometry
  from accepted replacement plates. Retain observation mappings by insertion
  age rather than random UUID order.
- Keep transport inside the window layout below the render view, with named
  keyboard-accessible native controls and a drag-stable timeline.

Opus and Muse Wonderful independently reviewed the initial design. Both found
the same writer/notes double-path migration trap and the need to test growing
fMP4 reads before claiming recording rewind. Implementation reviews follow the
native vertical slice.

## Validation boundaries

Development uses scoped headless/offscreen checks, synthetic native media and
one tracked release/CPU/CUDA integration matrix with GUI excluded while the
owner uses the desktop. New recordings are decoded through the production
player; active archive reads are checked while the writer remains active.
Failures remain in the tracked evidence ledger. These checks do not establish
screen-reader, optical-camera, installer or public-release acceptance.

## Initial local validation record (2026-10-18, commit 41eb8cd)

The tracked `scripts/agent_build.bat` completed with
`OKUFLOW_AGENT_EXCLUDE_GUI=1`, isolated Release output, CUDA 12.9.86, Qt 6.12.0
and VS 2022. Its final summary is PASS for Release compilation, the CPU suite
and the CUDA delta, explicitly marked partial because desktop tests were
excluded. Fingerprint reuse retained unchanged passing evidence; rebuilt
inputs received new recorded attempts. Earlier failures remain archived.

| Check | Result and scope |
| --- | --- |
| Shipping Release configuration | Compiled successfully with CUDA 12.9. |
| CPU CTest selection | 98 cases passed, zero unresolved. Two cases contain optional skipped Qt rows: `recording_manager` and `mcp_bridge_contract`. The explicit Original-native case ran separately within this selection; the optional private MCP SDK integration was not claimed. |
| CUDA delta | 10 cases passed, no skipped rows or unresolved cases. Includes the three synthetic Maxine lifecycle/navigation/async fixtures and the UCT CUDA fixture. |
| Actual-app native target, Qt offscreen | 43 Qt rows passed. Includes first-frame startup failure, source transitions, exact held-frame authority, repeated paused-poll admission, Save receipt identity and Stop/Undo restoration. No native desktop, device or provider was used. |
| Shared-source equivalence | Passed with PCH enabled in the tracked matrix and disabled in the isolated native fixture configuration. |
| Positioned UCT accessibility | 24 Qt rows at each of 100%, 150% and 200% scale passed in three offscreen CTest cases. These test Qt's accessibility interfaces and paint geometry, not the Windows UIA bridge or a real screen reader. |
| Native synthetic media | Active-before-Finalize AVC/AAC and AV1/AAC decode, continued recording/rewind, sparse-video audio tail, actual EOF, Waiting/resumption pacing and 4K lease pressure passed. The twelve-frame 4K case peaked at 99,532,800 decoded bytes within the unchanged 100,663,296-byte quota. |
| Localization | All 24 non-English catalogs compiled with 872 finished entries each. Small-agent review covered all 1,440 new/changed target entries; see the [review record](uct_recap_localization_review_2026_10_18.md). |

The first final native integration run caught source-scoped undo skipping a view
change across its own temporary camera restore. The repair binds the exception
to the exact owned restore and unchanged selected-media identity/generation;
synthetic native-selection cases and the actual-app fixture then passed.
See the [review disposition](uct_recap_review_2026_10_18.md) for the other
independently verified repairs.

Native desktop/focus/layout tests, the Windows UIA bridge, Narrator/NVDA/Magnifier,
mixed-monitor DPI, physical camera/phone and microphone quality, A/V sync,
long-soak resource measurements, live providers and installer acceptance remain
separate. No installer or public release is certified by this background run.
At that checkpoint, selected-media rendering used the documented CPU view
effects; stateful/neural effects were explicitly unavailable at that checkpoint.
The subsequent GPU work extends processing as described in GPU playback. The seven new media tools
are in the opt-in `dev-full`/`v1` catalogs; this work does not promote the default
MCP catalog or certify V1 acceptance.

## CPU seek/buffer follow-up (2026-10-18)

The tracked background matrix passed again after adaptive admission, small ready
queues, copy-free same-frame reticketing and seek-priority scheduling: Release
compiled, 99 CPU cases and all 10 CUDA delta cases passed. The same two optional
Qt-row skips remain inside passed cases. Native GUI tests were excluded.
New UHD/DCI fixtures retain the application's displayed pair and deliberately
hoard leases, check two actual ready frames, discard real codec preroll without
owned copies, validate superseded seeks, preserve failed-seek pixels and require
ordered Resume/EOF. AVC/AAC and AV1/AAC test pre-end seeks after Jump to End and
sparse-video continuous audio. Earlier diagnostic and Resume failures remain in
the per-attempt ledger; their causes were repaired before this matrix.

On the final short synthetic 5 fps 4K files, eight seek bursts measured UHD
median 217,121 us / maximum 279,443 us and DCI median 210,497 us / maximum
347,235 us from request to worker publication. Six proven same-frame seeks
measured UHD median 30 us / maximum 43 us and DCI median 15 us / maximum 25 us,
with the same shared pixel allocation and no native reads or owned pixel copies.
These small samples are observations, not screen-latency, 4K60, real-lecture or
hardware-decoder acceptance. The CPU path is retained as the compatibility
fallback for the subsequent GPU-playback implementation.


## GPU playback and full-effects follow-up (2026-10-19)

The tracked `scripts/agent_build.bat` finished with PASS for shipping Release
compilation, the CPU suite and the CUDA delta, using CUDA 12.9.86, Qt 6.12.0,
VS 2022 and an isolated Release output. `OKUFLOW_AGENT_EXCLUDE_GUI=1` kept native
desktop fixtures excluded. This is a background validation pass, not full release
or installer acceptance. The complete GPU ownership/effect design is recorded in
[GPU video playback](gpu_video_playback.md).

| Check | Result and scope |
| --- | --- |
| Shipping Release | Compiled the GPU decoder, independent CUDA media scene, full shared effect settings and native transport/app wiring. |
| CPU selection | 108 cases passed, zero unresolved. `recording_manager` and `mcp_bridge_contract` retain their two optional skipped-row cases; no whole case was skipped. Includes window-free GPU decode/conversion/snapshots and explicit Original recording because this machine has a supported GPU. |
| CUDA delta | All 17 cases passed, no skipped rows or unresolved cases. Includes the six kernel/architecture cases, UCT CUDA, three synthetic Maxine fixtures, full media scene, production presenter snapshot, AVC/AV1/UHD/DCI playback and native conversion. |
| Actual-app native target, Qt offscreen | 53 Qt rows passed with zero skips. Muse's added cases cover first-GPU diagnostic uncertainty, retained OCR leases across 125 view replays, bounded retry deadlines and one-second annotation expiry without retargeting. Other cases cover CPU/GPU control policy, candidate identities and prior-document receipt revocation. |
| Shared-source equivalence | Passed with PCH ON in the tracked configurations and PCH OFF in the isolated actual-app fixture. |
| Optional local format inputs | Synthetic HEVC Main10, VP9 and AV1 10-bit cases passed, requiring actual resident output, seek and EOF. These private inputs are declared runtime files, not repository assets or a new FFmpeg dependency. |
| Localization | Translation integrity gate passed for all 24 non-English catalogs, 872 entries each. This follow-up adds no new translation keys. Earlier full language review remains distinct from rendered/native-speaker acceptance. |

The GPU scene case checks actual GPU rotation, blur/color/text/temporal effects,
replay without duplicate history updates, reset boundaries, CPU BGRA uploads,
UCT device identity, paused keystone stepping and asynchronous neural ownership.
Its private fake Maxine SDK proves publication/cancellation and model retention,
not vendor inference quality or measured real-model FPS. The presenter case uses
an unattached composition swapchain, production draw/snapshot shaders and real
fence dependencies without taking the owner's desktop.

The first matrix caught an obsolete fixture expecting multiple queued CPU inputs
after admission was bounded to one in-flight processing pass. The repaired case
preserves its actual blocked-fence and pinned-pixel ownership checks, verifies
Pending/newer-frame rejection without history/scene/fence changes, and passes.
The earlier VP9 negotiation failure was repaired by decoder-output negotiation;
its focused native case and final selection pass. Fingerprint guards also refused
resume across changed PATH/toolchain inputs; those refusals and all original
failure attempts remain archived, alongside the subsequent repaired inputs.
The final run reused only matching passing evidence and has no unresolved cases.

Native GUI/focus/layout execution, Windows UIA/Narrator/NVDA/Magnifier,
mixed-monitor DPI, physical A/V sync, real Maxine quality/performance and long-soak
measurements remain separate acceptance. Two old hosted action-row failures were
traced to a five-button assertion after Open video became the sixth. At that
checkpoint the assertion covered all six; the source-picker follow-up below
moves video out again and preserves geometry checks for the five corner actions.
Native reruns belong on the free hosted desktop. No installer or public release is certified.


## Two-output experiment (2026-10-19)

The explicit [one/two-output comparison](media_output_buffer_benchmark.md) shares
one processing history and uses independent output fences. Thirty-two timed
trials measured GPU processing/drawing completion with minimum-duration phases,
plus target/view changes. Two outputs helped light 4K throughput but did not
improve the throughput/latency balance with text effects, so ordinary app
construction stays at one. The experiment is not part of recurring validation.
Afterward the tracked background Release/CPU/CUDA gate passed again (108 CPU,
17 CUDA), the offscreen native app passed 53 Qt rows, and PCH ON/OFF equivalence
passed. Native desktop/screen-reader and public release boundaries are unchanged.


## Source picker UI follow-up (2026-10-19)

Saved-video opening now belongs to Settings > Camera, through the View video
source option and conditional Select video button below it. The corner row keeps
five capture/assistive actions; Notes and files no longer duplicates the picker.
Ctrl+O and file drop remain available. The existing orientation control rotates
selected media independently of the camera's persisted orientation. Discovery,
file adoption/failure/cancel and Return to camera keep the dropdown honest.
The two new keys have complete 24-catalog structural parity and read-only small
model review of all 48 targets (two vetted Czech/Polish wording corrections).
This is separate from native-speaker, rendered RTL and native screen-reader review.


Validation for this follow-up: shipping Release compilation passed. The tracked
background gate passed 108 CPU cases (two contain optional skipped Qt rows) and
17 CUDA delta cases, with no skipped cases or unresolved background failures.
Matching exact-fingerprint passes were reused: 94 CPU and 16 CUDA cases. The
real-app offscreen fixture ran all 55 Qt rows successfully, including both new
source/orientation cases; PCH OFF shared-source equivalence passed alongside it.
Native GUI/focus, actual Windows file-picker interaction, screen-reader and mixed
DPI acceptance remain pending. No local desktop, camera, browser or phone was
operated for this validation. Logs remain under LocalAppData/OkuFlow/
video-source-ui-20261019. The initial scoped selector correctly required the full
background gate; its refusal log is retained. The previous merged branch's hosted
run also retains independent MCP shutdown-timing and phone-recovery failures;
this UI change does not claim to repair those failures.
